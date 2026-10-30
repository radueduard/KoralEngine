#include <kasset/parameters.h>

#include <kecs/json.h>

#include <format>
#include <utility>
#include <vector>

namespace kasset
{
    namespace {
        using kecs::json::Quote;
        using kecs::json::Value;

        /** An object's members as their JSON texts, in order: nothing for what is no object. */
        std::optional<std::vector<std::pair<std::string, std::string>>> Members(const std::string_view json)
        {
            std::vector<std::pair<std::string, std::string>> members;
            if (json.find_first_not_of(" \t\r\n") == std::string_view::npos) return members;
            Value root;
            std::string error;
            if (!kecs::json::Parse(json, root, error) || root.kind != Value::Kind::eObject) return std::nullopt;
            for (const auto& [name, value] : root.members) members.emplace_back(name, std::string(value.raw));
            return members;
        }

        std::string List(const std::vector<std::string>& texts)
        {
            std::string out = "[";
            for (std::size_t i = 0; i < texts.size(); ++i) out += std::format("{}{}", i ? ", " : "", Quote(texts[i]));
            return out + "]";
        }

        std::optional<std::vector<std::string>> Texts(const Value& list)
        {
            if (list.kind != Value::Kind::eArray) return std::nullopt;
            std::vector<std::string> texts;
            for (const auto& item : list.items) {
                if (item.kind != Value::Kind::eString) return std::nullopt;
                texts.push_back(item.string);
            }
            return texts;
        }

        std::optional<Value> Object(const std::string_view json)
        {
            Value root;
            std::string error;
            if (!kecs::json::Parse(json, root, error) || root.kind != Value::Kind::eObject) return std::nullopt;
            return root;
        }
    }

    std::string ParameterOf(const Meta& meta, const std::string_view key)
    {
        const auto members = Members(meta.parameters);
        if (!members) return {};
        for (const auto& [name, json] : *members) if (name == key) return json;
        return {};
    }

    bool SetParameter(Meta& meta, const std::string_view key, const std::string_view json)
    {
        auto members = Members(meta.parameters).value_or(std::vector<std::pair<std::string, std::string>> {});
        bool found = false, changed = false;
        for (auto it = members.begin(); it != members.end(); ++it) {
            if (it->first != key) continue;
            found = true;
            if (json.empty()) { members.erase(it); changed = true; }
            else if (it->second != json) { it->second = json; changed = true; }
            break;
        }
        if (!found && !json.empty()) { members.emplace_back(key, json); changed = true; }
        if (!changed) return false;
        std::string out;
        if (!members.empty()) {
            out = "{";
            for (std::size_t i = 0; i < members.size(); ++i) out += std::format("{}\n        {}: {}", i ? "," : "", Quote(members[i].first), members[i].second);
            out += "\n    }";
        }
        meta.parameters = std::move(out);
        return true;
    }

    std::string ToJson(const MeshImport& import)
    {
        std::string buffers = "[";
        for (std::size_t i = 0; i < import.buffers.size(); ++i) buffers += std::format("{}{}", i ? ", " : "", List(import.buffers[i]));
        buffers += "]";
        return std::format("{{ \"leaveOut\": {}, \"buffers\": {} }}", List(import.leaveOut), buffers);
    }

    std::optional<MeshImport> MeshImportFrom(const std::string_view json)
    {
        const auto root = Object(json);
        if (!root) return std::nullopt;
        MeshImport import;
        if (const Value* leaveOut = root->Find("leaveOut")) {
            auto texts = Texts(*leaveOut);
            if (!texts) return std::nullopt;
            import.leaveOut = std::move(*texts);
        }
        if (const Value* buffers = root->Find("buffers")) {
            if (buffers->kind != Value::Kind::eArray) return std::nullopt;
            for (const auto& buffer : buffers->items) {
                auto texts = Texts(buffer);
                if (!texts) return std::nullopt;
                if (!texts->empty()) import.buffers.push_back(std::move(*texts));
            }
        }
        return import;
    }

    std::string ToJson(const TextureImport& import)
    {
        return std::format("{{ \"format\": {}, \"quality\": {} }}", Quote(TextureFormatName(import.format)), Quote(TextureQualityName(import.quality)));
    }

    std::optional<TextureImport> TextureImportFrom(const std::string_view json)
    {
        const auto root = Object(json);
        if (!root) return std::nullopt;
        TextureImport import;
        if (const Value* format = root->Find("format")) {
            const auto named = format->kind == Value::Kind::eString ? TextureFormatNamed(format->string) : std::nullopt;
            if (!named) return std::nullopt;
            import.format = *named;
        }
        if (const Value* quality = root->Find("quality")) {
            const auto named = quality->kind == Value::Kind::eString ? TextureQualityNamed(quality->string) : std::nullopt;
            if (!named) return std::nullopt;
            import.quality = *named;
        }
        return import;
    }

    std::string ToJson(const std::map<std::string, std::string>& kinds)
    {
        std::string out = "{";
        bool first = true;
        for (const auto& [reported, kind] : kinds) { out += std::format("{} {}: {}", first ? "" : ",", Quote(reported), Quote(kind)); first = false; }
        return out + " }";
    }

    std::optional<std::map<std::string, std::string>> KindsFrom(const std::string_view json)
    {
        const auto root = Object(json);
        if (!root) return std::nullopt;
        std::map<std::string, std::string> kinds;
        for (const auto& [reported, kind] : root->members) {
            if (kind.kind != Value::Kind::eString) return std::nullopt;
            kinds[reported] = kind.string;
        }
        return kinds;
    }
}
