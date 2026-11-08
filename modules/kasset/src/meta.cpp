#include <kasset/meta.h>

#include <kecs/json.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace kasset
{
    namespace {
        constexpr std::array<std::pair<Kind, std::string_view>, 13> kinds { {
            { Kind::eModel, "model" }, { Kind::eMesh, "mesh" }, { Kind::eMaterial, "material" },
            { Kind::eTexture, "texture" }, { Kind::eShader, "shader" }, { Kind::eShaderEntry, "shaderEntry" },
            { Kind::eLight, "light" }, { Kind::eCamera, "camera" }, { Kind::eScript, "script" },
            { Kind::eScene, "scene" }, { Kind::ePrefab, "prefab" }, { Kind::eFunction, "function" },
            { Kind::eGenerated, "generated" },
        } };

        std::string Lower(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        /** A JSON object's text as it is to be written: none for nothing, or for an empty object. */
        bool Empty(const std::string_view object)
        {
            const auto inside = object.find_first_not_of(" \t\r\n{}");
            return inside == std::string_view::npos;
        }
    }

    std::string_view KindName(const Kind kind)
    {
        for (const auto& [k, name] : kinds) if (k == kind) return name;
        return {};
    }

    std::optional<Kind> KindNamed(const std::string_view name)
    {
        for (const auto& [k, n] : kinds) if (n == name) return k;
        return std::nullopt;
    }

    std::optional<Kind> KindOfFile(const std::filesystem::path& file)
    {
        const std::string name = Lower(file.filename().string());
        if (name.ends_with(".scene.json")) return Kind::eScene;
        const std::string extension = Lower(file.extension().string());
        // What the framework's model importer reads (assimp's, and glTF's own), and its image importer.
        static constexpr std::array<std::string_view, 12> models { ".gltf", ".glb", ".obj", ".fbx", ".dae", ".3ds", ".blend", ".ply", ".stl", ".x3d", ".usd", ".usdz" };
        static constexpr std::array<std::string_view, 11> textures { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr", ".ktx2", ".ktx", ".dds", ".psd" };
        if (std::ranges::contains(models, extension)) return Kind::eModel;
        if (std::ranges::contains(textures, extension)) return Kind::eTexture;
        if (extension == ".slang") return Kind::eShader;
        if (extension == ".cs" || extension == ".kt") return Kind::eScript;
        if (extension == ".prefab") return Kind::ePrefab;
        if (extension == ".kfunc") return Kind::eFunction;
        if (extension == ".kmat") return Kind::eMaterial;
        if (extension == ".kgen") return Kind::eGenerated;
        return std::nullopt;
    }

    const Part* Meta::FindPart(const Kind kind, const std::string_view name, int occurrence) const
    {
        for (const Part& part : parts)
            if (part.kind == kind && part.name == name && occurrence-- == 0) return &part;
        return nullptr;
    }

    const Part* Meta::FindPart(const Id id) const
    {
        const auto at = std::ranges::find(parts, id, &Part::id);
        return at == parts.end() ? nullptr : &*at;
    }

    const Part* Meta::PartAt(const Kind kind, const int index) const
    {
        for (const Part& part : parts) if (part.kind == kind && part.index == index) return &part;
        return nullptr;
    }

    std::filesystem::path MetaPathOf(const std::filesystem::path& root, const std::filesystem::path& file)
    {
        if (KindOfFile(file) == Kind::eGenerated) return file;
        const std::filesystem::path relative = file.is_absolute() && !root.empty() ? file.lexically_normal().lexically_relative(root.lexically_normal()) : file;
        std::filesystem::path meta = root / MetaFolder / relative;
        meta += ".meta";
        return meta;
    }

    std::filesystem::path FileOfMeta(const std::filesystem::path& root, const std::filesystem::path& meta)
    {
        std::filesystem::path file = root / meta.lexically_normal().lexically_relative((root / MetaFolder).lexically_normal());
        file.replace_extension();
        return file;
    }

    std::filesystem::path MetaBeside(const std::filesystem::path& file)
    {
        std::filesystem::path meta = file;
        meta += ".meta";
        return meta;
    }

    bool IsMeta(const std::filesystem::path& file) { return Lower(file.extension().string()) == ".meta"; }

    std::string Write(const Meta& meta)
    {
        using kecs::json::Quote;
        std::string out = "{\n";
        out += std::format("    \"id\": {},\n", Quote(meta.id.Text()));
        out += std::format("    \"kind\": {}", Quote(KindName(meta.kind)));
        if (!meta.producer.empty()) out += std::format(",\n    \"producer\": {}", Quote(meta.producer));
        if (!Empty(meta.parameters)) out += std::format(",\n    \"parameters\": {}", meta.parameters);
        if (!meta.parts.empty()) {
            out += ",\n    \"parts\": [";
            for (std::size_t i = 0; i < meta.parts.size(); ++i) {
                const Part& part = meta.parts[i];
                out += std::format("{}\n        {{ \"id\": {}, \"kind\": {}, \"name\": {}", i ? "," : "", Quote(part.id.Text()), Quote(KindName(part.kind)), Quote(part.name));
                if (part.index >= 0) out += std::format(", \"index\": {}", part.index);
                out += " }";
            }
            out += "\n    ]";
        }
        if (!Empty(meta.overrides)) out += std::format(",\n    \"overrides\": {}", meta.overrides);
        out += "\n}\n";
        return out;
    }

    std::expected<Meta, std::string> Read(const std::string_view text)
    {
        using kecs::json::Value;
        Value root;
        std::string error;
        if (!kecs::json::Parse(text, root, error)) return std::unexpected(error);
        if (root.kind != Value::Kind::eObject) return std::unexpected("it is no JSON object");

        Meta meta;
        const Value* id = root.Find("id");
        if (!id || id->kind != Value::Kind::eString) return std::unexpected("it has no \"id\"");
        const auto parsed = Id::Parse(id->string);
        if (!parsed) return std::unexpected(std::format("\"{}\" is no ID: an ID is 32 hex digits", id->string));
        meta.id = *parsed;

        const Value* kind = root.Find("kind");
        if (!kind || kind->kind != Value::Kind::eString) return std::unexpected("it has no \"kind\"");
        const auto named = KindNamed(kind->string);
        if (!named) return std::unexpected(std::format("\"{}\" is no kind of asset", kind->string));
        meta.kind = *named;

        if (const Value* producer = root.Find("producer"); producer && producer->kind == Value::Kind::eString) meta.producer = producer->string;
        if (const Value* parameters = root.Find("parameters"); parameters && parameters->kind == Value::Kind::eObject) meta.parameters = parameters->raw;
        if (const Value* overrides = root.Find("overrides"); overrides && overrides->kind == Value::Kind::eObject) meta.overrides = overrides->raw;
        if (const Value* parts = root.Find("parts"); parts && parts->kind == Value::Kind::eArray)
            for (const Value& item : parts->items) {
                if (item.kind != Value::Kind::eObject) return std::unexpected("a part is no JSON object");
                Part part;
                const Value* partId = item.Find("id");
                const auto partParsed = partId && partId->kind == Value::Kind::eString ? Id::Parse(partId->string) : std::nullopt;
                if (!partParsed) return std::unexpected("a part has no ID");
                part.id = *partParsed;
                const Value* partKind = item.Find("kind");
                const auto partNamed = partKind && partKind->kind == Value::Kind::eString ? KindNamed(partKind->string) : std::nullopt;
                if (!partNamed) return std::unexpected(std::format("part {} has no kind", part.id.Text()));
                part.kind = *partNamed;
                if (const Value* name = item.Find("name"); name && name->kind == Value::Kind::eString) part.name = name->string;
                if (const Value* index = item.Find("index"); index && index->kind == Value::Kind::eNumber) part.index = static_cast<int>(index->number);
                if (meta.FindPart(part.id)) return std::unexpected(std::format("two parts are {}", part.id.Text()));
                meta.parts.push_back(std::move(part));
            }
        return meta;
    }

    std::expected<Meta, std::string> Load(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::unexpected(std::format("{} could not be opened", path.string()));
        std::ostringstream text;
        text << in.rdbuf();
        const std::string read = text.str();
        auto meta = Read(read);
        if (!meta) return std::unexpected(std::format("{}: {}", path.string(), meta.error()));
        return meta;
    }

    std::expected<void, std::string> Save(const std::filesystem::path& path, const Meta& meta)
    {
        std::filesystem::path written = path;
        written += ".writing";
        {
            // The folders it is in: a meta is kept apart from its file, in folders of its own (MetaFolder).
            std::error_code made;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), made);
            std::ofstream out(written, std::ios::binary | std::ios::trunc);
            if (!out) return std::unexpected(std::format("{} could not be written", path.string()));
            out << Write(meta);
            if (!out.flush()) return std::unexpected(std::format("{} could not be written", path.string()));
        }
        std::error_code failed;
        std::filesystem::rename(written, path, failed);
        if (failed) {
            std::filesystem::remove(written, failed);
            return std::unexpected(std::format("{} could not be written: {}", path.string(), failed.message()));
        }
        return {};
    }
}
