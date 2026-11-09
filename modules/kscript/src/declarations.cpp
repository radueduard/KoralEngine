//
// kscript: a script's component, read from its source. Not a compiler: enough of C# and Kotlin to find the classes
// that are scripts, their public fields at the top of the class, and what each starts as.
//

#include <kscript/declarations.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>

namespace kscript
{
    namespace {
        /** A field type as both hosts describe it: its name among components, its size and its alignment. */
        struct Kind {
            const char* name;
            std::size_t size, alignment;
            char scalar;            // 'f' float, 'd' double, 'i' signed, 'u' unsigned, 'b' bool, 'e' an entity
            std::size_t numbers;    // how many make it up
        };

        const Kind* KindOf(const std::string& type, const bool kotlin)
        {
            static const std::map<std::string, Kind, std::less<>> csharp {
                { "float", { "f32", 4, 4, 'f', 1 } }, { "double", { "f64", 8, 8, 'd', 1 } }, { "bool", { "bool", 1, 1, 'b', 1 } },
                { "sbyte", { "i8", 1, 1, 'i', 1 } }, { "short", { "i16", 2, 2, 'i', 1 } }, { "int", { "i32", 4, 4, 'i', 1 } }, { "long", { "i64", 8, 8, 'i', 1 } },
                { "byte", { "u8", 1, 1, 'u', 1 } }, { "ushort", { "u16", 2, 2, 'u', 1 } }, { "uint", { "u32", 4, 4, 'u', 1 } }, { "ulong", { "u64", 8, 8, 'u', 1 } },
                { "Vec2", { "vec2", 8, 4, 'f', 2 } }, { "Vec3", { "vec3", 12, 4, 'f', 3 } }, { "Vec4", { "vec4", 16, 4, 'f', 4 } },
                { "IVec2", { "ivec2", 8, 4, 'i', 2 } }, { "IVec3", { "ivec3", 12, 4, 'i', 3 } }, { "IVec4", { "ivec4", 16, 4, 'i', 4 } },
                { "UVec2", { "uvec2", 8, 4, 'u', 2 } }, { "UVec3", { "uvec3", 12, 4, 'u', 3 } }, { "UVec4", { "uvec4", 16, 4, 'u', 4 } },
                { "Mat4", { "mat4", 64, 4, 'f', 16 } }, { "Quat", { "quat", 16, 4, 'f', 4 } }, { "Entity", { "entity", 8, 4, 'e', 2 } },
                { "string", { "string", sizeof(std::string), alignof(std::string), 's', 0 } },
            };
            static const std::map<std::string, Kind, std::less<>> kotlinTypes {
                { "Float", { "f32", 4, 4, 'f', 1 } }, { "Double", { "f64", 8, 8, 'd', 1 } }, { "Int", { "i32", 4, 4, 'i', 1 } },
                { "Long", { "i64", 8, 8, 'i', 1 } }, { "Boolean", { "bool", 1, 1, 'b', 1 } },
                { "Vec2", { "vec2", 8, 4, 'f', 2 } }, { "Vec3", { "vec3", 12, 4, 'f', 3 } }, { "Vec4", { "vec4", 16, 4, 'f', 4 } },
                { "Mat4", { "mat4", 64, 4, 'f', 16 } }, { "Quat", { "quat", 16, 4, 'f', 4 } }, { "Entity", { "entity", 8, 4, 'e', 2 } },
                { "String", { "string", sizeof(std::string), alignof(std::string), 's', 0 } },
            };
            const auto& types = kotlin ? kotlinTypes : csharp;
            // `Koral.Vec3`, `koral.Vec3`: the same type.
            const std::string bare = type.substr(type.rfind('.') == std::string::npos ? 0 : type.rfind('.') + 1);
            const auto it = types.find(bare);
            return it == types.end() ? nullptr : &it->second;
        }

        /** A Material's picture: a shared reference, laid out at the front of it. */
        const Kind TextureKind { "Image", TextureSize, alignof(std::shared_ptr<void>), 't', 0 };

        bool IsTexture(const std::string& written)
        {
            return written.substr(written.rfind('.') == std::string::npos ? 0 : written.rfind('.') + 1) == "Texture";
        }

        /**
         * What a Texture field starts as, by the name of the picture: `Texture.White` (`$white`), `Texture.Black`,
         * `Texture.Normal`, or one given by name — `"@textures/ramp.png"`, `new("...")`, `Texture("...")`. Null for what
         * is not understood without running it.
         */
        std::optional<std::string> PictureOf(const std::string& value)
        {
            if (value.empty()) return "$white";
            static const std::regex constant(R"(^(?:[\w.]+\.)?Texture\.(White|Black|Normal)$)");
            static const std::regex named(R"rx(^(?:new\s*(?:[\w.]+)?\s*\(|(?:[\w.]+\.)?Texture\s*\()?\s*"((?:[^"\\]|\\.)*)"\s*\)?$)rx");
            std::smatch found;
            if (std::regex_match(value, found, constant)) {
                std::string name = "$" + found[1].str();
                name[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[1])));
                return name;
            }
            if (!std::regex_match(value, found, named)) return std::nullopt;
            // Kotlin writes a dollar in a string as `\$`.
            std::string name;
            const std::string said = found[1].str();
            for (std::size_t i = 0; i < said.size(); ++i) {
                if (said[i] == '\\' && i + 1 < said.size()) ++i;
                name += said[i];
            }
            return name;
        }

        /** The source without its comments: each a space, its line breaks kept, so that lines are where they were. Strings are left as they are. */
        std::string Uncommented(const std::string_view source)
        {
            std::string out;
            out.reserve(source.size());
            for (std::size_t i = 0; i < source.size(); ++i) {
                const char c = source[i];
                if (c == '"') {
                    // A string, to its end: what is in it is no comment, and no brace.
                    out += c;
                    for (++i; i < source.size() && source[i] != '"'; ++i) {
                        if (source[i] == '\\' && i + 1 < source.size()) out += source[i++];
                        out += source[i];
                    }
                    if (i < source.size()) out += '"';
                } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
                    while (i < source.size() && source[i] != '\n') ++i;
                    out += '\n';
                } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                    for (i += 2; i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'); ++i)
                        if (source[i] == '\n') out += '\n';
                    ++i;
                    out += ' ';
                } else {
                    out += c;
                }
            }
            return out;
        }

        /** Where the brace that closes the one at @p open is: past the end, when none does. Braces in strings are none. */
        std::size_t Closing(const std::string& text, const std::size_t open)
        {
            int depth = 0;
            for (std::size_t i = open; i < text.size(); ++i) {
                if (text[i] == '"') { for (++i; i < text.size() && text[i] != '"'; ++i) if (text[i] == '\\') ++i; continue; }
                if (text[i] == '{') ++depth;
                else if (text[i] == '}' && --depth == 0) return i;
            }
            return text.size();
        }

        std::string Trim(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
            return std::string(text);
        }

        /**
         * What is declared at the top of a class's body, a statement each: up to a `;` (or, in Kotlin, the end of a line
         * outside brackets). What has a body of its own — a method, a property's accessors, an init block — is none.
         */
        std::vector<std::string> Statements(const std::string& body, const bool kotlin)
        {
            std::vector<std::string> statements;
            std::string current;
            int brackets = 0;
            bool nested = false;    // the statement has a body: it is no field
            const auto end = [&] {
                if (!nested && !Trim(current).empty()) statements.push_back(Trim(current));
                current.clear();
                nested = false;
                brackets = 0;
            };
            for (std::size_t i = 0; i < body.size(); ++i) {
                const char c = body[i];
                if (c == '"') {
                    // A string is what it says: no brace in it opens anything.
                    current += c;
                    for (++i; i < body.size() && body[i] != '"'; ++i) {
                        if (body[i] == '\\' && i + 1 < body.size()) current += body[i++];
                        current += body[i];
                    }
                    if (i < body.size()) current += '"';
                    continue;
                }
                if (c == '{') { nested = true; i = Closing(body, i); if (!kotlin) end(); continue; }
                if (c == '(' || c == '[' || c == '<') ++brackets;
                else if (c == ')' || c == ']' || c == '>') brackets = std::max(brackets - 1, 0);
                if (c == ';' || (kotlin && c == '\n' && brackets == 0)) { end(); continue; }
                current += c;
            }
            end();
            return statements;
        }

        /** The numbers of a starting value: `2f`, `-0.5`, `new(1f, 0.85f, 0.6f)`, `Vec3(1f, 1f, 1f)`, `true`. None when it is anything else. */
        std::optional<std::vector<double>> Numbers(const std::string& value)
        {
            static const std::regex number(R"(^\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)(?:[fFdDmM]|[uU]?[lL]{0,2}|[lL][uU])?\s*$)");
            static const std::regex made(R"(^\s*(?:new\s*)?(?:[\w.]+)?\s*\(([\s\S]*)\)\s*$)");
            std::smatch found;
            if (value == "true") return std::vector { 1.0 };
            if (value == "false") return std::vector { 0.0 };
            if (std::regex_match(value, found, number)) return std::vector { std::strtod(found[1].str().c_str(), nullptr) };
            if (!std::regex_match(value, found, made)) return std::nullopt;
            std::vector<double> numbers;
            std::stringstream parts(found[1].str());
            for (std::string part; std::getline(parts, part, ',');) {
                if (Trim(part).empty()) continue;
                auto one = Numbers(Trim(part));
                if (!one || one->size() != 1) return std::nullopt;
                numbers.push_back(one->front());
            }
            return numbers;
        }

        /** What a Kotlin property is when its type is not written: what it is initialised to says. */
        std::string Inferred(const std::string& value)
        {
            static const std::regex call(R"(^\s*([\w.]+)\s*\()");
            std::smatch found;
            if (std::regex_search(value, found, call)) return found[1].str();
            // A type's constant, `Entity.Null`, `Vec3.Zero`: of that type. (A capital first, so no number like 1.5.)
            static const std::regex constant(R"(^\s*([A-Z]\w*)\.[A-Za-z_]\w*\s*$)");
            if (std::regex_match(value, found, constant)) return found[1].str();
            if (value == "true" || value == "false") return "Boolean";
            if (!value.empty() && (value.back() == 'f' || value.back() == 'F')) return "Float";
            if (!value.empty() && value.back() == 'L') return "Long";
            if (value.find('.') != std::string::npos || value.find('e') != std::string::npos || value.find('E') != std::string::npos) return "Double";
            if (!value.empty() && (std::isdigit(static_cast<unsigned char>(value.back())))) return "Int";
            return {};
        }

        /** A number as a source writes it: `0`, `-1.5f`, `1e3`, `2.0d`. Null for what is no number. */
        std::optional<float> Literal(std::string text)
        {
            text = Trim(text);
            while (!text.empty() && (text.back() == 'f' || text.back() == 'F' || text.back() == 'd' || text.back() == 'D' || text.back() == 'L')) text.pop_back();
            if (text.empty()) return std::nullopt;
            char* end = nullptr;
            const float value = std::strtof(text.c_str(), &end);
            return end == text.c_str() + text.size() ? std::optional(value) : std::nullopt;
        }

        /**
         * What a field is kept within, as its attributes say: C#'s `[Range(0, 10)]` and `[Min(0)]`, Kotlin's `@Range(0f, 10f)`
         * and `@Min(0f)` — as Unity has them. What an editor lets it be set to; `[Logarithmic]`, how it is dragged; and
         * `[Shown]`, whether it is among the few shown of its component at first.
         */
        void Limits(const std::string& statement, kecs::FieldDescription& field)
        {
            static const std::regex range(R"([\[@,]\s*(?:[\w.]+\.)?Range\s*\(\s*(?:min\s*=\s*)?([^,()]+),\s*(?:max\s*=\s*)?([^,()]+)\))");
            static const std::regex least(R"([\[@,]\s*(?:[\w.]+\.)?Min\s*\(\s*(?:min\s*=\s*)?([^,()]+)\))");
            static const std::regex logarithmic(R"([\[@,]\s*(?:[\w.]+\.)?Logarithmic\b)");
            static const std::regex shown(R"([\[@,]\s*(?:[\w.]+\.)?Shown\b)");
            static const std::regex choices(R"([\[@,]\s*(?:[\w.]+\.)?Choices\s*\(([^)]*)\))");
            static const std::regex choice(R"rx("((?:[^"\\]|\\.)*)")rx");
            std::smatch found;
            field.logarithmic = std::regex_search(statement, logarithmic);
            field.shown = std::regex_search(statement, shown);
            if (std::regex_search(statement, found, choices)) {
                const std::string listed = found[1].str();
                for (auto it = std::sregex_iterator(listed.begin(), listed.end(), choice); it != std::sregex_iterator(); ++it) field.choices.push_back((*it)[1].str());
            }
            if (std::regex_search(statement, found, range)) {
                const auto low = Literal(found[1].str()), high = Literal(found[2].str());
                if (low && high && *low <= *high) { field.min = *low; field.max = *high; }
            } else if (std::regex_search(statement, found, least)) {
                if (const auto low = Literal(found[1].str())) field.min = *low;
            }
        }

        /** What a ResourceScript can make: its resource types, by the node type each is made as. */
        bool IsResourceKind(const std::string& written)
        {
            const std::string bare = written.substr(written.rfind('.') == std::string::npos ? 0 : written.rfind('.') + 1);
            return bare == "Image" || bare == "Buffer" || bare == "ImageView" || bare == "BufferView" || bare == "Sampler";
        }

        /** What is between brackets, apart at the commas that are not in brackets or strings of their own. */
        std::vector<std::string> Arguments(const std::string& text)
        {
            std::vector<std::string> parts;
            std::string current;
            int depth = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (c == '"') {
                    current += c;
                    for (++i; i < text.size() && text[i] != '"'; ++i) {
                        if (text[i] == '\\' && i + 1 < text.size()) current += text[i++];
                        current += text[i];
                    }
                    if (i < text.size()) current += '"';
                    continue;
                }
                if (c == '(' || c == '[' || c == '{') ++depth;
                else if (c == ')' || c == ']' || c == '}') --depth;
                if (c == ',' && depth == 0) { parts.push_back(Trim(current)); current.clear(); continue; }
                current += c;
            }
            if (!Trim(current).empty()) parts.push_back(Trim(current));
            return parts;
        }

        std::string JsonString(const std::string_view text)
        {
            std::string out = "\"";
            for (const char c : text) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            return out + '"';
        }

        /**
         * What a resource is made with, as its source writes it, as JSON: a string (`"eR32_SFLOAT"`, flags as
         * `"eSampled | eStorage"`), a number, true or false, a vector (`new(256, 256)`), a list (`["eSampled"]`,
         * `listOf(...)`), another field by name (`nameof(Height)`, `::height.name`, `height`), or an enumeration's value
         * (`Format.eR32_SFLOAT`). Null for what is not understood without running it.
         */
        std::optional<std::string> SettingJson(std::string value)
        {
            value = Trim(value);
            static const std::regex text(R"rx(^"((?:[^"\\]|\\.)*)"$)rx");
            static const std::regex nameOf(R"(^nameof\s*\(\s*([\w.]+)\s*\)$)");
            static const std::regex reference(R"(^(?:this)?::(\w+)\.name$)");
            static const std::regex word(R"(^[A-Za-z_][\w.]*$)");
            static const std::regex listed(R"(^(?:\[([\s\S]*)\]|new\s*(?:\w+\s*)?\[\s*\]\s*\{([\s\S]*)\}|(?:listOf|arrayOf|setOf)\s*\(([\s\S]*)\))$)");
            std::smatch found;
            if (std::regex_match(value, found, text)) {
                const std::string said = found[1].str();
                if (said.find('|') == std::string::npos) return JsonString(said);
                std::string list = "[";
                std::stringstream parts(said);
                for (std::string part; std::getline(parts, part, '|');)
                    if (!Trim(part).empty()) list += (list.size() > 1 ? ", " : "") + JsonString(Trim(part));
                return list + "]";
            }
            if (value == "true" || value == "false") return value;
            if (std::regex_match(value, found, nameOf) || std::regex_match(value, found, reference)) {
                const std::string name = found[1].str();
                return JsonString(name.substr(name.rfind('.') == std::string::npos ? 0 : name.rfind('.') + 1));
            }
            if (std::regex_match(value, found, listed)) {
                const std::string inside = found[1].matched ? found[1].str() : found[2].matched ? found[2].str() : found[3].str();
                std::string list = "[";
                for (const auto& part : Arguments(inside)) {
                    const auto one = SettingJson(part);
                    if (!one) return std::nullopt;
                    list += (list.size() > 1 ? ", " : "") + *one;
                }
                return list + "]";
            }
            if (const auto numbers = Numbers(value)) {
                if (numbers->size() == 1) return std::format("{}", numbers->front());
                std::string list = "[";
                for (const double n : *numbers) list += std::format("{}{}", list.size() > 1 ? ", " : "", n);
                return list + "]";
            }
            if (std::regex_match(value, word)) return JsonString(value.substr(value.rfind('.') == std::string::npos ? 0 : value.rfind('.') + 1));
            return std::nullopt;
        }

        /**
         * A resource a ResourceScript declares: its kind, and what its initialiser's arguments — each given by name —
         * make it with. What is not understood is said in @p problems, and left to start as its node type has it.
         */
        DeclaredResource Resource(const std::string& name, const std::string& written, const std::string& value, const std::string& where,
                                  std::vector<std::string>& problems)
        {
            DeclaredResource resource;
            resource.name = name;
            resource.kind = written.substr(written.rfind('.') == std::string::npos ? 0 : written.rfind('.') + 1);
            if (value.empty()) return resource;
            static const std::regex made(R"(^\s*(?:new\b\s*)?(?:[\w.]+)?\s*\(([\s\S]*)\)\s*$)");
            static const std::regex named(R"(^(\w+)\s*[:=]\s*([\s\S]+)$)");
            std::smatch found;
            if (!std::regex_match(value, found, made)) {
                problems.push_back(std::format("{}{} is made as '{}', which is not understood without running it: it is made as a {} node starts", where, name, value, resource.kind));
                return resource;
            }
            for (const auto& argument : Arguments(found[1].str())) {
                std::smatch part;
                if (!std::regex_match(argument, part, named)) {
                    problems.push_back(std::format("{}{} is given '{}': each of what a resource is made with is given by name, `format: ...`", where, name, argument));
                    continue;
                }
                std::string input = part[1].str();
                if (input == "width") input = "extent.x";
                else if (input == "height") input = "extent.y";
                else if (input == "depth") input = "extent.z";
                if (const auto json = SettingJson(part[2].str())) resource.settings.emplace_back(std::move(input), *json);
                else problems.push_back(std::format("{}{}'s {} is '{}', which is not understood without running it: it is as a {} node starts", where, name, part[1].str(), Trim(part[2].str()), resource.kind));
            }
            return resource;
        }

        /** One number of a field's starting value, written as the field holds it. */
        void Write(std::byte* at, const char scalar, const std::size_t size, const double number)
        {
            if (scalar == 'f') { const auto v = static_cast<float>(number); std::memcpy(at, &v, 4); return; }
            if (scalar == 'd') { std::memcpy(at, &number, 8); return; }
            if (scalar == 'b') { *at = std::byte { number != 0.0 ? std::uint8_t { 1 } : std::uint8_t { 0 } }; return; }
            const auto whole = static_cast<long long>(std::llround(number));
            std::memcpy(at, &whole, std::min<std::size_t>(size, sizeof whole));     // little-endian: the low bytes first
        }
    }

    namespace {
        /** What a script's class derives from at the root: what it is. */
        const std::vector<std::string_view> Roots { "Setup", "Script", "GraphicsScript", "ResourceScript", "Material", "Data" };

        /** A class as its source says it: what it derives from, and the statements at the top of its body — not yet fields. */
        struct Class {
            std::string name;           ///< As the source names it: what another class derives from it by.
            std::string component;      ///< Its component's name: its ScriptName's, or its own.
            std::string base;           ///< What it derives from, the first after the colon, bare.
            std::string where;          ///< Its file, as a problem says where.
            std::string path;           ///< Its file, whole: what its type says it is declared in.
            bool kotlin = false, isAbstract = false;
            std::vector<std::string> pieces;
            std::vector<std::string> statements;
        };

        /** Every class @p source declares that derives from anything, in the order it declares them. */
        std::vector<Class> Classes(const std::string_view source, const bool kotlin, const std::string_view file, const std::string_view path = {})
        {
            std::vector<Class> out;
            const std::string text = Uncommented(source);
            const std::string where = file.empty() ? std::string() : std::format("{}: ", file);
            // A class, what it is said to be before its body, and its body. One with no body (a Kotlin `data class Dot(...)`)
            // is passed by: what is said of it ends at the next class, whose body is not its.
            static const std::regex classes(R"(\bclass\s+(\w+)((?:(?!\bclass\s+\w)[^{;])*)\{)");
            // What it derives from: C#'s first after the colon; Kotlin's the one it calls the constructor of.
            static const std::regex csharpBase(R"(:\s*(?:[\w.]+\.)?(\w+)\b)");
            static const std::regex kotlinBase(R"(:\s*(?:[\w.]+\.)?(\w+)\s*\(\s*\))");
            static const std::regex named(R"rx(ScriptName\s*\(\s*(?:name\s*=\s*)?"([^"]*)"\s*\)\s*\]?\s*[\w\s]*$)rx");
            static const std::regex piece(R"(\b(PerFrame|FixedStep|BeforeDraw|MeshGenerator|ImageGenerator)\b)");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), classes); it != std::sregex_iterator(); ++it) {
                const std::smatch& found = *it;
                const std::string header = found[2].str();
                std::smatch base;
                if (!std::regex_search(header, base, kotlin ? kotlinBase : csharpBase)) continue;
                // What it is said to be, before `class`: abstract, and a ScriptName that names it otherwise.
                const std::size_t at = static_cast<std::size_t>(found.position(0));
                const std::size_t from = text.find_last_of(";}{", at == 0 ? 0 : at - 1);
                const std::string before = text.substr(from == std::string::npos ? 0 : from + 1, at - (from == std::string::npos ? 0 : from + 1));
                Class one;
                one.name = one.component = found[1].str();
                one.base = base[1].str();
                one.where = where;
                one.path = std::string(path);
                one.kotlin = kotlin;
                one.isAbstract = std::regex_search(before, std::regex(R"(\babstract\b)"));
                if (std::smatch name; std::regex_search(before, name, named)) one.component = name[1].str();
                // The pieces it takes: the names after its base, as its source writes them.
                for (auto p = std::sregex_iterator(header.begin(), header.end(), piece); p != std::sregex_iterator(); ++p) one.pieces.push_back((*p)[1].str());
                const std::size_t open = at + static_cast<std::size_t>(found.length(0)) - 1;
                const std::size_t close = Closing(text, open);
                one.statements = Statements(text.substr(open + 1, close - open - 1), kotlin);
                out.push_back(std::move(one));
            }
            return out;
        }

        /**
         * The script types of @p classes: each that is no abstract class — or is an abstract Data, a kind — whose bases, each
         * among them, come down from one of the Roots. A base's fields come first, as the hosts lay them out.
         */
        Declarations Resolve(const std::vector<Class>& classes)
        {
            Declarations out;
            static const std::regex csharpField(R"(^(?:\[[^\]]*\]\s*)*((?:\w+\s+)*)([\w.]+)\s+(\w+)\s*(?:=(?!>)\s*([\s\S]*))?$)");
            static const std::regex kotlinField(R"(^(?:@[\w.]+(?:\([^)]*\))?\s*)*((?:\w+\s+)*)(var|val)\s+(\w+)\s*(?::\s*([\w.]+))?\s*(?:=\s*([\s\S]*))?$)");
            // A Kotlin annotation on a line of its own is the next property's.
            static const std::regex annotationsOnly(R"(^(?:@[\w.]+(?:\([^)]*\))?\s*)+$)");
            const auto find = [&](const std::string& name, const bool kotlin) -> const Class* {
                for (const auto& c : classes) if (c.name == name && c.kotlin == kotlin) return &c;
                return nullptr;
            };
            for (const Class& declared : classes) {
                // Down from its root: the chain of what it derives from, its root's first.
                std::vector<const Class*> chain { &declared };
                std::string root;
                while (true) {
                    const std::string& base = chain.back()->base;
                    if (std::ranges::contains(Roots, base)) { root = base; break; }
                    const Class* above = find(base, declared.kotlin);
                    if (!above || std::ranges::contains(chain, above) || chain.size() > 32) break;   // no script's, or round in a circle
                    chain.push_back(above);
                }
                if (root.empty()) continue;
                const bool data = root == "Data" || root == "Material";
                // An abstract class is a kind when it is Data: what is one of it is found by it. Any other is no script.
                if (declared.isAbstract && root != "Data") continue;
                std::ranges::reverse(chain);

                ScriptType type;
                type.name = declared.component;
                type.file = declared.path;
                type.makesResources = root == "ResourceScript";
                type.material = root == "Material";
                type.data = data;
                type.abstract = declared.isAbstract;
                for (auto above = chain.rbegin() + 1; above != chain.rend(); ++above)
                    if ((*above)->isAbstract && root == "Data") type.kinds.push_back((*above)->component);     // nearest first
                for (const Class* each : chain) type.pieces.insert(type.pieces.end(), each->pieces.begin(), each->pieces.end());
                if (data && !type.pieces.empty()) {
                    out.problems.push_back(std::format("{}{} is {}, which is data and nothing else: it takes no {}", declared.where, type.name,
                                                       type.material ? "a Material" : "Data", type.pieces.front()));
                    continue;
                }

                bool fine = true;
                // Each field as it is written, laid out once they are all read: a material's pictures first.
                struct Parsed { std::string name, statement, value; const Kind* kind; };
                std::vector<Parsed> parsed;
                for (const Class* each : chain) {
                    const bool kotlin = each->kotlin;
                    std::string pending;
                    for (const std::string& line : each->statements) {
                        const std::string statement = pending.empty() ? line : pending + " " + line;
                        if (kotlin && std::regex_match(line, annotationsOnly)) { pending = statement; continue; }
                        pending.clear();
                        std::smatch field;
                        std::string name, written, value, modifiers;
                        if (kotlin) {
                            if (!std::regex_match(statement, field, kotlinField)) continue;
                            modifiers = field[1].str();
                            name = field[3].str();
                            written = field[4].str();
                            value = Trim(field[5].str());
                            if (std::regex_search(modifiers, std::regex(R"(\b(?:private|protected|internal)\b)"))) continue;
                            if (written.empty()) written = Inferred(value);
                            // A resource it makes is one, kept as it is made: a val.
                            if (type.makesResources && IsResourceKind(written)) { type.resources.push_back(Resource(name, written, value, each->where + type.name + ".", out.problems)); continue; }
                            if (field[2].str() == "val") continue;
                        } else {
                            if (!std::regex_match(statement, field, csharpField)) continue;
                            modifiers = field[1].str();
                            written = field[2].str();
                            name = field[3].str();
                            value = Trim(field[4].str());
                            if (!std::regex_search(modifiers, std::regex(R"(\bpublic\b)"))) continue;
                            if (type.makesResources && IsResourceKind(written) && !std::regex_search(modifiers, std::regex(R"(\b(?:static|const)\b)"))) {
                                type.resources.push_back(Resource(name, written, value, each->where + type.name + ".", out.problems));
                                continue;
                            }
                            if (std::regex_search(modifiers, std::regex(R"(\b(?:static|const|readonly|override|abstract|virtual|event)\b)"))) continue;
                        }
                        const Kind* kind = type.material && IsTexture(written) ? &TextureKind : KindOf(written, kotlin);
                        if (!kind) {
                            out.problems.push_back(std::format("{}{}.{} is a {}, which a {} cannot keep", each->where, type.name, name, written.empty() ? "type not written" : written,
                                                               IsTexture(written) ? "script (only a Material has pictures)" : "script"));
                            fine = false;
                            break;
                        }
                        parsed.push_back({ name, statement, value, kind });
                    }
                    if (!fine) break;
                }
                if (!fine) continue;
                std::ranges::stable_partition(parsed, [](const Parsed& p) { return p.kind == &TextureKind; });
                std::size_t offset = 0, alignment = 1;
                struct Start { std::size_t offset; const Kind* kind; std::vector<double> numbers; };
                std::vector<Start> starts;
                const std::string& where = declared.where;
                for (const auto& [name, statement, value, kind] : parsed) {
                    offset = (offset + kind->alignment - 1) / kind->alignment * kind->alignment;
                    type.fields.push_back({ .name = name, .type = kind->name, .offset = offset, .size = kind->size });
                    Limits(statement, type.fields.back());
                    if (kind->scalar == 's') {
                        // Text: what is between its quotes, as it is written there — or nothing.
                        static const std::regex quoted(R"rx(^@?"((?:[^"\\]|\\.)*)"$)rx");
                        std::smatch found;
                        if (value.empty() || value == "null" || value == "\"\"" || value == "string.Empty" || value == "String.Empty") {}
                        else if (std::regex_match(value, found, quoted)) {
                            std::string text;
                            const std::string said = found[1].str();
                            for (std::size_t i = 0; i < said.size(); ++i) {
                                if (said[i] != '\\' || i + 1 == said.size()) { text += said[i]; continue; }
                                const char next = said[++i];
                                text += next == 'n' ? '\n' : next == 't' ? '\t' : next;
                            }
                            type.texts[name] = std::move(text);
                        } else {
                            out.problems.push_back(std::format("{}{}.{} starts as '{}', which is not understood without running it: it starts empty", where, type.name, name, value));
                        }
                    } else if (kind == &TextureKind) {
                        const auto picture = PictureOf(value);
                        if (!picture) out.problems.push_back(std::format("{}{}.{} starts as '{}', which is not understood without running it: it starts white", where, type.name, name, value));
                        type.textures.emplace_back(name, picture.value_or("$white"));
                    } else if (!value.empty()) {
                        auto numbers = Numbers(value);
                        const bool identity = kind->numbers == 16 && numbers && numbers->empty();
                        if (identity) numbers = std::vector<double> { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
                        // One number for a whole vector, `Vec2(1f)`: each of its numbers.
                        if (numbers && numbers->size() == 1 && kind->numbers >= 2 && kind->numbers <= 4) numbers = std::vector<double>(kind->numbers, numbers->front());
                        if (numbers && kind->scalar != 'e' && (numbers->size() == kind->numbers || numbers->empty())) starts.push_back({ offset, kind, std::move(*numbers) });
                        else if (kind->scalar == 'e') {}   // an entity starts as none, whatever it is said to start as: one is set in the editor
                        else out.problems.push_back(std::format("{}{}.{} starts as '{}', which is not understood without running it: it starts at zero", where, type.name, name, value));
                    }
                    offset += kind->size;
                    alignment = std::max(alignment, kind->alignment);
                }
                type.alignment = alignment;
                type.size = (offset + alignment - 1) / alignment * alignment;
                type.initial.assign(type.size, std::byte { 0 });
                for (const auto& start : starts)
                    for (std::size_t n = 0; n < start.numbers.size(); ++n) {
                        const std::size_t width = start.kind->size / start.kind->numbers;
                        Write(type.initial.data() + start.offset + n * width, start.kind->scalar, width, start.numbers[n]);
                    }
                if (std::ranges::all_of(type.initial, [](const std::byte b) { return b == std::byte { 0 }; })) type.initial.clear();
                out.types.push_back(std::move(type));
            }
            return out;
        }
    }

    Declarations Declare(const std::string_view source, const bool kotlin, const std::string_view file)
    {
        return Resolve(Classes(source, kotlin, file));
    }

    Declarations DeclareFiles(const std::filesystem::path& path)
    {
        std::vector<std::filesystem::path> files;
        std::error_code ignored;
        if (std::filesystem::is_directory(path, ignored)) {
            for (auto it = std::filesystem::recursive_directory_iterator(path, ignored); it != std::filesystem::recursive_directory_iterator(); it.increment(ignored)) {
                const auto& file = it->path();
                const std::string text = file.generic_string();
                if (text.find("/bin/") != std::string::npos || text.find("/obj/") != std::string::npos || text.find("/build/") != std::string::npos) continue;
                if (file.extension() == ".cs" || file.extension() == ".kt") files.push_back(file);
            }
            std::ranges::sort(files);
        } else {
            files.push_back(path);
        }
        return DeclareFiles(files);
    }

    Declarations DeclareFiles(const std::vector<std::filesystem::path>& files)
    {
        Declarations out;
        // Every class of every file first, then what each is: a class may derive from one in another file.
        std::vector<Class> classes;
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) { out.problems.push_back(std::format("{} could not be read", file.generic_string())); continue; }
            const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto found = Classes(source, file.extension() == ".kt", file.filename().generic_string(), file.generic_string());
            classes.insert(classes.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        }
        auto resolved = Resolve(classes);
        for (auto& type : resolved.types) out.types.push_back(std::move(type));
        for (auto& problem : resolved.problems) out.problems.push_back(std::move(problem));
        return out;
    }

    std::filesystem::path PackageDirectory()
    {
        if (const char* given = std::getenv("KORAL_ENGINE_PACKAGE"); given && *given) return given;
#ifdef KORAL_ENGINE_PACKAGE
        return KORAL_ENGINE_PACKAGE;
#else
        return {};
#endif
    }

    std::vector<std::string> DeclarePackage(Scripts& scripts, std::vector<ScriptType>* materials)
    {
        const std::filesystem::path folder = PackageDirectory() / "scripts";
        std::error_code ignored;
        if (!std::filesystem::is_directory(folder, ignored)) return { std::format("the engine's package is not at {}", folder.generic_string()) };
        // In C# and in Kotlin alike, for the scripts of either: the same types, by the C# files.
        // Every file's classes at once: a Camera's kinds are in a file of their own.
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
            if (entry.path().extension() == ".cs") files.push_back(entry.path());
        std::ranges::sort(files);
        std::vector<Class> classes;
        std::vector<std::string> unread;
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) { unread.push_back(std::format("{} could not be read", file.generic_string())); continue; }
            const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto found = Classes(source, false, file.filename().generic_string(), file.generic_string());
            classes.insert(classes.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        }
        Declarations found = Resolve(classes);
        found.problems.insert(found.problems.begin(), unread.begin(), unread.end());
        std::ranges::sort(found.types, {}, &ScriptType::name);
        for (auto& type : found.types) {
            if (type.makesResources) continue;      // a script that makes resources is no component
            if (type.material) { if (materials) materials->push_back(std::move(type)); continue; }
            std::string error;
            const std::string name = type.name;
            if (scripts.Declare(std::move(type), &error) == kecs::NoComponent) found.problems.push_back(std::format("{}: {}", name, error));
        }
        return found.problems;
    }
}
