#include <kecs/json.h>

#include <charconv>
#include <format>

namespace kecs::json
{
    const Value* Value::Find(const std::string_view key) const
    {
        for (const auto& [name, value] : members) if (name == key) return &value;
        return nullptr;
    }

    namespace {
        struct Parser {
            std::string_view text;
            std::size_t at = 0;
            std::string error;

            bool Fail(const std::string_view what)
            {
                if (error.empty()) error = std::format("{} at character {}", what, at);
                return false;
            }
            void Space() { while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\t' || text[at] == '\r')) ++at; }

            bool String(std::string& out)
            {
                if (at >= text.size() || text[at] != '"') return Fail("expected a string");
                ++at;
                while (at < text.size() && text[at] != '"') {
                    char c = text[at++];
                    if (c != '\\') { out += c; continue; }
                    if (at >= text.size()) return Fail("unfinished escape");
                    c = text[at++];
                    switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (at + 4 > text.size()) return Fail("unfinished \\u escape");
                        unsigned code = 0;
                        if (std::from_chars(text.data() + at, text.data() + at + 4, code, 16).ptr != text.data() + at + 4) return Fail("bad \\u escape");
                        at += 4;
                        // As UTF-8; pairs of surrogates are left as they are, which a saved world never writes.
                        if (code < 0x80) out += static_cast<char>(code);
                        else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
                        else { out += static_cast<char>(0xE0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
                        break;
                    }
                    default: out += c; break;   // \" \\ \/
                    }
                }
                if (at >= text.size()) return Fail("unfinished string");
                ++at;
                return true;
            }

            bool Parse(Value& out, const int depth = 0)
            {
                if (depth > 200) return Fail("nested too deep");
                Space();
                if (at >= text.size()) return Fail("unexpected end");
                const std::size_t start = at;
                const char c = text[at];
                if (c == '{') {
                    out.kind = Value::Kind::eObject;
                    ++at;
                    Space();
                    if (at < text.size() && text[at] == '}') { ++at; }
                    else for (;;) {
                        Space();
                        std::string key;
                        if (!String(key)) return false;
                        Space();
                        if (at >= text.size() || text[at] != ':') return Fail("expected ':'");
                        ++at;
                        Value member;
                        if (!Parse(member, depth + 1)) return false;
                        out.members.emplace_back(std::move(key), std::move(member));
                        Space();
                        if (at < text.size() && text[at] == ',') { ++at; continue; }
                        if (at < text.size() && text[at] == '}') { ++at; break; }
                        return Fail("expected ',' or '}'");
                    }
                } else if (c == '[') {
                    out.kind = Value::Kind::eArray;
                    ++at;
                    Space();
                    if (at < text.size() && text[at] == ']') { ++at; }
                    else for (;;) {
                        Value item;
                        if (!Parse(item, depth + 1)) return false;
                        out.items.push_back(std::move(item));
                        Space();
                        if (at < text.size() && text[at] == ',') { ++at; continue; }
                        if (at < text.size() && text[at] == ']') { ++at; break; }
                        return Fail("expected ',' or ']'");
                    }
                } else if (c == '"') {
                    out.kind = Value::Kind::eString;
                    if (!String(out.string)) return false;
                } else if (text.substr(at, 4) == "true") { out.kind = Value::Kind::eBool; out.boolean = true; at += 4; }
                else if (text.substr(at, 5) == "false") { out.kind = Value::Kind::eBool; at += 5; }
                else if (text.substr(at, 4) == "null") { at += 4; }
                else {
                    out.kind = Value::Kind::eNumber;
                    const auto [end, problem] = std::from_chars(text.data() + at, text.data() + text.size(), out.number);
                    if (problem != std::errc() || end == text.data() + at) return Fail("expected a value");
                    at = static_cast<std::size_t>(end - text.data());
                }
                out.raw = text.substr(start, at - start);
                return true;
            }
        };
    }

    bool Parse(const std::string_view text, Value& out, std::string& error)
    {
        Parser parser { text, 0, {} };
        if (!parser.Parse(out)) { error = parser.error; return false; }
        parser.Space();
        if (parser.at != text.size()) { parser.Fail("something after the end"); error = parser.error; return false; }
        return true;
    }

    std::string Quote(const std::string_view text)
    {
        std::string out = "\"";
        for (const char c : text) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else out += c;
            }
        }
        return out + "\"";
    }
}
