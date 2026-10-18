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
            };
            static const std::map<std::string, Kind, std::less<>> kotlinTypes {
                { "Float", { "f32", 4, 4, 'f', 1 } }, { "Double", { "f64", 8, 8, 'd', 1 } }, { "Int", { "i32", 4, 4, 'i', 1 } },
                { "Long", { "i64", 8, 8, 'i', 1 } }, { "Boolean", { "bool", 1, 1, 'b', 1 } },
                { "Vec2", { "vec2", 8, 4, 'f', 2 } }, { "Vec3", { "vec3", 12, 4, 'f', 3 } }, { "Vec4", { "vec4", 16, 4, 'f', 4 } },
                { "Mat4", { "mat4", 64, 4, 'f', 16 } }, { "Quat", { "quat", 16, 4, 'f', 4 } }, { "Entity", { "entity", 8, 4, 'e', 2 } },
            };
            const auto& types = kotlin ? kotlinTypes : csharp;
            // `Koral.Vec3`, `koral.Vec3`: the same type.
            const std::string bare = type.substr(type.rfind('.') == std::string::npos ? 0 : type.rfind('.') + 1);
            const auto it = types.find(bare);
            return it == types.end() ? nullptr : &it->second;
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
            if (value == "true" || value == "false") return "Boolean";
            if (!value.empty() && (value.back() == 'f' || value.back() == 'F')) return "Float";
            if (!value.empty() && value.back() == 'L') return "Long";
            if (value.find('.') != std::string::npos || value.find('e') != std::string::npos || value.find('E') != std::string::npos) return "Double";
            if (!value.empty() && (std::isdigit(static_cast<unsigned char>(value.back())))) return "Int";
            return {};
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

    Declarations Declare(const std::string_view source, const bool kotlin, const std::string_view file)
    {
        Declarations out;
        const std::string text = Uncommented(source);
        const std::string where = file.empty() ? std::string() : std::format("{}: ", file);
        // A class, what it is said to be before its body, and its body.
        static const std::regex classes(R"(\bclass\s+(\w+)([^{;]*)\{)");
        static const std::regex csharpBase(R"(:\s*(?:[\w.]+\.)?(?:Script|GraphicsScript)\b)");
        static const std::regex kotlinBase(R"(:\s*(?:[\w.]+\.)?(?:Script|GraphicsScript)\s*\(\s*\))");
        static const std::regex named(R"rx(ScriptName\s*\(\s*(?:name\s*=\s*)?"([^"]*)"\s*\)\s*\]?\s*[\w\s]*$)rx");
        static const std::regex csharpField(R"(^(?:\[[^\]]*\]\s*)*((?:\w+\s+)*)([\w.]+)\s+(\w+)\s*(?:=(?!>)\s*([\s\S]*))?$)");
        static const std::regex kotlinField(R"(^(?:@[\w.]+(?:\([^)]*\))?\s*)*((?:\w+\s+)*)var\s+(\w+)\s*(?::\s*([\w.]+))?\s*(?:=\s*([\s\S]*))?$)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), classes); it != std::sregex_iterator(); ++it) {
            const std::smatch& found = *it;
            const std::string header = found[2].str();
            if (!std::regex_search(header, kotlin ? kotlinBase : csharpBase)) continue;
            // What it is said to be, before `class`: abstract is no script, and a ScriptName names it otherwise.
            const std::size_t at = static_cast<std::size_t>(found.position(0));
            const std::size_t from = text.find_last_of(";}{", at == 0 ? 0 : at - 1);
            const std::string before = text.substr(from == std::string::npos ? 0 : from + 1, at - (from == std::string::npos ? 0 : from + 1));
            if (std::regex_search(before, std::regex(R"(\babstract\b)"))) continue;
            ScriptType type;
            type.name = found[1].str();
            if (std::smatch name; std::regex_search(before, name, named)) type.name = name[1].str();

            const std::size_t open = at + static_cast<std::size_t>(found.length(0)) - 1;
            const std::size_t close = Closing(text, open);
            bool fine = true;
            std::size_t offset = 0, alignment = 1;
            struct Start { std::size_t offset; const Kind* kind; std::vector<double> numbers; };
            std::vector<Start> starts;
            for (const std::string& statement : Statements(text.substr(open + 1, close - open - 1), kotlin)) {
                std::smatch field;
                std::string name, written, value, modifiers;
                if (kotlin) {
                    if (!std::regex_match(statement, field, kotlinField)) continue;
                    modifiers = field[1].str();
                    name = field[2].str();
                    written = field[3].str();
                    value = Trim(field[4].str());
                    if (std::regex_search(modifiers, std::regex(R"(\b(?:private|protected|internal)\b)"))) continue;
                    if (written.empty()) written = Inferred(value);
                } else {
                    if (!std::regex_match(statement, field, csharpField)) continue;
                    modifiers = field[1].str();
                    written = field[2].str();
                    name = field[3].str();
                    value = Trim(field[4].str());
                    if (!std::regex_search(modifiers, std::regex(R"(\bpublic\b)"))
                        || std::regex_search(modifiers, std::regex(R"(\b(?:static|const|readonly|override|abstract|virtual|event)\b)")))
                        continue;
                }
                const Kind* kind = KindOf(written, kotlin);
                if (!kind) {
                    out.problems.push_back(std::format("{}{}.{} is a {}, which a script cannot keep", where, type.name, name, written.empty() ? "type not written" : written));
                    fine = false;
                    break;
                }
                offset = (offset + kind->alignment - 1) / kind->alignment * kind->alignment;
                type.fields.push_back({ .name = name, .type = kind->name, .offset = offset, .size = kind->size });
                if (!value.empty()) {
                    auto numbers = Numbers(value);
                    const bool identity = kind->numbers == 16 && numbers && numbers->empty();
                    if (identity) numbers = std::vector<double> { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
                    if (numbers && kind->scalar != 'e' && (numbers->size() == kind->numbers || numbers->empty())) starts.push_back({ offset, kind, std::move(*numbers) });
                    else if (kind->scalar == 'e') {}   // an entity starts as none, whatever it is said to start as: one is set in the editor
                    else out.problems.push_back(std::format("{}{}.{} starts as '{}', which is not understood without running it: it starts at zero", where, type.name, name, value));
                }
                offset += kind->size;
                alignment = std::max(alignment, kind->alignment);
            }
            if (!fine) continue;
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

    Declarations DeclareFiles(const std::filesystem::path& path)
    {
        Declarations out;
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
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) { out.problems.push_back(std::format("{} could not be read", file.generic_string())); continue; }
            const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto found = Declare(source, file.extension() == ".kt", file.filename().generic_string());
            for (auto& type : found.types) out.types.push_back(std::move(type));
            for (auto& problem : found.problems) out.problems.push_back(std::move(problem));
        }
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

    std::vector<std::string> DeclarePackage(Scripts& scripts)
    {
        const std::filesystem::path folder = PackageDirectory() / "scripts";
        std::error_code ignored;
        if (!std::filesystem::is_directory(folder, ignored)) return { std::format("the engine's package is not at {}", folder.generic_string()) };
        // In C# and in Kotlin alike, for the scripts of either: the same types, by the C# files.
        Declarations found;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
            if (entry.path().extension() == ".cs") {
                auto one = DeclareFiles(entry.path());
                for (auto& type : one.types) found.types.push_back(std::move(type));
                for (auto& problem : one.problems) found.problems.push_back(std::move(problem));
            }
        std::ranges::sort(found.types, {}, &ScriptType::name);
        for (auto& type : found.types) {
            std::string error;
            const std::string name = type.name;
            if (scripts.Declare(std::move(type), &error) == kecs::NoComponent) found.problems.push_back(std::format("{}: {}", name, error));
        }
        return found.problems;
    }
}
