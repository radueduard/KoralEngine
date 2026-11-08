#include <kasset/rewrite.h>

#include <kasset/database.h>
#include <kasset/id.h>

#include <kecs/json.h>

#include <format>

namespace kasset
{
    namespace {
        /** A JSON string's text, unescaped: what is between its quotes. */
        std::optional<std::string> Unquoted(const std::string_view quoted)
        {
            kecs::json::Value value;
            std::string error;
            if (!kecs::json::Parse(quoted, value, error) || value.kind != kecs::json::Value::Kind::eString) return std::nullopt;
            return value.string;
        }
    }

    std::string RewriteStrings(const std::string_view json, const std::function<std::optional<std::string>(std::string_view)>& replace)
    {
        std::string out;
        out.reserve(json.size());
        std::size_t at = 0;
        while (at < json.size()) {
            const std::size_t open = json.find('"', at);
            if (open == std::string_view::npos) { out.append(json.substr(at)); break; }
            out.append(json.substr(at, open - at));
            std::size_t close = open + 1;
            while (close < json.size() && json[close] != '"') close += json[close] == '\\' ? 2 : 1;
            if (close >= json.size()) { out.append(json.substr(open)); break; }    // not JSON: as it is
            const std::string_view quoted = json.substr(open, close + 1 - open);
            at = close + 1;
            // A key is followed by a colon.
            std::size_t next = at;
            while (next < json.size() && (json[next] == ' ' || json[next] == '\t' || json[next] == '\r' || json[next] == '\n')) ++next;
            if (next < json.size() && json[next] == ':') { out.append(quoted); continue; }
            const auto text = Unquoted(quoted);
            const auto replaced = text ? replace(*text) : std::nullopt;
            out.append(replaced ? kecs::json::Quote(*replaced) : std::string(quoted));
        }
        return out;
    }

    namespace {
        /**
         * @p text with what @p replace gives for what names something: the whole of it, or — a line at a time — what each
         * line ends with after a space, as a material's link to its asset is written (`PbrMaterial @ship.gltf#material:3`).
         */
        std::optional<std::string> Named(const std::string_view text, const std::function<std::optional<std::string>(std::string_view)>& replace)
        {
            if (auto whole = replace(text)) return whole;
            if (text.find(' ') == std::string_view::npos) return std::nullopt;
            std::string out;
            bool any = false;
            for (std::size_t at = 0; at <= text.size();) {
                const std::size_t end = std::min(text.find('\n', at), text.size());
                const std::string_view line = text.substr(at, end - at);
                const std::size_t space = line.find(' ');
                const auto named = space == std::string_view::npos ? std::nullopt : replace(line.substr(space + 1));
                out += named ? std::format("{} {}", line.substr(0, space), *named) : std::string(line);
                any = any || named.has_value();
                if (end < text.size()) out += '\n';
                at = end + 1;
            }
            return any ? std::optional<std::string>(std::move(out)) : std::nullopt;
        }
    }

    std::string ToIds(const std::string_view json, Database& assets)
    {
        const auto replace = [&](const std::string_view text) -> std::optional<std::string> {
            // Only a file's — and a function's, as a call keeps it (`function:Scale`): the engine's own never move, and keep the
            // names every program knows them by.
            if (!text.starts_with('@') && !text.starts_with("function:")) return std::nullopt;
            const auto reference = assets.FromLegacy(text);
            if (!reference) return std::nullopt;
            return reference->Text();
        };
        return RewriteStrings(json, [&](const std::string_view text) { return Named(text, replace); });
    }

    std::string FromIds(const std::string_view json, const Database& assets, std::size_t* missing)
    {
        if (missing) *missing = 0;
        const auto replace = [&](const std::string_view text) -> std::optional<std::string> {
            const auto reference = Reference::Parse(text);
            if (!reference) return std::nullopt;
            std::string named = assets.ToLegacy(*reference);
            if (named.empty()) {
                if (missing) ++*missing;
                return std::nullopt;
            }
            return named;
        };
        return RewriteStrings(json, [&](const std::string_view text) { return Named(text, replace); });
    }
}
