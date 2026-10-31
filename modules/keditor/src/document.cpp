#include <keditor/document.h>

#include <kasset/cache.h>
#include <kasset/parameters.h>
#include <kasset/rewrite.h>

#include <algorithm>
#include <array>
#include <set>
#include <cmath>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>

#include <kecs/json.h>
#include <kgraph/catalog.h>
#include <kgraph/meshes.h>
#include <kgraph/entities.h>
#include <kscript/declarations.h>
#include <shader.h>

namespace keditor
{
    using kgraph::Value;

    namespace {
        using Held = std::shared_ptr<kgraph::Resource>;

        /** What a field of plain numbers is made of: how many, of what. */
        struct Numbers {
            char scalar = 0;        // 'f' float, 'd' double, 'i' int, 'u' unsigned, 'b' bool
            std::size_t bytes = 0;  // of one
            std::size_t count = 0;  // 1: shown as a number, not a list
        };
        bool NumbersOf(const std::string_view type, Numbers& out)
        {
            static const std::pair<std::string_view, Numbers> scalars[] {
                { "f32", { 'f', 4, 1 } }, { "f64", { 'd', 8, 1 } }, { "bool", { 'b', 1, 1 } }, { "bool32", { 'b', 4, 1 } },
                { "i8", { 'i', 1, 1 } }, { "i16", { 'i', 2, 1 } }, { "i32", { 'i', 4, 1 } }, { "i64", { 'i', 8, 1 } },
                { "u8", { 'u', 1, 1 } }, { "u16", { 'u', 2, 1 } }, { "u32", { 'u', 4, 1 } }, { "u64", { 'u', 8, 1 } },
                { "mat4", { 'f', 4, 16 } }, { "quat", { 'f', 4, 4 } } };
            for (const auto& [name, numbers] : scalars)
                if (name == type) { out = numbers; return true; }
            for (const auto& [prefix, scalar] : { std::pair<std::string_view, char> { "vec", 'f' }, { "ivec", 'i' }, { "uvec", 'u' }, { "bvec", 'u' } })
                if (type.size() == prefix.size() + 1 && type.starts_with(prefix) && type.back() >= '2' && type.back() <= '4') {
                    out = { scalar, 4, static_cast<std::size_t>(type.back() - '0') };
                    return true;
                }
            return false;
        }
        double ReadNumber(const Numbers& numbers, const std::byte* at)
        {
            switch (numbers.scalar) {
            case 'f': { float v; std::memcpy(&v, at, 4); return v; }
            case 'd': { double v; std::memcpy(&v, at, 8); return v; }
            case 'i': { std::int64_t v = 0; std::memcpy(&v, at, numbers.bytes);
                        const int shift = static_cast<int>(64 - numbers.bytes * 8); return static_cast<double>((v << shift) >> shift); }
            default: { std::uint64_t v = 0; std::memcpy(&v, at, numbers.bytes); return static_cast<double>(v); }
            }
        }
        void WriteNumber(const Numbers& numbers, std::byte* at, const double value)
        {
            switch (numbers.scalar) {
            case 'f': { const auto v = static_cast<float>(value); std::memcpy(at, &v, 4); break; }
            case 'd': std::memcpy(at, &value, 8); break;
            case 'i': { const auto v = static_cast<std::int64_t>(std::llround(value)); std::memcpy(at, &v, numbers.bytes); break; }
            case 'b': { const std::uint64_t v = value != 0.0 ? 1 : 0; std::memcpy(at, &v, numbers.bytes); break; }
            default: { const auto v = static_cast<std::uint64_t>(std::llround(std::max(value, 0.0))); std::memcpy(at, &v, numbers.bytes); break; }
            }
        }

        // What the editor keeps on an entity of its own, which is no component to show, give or copy: a name from an
        // older file, and which prefab an instance is of.
        bool Structural(const std::string_view component) { return component == "Name" || component == "Prefab" || component == "PrefabPart"; }
    }

    bool HoldsResource(const std::string_view type)
    {
        return type == "resource" || type == "Image" || type == "Buffer" || type == "Sampler" || type == "BufferView" || type == "AccelerationStructure"
            || type == "Mesh";
    }
    bool Editable(const std::string_view type)
    {
        Numbers numbers;
        return type == "string" || type == "entity" || HoldsResource(type) || NumbersOf(type, numbers);
    }

    Value EntityValue(const kecs::Entity entity)
    {
        if (entity.IsNull()) return {};
        return Value(Value::Members { { "entity", static_cast<double>(entity.index) }, { "generation", static_cast<double>(entity.generation) } });
    }

    std::optional<kecs::Entity> EntityOfValue(const Value& value)
    {
        if (value.IsNull()) return kecs::NullEntity;
        const Value* index = value.Find("entity");
        const Value* generation = value.Find("generation");
        if (!index || !index->Number() || !generation || !generation->Number()) return std::nullopt;
        return kecs::Entity { static_cast<std::uint32_t>(*index->Number()), static_cast<std::uint32_t>(*generation->Number()) };
    }

    namespace {
        std::map<const kor::FrameGraph*, Document*>& Documents()
        {
            static std::map<const kor::FrameGraph*, Document*> documents;
            return documents;
        }
    }

    namespace {
        std::map<const kor::FrameGraph*, const kor::FrameGraph*>& Aliases()
        {
            static std::map<const kor::FrameGraph*, const kor::FrameGraph*> aliases;
            return aliases;
        }
    }

    Document* Document::Of(const kor::FrameGraph* frameGraph)
    {
        if (const auto alias = Aliases().find(frameGraph); alias != Aliases().end()) frameGraph = alias->second;
        const auto it = Documents().find(frameGraph);
        return it == Documents().end() ? nullptr : it->second;
    }

    void Document::Alias(const kor::FrameGraph* other, const kor::FrameGraph* frameGraph)
    {
        if (frameGraph) Aliases()[other] = frameGraph;
        else Aliases().erase(other);
    }

    Document::Document(kor::FrameGraph& frameGraph) : _frameGraph(&frameGraph), _runtime(std::make_unique<kgraph::Runtime>(frameGraph, &_world))
    {
        Documents()[_frameGraph] = this;
        kgraph::RegisterCodecs(_codecs, _runtime.get());
        _world.Id<Label>();
        _world.Id<PrefabLink>();
        _world.Id<MaterialLink>();
        _world.Id<PrefabPart>();
        _world.Id<kgraph::Mesh>();
        // What every scene has, whatever its project: the engine's package — the Camera, the lights, the Sky — and its
        // shaders' modules, for any shader to import (`import entity;`).
        std::vector<kscript::ScriptType> materials;
        for (auto& problem : kscript::DeclarePackage(_scripts, &materials)) _notes.push_back(std::move(problem));
        for (auto& kind : materials) DeclareMaterialKind(std::move(kind));
        _saved = _revision;     // what every scene has is no change to it
        static const bool searched = (kor::Shader::AddSearchPath(kscript::PackageDirectory() / "shaders"), true);
        (void)searched;
    }

    void Document::DeclareScripts(const std::filesystem::path& folder)
    {
        std::error_code ignored;
        if (!std::filesystem::exists(folder, ignored)) return;     // a folder of scripts, or one script's file
        auto declared = kscript::DeclareFiles(folder);
        for (auto& problem : declared.problems) _notes.push_back(std::move(problem));
        for (auto& type : declared.types) {
            if (type.makesResources) { DeclareResourceScript(std::move(type)); continue; }    // what a Script node runs, no component
            if (type.material) { DeclareMaterialKind(std::move(type)); continue; }
            std::string error;
            const std::string name = type.name;
            _scriptPieces[name] = type.pieces;
            if (_scripts.Declare(std::move(type), &error) == kecs::NoComponent) _notes.push_back(std::format("{}: {}", name, error));
        }
        Changed();
    }

    bool Document::GivePicture(const kecs::Entity entity, const std::string_view field, const std::string& id)
    {
        if (!_world.Alive(entity)) return false;
        for (const auto& component : ComponentsOf(entity)) {
            const kecs::ComponentId type = _world.Find(component);
            const kecs::FieldDescription* described = type == kecs::NoComponent ? nullptr : _world.Describe(type).FindField(field);
            if (!described || described->type != "Image") continue;
            return SetField(entity, component, field, Value(id));
        }
        return false;
    }

    bool Document::SaveGenerator(const kecs::Entity entity, const std::string_view script, const std::filesystem::path& file, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const kecs::ComponentId type = _world.Find(script);
        if (!_world.Alive(entity) || type == kecs::NoComponent || !_world.Has(entity, type)) return fail(std::format("{} has no {}", NameOf(entity), script));
        // Its fields, each as the scene saves it.
        std::string fields;
        for (const auto& field : _world.Describe(type).fields)
            fields += std::format("{}{}: {}", fields.empty() ? "" : ", ", kecs::json::Quote(field.name), Field(entity, script, field.name).ToJson());
        kasset::Meta meta;
        meta.id = kasset::Id::New();
        meta.kind = kasset::Kind::eGenerated;
        meta.producer = std::string(script);
        meta.parameters = std::format("{{ \"fields\": {{ {} }} }}", fields);
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (auto saved = kasset::Save(file, meta); !saved) return fail(saved.error());
        if (_assets) (void)_assets->Add(file);
        return true;
    }

    bool Document::GiveGenerator(const kecs::Entity entity, const std::filesystem::path& file, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const auto meta = kasset::Load(file);
        if (!meta) return fail(meta.error());
        if (meta->kind != kasset::Kind::eGenerated || meta->producer.empty()) return fail(std::format("{} is no generator", file.filename().string()));
        if (_world.Find(meta->producer) == kecs::NoComponent) return fail(std::format("there is no script {}: the project's scripts are what a generator runs", meta->producer));
        if (!_world.Alive(entity)) return fail("the entity is not in the scene");
        // One generator of a kind an entity: what it had of it is this one now, afresh.
        if (!std::ranges::contains(ComponentsOf(entity), meta->producer)) AddComponent(entity, meta->producer);
        const GeneratorAsset asset = GeneratorOf(file);
        _following = true;
        for (const auto& [name, value] : asset.fields) SetField(entity, meta->producer, name, value);
        _following = false;
        std::vector<std::string> links = LinkLines(entity, false), overrides = LinkLines(entity, true);
        std::erase_if(links, [&](const std::string& line) { return line.starts_with(meta->producer + " "); });
        std::erase_if(overrides, [&](const std::string& line) { return line.starts_with(meta->producer + "."); });
        links.push_back(std::format("{} {}", meta->producer, _runtime->AssetId(file)));
        SetLinkLines(entity, false, links);
        SetLinkLines(entity, true, overrides);
        return true;
    }

    Document::GeneratorAsset Document::GeneratorOf(const std::filesystem::path& file) const
    {
        GeneratorAsset asset;
        const auto meta = kasset::Load(file);
        if (!meta || meta->kind != kasset::Kind::eGenerated) return asset;
        asset.script = meta->producer;
        const std::string json = kasset::ParameterOf(*meta, "fields");
        kecs::json::Value root;
        std::string why;
        if (!json.empty() && kecs::json::Parse(json, root, why))
            for (const auto& [name, value] : root.members) asset.fields.emplace_back(name, Value::FromJson(value));
        return asset;
    }

    bool Document::SetGeneratorField(const std::filesystem::path& file, const std::string& field, const Value& value, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        auto meta = kasset::Load(file);
        if (!meta) return fail(meta.error());
        GeneratorAsset asset = GeneratorOf(file);
        if (asset.script.empty()) return fail(std::format("{} is no generator", file.filename().string()));
        if (const auto at = std::ranges::find(asset.fields, field, &std::pair<std::string, Value>::first); at != asset.fields.end()) at->second = value;
        else asset.fields.emplace_back(field, value);
        std::string fields;
        for (const auto& [name, each] : asset.fields) fields += std::format("{}{}: {}", fields.empty() ? "" : ", ", kecs::json::Quote(name), each.ToJson());
        kasset::SetParameter(*meta, "fields", std::format("{{ {} }}", fields));
        if (auto saved = kasset::Save(file, *meta); !saved) return fail(saved.error());
        if (_assets) (void)_assets->Scan();     // the file is what it knows the asset by
        // Every entity that follows it, and has not made the field its own.
        const std::string line = std::format("{} {}", asset.script, _runtime->AssetId(file));
        _following = true;
        for (const kecs::Entity entity : _world.All())
            if (std::ranges::contains(LinkLines(entity, false), line) && !std::ranges::contains(MaterialOverridesOf(entity, asset.script), field))
                SetField(entity, asset.script, field, value);
        _following = false;
        Changed();
        return true;
    }

    void Document::FollowGenerators()
    {
        _following = true;
        for (const kecs::Entity entity : _world.All())
            for (const auto& line : LinkLines(entity, false)) {
                const auto space = line.find(' ');
                if (space == std::string::npos || !line.ends_with(".kgen") || !line.substr(space + 1).starts_with('@')) continue;
                const std::string script = line.substr(0, space), named = line.substr(space + 2);
                const std::filesystem::path path(named);
                const auto own = MaterialOverridesOf(entity, script);
                for (const auto& [field, value] : GeneratorOf(path.is_absolute() ? path : _runtime->AssetDirectory() / path).fields)
                    if (!std::ranges::contains(own, field)) SetField(entity, script, field, value);
            }
        _following = false;
    }

    std::vector<std::string> Document::ScriptsTaking(const std::string_view piece) const
    {
        std::vector<std::string> names;
        for (const auto& [name, pieces] : _scriptPieces)
            if (std::ranges::contains(pieces, piece)) names.push_back(name);
        return names;
    }

    namespace {
        kgraph::ResourceScript ResourceScriptOf(kscript::ScriptType type)
        {
            kgraph::ResourceScript script;
            script.name = std::move(type.name);
            for (auto& declared : type.resources) {
                kgraph::ScriptResource resource { std::move(declared.name), std::move(declared.kind), {} };
                for (auto& [input, json] : declared.settings) {
                    kecs::json::Value parsed;
                    std::string ignored;
                    if (kecs::json::Parse(json, parsed, ignored)) resource.settings.emplace_back(std::move(input), kgraph::Value::FromJson(parsed));
                }
                script.resources.push_back(std::move(resource));
            }
            script.fields = std::move(type.fields);
            script.initial = std::move(type.initial);
            script.size = type.size;
            return script;
        }
    }

    void Document::DeclareResourceScript(kscript::ScriptType type)
    {
        // One that runs already is what its host made of it: newer than what its source is read as.
        for (const auto& running : _runtime->ResourceScripts())
            if (running.name == type.name && running.fill) return;
        _runtime->SetResourceScript(ResourceScriptOf(std::move(type)));
        Changed();
    }

    void Document::DeclareMaterialKind(kscript::ScriptType type)
    {
        kgraph::MaterialKind kind { .name = std::move(type.name), .fields = std::move(type.fields), .size = type.size, .alignment = type.alignment,
                                    .initial = std::move(type.initial), .textures = std::move(type.textures) };
        const std::string name = kind.name;
        std::string error;
        if (_runtime->DeclareMaterialKind(std::move(kind), &error) == kecs::NoComponent) _notes.push_back(std::format("{}: {}", name, error));
        for (const auto& note : _runtime->MaterialNotes())
            if (std::ranges::find(_notes, note) == _notes.end()) _notes.push_back(note);
        Changed();
    }

    // ---- materials ----------------------------------------------------------------------------------------------

    namespace {
        constexpr const char* ModelMaterialsEnding = ".materials.json";

        std::vector<std::string> Lines(const std::string& text)
        {
            std::vector<std::string> lines;
            std::stringstream in(text);
            for (std::string line; std::getline(in, line);) if (!line.empty()) lines.push_back(line);
            return lines;
        }
        std::string Joined(const std::vector<std::string>& lines)
        {
            std::string text;
            for (const auto& line : lines) text += line + '\n';
            return text;
        }
        /** Named values as JSON, and back. */
        std::string ValuesJson(const std::vector<std::pair<std::string, Value>>& values)
        {
            std::string out = "{";
            for (const auto& [name, value] : values) out += std::format("{}{}: {}", out.size() > 1 ? ", " : " ", kecs::json::Quote(name), value.ToJson());
            return out + (out.size() > 1 ? " }" : "}");
        }
        std::vector<std::pair<std::string, Value>> ValuesOf(const kecs::json::Value& json)
        {
            std::vector<std::pair<std::string, Value>> values;
            for (const auto& [name, value] : json.members) values.emplace_back(name, Value::FromJson(value));
            return values;
        }
        void Put(std::vector<std::pair<std::string, Value>>& values, const std::string_view name, Value value)
        {
            const auto it = std::ranges::find(values, name, &std::pair<std::string, Value>::first);
            if (it != values.end()) it->second = std::move(value);
            else values.emplace_back(std::string(name), std::move(value));
        }
        /** The model file and the material's number an asset id names: `@sponza/Sponza.gltf#material:3`. */
        std::optional<std::pair<std::string, int>> ModelMaterialOf(const std::string_view id)
        {
            const auto at = id.rfind("#material:");
            if (!id.starts_with('@') || at == std::string_view::npos) return std::nullopt;
            return std::pair { std::string(id.substr(0, at)), std::atoi(std::string(id.substr(at + 10)).c_str()) };
        }
    }

    void Document::SetImportKind(const kgraph::ModelMaterial::Model model, std::string kind, const std::filesystem::path& file)
    {
        const std::string key = file.empty() ? std::string() : _runtime->AssetId(file);
        auto [it, made] = _importKinds.try_emplace(key);
        it->second[model == kgraph::ModelMaterial::Model::eClassic ? 1 : 0] = std::move(kind);
        if (!key.empty() && it->second[0].empty() && it->second[1].empty()) _importKinds.erase(it);
        Changed();
    }

    std::string Document::ImportKind(const kgraph::ModelMaterial::Model model, const std::filesystem::path& file) const
    {
        const std::size_t which = model == kgraph::ModelMaterial::Model::eClassic ? 1 : 0;
        if (!file.empty())
            if (const auto it = _importKinds.find(_runtime->AssetId(file)); it != _importKinds.end() && !it->second[which].empty()) return it->second[which];
        return _importKinds.at("")[which];
    }

    void Document::AddMaterialAsset(MaterialAsset asset)
    {
        std::string id = asset.id;
        _materialAssets.insert_or_assign(std::move(id), std::move(asset));
    }

    const MaterialAsset* Document::MaterialAssetOf(const std::string_view id)
    {
        if (const auto it = _materialAssets.find(id); it != _materialAssets.end()) return &it->second;
        if (const auto model = ModelMaterialOf(id)) {
            // A model's, once the model is read.
            const std::filesystem::path file = _runtime->AssetDirectory().empty() ? std::filesystem::path(model->first.substr(1)) : _runtime->AssetDirectory() / model->first.substr(1);
            if (const auto read = _runtime->ModelOf(file)) MakeModelMaterials(file, *read);
        } else if (id.starts_with('@') && id.ends_with(".kmat")) {
            // A project's own: its file.
            const std::filesystem::path file = _runtime->AssetDirectory() / std::string(id.substr(1));
            std::ifstream in(file, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (_assets) text = kasset::FromIds(text, *_assets);
            kecs::json::Value parsed;
            std::string error;
            if (in && kecs::json::Parse(text, parsed, error))
                if (const auto* kind = parsed.Find("kind"); kind && kind->kind == kecs::json::Value::Kind::eString) {
                    MaterialAsset asset { .id = std::string(id), .name = file.stem().string(), .kind = kind->string };
                    if (const auto* values = parsed.Find("values")) asset.values = ValuesOf(*values);
                    AddMaterialAsset(std::move(asset));
                }
        }
        const auto it = _materialAssets.find(id);
        return it == _materialAssets.end() ? nullptr : &it->second;
    }

    std::vector<std::string> Document::MaterialAssetIds() const
    {
        std::vector<std::string> ids;
        for (const auto& id : _materialAssets | std::views::keys) ids.push_back(id);
        return ids;
    }

    void Document::RecordModelParts(const std::filesystem::path& file, const kgraph::Model& model)
    {
        if (!_assets) return;
        const std::string named = _runtime->AssetId(file);
        const auto reference = _assets->FromLegacy(named);
        if (!reference || reference->part) return;
        // Each part by its name in the file: one the file now has elsewhere among its meshes is the same part, and what
        // held it is led to where it is now.
        std::map<int, int> meshes, materials;
        const auto record = [&](const kasset::Kind kind, const std::vector<std::string>& names, std::map<int, int>& moved) {
            std::map<kasset::Id, int> was;
            for (const kasset::Part& part : _assets->Find(reference->asset)->meta.parts)
                if (part.kind == kind && part.index >= 0) was[part.id] = part.index;
            const auto recorded = _assets->RecordParts(reference->asset, kind, names);
            if (!recorded) { _notes.push_back(recorded.error()); return; }
            for (std::size_t i = 0; i < recorded->ids.size(); ++i)
                if (const auto at = was.find(recorded->ids[i]); at != was.end() && at->second != static_cast<int>(i)) moved[at->second] = static_cast<int>(i);
            for (const kasset::Part& gone : recorded->gone)
                _notes.push_back(std::format("{} no longer has the {} '{}': what had it has nothing now", file.filename().string(), kasset::KindName(kind),
                                             gone.name.empty() ? std::format("#{}", gone.index) : gone.name));
        };
        std::vector<std::string> names;
        for (const auto& material : model.materials) names.push_back(material.name);
        record(kasset::Kind::eMesh, model.meshNames, meshes);
        record(kasset::Kind::eMaterial, names, materials);
        if (meshes.empty() && materials.empty()) return;
        // What holds them — an entity's Mesh, the material it follows — named by where they were: named again.
        const auto moved = [&](const std::string& id) -> std::optional<std::string> {
            if (!id.starts_with(named + "#")) return std::nullopt;
            std::string_view rest = std::string_view(id).substr(named.size() + 1);
            const bool material = rest.starts_with("material:");
            if (material) rest.remove_prefix(9);
            const auto& map = material ? materials : meshes;
            const auto at = map.find(std::atoi(std::string(rest).c_str()));
            if (at == map.end()) return std::nullopt;
            return std::format("{}#{}{}", named, material ? "material:" : "", at->second);
        };
        const kecs::ComponentId mesh = _world.Find("Mesh");
        for (const kecs::Entity entity : _world.All()) {
            if (mesh != kecs::NoComponent && _world.Has(entity, mesh)) {
                const kgraph::Value held = Field(entity, "Mesh", "mesh");    // (kept: what String gives is of it)
                if (const std::string* id = held.String())
                    if (const auto now = moved(*id)) SetField(entity, "Mesh", "mesh", kgraph::Value(*now));
            }
            std::vector<std::string> lines = LinkLines(entity, false);
            bool changed = false;
            for (std::string& line : lines)
                if (const auto space = line.find(' '); space != std::string::npos)
                    if (const auto now = moved(line.substr(space + 1))) { line = line.substr(0, space + 1) + *now; changed = true; }
            if (changed) SetLinkLines(entity, false, lines);
        }
    }

    void Document::MakeModelMaterials(const std::filesystem::path& file, const kgraph::Model& model)
    {
        RecordModelParts(file, model);
        const std::string named = _runtime->AssetId(file);
        // What was changed of them here, kept beside the model.
        kecs::json::Value changed;
        std::ifstream in(std::filesystem::path(file).concat(ModelMaterialsEnding), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (_assets) text = kasset::FromIds(text, *_assets);
        std::string ignored;
        const bool hasChanges = in && kecs::json::Parse(text, changed, ignored);
        for (std::size_t i = 0; i < model.materials.size(); ++i) {
            const kgraph::ReportedMaterial report = kgraph::ReportOf(model.materials[i]);
            MaterialAsset asset;
            asset.id = std::format("{}#material:{}", named, i);
            asset.name = report.name.empty() ? std::format("{} {}", file.stem().string(), i) : report.name;
            asset.kind = ImportKind(report.model, file);
            // Filled by name: what of the report the kind has a field for — or, where the model says, another of the report's
            // values (MaterialFieldsOf). The rest is as the kind starts it.
            const kgraph::MaterialKind* kind = _runtime->MaterialKindOf(asset.kind);
            const auto mapped = MaterialFieldsOf(file, asset.kind);
            for (const auto& [name, value] : report.values)
                if ((!kind || std::ranges::contains(kind->fields, name, &kecs::FieldDescription::name)) && !mapped.contains(name)) asset.reported.emplace_back(name, value);
            for (const auto& [field, from] : mapped)
                if (const Value* value = report.Find(from); value && (!kind || std::ranges::contains(kind->fields, field, &kecs::FieldDescription::name)))
                    asset.reported.emplace_back(field, *value);
            asset.values = asset.reported;
            if (hasChanges)
                if (const auto* materials = changed.Find("materials"))
                    if (const auto* own = materials->Find(std::to_string(i)))
                        for (auto& [name, value] : ValuesOf(*own)) Put(asset.values, name, std::move(value));
            AddMaterialAsset(std::move(asset));
        }
    }

    std::map<std::string, std::string> Document::MaterialFieldsOf(const std::filesystem::path& file, const std::string_view kind) const
    {
        if (!_assets) return {};
        const auto reference = _assets->Lookup(_runtime->AssetId(file));
        const kasset::Entry* entry = reference && !reference->part ? _assets->Find(reference->asset) : nullptr;
        if (!entry) return {};
        kecs::json::Value root;
        std::string error;
        const std::string json = kasset::ParameterOf(entry->meta, "materialFields");
        std::map<std::string, std::string> fields;
        if (json.empty() || !kecs::json::Parse(json, root, error)) return fields;
        if (const auto* of = root.Find(kind))
            for (const auto& [field, from] : of->members)
                if (from.kind == kecs::json::Value::Kind::eString) fields[field] = from.string;
        return fields;
    }

    bool Document::SetMaterialField(const std::filesystem::path& file, const std::string_view kind, const std::string& field, const std::string& reported, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const auto reference = _assets ? _assets->Lookup(_runtime->AssetId(file)) : std::nullopt;
        const kasset::Entry* entry = reference && !reference->part ? _assets->Find(reference->asset) : nullptr;
        if (!entry || entry->meta.kind != kasset::Kind::eModel) return fail(std::format("{} is no model of the project's", file.filename().string()));
        // Every kind's, as the meta has them, with this one's changed.
        std::map<std::string, std::map<std::string, std::string>> all;
        kecs::json::Value root;
        std::string ignored;
        if (const std::string json = kasset::ParameterOf(entry->meta, "materialFields"); !json.empty() && kecs::json::Parse(json, root, ignored))
            for (const auto& [name, of] : root.members)
                for (const auto& [f, from] : of.members) all[name][f] = from.string;
        if (reported.empty()) all[std::string(kind)].erase(field);
        else all[std::string(kind)][field] = reported;
        std::erase_if(all, [](const auto& each) { return each.second.empty(); });
        std::string json;
        for (const auto& [name, of] : all) {
            std::string fields;
            for (const auto& [f, from] : of) fields += std::format("{}{}: {}", fields.empty() ? "" : ", ", kecs::json::Quote(f), kecs::json::Quote(from));
            json += std::format("{}{}: {{ {} }}", json.empty() ? "" : ", ", kecs::json::Quote(name), fields);
        }
        if (auto set = _assets->SetParameter(reference->asset, "materialFields", json.empty() ? std::string() : "{ " + json + " }"); !set) return fail(set.error());
        // Made again so, and what follows each has it.
        const auto model = _runtime->ModelOf(file);
        if (!model) return true;    // read later, it is read so
        MakeModelMaterials(file, *model);
        const std::string named = _runtime->AssetId(file);
        for (const kecs::Entity entity : _world.All()) {
            const std::string follows = MaterialAssetOfEntity(entity, kind);
            if (!follows.starts_with(named + "#material:")) continue;
            const MaterialAsset* asset = MaterialAssetOf(follows);
            if (!asset) continue;
            const auto own = MaterialOverridesOf(entity, kind);
            _following = true;
            for (const auto& [name, value] : asset->values)
                if (!std::ranges::contains(own, name)) SetField(entity, kind, name, value);
            _following = false;
        }
        Changed();
        return true;
    }

    std::vector<std::string> Document::UnusedReported(const std::string_view id)
    {
        const auto model = ModelMaterialOf(id);
        const MaterialAsset* asset = model ? MaterialAssetOf(id) : nullptr;
        if (!asset) return {};
        const std::filesystem::path file = _runtime->AssetDirectory().empty() ? std::filesystem::path(model->first.substr(1)) : _runtime->AssetDirectory() / model->first.substr(1);
        const auto read = _runtime->ModelOf(file);
        if (!read || model->second < 0 || model->second >= static_cast<int>(read->materials.size())) return {};
        const kgraph::ReportedMaterial report = kgraph::ReportOf(read->materials[static_cast<std::size_t>(model->second)]);
        const kgraph::MaterialKind* kind = _runtime->MaterialKindOf(asset->kind);
        const auto mapped = MaterialFieldsOf(file, asset->kind);
        std::vector<std::string> unused;
        for (const auto& [name, value] : report.values) {
            const bool byName = kind && std::ranges::contains(kind->fields, name, &kecs::FieldDescription::name) && !mapped.contains(name);
            const bool taken = std::ranges::any_of(mapped, [&](const auto& each) { return each.second == name; });
            if (!byName && !taken) unused.push_back(name);
        }
        return unused;
    }

    void Document::SaveMaterialAsset(const MaterialAsset& asset)
    {
        if (const auto model = ModelMaterialOf(asset.id)) {
            // A model's: what differs from what the file reports, for each of its materials, beside the model.
            const std::filesystem::path file = _runtime->AssetDirectory() / model->first.substr(1);
            std::string materials;
            for (const auto& [id, each] : _materialAssets) {
                const auto of = ModelMaterialOf(id);
                if (!of || of->first != model->first) continue;
                std::vector<std::pair<std::string, Value>> differs;
                for (const auto& [name, value] : each.values)
                    if (const auto was = std::ranges::find(each.reported, name, &std::pair<std::string, Value>::first); was == each.reported.end() || was->second != value)
                        differs.emplace_back(name, value);
                if (!differs.empty()) materials += std::format("{}    {}: {}", materials.empty() ? "" : ",\n", kecs::json::Quote(std::to_string(of->second)), ValuesJson(differs));
            }
            const std::filesystem::path beside = std::filesystem::path(file).concat(ModelMaterialsEnding);
            std::error_code ec;
            if (materials.empty()) { std::filesystem::remove(beside, ec); return; }
            const std::string json = std::format("{{\n  \"koral-materials\": 1,\n  \"materials\": {{\n{}\n  }}\n}}\n", materials);
            std::ofstream(beside, std::ios::binary | std::ios::trunc) << (_assets ? kasset::ToIds(json, *_assets) : json);
            return;
        }
        if (asset.id.starts_with('@') && asset.id.ends_with(".kmat")) {
            const std::string json = std::format("{{\n  \"koral-material\": 1,\n  \"kind\": {},\n  \"values\": {}\n}}\n", kecs::json::Quote(asset.kind), ValuesJson(asset.values));
            std::ofstream(_runtime->AssetDirectory() / asset.id.substr(1), std::ios::binary | std::ios::trunc) << (_assets ? kasset::ToIds(json, *_assets) : json);
        }
    }

    std::vector<std::string> Document::LinkLines(const kecs::Entity entity, const bool overrides) const
    {
        const MaterialLink* link = _world.Alive(entity) ? _world.Get<MaterialLink>(entity) : nullptr;
        return link ? Lines(overrides ? link->overrides : link->assets) : std::vector<std::string>();
    }

    void Document::SetLinkLines(const kecs::Entity entity, const bool overrides, const std::vector<std::string>& lines)
    {
        MaterialLink link = _world.Has<MaterialLink>(entity) ? *_world.Get<MaterialLink>(entity) : MaterialLink {};
        (overrides ? link.overrides : link.assets) = Joined(lines);
        if (link.assets.empty() && link.overrides.empty()) _world.Remove<MaterialLink>(entity);
        else _world.Add<MaterialLink>(entity, std::move(link));
    }

    std::string Document::MaterialAssetOfEntity(const kecs::Entity entity, const std::string_view kind) const
    {
        for (const auto& line : LinkLines(entity, false))
            if (line.size() > kind.size() && line.starts_with(kind) && line[kind.size()] == ' ') return line.substr(kind.size() + 1);
        return {};
    }

    std::vector<std::string> Document::MaterialOverridesOf(const kecs::Entity entity, const std::string_view kind) const
    {
        std::vector<std::string> fields;
        for (const auto& line : LinkLines(entity, true))
            if (line.size() > kind.size() && line.starts_with(kind) && line[kind.size()] == '.') fields.push_back(line.substr(kind.size() + 1));
        return fields;
    }

    std::string Document::ExtractMaterial(const std::string_view id, const std::filesystem::path& file, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return std::string(); };
        if (!ModelMaterialOf(id)) return fail(std::format("'{}' is no model's material", id));
        const MaterialAsset* asset = MaterialAssetOf(id);
        if (!asset) return fail(std::format("'{}' is not read yet: its model is still being read", id));
        std::error_code ec;
        if (file.extension() != ".kmat") return fail(std::format("{} is no material's file: it ends in .kmat", file.filename().string()));
        if (std::filesystem::exists(file, ec)) return fail(std::format("{} is there already", file.filename().string()));
        std::filesystem::create_directories(file.parent_path(), ec);
        MaterialAsset own = *asset;
        own.id = _runtime->AssetId(file);
        own.name = file.stem().string();
        own.reported.clear();
        if (!own.id.starts_with('@')) return fail(std::format("{} is not among the project's assets", file.generic_string()));
        SaveMaterialAsset(own);
        if (!std::filesystem::exists(file, ec)) return fail(std::format("{} could not be written", file.generic_string()));
        if (_assets) (void)_assets->Add(file);
        AddMaterialAsset(own);
        // The model says so in its meta: what its things are made of, made again, is this — its prefab's too.
        const auto model = ModelMaterialOf(id);
        const std::filesystem::path modelFile = _runtime->AssetDirectory().empty() ? std::filesystem::path(model->first.substr(1)) : _runtime->AssetDirectory() / model->first.substr(1);
        if (_assets)
            if (const auto of = _assets->Lookup(model->first); of && !of->part)
                if (const auto kept = _assets->FromLegacy(own.id)) {
                    const kasset::Entry* entry = _assets->Find(of->asset);
                    auto extracted = kasset::KindsFrom(kasset::ParameterOf(entry->meta, "extracted")).value_or(std::map<std::string, std::string> {});
                    extracted[std::to_string(model->second)] = kept->Text();
                    (void)_assets->SetParameter(of->asset, "extracted", kasset::ToJson(extracted));
                }
        // Who followed the model's follows this, with what each made its own still its own.
        for (const kecs::Entity entity : _world.All()) {
            std::vector<std::string> lines = LinkLines(entity, false);
            bool changed = false;
            for (std::string& line : lines)
                if (line == std::format("{} {}", own.kind, id)) { line = std::format("{} {}", own.kind, own.id); changed = true; }
            if (changed) SetLinkLines(entity, false, lines);
        }
        if (const auto read = _runtime->ModelOf(modelFile); read && _modelPrefabs.contains(modelFile)) RemakeModelPrefab(modelFile, *read);
        Changed();
        return own.id;
    }

    bool Document::GiveMaterial(const kecs::Entity entity, const std::string_view id, std::string* error)
    {
        const MaterialAsset* asset = MaterialAssetOf(id);
        if (!asset) { if (error) *error = std::format("there is no material '{}'", id); return false; }
        const MaterialAsset given = *asset;     // (what setting fields does may add assets: a copy)
        if (!_world.Alive(entity)) { if (error) *error = "the entity is not in the scene"; return false; }
        const kecs::ComponentId kind = _world.Find(given.kind);
        if (kind == kecs::NoComponent) { if (error) *error = std::format("'{}' is a {}, and there is no such material kind", given.name, given.kind); return false; }
        // Afresh: as the kind starts it, then as the asset has it.
        _world.Add(entity, kind);
        _following = true;
        for (const auto& [field, value] : given.values)
            if (_world.Describe(kind).FindField(field)) SetField(entity, given.kind, field, value);
        _following = false;
        std::vector<std::string> assets = LinkLines(entity, false), overrides = LinkLines(entity, true);
        std::erase_if(assets, [&](const std::string& line) { return line.starts_with(given.kind + " "); });
        std::erase_if(overrides, [&](const std::string& line) { return line.starts_with(given.kind + "."); });
        assets.push_back(std::format("{} {}", given.kind, given.id));
        SetLinkLines(entity, false, assets);
        SetLinkLines(entity, true, overrides);
        SyncAlphaTags(entity);
        Changed();
        return true;
    }

    bool Document::SetMaterialAssetField(const std::string_view id, const std::string_view field, const Value& value, std::string* error)
    {
        if (!MaterialAssetOf(id)) { if (error) *error = std::format("there is no material '{}'", id); return false; }
        MaterialAsset& asset = _materialAssets.find(id)->second;
        const kecs::ComponentId kind = _world.Find(asset.kind);
        if (kind == kecs::NoComponent || !_world.Describe(kind).FindField(field)) {
            if (error) *error = std::format("a {} has no field '{}'", asset.kind, field);
            return false;
        }
        Put(asset.values, field, value);
        SaveMaterialAsset(asset);
        // Every entity that follows it, and has not made the field its own.
        const std::string line = std::format("{} {}", asset.kind, asset.id);
        _following = true;
        for (const kecs::Entity entity : _world.All()) {
            const auto links = LinkLines(entity, false);
            if (!std::ranges::contains(links, line) || std::ranges::contains(MaterialOverridesOf(entity, asset.kind), field)) continue;
            SetField(entity, asset.kind, field, value);
            if (field == "alphaMode") SyncAlphaTags(entity);
        }
        _following = false;
        Changed();
        return true;
    }

    bool Document::ApplyMaterial(const kecs::Entity entity, const std::string_view kind)
    {
        const std::string id = MaterialAssetOfEntity(entity, kind);
        if (id.empty()) return false;
        const auto fields = MaterialOverridesOf(entity, kind);
        // Its own no longer: the asset's, and so every entity's that follows it.
        std::vector<std::string> overrides = LinkLines(entity, true);
        std::erase_if(overrides, [&](const std::string& line) { return line.starts_with(std::string(kind) + "."); });
        SetLinkLines(entity, true, overrides);
        for (const auto& field : fields) SetMaterialAssetField(id, field, Field(entity, kind, field));
        return true;
    }

    bool Document::RevertMaterial(const kecs::Entity entity, const std::string_view kind)
    {
        const std::string id = MaterialAssetOfEntity(entity, kind);
        return !id.empty() && GiveMaterial(entity, id);
    }

    bool Document::UnlinkMaterial(const kecs::Entity entity, const std::string_view kind)
    {
        if (MaterialAssetOfEntity(entity, kind).empty()) return false;
        std::vector<std::string> assets = LinkLines(entity, false), overrides = LinkLines(entity, true);
        std::erase_if(assets, [&](const std::string& line) { return line.starts_with(std::string(kind) + " "); });
        std::erase_if(overrides, [&](const std::string& line) { return line.starts_with(std::string(kind) + "."); });
        SetLinkLines(entity, false, assets);
        SetLinkLines(entity, true, overrides);
        Changed();
        return true;
    }

    void Document::FollowModelMaterial(const kecs::Entity entity)
    {
        if (!_world.Alive(entity) || !LinkLines(entity, false).empty()) return;
        const Value mesh = Field(entity, "Mesh", "mesh");
        const std::string* id = mesh.String();
        const auto hash = id ? id->rfind('#') : std::string::npos;
        if (!id || !id->starts_with('@') || hash == std::string::npos) return;
        const std::string named = id->substr(1, hash - 1);
        const std::filesystem::path file = _runtime->AssetDirectory().empty() ? std::filesystem::path(named) : _runtime->AssetDirectory() / named;
        _toFollow.emplace_back(file, entity);
        (void)_runtime->ModelOf(file);     // read off the frame from now
    }

    namespace {
        /**
         * Whether a field an old scene set is as the file has it: the same numbers (as a float keeps them), the same
         * picture however its path is written, and — for a normal map — white and the flat normal alike, as neither bends it.
         */
        bool Alike(const std::string_view field, const Value& mine, const Value& theirs)
        {
            if (mine.Number() && theirs.Number()) return std::abs(*mine.Number() - *theirs.Number()) <= 1e-5 * std::max(1.0, std::abs(*theirs.Number()));
            if (mine.Items() && theirs.Items()) {
                const auto &a = *mine.Items(), &b = *theirs.Items();
                if (a.size() != b.size()) return false;
                for (std::size_t i = 0; i < a.size(); ++i) if (!Alike(field, a[i], b[i])) return false;
                return true;
            }
            if (mine.String() && theirs.String()) {
                const auto normal = [](const std::string& picture) { return std::filesystem::path(picture).lexically_normal().generic_string(); };
                if (normal(*mine.String()) == normal(*theirs.String())) return true;
                const auto flat = [](const std::string& picture) { return picture == "$white" || picture == "$normal"; };
                return field.find("ormal") != std::string_view::npos && flat(*mine.String()) && flat(*theirs.String());
            }
            return mine == theirs;
        }
    }

    void Document::FollowedModel(const kecs::Entity entity, const std::filesystem::path& file, const kgraph::Model& model)
    {
        if (!_world.Alive(entity) || !LinkLines(entity, false).empty()) return;
        const Value mesh = Field(entity, "Mesh", "mesh");
        const std::string* id = mesh.String();
        if (!id) return;
        int material = -1;
        for (const auto& node : model.nodes)
            for (const auto& part : node.parts) if (part.mesh == *id) material = part.material;
        if (material < 0 || material >= static_cast<int>(model.materials.size())) return;
        const std::string asset = std::format("{}#material:{}", _runtime->AssetId(file), material);
        const MaterialAsset* followed = MaterialAssetOf(asset);
        if (!followed) return;
        const std::string kind = followed->kind;
        // What a scene set before materials were their own (MigrateMaterials): kept where the file says otherwise.
        static constexpr std::array set { "baseColor", "metallic", "roughness", "emission", "alphaMode", "alphaCutoff", "baseColorMap", "normalMap", "metallicRoughnessMap", "emissiveMap" };
        std::vector<std::pair<std::string, Value>> had;
        if (_world.Has(entity, _world.Find(kind)))
            for (const char* field : set)
                if (Value value = Field(entity, kind, field); !value.IsNull()) had.emplace_back(field, std::move(value));
        if (!GiveMaterial(entity, asset)) return;
        for (const auto& [field, value] : had)
            if (!Alike(field, value, Field(entity, kind, field))) SetField(entity, kind, field, value);
    }

    void Document::SyncAlphaTags(const kecs::Entity entity)
    {
        // The most see-through of its materials says: blended over masked over opaque.
        int mode = 0;
        for (const auto& kind : _runtime->MaterialKinds())
            if (const kecs::ComponentId id = _world.Find(kind.name); id != kecs::NoComponent && _world.Has(entity, id)) {
                const Value alpha = Field(entity, kind.name, "alphaMode");     // (kept: what Number gives is of it)
                if (const auto* number = alpha.Number()) mode = std::max(mode, static_cast<int>(*number));
            }
        for (const auto& [tag, wanted] : { std::pair { "Masked", 1 }, std::pair { "Blended", 2 } }) {
            const kecs::ComponentId id = AddTag(tag);
            if ((mode == wanted) != _world.Has(entity, id)) {
                if (mode == wanted) _world.Add(entity, id);
                else _world.Remove(entity, id);
            }
        }
    }

    void Document::SetAssetDirectory(const std::filesystem::path& directory)
    {
        _runtime->SetAssetDirectory(directory);
        if (!directory.empty()) _runtime->SetGeneratedShaderDirectory(directory.parent_path() / "cache" / "shaders");
        // The project's assets, when the folder is a project's — and only then: a folder that is not one is given no metas.
        // Found again only for another project.
        const std::filesystem::path root = directory.empty() ? std::filesystem::path() : directory.lexically_normal().parent_path();
        if (_assets && _assets->Root() == root) return;
        _assets.reset();
        _assetProblems.clear();
        std::error_code failed;
        if (root.empty() || !std::filesystem::is_regular_file(root / "project.json", failed)) return;
        _assets = std::make_unique<kasset::Database>(root);
        for (const auto& name : kgraph::DefaultMeshNames()) _assets->AddBuiltIn(kasset::Kind::eMesh, "mesh/" + name, kgraph::DefaultMeshId(name));
        for (const std::string_view texel : { "white", "black", "normal" })
            _assets->AddBuiltIn(kasset::Kind::eTexture, std::format("texture/{}", texel), std::format("${}", texel));
        _assetProblems = _assets->Scan();
    }

    void Document::ImportSettingsInto(kgraph::Graph& graph) const
    {
        if (!_assets) return;
        for (const kasset::Entry* entry : _assets->Entries()) {
            const std::string named = _assets->ToLegacy({ entry->meta.id, {} });
            if (!named.starts_with('@')) continue;
            if (entry->meta.kind == kasset::Kind::eModel) {
                if (const auto import = kasset::MeshImportFrom(kasset::ParameterOf(entry->meta, "mesh"))) {
                    if (import->Default()) graph.meshImports.erase(named.substr(1));
                    else graph.meshImports[named.substr(1)] = *import;
                }
            } else if (entry->meta.kind == kasset::Kind::eTexture) {
                if (const auto import = kasset::TextureImportFrom(kasset::ParameterOf(entry->meta, "texture"))) {
                    if (import->Default()) graph.textureImports.erase(named);
                    else graph.textureImports[named] = *import;
                }
            }
        }
    }

    void Document::StoreImportSettings()
    {
        if (!_assets) return;
        std::set<kasset::Id> stored;
        const auto store = [&](const std::string& named, const std::string_view key, const std::string& json) {
            const auto reference = _assets->FromLegacy(named);
            if (!reference || reference->part) return;
            stored.insert(reference->asset);
            if (auto set = _assets->SetParameter(reference->asset, key, json); !set) _notes.push_back(set.error());
        };
        for (const auto& [file, import] : Graph().meshImports) store("@" + file, "mesh", import.Default() ? std::string() : kasset::ToJson(import));
        for (const auto& [file, import] : Graph().textureImports) store(file, "texture", import.Default() ? std::string() : kasset::ToJson(import));
        // What the scene no longer says anything of is read as it is by default: what its meta said of it is taken away.
        for (const kasset::Entry* entry : _assets->Entries()) {
            if (stored.contains(entry->meta.id)) continue;
            const std::string_view key = entry->meta.kind == kasset::Kind::eModel ? "mesh" : entry->meta.kind == kasset::Kind::eTexture ? "texture" : "";
            if (!key.empty() && !kasset::ParameterOf(entry->meta, key).empty())
                if (auto set = _assets->SetParameter(entry->meta.id, key, ""); !set) _notes.push_back(set.error());
        }
    }

    void Document::RegisterResourceScript(kscript::ScriptType type, std::function<std::string(std::span<kgraph::Contents>, std::span<const std::byte>)> fill)
    {
        kgraph::ResourceScript script = ResourceScriptOf(std::move(type));
        script.fill = std::move(fill);
        // What its fields start as is its source's to say: a host registers the shape, and not that. And what it makes,
        // when the host does not say.
        for (const auto& declared : _runtime->ResourceScripts()) {
            if (declared.name != script.name) continue;
            if (script.initial.empty() && declared.size == script.size) script.initial = declared.initial;
            if (script.resources.empty()) script.resources = declared.resources;
            for (auto& field : script.fields)
                if (const auto was = std::ranges::find(declared.fields, field.name, &kecs::FieldDescription::name);
                    was != declared.fields.end() && !std::isfinite(field.min) && !std::isfinite(field.max) && !field.logarithmic) {
                    field.min = was->min;
                    field.max = was->max;
                    field.logarithmic = was->logarithmic;
                }
        }
        _runtime->SetResourceScript(std::move(script));
        Changed();
    }

    void Document::UnregisterResourceScript(const std::string_view name)
    {
        // Its code is going: it is declared still, as its source says, and nothing runs it.
        for (auto script : _runtime->ResourceScripts())
            if (script.name == name && script.fill) {
                script.fill = nullptr;
                _runtime->SetResourceScript(std::move(script));
                Changed();
            }
    }

    std::vector<kgraph::ResourceScript> Document::ResourceScripts() const { return _runtime->ResourceScripts(); }
    Document::~Document()
    {
        _scripts.Shutdown();    // while the world they are told of is whole
        if (const auto it = Documents().find(_frameGraph); it != Documents().end() && it->second == this) Documents().erase(it);
    }

    void Document::Update()
    {
        _runtime->Update();
        // A model that was being read, read: its things are made under the entity that stood for it.
        for (auto it = _importing.begin(); it != _importing.end();) {
            std::string why;
            if (const auto model = _runtime->ModelOf(it->first, &why)) Imported(it->first, it->second, *model);
            else if (why.empty()) { ++it; continue; }
            else { _notes.push_back(why); Changed(); }
            it = _importing.erase(it);
        }
        // A model placed as its prefab's instance, read again: its prefab made again, and every instance follows.
        for (auto& [file, seen] : _modelPrefabs)
            if (const auto model = _runtime->ModelOf(file); model && model.get() != seen) {
                seen = model.get();
                RemakeModelPrefab(file, *model);
            }
        // A model a scene has parts of, read: its parts known by their names, and what holds them led to where they are now.
        for (auto it = _partsToRecord.begin(); it != _partsToRecord.end();) {
            std::string why;
            if (const auto model = _runtime->ModelOf(*it, &why)) RecordModelParts(*it, *model);
            else if (why.empty()) { ++it; continue; }
            it = _partsToRecord.erase(it);
        }
        // A model read whose material an entity is to follow.
        for (auto it = _toFollow.begin(); it != _toFollow.end();) {
            std::string why;
            if (const auto model = _runtime->ModelOf(it->first, &why)) FollowedModel(it->second, it->first, *model);
            else if (why.empty()) { ++it; continue; }
            it = _toFollow.erase(it);
        }
        // A shader edited under the graph: what it took off the nodes is a change like any other.
        for (auto& taken : _runtime->TakeReset()) {
            _notes.push_back(std::format("'{}' was taken off: its shader no longer has it", taken));
            Changed();
        }
        // What the passes say of the entities: it changes with the world, a frame after whatever changed it. Worked out
        // every few frames: it hashes what every node says, for panels that only need to hear of it soon.
        if ((++_reportsAsked & 3u) != 0u) return;
        std::size_t digest = 0;
        const auto mix = [&](const std::size_t value) { digest ^= value + 0x9e3779b97f4a7c15ull + (digest << 6) + (digest >> 2); };
        for (const auto& node : Graph().nodes) {
            // Whether each node is built can change with nothing edited: a shader's file turning up, or being mended.
            if (const auto* status = _runtime->Status(node.id)) {
                mix(status->ok ? 1 : 2);
                mix(std::hash<std::string>{}(status->error));
                mix(std::hash<std::string>{}(status->note));    // a script it waits for, run now
            }
            if (const auto* report = _runtime->ReportOf(node.id)) {
                // (How many it is for, in sight or out of it: which of them are in sight changes as the camera turns, and
                // is nothing the panels need to be made again for every frame.)
                mix(report->ran + report->culled);
                mix(report->skipped);
                mix(std::hash<std::string>{}(report->why));
            }
        }
        if (digest != _reports) { _reports = digest; ++_reported; }
    }

    // ---- the graph ----------------------------------------------------------------------------------------------

    void Document::Apply(kgraph::Graph graph)
    {
        graph.OrderByRate();    // what is made once comes first, then the fixed step, whatever the edit did to the order
        // Where every node is seen at once: what is gone is not anywhere there, and what is made there is where it was put.
        std::erase_if(graph.overview, [&](const auto& entry) { return !graph.Find(entry.first); });
        if (_makeInOverview && std::ranges::any_of(graph.nodes, [&](const kgraph::Node& node) { return !Graph().Find(node.id); })) {
            // The first made there holds the rest where they are seen, so that it is beside what it was put beside.
            if (graph.overview.empty())
                for (const auto& [id, at] : OverviewPlaces())
                    if (graph.Find(id)) graph.overview[id] = at;
            for (const auto& node : graph.nodes)
                if (!graph.overview.contains(node.id)) graph.overview[node.id] = { node.x, node.y };
        }
        WithFunctions(graph);
        _runtime->Set(std::move(graph));
        Changed();
    }
    void Document::SetGraph(kgraph::Graph graph) { Apply(std::move(graph)); }

    std::string Document::AddNode(std::string_view type, const double x, const double y, const std::string_view id, const std::string_view pass)
    {
        // A node of a component — `PointLight`, `PointLight Buffer` — is a node of a type there is, set up for it.
        std::optional<kgraph::Preset> preset;
        if (!kgraph::FindType(type)) {
            const auto presets = Presets();
            const auto found = std::ranges::find(presets, type, &kgraph::Preset::name);
            if (found == presets.end()) return {};
            preset = *found;
        }
        const std::string kind = preset ? preset->type : std::string(type);
        kgraph::Graph graph = Graph();
        std::string name(id);
        if (name.empty() || graph.Find(name)) {
            std::string base(id.empty() ? type : id);
            if (id.empty()) std::ranges::transform(base, base.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            name = base;
            for (int n = 2; graph.Find(name); ++n) name = std::format("{} {}", base, n);
        }
        auto& node = graph.Add(name, kind);
        node.x = x;
        node.y = y;
        node.pass = std::string(pass);
        if (preset) node.title = preset->name;
        if (!pass.empty() && std::ranges::find(graph.passes, pass) == graph.passes.end()) graph.passes.emplace_back(pass);
        // It starts with what a node of its kind needs, set to something that works: an image has a size, a format
        // and a usage from the first, and is an image, not a list of things to fill in before it is one.
        for (const auto& port : kgraph::PortsFor(*kgraph::FindType(kind), node))
            if (!port.optional && port.kind == kgraph::PortKind::eValue && !port.initial.IsNull()) node.Set(port.name, port.initial);
        if (preset)
            for (const auto& [input, value] : preset->inputs) node.Set(input, value);
        Apply(std::move(graph));
        return name;
    }

    namespace {
        /** The References something is wired from. */
        std::set<std::string> WiredReferences(const kgraph::Graph& graph)
        {
            std::set<std::string> wired;
            for (const auto& node : graph.nodes)
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link)
                        if (const kgraph::Node* from = graph.Find(input.link->node); from && from->type == "Reference") wired.insert(from->id);
            return wired;
        }

        /**
         * A Reference that loses the last wire from it goes with it: it is there to be wired from, and what it stands for
         * is still in the drawer to bring in again. One that was never wired — just brought in — stays.
         */
        void LetGoOfUnwired(kgraph::Graph& graph, const kgraph::Graph& was)
        {
            const auto before = WiredReferences(was), now = WiredReferences(graph);
            std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return node.type == "Reference" && before.contains(node.id) && !now.contains(node.id); });
        }
    }

    // ---- passes -----------------------------------------------------------------------------------------------

    namespace {
        /** Where a pass of a rate can be in the list: among the others of its rate, which are together — setup, fixed, every frame. */
        std::pair<std::ptrdiff_t, std::ptrdiff_t> RangeOf(const kgraph::Graph& graph, const kgraph::PassRate rate)
        {
            const auto count = [&](const kgraph::PassRate of) {
                return static_cast<std::ptrdiff_t>(std::ranges::count_if(graph.passes, [&](const std::string& pass) { return graph.RateOf(pass) == of; }));
            };
            const std::ptrdiff_t setups = count(kgraph::PassRate::eSetup), fixed = count(kgraph::PassRate::eFixed);
            switch (rate) {
            case kgraph::PassRate::eSetup: return { 0, setups };
            case kgraph::PassRate::eFixed: return { setups, setups + fixed };
            default: return { setups + fixed, static_cast<std::ptrdiff_t>(graph.passes.size()) };
            }
        }

        /** The setup and fixed pages as the list has them: the order they run in. */
        void RatesInOrder(kgraph::Graph& graph)
        {
            std::vector<std::string> setup, fixed;
            for (const auto& pass : graph.passes) {
                if (graph.IsSetup(pass)) setup.push_back(pass);
                else if (graph.IsFixed(pass)) fixed.push_back(pass);
            }
            graph.setup = std::move(setup);
            graph.fixed = std::move(fixed);
        }
    }

    bool Document::AddPass(const std::string_view name, const int index, const kgraph::PassRate rate)
    {
        kgraph::Graph graph = Graph();
        if (name.empty() || std::ranges::find(graph.passes, name) != graph.passes.end()) return false;
        const auto [first, end] = RangeOf(graph, rate);
        const std::ptrdiff_t at = index < 0 ? end : std::clamp<std::ptrdiff_t>(index, first, end);
        graph.passes.insert(graph.passes.begin() + at, std::string(name));
        if (rate == kgraph::PassRate::eSetup) graph.setup.emplace_back(name);
        if (rate == kgraph::PassRate::eFixed) graph.fixed.emplace_back(name);
        RatesInOrder(graph);
        Apply(std::move(graph));
        return true;
    }

    bool Document::SetPassRate(const std::string_view pass, const kgraph::PassRate rate)
    {
        kgraph::Graph graph = Graph();
        const auto at = std::ranges::find(graph.passes, pass);
        const kgraph::PassRate was = graph.RateOf(pass);
        if (at == graph.passes.end() || was == rate) return false;
        // Where it was, as near as it can be: at the end of its new rate's run coming from before it, at the start
        // coming from after it (setup, fixed, every frame).
        const std::string name = *at;
        graph.passes.erase(at);
        std::erase(graph.setup, name);
        std::erase(graph.fixed, name);
        const auto order = [](const kgraph::PassRate r) { return r == kgraph::PassRate::eSetup ? 0 : r == kgraph::PassRate::eFixed ? 1 : 2; };
        const auto [first, end] = RangeOf(graph, rate);
        graph.passes.insert(graph.passes.begin() + (order(was) < order(rate) ? first : end), name);
        if (rate == kgraph::PassRate::eSetup) graph.setup.push_back(name);
        if (rate == kgraph::PassRate::eFixed) graph.fixed.push_back(name);
        RatesInOrder(graph);
        Apply(std::move(graph));
        return true;
    }

    bool Document::RenamePass(const std::string_view pass, const std::string_view name)
    {
        kgraph::Graph graph = Graph();
        const auto at = std::ranges::find(graph.passes, pass);
        if (at == graph.passes.end() || name.empty() || std::ranges::find(graph.passes, name) != graph.passes.end()) return false;
        *at = std::string(name);
        std::ranges::replace(graph.setup, std::string(pass), std::string(name));
        std::ranges::replace(graph.fixed, std::string(pass), std::string(name));
        for (auto& node : graph.nodes)
            if (node.pass == pass) node.pass = std::string(name);
        Apply(std::move(graph));
        return true;
    }

    bool Document::MovePass(const std::string_view pass, const int index)
    {
        kgraph::Graph graph = Graph();
        const auto at = std::ranges::find(graph.passes, pass);
        if (at == graph.passes.end()) return false;
        const std::string name = *at;
        graph.passes.erase(at);
        // Among those of its rate: setup before the fixed step, and that before anything done every frame.
        const auto [first, end] = RangeOf(graph, graph.RateOf(name));
        const auto to = std::clamp<std::ptrdiff_t>(index, first, end);
        graph.passes.insert(graph.passes.begin() + to, name);
        RatesInOrder(graph);
        Apply(std::move(graph));
        return true;
    }

    bool Document::RemovePass(const std::string_view pass, const bool keepNodes)
    {
        kgraph::Graph graph = Graph();
        if (std::erase(graph.passes, pass) == 0) return false;
        std::erase(graph.setup, pass);
        std::erase(graph.fixed, pass);
        std::set<std::string> gone;
        for (auto& node : graph.nodes)
            if (node.pass == pass) {
                if (keepNodes) node.pass.clear();
                else gone.insert(node.id);
            }
        if (!gone.empty()) {
            std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return gone.contains(node.id); });
            // What was wired to one of them is no longer wired to anything; a Reference to one stands for nothing.
            for (auto& node : graph.nodes)
                std::erase_if(node.inputs, [&](const auto& input) {
                    return (input.second.link && gone.contains(input.second.link->node))
                        || (node.type == "Reference" && input.first == "node" && input.second.value.String() && gone.contains(*input.second.value.String()));
                });
        }
        Apply(std::move(graph));
        return true;
    }

    bool Document::SetNodePass(const std::string_view id, const std::string_view pass)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node) return false;
        node->pass = std::string(pass);
        if (!pass.empty() && std::ranges::find(graph.passes, pass) == graph.passes.end()) graph.passes.emplace_back(pass);
        Apply(std::move(graph));
        return true;
    }

    namespace {
        /**
         * Wires the nodes of @p graph that @p touched names — and those wired from them — as passes are wired: a node
         * takes what another pass made through a Reference to it in its own pass, when that pass is before its own (or
         * the node is in none); straight, when the two are of one pass, or the other is a later one. A Reference made
         * for it is put beside the node that takes it. Those that stand for a node of their own pass, wired from by
         * nothing now, go.
         */
        void WireAcrossPasses(kgraph::Graph& graph, const std::set<std::string>& touched)
        {
            const auto order = [&](const std::string& pass) -> int {
                if (pass.empty()) return -1;
                const auto at = std::ranges::find(graph.passes, pass);
                return at == graph.passes.end() ? static_cast<int>(graph.passes.size()) : static_cast<int>(at - graph.passes.begin());
            };
            const auto standsFor = [&](const kgraph::Node& node) -> std::string {
                const auto target = node.inputs.find("node");
                return node.type == "Reference" && target != node.inputs.end() && target->second.value.String() ? *target->second.value.String() : std::string();
            };
            // What a node is, through the References between: the node that makes it.
            const auto origin = [&](std::string id) {
                for (int hops = 0; hops < 16; ++hops) {
                    const kgraph::Node* node = graph.Find(id);
                    const std::string target = node ? standsFor(*node) : std::string();
                    if (target.empty() || !graph.Find(target)) return id;
                    id = target;
                }
                return id;
            };
            const auto referenceIn = [&](const std::string& target, const std::string& pass, const double x, const double y) {
                for (const auto& node : graph.nodes)
                    if (node.pass == pass && standsFor(node) == target) return node.id;
                std::string name = std::format("{} (in {})", target, pass);
                for (int n = 2; graph.Find(name); ++n) name = std::format("{} (in {}) {}", target, pass, n);
                auto& made = graph.Add(name, "Reference");
                made.title = target;
                made.x = x;
                made.y = y;
                made.pass = pass;
                made.Set("node", target);
                return name;
            };
            // What was wired from before: a Reference that loses the last that took it, here, goes after.
            std::set<std::string> wiredBefore;
            for (const auto& node : graph.nodes)
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link) wiredBefore.insert(input.link->node);
            // Who is wired again: the nodes touched, and every node wired from one of them.
            std::vector<std::string> takers;
            for (const auto& node : graph.nodes) {
                if (node.type == "Reference") continue;
                bool wire = touched.contains(node.id);
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link && (touched.contains(input.link->node) || touched.contains(origin(input.link->node)))) wire = true;
                if (wire) takers.push_back(node.id);
            }
            for (const std::string& id : takers) {
                std::vector<std::pair<std::string, std::string>> rewired;     // input, the node it is wired from now
                const kgraph::Node* taker = graph.Find(id);
                if (!taker) continue;
                const std::string pass = taker->pass;
                const double x = taker->x, y = taker->y;
                // Its wires, copied: a Reference made below is a node more, and the nodes may move in memory for it.
                std::vector<std::pair<std::string, std::string>> wires;     // input, the node it is wired from
                for (const auto& [name, input] : taker->inputs)
                    if (input.link) wires.emplace_back(name, input.link->node);
                for (const auto& [name, wiredFrom] : wires) {
                    const std::string source = origin(wiredFrom);
                    const kgraph::Node* made = graph.Find(source);
                    if (!made) continue;
                    // Of an earlier pass (or of none) into one of a pass: brought in. Otherwise wired straight to it.
                    const bool bring = !pass.empty() && made->pass != pass && order(made->pass) < order(pass);
                    const std::string from = bring ? referenceIn(source, pass, x - 260.0, y) : source;
                    if (from != wiredFrom) rewired.emplace_back(name, from);
                }
                kgraph::Node* node = graph.Find(id);     // (found again: a Reference made may have moved the nodes)
                for (const auto& [name, from] : rewired) node->inputs.at(name).link->node = from;
            }
            // A Reference that nothing is wired from now — one that stands for a node of its own pass, or one whose last
            // taker this took from it: nothing to bring in. (One nothing was wired from before is left as it was.)
            std::set<std::string> wiredFrom;
            for (const auto& node : graph.nodes)
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link) wiredFrom.insert(input.link->node);
            std::erase_if(graph.nodes, [&](const kgraph::Node& node) {
                const std::string target = standsFor(node);
                const kgraph::Node* of = target.empty() ? nullptr : graph.Find(target);
                if (!of || wiredFrom.contains(node.id)) return false;
                return of->pass == node.pass || wiredBefore.contains(node.id);
            });
        }
    }

    bool Document::MoveNodesToPass(const std::vector<std::string>& ids, const std::string_view pass)
    {
        kgraph::Graph graph = Graph();
        std::set<std::string> moved;
        for (const auto& id : ids)
            if (kgraph::Node* node = graph.Find(id)) { node->pass = std::string(pass); moved.insert(node->id); }
        if (moved.empty()) return false;
        if (!pass.empty() && std::ranges::find(graph.passes, pass) == graph.passes.end()) graph.passes.emplace_back(pass);
        WireAcrossPasses(graph, moved);
        Apply(std::move(graph));
        return true;
    }

    std::string Document::MakeBufferFor(const std::string_view pipeline, std::string_view parameter, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return std::string(); };
        if (parameter.starts_with("param.")) parameter.remove_prefix(6);
        const kgraph::Node* found = Graph().Find(pipeline);
        if (!found) return fail(std::format("there is no node '{}'", pipeline));
        const kgraph::Node node = *found;       // (a copy: every step below makes the graph anew)
        // The shaders it names, a stage each: the module, and the entry point — the stage's name and Main when not said.
        std::vector<std::pair<std::string, std::string>> shaders;
        for (const auto& [name, input] : node.inputs) {
            if (!name.ends_with(".module") || !input.value.String() || input.value.String()->empty()) continue;
            const std::string stage = name.substr(0, name.size() - 7);
            const auto entry = node.inputs.find(stage + ".entry");
            shaders.emplace_back(*input.value.String(), entry != node.inputs.end() && entry->second.value.String() && !entry->second.value.String()->empty()
                                                           ? *entry->second.value.String() : stage.substr(0, stage.find_first_of(".[")) + "Main");
        }
        if (shaders.empty()) return fail(std::format("'{}' names no shader yet: the buffer is laid out as its shader declares it", pipeline));

        // The buffer is laid out by a shader that uses it: each stage's is tried, and the first that declares it is the one.
        const std::string id = AddNode("ShaderBuffer", node.x - 320.0, node.y + 40.0, std::format("{} data", parameter), node.pass);
        SetInput(id, "parameter", std::string(parameter));
        std::string why;
        for (const auto& [module, entry] : shaders) {
            SetInput(id, "module", module);
            SetInput(id, "entry", entry);
            const kgraph::NodeStatus* status = _runtime->Status(id);
            if (status && status->ok) {
                Connect(id, "out", node.id, std::format("param.{}", parameter));
                return id;
            }
            if (status) why = status->error;
        }
        RemoveNodes({ id });
        return fail(why.empty() ? std::format("no shader of '{}' declares a buffer '{}'", pipeline, parameter) : why);
    }

    std::string Document::Import(const std::string_view id, const std::string_view pass, const double x, const double y)
    {
        kgraph::Graph graph = Graph();
        if (!graph.Find(id)) return {};
        // One that is there already is the one: a thing is imported into a pass once.
        for (const auto& node : graph.nodes)
            if (node.type == "Reference" && node.pass == pass)
                if (const auto target = node.inputs.find("node"); target != node.inputs.end() && target->second.value.String() && *target->second.value.String() == id) return node.id;
        std::string name = pass.empty() ? std::format("{} (imported)", id) : std::format("{} (in {})", id, pass);
        for (int n = 2; graph.Find(name); ++n) name = std::format("{} (in {}) {}", id, pass, n);
        auto& node = graph.Add(name, "Reference");
        node.title = std::string(id);
        node.x = x;
        node.y = y;
        node.pass = std::string(pass);
        node.Set("node", std::string(id));
        Apply(std::move(graph));
        return name;
    }

    std::vector<std::string> Document::Importable(const std::string_view pass) const
    {
        const auto& passes = Graph().passes;
        const auto here = std::ranges::find(passes, pass);
        std::vector<std::string> ids;
        for (const auto& node : Graph().nodes) {
            if (node.type == "Reference" || node.pass == pass) continue;
            const kgraph::NodeType* type = kgraph::FindType(node.type);
            if (!type || type->outputs.empty() || type->category == "Passes") continue;    // a pass node gives nothing to take
            // From the passes before this one, and from what is in no pass.
            if (!node.pass.empty() && std::ranges::find(passes.begin(), here, node.pass) == here) continue;
            ids.push_back(node.id);
        }
        return ids;
    }

    void Document::RemoveNodes(const std::vector<std::string>& ids)
    {
        kgraph::Graph graph = Graph();
        const auto gone = [&](const std::string& id) { return std::ranges::find(ids, id) != ids.end(); };
        std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return gone(node.id); });
        for (auto& node : graph.nodes)
            std::erase_if(node.inputs, [&](const auto& input) { return input.second.link && gone(input.second.link->node); });
        LetGoOfUnwired(graph, Graph());
        Apply(std::move(graph));
    }

    void Document::MoveNodes(const std::vector<std::string>& ids, const double dx, const double dy)
    {
        kgraph::Graph graph = Graph();
        for (const auto& id : ids)
            if (auto* node = graph.Find(id)) { node->x += dx; node->y += dy; }
        Apply(std::move(graph));
    }

    std::vector<std::string> Document::PasteNodes(std::vector<kgraph::Node> nodes, const double x, const double y, const std::string* pass)
    {
        std::vector<std::string> made;
        if (nodes.empty()) return made;
        kgraph::Graph graph = Graph();
        double left = nodes.front().x, top = nodes.front().y;
        for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }
        // A name of its own for each: what it was called, and the first number after it that is free.
        std::map<std::string, std::string> renamed;
        std::set<std::string> taken;
        for (const auto& node : graph.nodes) taken.insert(node.id);
        for (const auto& node : nodes) {
            std::string base = node.id;
            if (const auto space = base.rfind(' '); space != std::string::npos && space + 1 < base.size()
                && std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space) + 1, base.end(), [](const unsigned char c) { return std::isdigit(c) != 0; }))
                base.erase(space);
            std::string name = base;
            for (int n = 2; taken.contains(name); ++n) name = std::format("{} {}", base, n);
            taken.insert(name);
            renamed[node.id] = name;
        }
        // What a node is, through the References between: one that brings in a node copied with it is that node.
        const auto through = [&](std::string id) {
            for (int hops = 0; hops < 16 && !renamed.contains(id); ++hops) {
                const kgraph::Node* node = graph.Find(id);
                const auto target = node && node->type == "Reference" ? node->inputs.find("node") : decltype(node->inputs.end()) {};
                if (!node || node->type != "Reference" || target == node->inputs.end() || !target->second.value.String()) break;
                id = *target->second.value.String();
            }
            return id;
        };
        for (auto& node : nodes) {
            // Wired among themselves as they were — straight, or through a Reference to one of them; from the rest, as
            // far as the rest is still here.
            for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                if (auto& link = it->second.link) {
                    if (const auto within = renamed.find(through(link->node)); within != renamed.end()) link->node = within->second;
                    else if (!graph.Find(link->node)) { it = node.inputs.erase(it); continue; }
                }
                ++it;
            }
            if (node.title == node.id) node.title.clear();
            node.id = renamed[node.id];
            node.x += x - left;
            node.y += y - top;
            if (pass) node.pass = *pass;
            if (!node.pass.empty() && std::ranges::find(graph.passes, node.pass) == graph.passes.end()) graph.passes.push_back(node.pass);
            made.push_back(node.id);
            graph.nodes.push_back(std::move(node));
        }
        // Into a pass: what they take of the others is brought in, as a pass brings in what it uses.
        if (pass) {
            WireAcrossPasses(graph, std::set<std::string>(made.begin(), made.end()));
            std::erase_if(made, [&](const std::string& id) { return !graph.Find(id); });  // a Reference with nothing left to bring in
        }
        Apply(std::move(graph));
        return made;
    }

    std::vector<std::string> Document::DuplicateNodes(const std::vector<std::string>& ids, const double dx, const double dy)
    {
        std::vector<kgraph::Node> nodes;
        for (const auto& id : ids)
            if (const kgraph::Node* node = Graph().Find(id)) nodes.push_back(*node);
        if (nodes.empty()) return {};
        double left = nodes.front().x, top = nodes.front().y;
        for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }
        return PasteNodes(std::move(nodes), left + dx, top + dy);
    }

    void Document::PlaceNodes(const std::map<std::string, std::pair<double, double>>& where)
    {
        kgraph::Graph graph = Graph();
        bool moved = false;
        for (const auto& [id, at] : where)
            if (auto* node = graph.Find(id); node && (node->x != at.first || node->y != at.second)) { node->x = at.first; node->y = at.second; moved = true; }
        if (moved) Apply(std::move(graph));
    }

    std::map<std::string, std::pair<double, double>> Document::OverviewPlaces() const
    {
        const kgraph::Graph& graph = Graph();
        std::map<std::string, std::pair<double, double>> where;
        // Each pass's page, where it would be: beside the one before, by as much as that one is wide and a gap.
        std::vector<std::string> order { std::string() };
        order.insert(order.end(), graph.passes.begin(), graph.passes.end());
        constexpr double CardWidth = 320, Gap = 240;
        double x = 0;
        for (const auto& pass : order) {
            double left = 0, top = 0, right = 0;
            bool any = false;
            for (const auto& node : graph.nodes) {
                if (node.pass != pass) continue;
                if (!any) { left = right = node.x; top = node.y; any = true; }
                left = std::min(left, node.x); right = std::max(right, node.x); top = std::min(top, node.y);
            }
            if (!any) continue;
            for (const auto& node : graph.nodes)
                if (node.pass == pass) where[node.id] = { std::round(node.x - left + x), std::round(node.y - top) };
            x += right - left + CardWidth + Gap;
        }
        // What is in no pass the graph lists: where it is.
        for (const auto& node : graph.nodes)
            if (!where.contains(node.id)) where[node.id] = { node.x, node.y };
        for (const auto& [id, at] : graph.overview)
            if (where.contains(id)) where[id] = at;
        return where;
    }

    void Document::PlaceInOverview(const std::map<std::string, std::pair<double, double>>& where)
    {
        kgraph::Graph graph = Graph();
        // Put there once, every node is held where it is seen: none moves for a page's being changed after.
        auto placed = OverviewPlaces();
        for (const auto& [id, at] : where)
            if (placed.contains(id)) placed[id] = at;
        if (placed == graph.overview) return;
        graph.overview = std::move(placed);
        Apply(std::move(graph));
    }

    void Document::MoveInOverview(const std::vector<std::string>& ids, const double dx, const double dy)
    {
        const auto placed = OverviewPlaces();
        std::map<std::string, std::pair<double, double>> where;
        for (const auto& id : ids)
            if (const auto at = placed.find(id); at != placed.end()) where[id] = { at->second.first + dx, at->second.second + dy };
        PlaceInOverview(where);
    }

    void Document::SetMeshImport(const std::string& file, kgraph::MeshImport import)
    {
        kgraph::Graph graph = Graph();
        if (import.Default()) graph.meshImports.erase(file);
        else graph.meshImports[file] = std::move(import);
        if (graph.meshImports == Graph().meshImports) return;
        Apply(std::move(graph));
    }

    void Document::SetTextureSettings(std::map<std::string, kgraph::TextureImport> imports, std::map<std::string, kgraph::TexturePack> packs)
    {
        std::erase_if(imports, [](const auto& entry) { return entry.second.Default(); });
        if (imports == Graph().textureImports && packs == Graph().texturePacks) return;
        kgraph::Graph graph = Graph();
        graph.textureImports = std::move(imports);
        graph.texturePacks = std::move(packs);
        Apply(std::move(graph));
    }

    void Document::SetDefaultMeshTangents(const kgraph::Tangents tangents)
    {
        if (Graph().defaultMeshTangents == tangents) return;
        kgraph::Graph graph = Graph();
        graph.defaultMeshTangents = tangents;
        Apply(std::move(graph));
    }

    kgraph::MeshImport Document::MeshImportOf(const std::string_view file) const
    {
        const auto it = Graph().meshImports.find(std::string(file));
        return it == Graph().meshImports.end() ? kgraph::MeshImport {} : it->second;
    }

    bool Document::SetPlacement(const std::string_view id, const std::string_view input, const kgraph::Placement where)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        const kgraph::NodeType* type = node ? kgraph::FindType(node->type) : nullptr;
        if (!type || where == kgraph::Placement::eWire) return false;
        // What its type says of it — the listed input, for one given more than once.
        std::string listed(input);
        if (const auto open = listed.find('['); open != std::string::npos)
            if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
        const auto port = std::ranges::find(type->inputs, listed, &kgraph::Port::name);
        const kgraph::Placement own = port == type->inputs.end() ? kgraph::Placement::eInspector : port->place;
        if (own == kgraph::Placement::eWire) return false;
        if (where == own) node->placed.erase(std::string(input));
        else node->placed[std::string(input)] = where == kgraph::Placement::eNode ? "node" : "inspector";
        Apply(std::move(graph));
        return true;
    }

    std::string Document::MakeCompanion(const std::string_view id, const std::string_view input, std::string* error)
    {
        const kgraph::Node* node = Graph().Find(id);
        const kgraph::NodeType* type = node ? kgraph::FindType(node->type) : nullptr;
        std::string listed(input);
        if (const auto open = listed.find('['); open != std::string::npos)
            if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
        const auto port = type ? std::ranges::find(type->inputs, listed, &kgraph::Port::name) : decltype(type->inputs.end()) {};
        if (!type || port == type->inputs.end() || port->companion.empty()) {
            if (error) *error = std::format("'{}' of '{}' has no node of its own to set it up", input, id);
            return {};
        }
        const std::string pass = node->pass;     // (the node is another once a node is added)
        const double x = node->x - 300.0, y = node->y;
        const std::string made = AddNode(port->companion, x, y, {}, pass);
        if (made.empty() || !Connect(made, "out", id, input)) {
            if (error) *error = std::format("a {} could not be made and wired to '{}'", port->companion, input);
            return {};
        }
        return made;
    }

    bool Document::SetTitle(const std::string_view id, std::string title)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node) return false;
        node->title = std::move(title);
        Apply(std::move(graph));
        return true;
    }

    namespace {
        /** An input is given whole or in its parts, not both: giving one takes the other away. */
        void OneWayOfGiving(kgraph::Node& node, const std::string_view input)
        {
            const kgraph::NodeType* type = kgraph::FindType(node.type);
            if (!type) return;
            for (const auto& part : kgraph::PartsOf(*type, input)) node.inputs.erase(part);
            if (const auto dot = input.rfind('.'); dot != std::string_view::npos && dot + 2 == input.size()) {
                const std::string whole(input.substr(0, dot));
                if (std::ranges::contains(kgraph::PartsOf(*type, whole), input)) node.inputs.erase(whole);
            }
        }
    }

    bool Document::SetInput(const std::string_view id, const std::string_view input, Value value)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node) return false;
        OneWayOfGiving(*node, input);
        // What is set is what the input takes: an Int's a whole number, a Float's a float's, a float4's four of them.
        for (const auto& port : _runtime->PortsOf(id))
            if (port.name == input) { value = kgraph::Conform(value, port.shape); break; }
        // Another component's buffer holds what that one has: the fields ticked of the one before go with it.
        if (node->type == "ComponentBuffer" && input == "component" && node->inputs.contains("component") && node->inputs.at("component").value != value) node->inputs.erase("fields");
        node->Set(std::string(input), std::move(value));
        Apply(std::move(graph));
        return true;
    }

    bool Document::Connect(const std::string_view fromNode, const std::string_view output, const std::string_view id, const std::string_view input)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node || !graph.Find(fromNode)) return false;
        OneWayOfGiving(*node, input);
        node->Wire(std::string(input), std::string(fromNode), std::string(output));
        // A pipeline's parameter wired is the same for every draw: what its draws were given of it is theirs no more.
        if (node->type.ends_with("Pipeline") && input.starts_with("param."))
            for (auto& other : graph.nodes) {
                const auto pipeline = other.inputs.find("pipeline");
                if (pipeline != other.inputs.end() && pipeline->second.link && pipeline->second.link->node == id) other.inputs.erase(std::string(input));
            }
        LetGoOfUnwired(graph, Graph());
        Apply(std::move(graph));
        return true;
    }

    bool Document::ClearInput(const std::string_view id, const std::string_view input)
    {
        kgraph::Graph graph = Graph();
        auto* node = graph.Find(id);
        if (!node || node->inputs.erase(std::string(input)) == 0) return false;
        LetGoOfUnwired(graph, Graph());
        Apply(std::move(graph));
        return true;
    }

    std::size_t Document::DisconnectOutput(const std::string_view id, const std::string_view output)
    {
        kgraph::Graph graph = Graph();
        std::size_t taken = 0;
        for (auto& node : graph.nodes)
            taken += std::erase_if(node.inputs, [&](const auto& input) { return input.second.link && input.second.link->node == id && input.second.link->output == output; });
        if (taken > 0) {
            LetGoOfUnwired(graph, Graph());
            Apply(std::move(graph));
        }
        return taken;
    }

    // ---- entities -----------------------------------------------------------------------------------------------

    kecs::Entity Document::CreateEntity(const std::string_view name, const kecs::Entity parent)
    {
        const kecs::Entity entity = _world.Create();
        _world.Rename(entity, std::string(name));
        if (!parent.IsNull()) _world.SetParent(entity, parent, -1, false);
        Changed();
        return entity;
    }

    bool Document::SetParent(const kecs::Entity entity, const kecs::Entity parent, const int index)
    {
        if (!_world.SetParent(entity, parent, index)) return false;
        Changed();
        return true;
    }

    bool Document::SetPosition(const kecs::Entity entity, const kecs::Vec3& position)
    {
        if (!_world.SetPosition(entity, position)) return false;
        Override(entity, "position");
        Shifted();
        return true;
    }

    bool Document::SetRotation(const kecs::Entity entity, const kecs::Vec3& degrees)
    {
        if (!_world.SetRotation(entity, degrees)) return false;
        Override(entity, "rotation");
        Shifted();
        return true;
    }

    bool Document::SetScale(const kecs::Entity entity, const kecs::Vec3& scale)
    {
        if (!_world.SetScale(entity, scale)) return false;
        Override(entity, "scale");
        Shifted();
        return true;
    }

    void Document::Moved(const kecs::Entity entity, const std::string_view what)
    {
        Override(entity, what);
        Shifted();
    }

    bool Document::DestroyEntity(const kecs::Entity entity)
    {
        // A prefab's entity taken from its instance stays taken: the instance made again does not have it.
        if (_world.Alive(entity)) Override(entity, "gone");
        if (!_world.Destroy(entity)) return false;
        Changed();
        return true;
    }

    std::vector<kecs::Entity> Document::Entities() const { return _world.All(); }

    std::string Document::NameOf(const kecs::Entity entity) const { return _world.NameOf(entity); }

    bool Document::Rename(const kecs::Entity entity, std::string name)
    {
        if (!_world.Rename(entity, std::move(name))) return false;
        Override(entity, "name");
        Changed();
        return true;
    }

    kecs::ComponentId Document::AddTag(const std::string_view name)
    {
        if (name.empty()) return kecs::NoComponent;
        if (const kecs::ComponentId known = _world.Find(name); known != kecs::NoComponent) return known;
        kecs::ComponentDescription tag;
        tag.name = std::string(name);
        _tags.emplace_back(name);
        Changed();
        return _world.Register(std::move(tag));
    }

    std::vector<std::string> Document::ComponentNames() const
    {
        std::vector<std::string> names;
        for (kecs::ComponentId id = 0; id < _world.ComponentCount(); ++id)
            if (const auto& name = _world.Describe(id).name; !Structural(name)) names.push_back(name);
        std::ranges::sort(names);
        return names;
    }

    std::vector<std::string> Document::ComponentsOf(const kecs::Entity entity) const
    {
        std::vector<std::string> names;
        if (!_world.Alive(entity)) return names;
        for (const kecs::ComponentId id : _world.ComponentsOf(entity))
            if (const auto& name = _world.Describe(id).name; !Structural(name)) names.push_back(name);
        return names;
    }

    bool Document::AddComponent(const kecs::Entity entity, const std::string_view component)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || _world.Has(entity, id)) return false;
        _world.Add(entity, id);
        Override(entity, std::format("+{}", component));
        Changed();
        return true;
    }

    bool Document::RemoveComponent(const kecs::Entity entity, const std::string_view component)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.Remove(entity, id)) return false;
        Override(entity, std::format("-{}", component));
        Changed();
        return true;
    }

    bool Document::SetEnabled(const kecs::Entity entity, const std::string_view component, const bool enabled)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.SetEnabled(entity, id, enabled)) return false;
        Override(entity, std::format("~{}", component));
        Changed();
        return true;
    }

    bool Document::IsEnabled(const kecs::Entity entity, const std::string_view component) const
    {
        const kecs::ComponentId id = _world.Find(component);
        return id != kecs::NoComponent && _world.Alive(entity) && _world.IsEnabled(entity, id);
    }

    Value Document::Field(const kecs::Entity entity, const std::string_view component, const std::string_view field) const
    {
        const kecs::ComponentId id = _world.Find(component);
        return id == kecs::NoComponent ? Value {} : ReadField(_world, entity, id, field);
    }

    Value Document::ReadField(const kecs::World& world, const kecs::Entity entity, const kecs::ComponentId id, const std::string_view field)
    {
        if (!world.Alive(entity)) return {};
        const auto* bytes = static_cast<const std::byte*>(world.Get(entity, id));
        const kecs::FieldDescription* found = world.Describe(id).FindField(field);
        if (!bytes || !found) return {};
        const std::byte* at = bytes + found->offset;
        if (found->type == "string") return Value(*reinterpret_cast<const std::string*>(at));
        if (found->type == "entity") return EntityValue(*reinterpret_cast<const kecs::Entity*>(at));
        if (HoldsResource(found->type)) {
            const Held& held = *reinterpret_cast<const Held*>(at);
            return held && !held->node.empty() ? Value(held->node) : Value {};
        }
        Numbers numbers;
        if (!NumbersOf(found->type, numbers)) return {};
        if (numbers.count == 1) return numbers.scalar == 'b' ? Value(ReadNumber(numbers, at) != 0.0) : Value(ReadNumber(numbers, at));
        Value::List list;
        for (std::size_t i = 0; i < numbers.count; ++i) list.emplace_back(ReadNumber(numbers, at + i * numbers.bytes));
        return Value(std::move(list));
    }

    bool Document::WriteField(kecs::World& world, const kecs::Entity entity, const kecs::ComponentId id, const std::string_view field, const Value& value,
                              const std::function<std::shared_ptr<kgraph::Resource>(const std::string&)>& resolve,
                              const std::function<void(std::shared_ptr<kgraph::Resource>)>& retire, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const std::string component = world.Describe(id).name;
        auto* bytes = static_cast<std::byte*>(const_cast<void*>(std::as_const(world).Get(entity, id)));
        const kecs::FieldDescription* found = world.Describe(id).FindField(field);
        if (!found) return fail(std::format("'{}' has no field '{}'", component, field));
        std::byte* at = bytes + found->offset;
        Numbers numbers;
        if (found->type == "string") {
            if (!value.String()) return fail(std::format("'{}' is text", field));
            *reinterpret_cast<std::string*>(at) = *value.String();
        } else if (found->type == "entity") {
            // Another entity of this scene, or none.
            const auto other = EntityOfValue(value);
            if (!other) return fail(std::format("'{}' is an entity: one of the scene's, or nothing", field));
            if (!other->IsNull() && !world.Alive(*other)) return fail(std::format("'{}' is given an entity that is not in the scene", field));
            *reinterpret_cast<kecs::Entity*>(at) = *other;
        } else if (HoldsResource(found->type)) {
            Held held;
            if (!value.IsNull()) {
                if (!value.String()) return fail(std::format("'{}' holds a resource: the id of the node that makes it", field));
                held = resolve(*value.String());
                if (!held) return fail(std::format("there is no node '{}' that makes a resource", *value.String()));
            }
            // What it held may be in what this frame has drawn with: let go of once that is done.
            auto old = std::exchange(*reinterpret_cast<Held*>(at), std::move(held));
            if (retire) retire(std::move(old));
        } else if (NumbersOf(found->type, numbers)) {
            if (numbers.count == 1) {
                if (const bool* flag = value.Bool()) WriteNumber(numbers, at, *flag ? 1.0 : 0.0);
                else if (const double* number = value.Number()) WriteNumber(numbers, at, *number);
                else if (const std::string* name = value.String(); name && !found->choices.empty()) {
                    // One of so many things, by its name — any case.
                    const auto same = [&](const std::string& choice) {
                        return std::ranges::equal(choice, *name, [](const char a, const char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
                    };
                    const auto it = std::ranges::find_if(found->choices, same);
                    if (it == found->choices.end()) {
                        std::string names;
                        for (const auto& choice : found->choices) names += (names.empty() ? "" : ", ") + choice;
                        return fail(std::format("'{}' is one of {}: not '{}'", field, names, *name));
                    }
                    WriteNumber(numbers, at, static_cast<double>(it - found->choices.begin()));
                }
                else return fail(std::format("'{}' is a {}", field, numbers.scalar == 'b' ? "true or false" : "number"));
            } else {
                const auto* items = value.Items();
                if (!items || items->size() != numbers.count || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; }))
                    return fail(std::format("'{}' is {} numbers", field, numbers.count));
                for (std::size_t i = 0; i < numbers.count; ++i) WriteNumber(numbers, at + i * numbers.bytes, *(*items)[i].Number());
            }
        } else {
            return fail(std::format("'{}' is a {}, which cannot be set here", field, found->type));
        }
        world.MarkChanged(entity, id);
        return true;
    }

    bool Document::SetField(const kecs::Entity entity, const std::string_view component, const std::string_view field, const Value& value, std::string* error)
    {
        const kecs::ComponentId id = _world.Find(component);
        if (id == kecs::NoComponent || !_world.Alive(entity) || !_world.Has(entity, id)) {
            if (error) *error = std::format("the entity has no '{}'", component);
            return false;
        }
        if (!WriteField(_world, entity, id, field, value, [this](const std::string& name) { return _runtime->ResourceOf(name); },
                        [this](Held old) { _runtime->Retire(std::move(old)); }, error)) return false;
        Override(entity, std::format("{}.{}", component, field));
        // A material that follows an asset, set on the entity itself: that field is the entity's own from now on.
        if (!_following && !MaterialAssetOfEntity(entity, component).empty()) {
            const std::string line = std::format("{}.{}", component, field);
            std::vector<std::string> overrides = LinkLines(entity, true);
            if (!std::ranges::contains(overrides, line)) {
                overrides.push_back(line);
                SetLinkLines(entity, true, overrides);
            }
        }
        if (_runtime->MaterialKindOf(component) && field == "alphaMode" && !_following) SyncAlphaTags(entity);
        Changed();
        return true;
    }

    std::vector<std::string> Document::NodesFor(const std::string_view fieldType) const
    {
        std::vector<std::string> ids;
        for (const auto& node : Graph().nodes) {
            if (node.type == "Reference" || node.type == "Screen") continue;
            const auto resource = _runtime->ResourceOf(node.id);
            if (!resource || resource->screen) continue;
            const bool fits = fieldType == "resource" || resource->kind == fieldType || (fieldType == "Image" && resource->kind == "ImageView");
            if (fits) ids.push_back(node.id);
        }
        // And the meshes there are without a node for any of them.
        if (fieldType == "Mesh" || fieldType == "resource")
            for (const auto& name : kgraph::DefaultMeshNames()) ids.push_back(kgraph::DefaultMeshId(name));
        return ids;
    }

    // ---- entities as data ---------------------------------------------------------------------------------------

    namespace {
        /** A float as the number someone typed, not as the nearest double to what a float holds of it: 0.3, not 0.30000001192. */
        double Tidy(const double value)
        {
            if (!std::isfinite(value) || value == 0.0) return value;
            const double scale = std::pow(10.0, 6 - static_cast<int>(std::ceil(std::log10(std::abs(value)))));
            return std::round(value * scale) / scale;
        }
        Value Tidied(const Value& value)
        {
            if (const double* number = value.Number()) return Value(Tidy(*number));
            if (const auto* items = value.Items()) {
                Value::List out;
                for (const auto& item : *items) out.push_back(Tidied(item));
                return Value(std::move(out));
            }
            return value;
        }
        Value Three(const kecs::Vec3& v) { return Value(Value::List { Tidy(v.x), Tidy(v.y), Tidy(v.z) }); }
        bool Three(const Value* value, kecs::Vec3& out)
        {
            const auto* items = value ? value->Items() : nullptr;
            if (!items || items->size() != 3 || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; })) return false;
            out = { static_cast<float>(*(*items)[0].Number()), static_cast<float>(*(*items)[1].Number()), static_cast<float>(*(*items)[2].Number()) };
            return true;
        }
    }

    Value Document::WriteEntity(const kecs::Entity entity) const
    {
        Value::Members out;
        out.emplace_back("name", _world.NameOf(entity));
        out.emplace_back("position", Three(_world.PositionOf(entity)));
        out.emplace_back("rotation", Three(_world.RotationOf(entity)));
        out.emplace_back("scale", Three(_world.ScaleOf(entity)));
        Value::Members components;
        Value::List disabled;
        for (const std::string& component : ComponentsOf(entity)) {
            Value::Members fields;
            for (const auto& field : _world.Describe(_world.Find(component)).fields) {
                fields.emplace_back(field.name, Tidied(Field(entity, component, field.name)));
            }
            components.emplace_back(component, Value(std::move(fields)));
            if (!IsEnabled(entity, component)) disabled.emplace_back(component);
        }
        out.emplace_back("components", Value(std::move(components)));
        if (!disabled.empty()) out.emplace_back("disabled", Value(std::move(disabled)));
        Value::List children;
        for (const kecs::Entity child : _world.ChildrenOf(entity)) children.push_back(WriteEntity(child));
        out.emplace_back("children", Value(std::move(children)));
        return Value(std::move(out));
    }

    kecs::Entity Document::ReadEntity(const Value& data, const kecs::Entity parent, std::vector<std::string>* problems)
    {
        const auto problem = [&](std::string what) { if (problems) problems->push_back(std::move(what)); };
        const Value* name = data.Find("name");
        const kecs::Entity entity = CreateEntity(name && name->String() ? *name->String() : std::string("Entity"), parent);
        kecs::Vec3 three;
        if (Three(data.Find("position"), three)) _world.SetPosition(entity, three);
        if (Three(data.Find("rotation"), three)) _world.SetRotation(entity, three);
        if (Three(data.Find("scale"), three)) _world.SetScale(entity, three);
        if (const Value* components = data.Find("components"); components && components->Named())
            for (const auto& [component, fields] : *components->Named()) {
                if (!AddComponent(entity, component)) { problem(std::format("there is no component '{}': left out", component)); continue; }
                if (!fields.Named()) continue;
                for (const auto& [field, given] : *fields.Named()) {
                    if (given.IsNull()) continue;
                    std::string error;
                    if (!SetField(entity, component, field, given, &error)) problem(std::format("'{}.{}': {}", component, field, error));
                }
            }
        if (const Value* disabled = data.Find("disabled"); disabled && disabled->Items())
            for (const auto& component : *disabled->Items())
                if (component.String()) SetEnabled(entity, *component.String(), false);
        if (const Value* children = data.Find("children"); children && children->Items())
            for (const auto& child : *children->Items()) ReadEntity(child, entity, problems);
        Changed();
        return entity;
    }

    std::string Document::UniqueName(const std::string& name, const std::vector<std::string>& siblings)
    {
        if (std::ranges::find(siblings, name) == siblings.end()) return name;
        // "Cube 2" copied is "Cube 3", not "Cube 2 2".
        std::string base = name;
        if (const auto space = base.rfind(' '); space != std::string::npos && space + 1 < base.size()
            && std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space) + 1, base.end(), [](const unsigned char c) { return std::isdigit(c) != 0; }))
            base.erase(space);
        for (int n = 2;; ++n)
            if (std::string candidate = std::format("{} {}", base, n); std::ranges::find(siblings, candidate) == siblings.end()) return candidate;
    }

    kecs::Entity Document::Duplicate(const kecs::Entity entity)
    {
        if (!_world.Alive(entity)) return kecs::NullEntity;
        const kecs::Entity parent = _world.ParentOf(entity);
        const auto& siblings = parent.IsNull() ? _world.Roots() : _world.ChildrenOf(parent);
        std::vector<std::string> names;
        for (const kecs::Entity sibling : siblings) names.push_back(_world.NameOf(sibling));
        const auto place = static_cast<int>(std::ranges::find(siblings, entity) - siblings.begin()) + 1;
        const kecs::Entity copy = ReadEntity(WriteEntity(entity), parent);
        _world.Rename(copy, UniqueName(_world.NameOf(entity), names));
        _world.SetParent(copy, parent, place, false);
        // A copy of a prefab's instance is another instance of it, with what the first made its own. A copy of an entity
        // that is part of an instance is one put under it: its own, no part of the prefab.
        const kecs::Entity outer = _world.Has<PrefabLink>(entity) ? kecs::NullEntity : PrefabRootOf(entity);
        const std::function<void(kecs::Entity, kecs::Entity)> carry = [&](const kecs::Entity from, const kecs::Entity to) {
            const bool outers = !outer.IsNull() && PrefabRootOf(from) == outer;
            if (const PrefabLink* link = _world.Get<PrefabLink>(from)) _world.Add<PrefabLink>(to, *link);
            if (const PrefabPart* part = _world.Get<PrefabPart>(from); part && !outers) _world.Add<PrefabPart>(to, *part);
            const auto& a = _world.ChildrenOf(from);
            const auto& b = _world.ChildrenOf(to);
            for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) carry(a[i], b[i]);
        };
        carry(entity, copy);
        Changed();
        return copy;
    }

    std::string Document::WorldJson() const { return kecs::Save(_world, _codecs).json; }

    bool Document::RestoreWorld(const std::string_view json, std::string& error)
    {
        for (const kecs::Entity root : std::vector(_world.Roots())) _world.Destroy(root);
        const auto loaded = kecs::Load(_world, json, _codecs);
        Changed();
        if (!loaded.ok) { error = loaded.error; return false; }
        return true;
    }

    // ---- the file -----------------------------------------------------------------------------------------------

    std::string Document::ToJson() const
    {
        std::string tags;
        for (const auto& tag : _tags) tags += std::format("{}{}", tags.empty() ? "" : ", ", kecs::json::Quote(tag));
        // How the project's files are read is theirs, in their metas: the scene keeps only what is of a file it has no meta of.
        kgraph::Graph graph = Graph();
        if (_assets) {
            std::erase_if(graph.meshImports, [&](const auto& entry) { return _assets->FromLegacy("@" + entry.first).has_value(); });
            std::erase_if(graph.textureImports, [&](const auto& entry) { return _assets->FromLegacy(entry.first).has_value(); });
        }
        return std::format("{{\n\"koral-scene\": 1,\n\"tags\": [{}],\n\"graph\": {},\n\"world\": {}\n}}\n", tags, graph.ToJson(), kecs::Save(_world, _codecs).json);
    }

    bool Document::FromJson(const std::string_view json, std::string& error)
    {
        kecs::json::Value file;
        if (!kecs::json::Parse(json, file, error)) return false;
        const auto* version = file.Find("koral-scene");
        const auto* graphText = file.Find("graph");
        const auto* worldText = file.Find("world");
        if (!version || version->number != 1.0 || !graphText || !worldText) { error = "it is not a scene: 'koral-scene' 1, with a 'graph' and a 'world'"; return false; }

        kgraph::Graph graph;
        if (!kgraph::Graph::FromJson(graphText->raw, graph, error)) return false;
        ImportSettingsInto(graph);
        // The graph first: it makes the components its pipelines' entities have, and the resources they hold.
        WithFunctions(graph);
        _runtime->Set(std::move(graph));
        if (const auto* tags = file.Find("tags"))
            for (const auto& tag : tags->items) AddTag(tag.string);
        const auto loaded = kecs::Load(_world, worldText->raw, _codecs);
        // A scene saved before a name was the world's own had it as a component: the name is taken from it.
        for (const kecs::Entity entity : loaded.entities)
            if (const Label* label = _world.Get<Label>(entity)) {
                _world.Rename(entity, label->text);
                _world.Remove<Label>(entity);
            }
        if (!loaded.ok) { error = loaded.error; return false; }
        for (const auto& problem : loaded.problems) _notes.push_back(problem);
        MigrateMaterials(*worldText, loaded.entities);
        FollowGenerators();
        // Each model placed as its prefab's instance: its prefab made again once it is read, should the model have changed.
        if (_assets)
            for (const kecs::Entity entity : _world.All())
                if (const PrefabLink* link = _world.Get<PrefabLink>(entity)) {
                    const std::filesystem::path prefab(link->prefab);
                    const auto id = kasset::Id::Parse(prefab.stem().string());
                    const kasset::Entry* model = id && prefab.parent_path().filename() == "prefabs" ? _assets->Find(*id) : nullptr;
                    if (model && model->meta.kind == kasset::Kind::eModel) _modelPrefabs.try_emplace(_assets->Root() / model->file, nullptr);
                }
        // Each model whose parts it has: once each is read, its parts are given IDs by their names.
        if (_assets) {
            const auto model = [&](const std::string_view id) {
                const auto hash = id.rfind('#');
                if (!id.starts_with('@') || hash == std::string_view::npos) return;
                const std::filesystem::path file(std::string(id.substr(1, hash - 1)));
                _partsToRecord.insert(file.is_absolute() || _runtime->AssetDirectory().empty() ? file : _runtime->AssetDirectory() / file);
            };
            const kecs::ComponentId mesh = _world.Find("Mesh");
            for (const kecs::Entity entity : _world.All()) {
                if (mesh != kecs::NoComponent && _world.Has(entity, mesh)) {
                    const kgraph::Value held = Field(entity, "Mesh", "mesh");    // (kept: what String gives is of it)
                    if (const std::string* id = held.String()) model(*id);
                }
                for (const auto& line : LinkLines(entity, false))
                    if (const auto space = line.find(' '); space != std::string::npos) model(std::string_view(line).substr(space + 1));
            }
        }
        // Each prefab's instance as its prefab is now: what was saved of it, and what has changed in the prefab since.
        UpdatePrefabs();
        Changed();
        _saved = _revision;
        return true;
    }

    void Document::MigrateMaterials(const kecs::json::Value& world, const std::vector<kecs::Entity>& entities)
    {
        // A scene saved when a pipeline's component held what a thing is made of — its albedo, its pictures — and no
        // material did: each such entity is given a PbrMaterial of those values, of no asset. What was the pipeline's
        // besides is its Pipeline parameters still.
        const kecs::json::Value* saved = world.Find("entities");
        const kecs::ComponentId pbr = _world.Find("PbrMaterial");
        if (!saved || pbr == kecs::NoComponent) return;
        const auto hasKind = [&](const kecs::Entity entity) {
            return std::ranges::any_of(_runtime->MaterialKinds(), [&](const kgraph::MaterialKind& kind) { return _world.Has(entity, _world.Find(kind.name)); });
        };
        std::size_t migrated = 0;
        for (std::size_t i = 0; i < saved->items.size() && i < entities.size(); ++i) {
            const kecs::json::Value* components = saved->items[i].Find("components");
            if (!components || !_world.Alive(entities[i]) || hasKind(entities[i])) continue;
            for (const auto& [name, values] : components->members) {
                const kecs::json::Value* albedo = values.Find("albedo");
                if (!albedo || albedo->kind != kecs::json::Value::Kind::eArray || albedo->items.size() < 3 || _runtime->MaterialKindOf(name)) continue;
                const kecs::Entity entity = entities[i];
                _world.Add(entity, pbr);
                const auto number = [&](const char* field, const double otherwise) {
                    const kecs::json::Value* v = values.Find(field);
                    return v && v->kind == kecs::json::Value::Kind::eNumber ? v->number : otherwise;
                };
                const double transparency = number("transparency", 0.0), cutoff = number("alphaCutoff", 0.0);
                SetField(entity, "PbrMaterial", "baseColor", Value(Value::List { albedo->items[0].number, albedo->items[1].number, albedo->items[2].number, 1.0 - transparency }));
                SetField(entity, "PbrMaterial", "metallic", Value(number("metallic", 0.0)));
                SetField(entity, "PbrMaterial", "roughness", Value(number("roughness", 0.5)));
                if (const kecs::json::Value* emission = values.Find("emission"); emission && emission->items.size() >= 3)
                    SetField(entity, "PbrMaterial", "emission", Value(Value::List { emission->items[0].number, emission->items[1].number, emission->items[2].number }));
                SetField(entity, "PbrMaterial", "alphaMode", Value(transparency > 0.0 || _world.Has(entity, _world.Find("Blended")) ? 2 : cutoff > 0.0 ? 1 : 0));
                if (cutoff > 0.0) SetField(entity, "PbrMaterial", "alphaCutoff", Value(cutoff));
                for (const auto& [from, to] : { std::pair { "albedoMap", "baseColorMap" }, { "normalMap", "normalMap" }, { "roughnessMap", "metallicRoughnessMap" }, { "emissiveMap", "emissiveMap" } })
                    if (const kecs::json::Value* map = values.Find(from); map && map->kind == kecs::json::Value::Kind::eString && !map->string.empty())
                        SetField(entity, "PbrMaterial", to, Value(map->string));
                SyncAlphaTags(entity);
                FollowModelMaterial(entity);
                ++migrated;
                break;
            }
        }
        if (migrated > 0)
            _notes.push_back(std::format("{} {} what {} made of from {} pipeline's component: a PbrMaterial each now, following its model's where it has one", migrated,
                                         migrated == 1 ? "entity had" : "entities had", migrated == 1 ? "it is" : "each is", migrated == 1 ? "its" : "a"));
    }

    bool Document::Save(const std::filesystem::path& path, std::string& error)
    {
        // A function open in place is closed first, keeping what was done to it: its page is no part of the scene.
        if (_opened && !CloseFunction(true, &error)) return false;
        if (_runtime->AssetDirectory().empty()) SetAssetDirectory(std::filesystem::absolute(path).parent_path().parent_path() / "assets");
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { error = std::format("'{}' cannot be written", path.string()); return false; }
        StoreImportSettings();
        // Its files kept by their IDs: moved or renamed, they are still its.
        out << (_assets ? kasset::ToIds(ToJson(), *_assets) : ToJson());
        if (!out.flush()) { error = std::format("'{}' could not be written whole", path.string()); return false; }
        _saved = _revision;
        return true;
    }

    kecs::Entity Document::ImportModel(const std::filesystem::path& file, std::string* error)
    {
        std::error_code ec;
        const std::filesystem::path where = file.is_absolute() || _runtime->AssetDirectory().empty() ? file : _runtime->AssetDirectory() / file;
        if (!std::filesystem::is_regular_file(where, ec)) { if (error) *error = std::format("there is no file '{}'", where.generic_string()); return kecs::NullEntity; }
        // The file is read off the frame: the entity over what it holds is there now, and the rest when it is read.
        const kecs::Entity root = CreateEntity(file.stem().string());
        _importing.push_back({ file, root });
        (void)_runtime->ModelOf(file);
        return root;
    }

    namespace {
        bool WritePrefabFile(const std::filesystem::path& file, const kgraph::Value& root, std::string& error, kasset::Database* assets);
    }

    std::filesystem::path Document::ModelPrefabOf(const std::filesystem::path& model) const
    {
        if (!_assets) return {};
        const auto reference = _assets->Lookup(_runtime->AssetId(model));
        if (!reference || reference->part) return {};
        return kasset::CacheDirectoryOf(_assets->Root()) / "prefabs" / (reference->asset.Text() + ".prefab");
    }

    void Document::Imported(const std::filesystem::path& file, const kecs::Entity root, const kgraph::Model& read)
    {
        BuildModel(file, root, read);
        // What was put in the scene is an instance of the model's prefab: read again, the model makes it again, and this follows.
        const std::filesystem::path prefab = ModelPrefabOf(file);
        if (prefab.empty() || !_world.Alive(root)) return;
        std::string why;
        if (!SaveAsPrefab(root, prefab, &why)) { _notes.push_back(why); return; }
        _modelPrefabs[file] = &read;
    }

    void Document::RemakeModelPrefab(const std::filesystem::path& file, const kgraph::Model& model)
    {
        const std::filesystem::path prefab = ModelPrefabOf(file);
        if (prefab.empty()) return;
        const bool clean = !Dirty();
        // Made where nothing sees it, written, and gone again.
        const kecs::Entity scratch = CreateEntity(file.stem().string());
        BuildModel(file, scratch, model);
        _world.Add<PrefabLink>(scratch, PrefabLink { prefab.generic_string(), {}, -1 });
        const Value written = WritePrefab(scratch);
        DestroyEntity(scratch);
        std::ifstream in(prefab, std::ios::binary);
        const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::string why;
        if (!WritePrefabFile(prefab, written, why, _assets.get())) { _notes.push_back(why); return; }
        std::ifstream again(prefab, std::ios::binary);
        const std::string after((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
        // As it was: nothing of the scene is any other, and a scene just opened is still as it was saved.
        if (after == before) { if (clean) _saved = _revision; return; }
        UpdatePrefabs(prefab);
    }

    void Document::BuildModel(const std::filesystem::path& file, const kecs::Entity root, const kgraph::Model& read)
    {
        using kgraph::Value;
        const kgraph::Model* model = &read;
        if (!_world.Alive(root)) return;    // taken out again while its file was being read

        // What each drawn thing is made of: one of the file's materials, an asset its things follow — of the kind what
        // the file reports it as is imported as (ImportKind), filled from the report a field by name.
        AddTag("Masked");
        AddTag("Blended");
        MakeModelMaterials(file, read);
        const std::string named = _runtime->AssetId(file);
        std::map<std::string, std::string> extracted;
        if (_assets)
            if (const auto of = _assets->Lookup(named); of && !of->part)
                extracted = kasset::KindsFrom(kasset::ParameterOf(_assets->Find(of->asset)->meta, "extracted")).value_or(std::map<std::string, std::string> {});
        const auto vec = [](const float* v) { return Value(Value::List { v[0], v[1], v[2] }); };
        const auto drawn = [&](const kecs::Entity entity, const kgraph::ModelNode::Part& part) {
            AddComponent(entity, "Mesh");
            SetField(entity, "Mesh", "mesh", Value(part.mesh));
            if (part.material < 0 || part.material >= static_cast<int>(model->materials.size())) {
                // The file says nothing of what it is made of: something matte and pale, rather than nothing at all.
                if (!MaterialAssetOf("material:Default"))
                    AddMaterialAsset({ .id = "material:Default", .name = "Default", .kind = ImportKind(kgraph::ModelMaterial::Model::ePbr),
                                       .values = { { "baseColor", Value(Value::List { 0.8, 0.8, 0.8, 1.0 }) }, { "roughness", Value(0.8) } } });
                GiveMaterial(entity, "material:Default");
                return;
            }
            std::string why;
            std::string material = std::format("{}#material:{}", named, part.material);
            // One made the project's own (ExtractMaterial) is that.
            if (const auto at = extracted.find(std::to_string(part.material)); at != extracted.end())
                if (const auto reference = kasset::Reference::Parse(at->second))
                    if (std::string own = _assets->ToLegacy(*reference); !own.empty()) material = std::move(own);
            if (!GiveMaterial(entity, material, &why)) _notes.push_back(std::format("{}: {}", file.filename().string(), why));
        };

        std::vector<kecs::Entity> made(model->nodes.size(), root);
        for (std::size_t i = 0; i < model->nodes.size(); ++i) {
            const kgraph::ModelNode& node = model->nodes[i];
            const kecs::Entity parent = node.parent >= 0 ? made[static_cast<std::size_t>(node.parent)] : root;
            const kecs::Entity entity = made[i] = CreateEntity(node.name.empty() ? std::format("Node {}", i) : node.name, parent);
            SetPosition(entity, { node.position[0], node.position[1], node.position[2] });
            SetRotation(entity, { node.rotation[0], node.rotation[1], node.rotation[2] });
            SetScale(entity, { node.scale[0], node.scale[1], node.scale[2] });
            // One part is the thing itself; several are each a thing under it.
            if (node.parts.size() == 1) drawn(entity, node.parts.front());
            else for (std::size_t part = 0; part < node.parts.size(); ++part) drawn(CreateEntity(std::format("{} [{}]", NameOf(entity), part), entity), node.parts[part]);
        }
        // Its lights and cameras: each on the thing of the file's it is on, whose place and turn are all of it; one with a
        // place or a turn of its own besides, a thing of its own under the model, where it is in the file.
        const auto holder = [&](const int node, const bool atNode, const std::string& name, const float* position, const float* direction) {
            if (atNode && node >= 0 && static_cast<std::size_t>(node) < made.size()) return made[static_cast<std::size_t>(node)];
            const kecs::Entity entity = CreateEntity(name, root);
            SetPosition(entity, { position[0], position[1], position[2] });
            // Forward is -Z: turned about Y to face the way it looks, then tipped up or down.
            static constexpr float Degrees = 180.f / 3.14159265358979323846f;
            SetRotation(entity, { std::asin(std::clamp(direction[1], -1.f, 1.f)) * Degrees, std::atan2(-direction[0], -direction[2]) * Degrees, 0.f });
            return entity;
        };
        for (const auto& light : model->lights) {
            const kecs::Entity entity = holder(light.node, light.atNode, light.name.empty() ? std::string("Light") : light.name, light.position, light.direction);
            const char* component = light.kind == kgraph::ModelLight::Kind::eDirectional ? "DirectionalLight" : light.kind == kgraph::ModelLight::Kind::eSpot ? "SpotLight" : "PointLight";
            AddComponent(entity, component);
            SetField(entity, component, "color", vec(light.color));
            if (light.intensity > 0.f) SetField(entity, component, "intensity", Value(light.intensity));
            if (light.range > 0.f && light.kind != kgraph::ModelLight::Kind::eDirectional) SetField(entity, component, "range", Value(light.range));
            if (light.kind == kgraph::ModelLight::Kind::eSpot && light.outerAngle > 0.f) {
                SetField(entity, component, "innerAngle", Value(light.innerAngle));
                SetField(entity, component, "outerAngle", Value(light.outerAngle));
            }
        }
        for (const auto& camera : model->cameras) {
            const kecs::Entity entity = holder(camera.node, camera.atNode, camera.name.empty() ? std::string("Camera") : camera.name, camera.position, camera.direction);
            AddComponent(entity, "Camera");
            SetField(entity, "Camera", "orthographic", Value(camera.orthographic));
            if (camera.orthographic) SetField(entity, "Camera", "height", Value(camera.height));
            else SetField(entity, "Camera", "fieldOfView", Value(std::clamp(camera.fieldOfView, 1.f, 179.f)));
            if (camera.zNear > 0.f) SetField(entity, "Camera", "near", Value(camera.zNear));
            SetField(entity, "Camera", "far", Value(camera.zFar));     // 0: as far as there is
        }
        Changed();
    }

    // ---- functions ----------------------------------------------------------------------------------------------

    namespace {
        constexpr const char* FunctionEnding = ".kfunc";

        bool ReadFunction(const std::filesystem::path& file, kgraph::Graph& out, std::string& error, const kasset::Database* assets = nullptr)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) { error = std::format("'{}' cannot be read", file.generic_string()); return false; }
            std::stringstream read;
            read << in.rdbuf();
            // (kept: what the parse gives of it is views into it) Its files named by where they are now.
            const std::string text = assets ? kasset::FromIds(read.str(), *assets) : read.str();
            kecs::json::Value parsed;
            if (!kecs::json::Parse(text, parsed, error)) return false;
            const auto* version = parsed.Find("koral-function");
            const auto* graph = parsed.Find("graph");
            if (!version || version->number != 1.0 || !graph) { error = std::format("'{}' is not a function: 'koral-function' 1, with a 'graph'", file.generic_string()); return false; }
            return kgraph::Graph::FromJson(graph->raw, out, error);
        }

        /** What a function's file says it is for, besides its graph. Nothing, for one that says nothing or can't be read. */
        FunctionAbout ReadAbout(const std::filesystem::path& file)
        {
            FunctionAbout about;
            std::ifstream in(file, std::ios::binary);
            if (!in) return about;
            std::stringstream read;
            read << in.rdbuf();
            const std::string text = read.str();
            kecs::json::Value parsed;
            std::string error;
            if (!kecs::json::Parse(text, parsed, error)) return about;
            if (const auto* purpose = parsed.Find("purpose")) about.purpose = purpose->string;
            if (const auto* description = parsed.Find("description")) about.description = description->string;
            if (const auto* tags = parsed.Find("tags"))
                for (const auto& tag : tags->items) about.tags.push_back(tag.string);
            return about;
        }

        bool WriteFunction(const std::filesystem::path& file, const kgraph::Graph& graph, const FunctionAbout& about, std::string& error, kasset::Database* assets = nullptr)
        {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) { error = std::format("'{}' cannot be written", file.generic_string()); return false; }
            // What it is for goes before its graph, where someone reading the file finds it first.
            std::string said;
            if (!about.purpose.empty()) said += std::format("\"purpose\": {},\n", kecs::json::Quote(about.purpose));
            if (!about.description.empty()) said += std::format("\"description\": {},\n", kecs::json::Quote(about.description));
            if (!about.tags.empty()) {
                std::string tags;
                for (const auto& tag : about.tags) tags += (tags.empty() ? "" : ", ") + kecs::json::Quote(tag);
                said += std::format("\"tags\": [{}],\n", tags);
            }
            const std::string json = std::format("{{\n\"koral-function\": 1,\n{}\"graph\": {}\n}}\n", said, graph.ToJson());
            out << (assets ? kasset::ToIds(json, *assets) : json);
            if (!out.flush()) { error = std::format("'{}' could not be written whole", file.generic_string()); return false; }
            return true;
        }

        bool IsFunctionName(const std::string& name)
        {
            return !name.empty() && name.front() != ' ' && name.back() != ' '
                && std::ranges::all_of(name, [](const unsigned char c) { return std::isalnum(c) || c == ' ' || c == '_' || c == '-'; });
        }

        const std::string* TextOf(const kgraph::Node& node, const char* input)
        {
            const auto it = node.inputs.find(input);
            const std::string* text = it == node.inputs.end() || it->second.link ? nullptr : it->second.value.String();
            return text && !text->empty() ? text : nullptr;
        }

        /**
         * What of @p node names another node by its id, as text rather than a wire: a Reference's `node`, a Previous's and
         * an Element's `of` and `first`. Each is renamed by @p rename, as the nodes are when a function is opened, closed
         * or written out.
         */
        void RenameNamed(kgraph::Node& node, const std::function<std::optional<std::string>(const std::string&)>& rename)
        {
            std::vector<const char*> named;
            if (node.type == "Reference") named = { "node" };
            else if (node.type == "Previous" || node.type == "Element") named = { "of", "first" };
            for (const char* input : named)
                if (auto target = node.inputs.find(input); target != node.inputs.end() && !target->second.link && target->second.value.String() && !target->second.value.String()->empty())
                    if (auto renamed = rename(*target->second.value.String())) target->second.value = kgraph::Value(std::move(*renamed));
        }

        std::string Unique(const std::string& base, const std::function<bool(const std::string&)>& taken)
        {
            std::string name = base;
            for (int n = 2; taken(name); ++n) name = std::format("{} {}", base, n);
            return name;
        }

        /** What an edge is called for the port it went to or came from: its last part, as `param.instances` is `instances`. */
        std::string EdgeName(const std::string_view port)
        {
            std::string_view name = port;
            if (const auto dot = name.rfind('.'); dot != std::string_view::npos) name.remove_prefix(dot + 1);
            if (const auto open = name.find('['); open != std::string_view::npos) name = name.substr(0, open);
            return name.empty() ? std::string("value") : std::string(name);
        }

        bool IsEdgeType(const std::string_view type) { return type == "FunctionInput" || type == "FunctionOutput"; }

        std::string StandsForOf(const kgraph::Node& node)
        {
            const auto target = node.inputs.find("node");
            return node.type == "Reference" && target != node.inputs.end() && target->second.value.String() ? *target->second.value.String() : std::string();
        }
    }

    std::filesystem::path Document::FunctionsDirectory() const
    {
        const std::filesystem::path project = ProjectDirectory();
        return project.empty() ? std::filesystem::path() : project / "functions";
    }

    std::filesystem::path Document::EngineFunctionsDirectory() { return kscript::PackageDirectory() / "functions"; }

    std::vector<std::string> Document::Functions() const
    {
        std::set<std::string> names;
        std::error_code ignored;
        for (const auto& folder : { FunctionsDirectory(), EngineFunctionsDirectory() }) {
            if (folder.empty()) continue;
            for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
                if (entry.path().extension() == FunctionEnding) names.insert(entry.path().stem().string());
        }
        return { names.begin(), names.end() };
    }

    bool Document::IsEngineFunction(const std::string_view name) const
    {
        std::error_code ignored;
        const std::string file = std::string(name) + FunctionEnding;
        const std::filesystem::path project = FunctionsDirectory();
        if (!project.empty() && std::filesystem::exists(project / file, ignored)) return false;
        return std::filesystem::exists(EngineFunctionsDirectory() / file, ignored);
    }

    FunctionAbout Document::AboutFunction(const std::string_view name) const
    {
        const std::string file = std::string(name) + FunctionEnding;
        return ReadAbout(IsEngineFunction(name) ? EngineFunctionsDirectory() / file : FunctionsDirectory() / file);
    }

    std::vector<kgraph::Preset> Document::Presets() const
    {
        std::vector<kgraph::Preset> presets = _runtime->Presets();
        for (const auto& name : Functions()) {
            kgraph::Preset call;
            call.name = kgraph::FindType(name) ? std::format("{} (function)", name) : name;
            call.type = "Function";
            const FunctionAbout about = AboutFunction(name);
            const std::string what = about.description.empty() ? std::string() : about.description + " ";
            call.doc = IsEngineFunction(name) ? std::format("{}A call of the engine's function {}: its nodes, as one. Pressed twice, it is opened; changed, it is saved as the project's own.", what, name)
                                              : std::format("{}A call of the project's function {}: its nodes, as one. Pressed twice, it is opened.", what, name);
            call.group = about.purpose;
            for (const auto& tag : about.tags) call.keywords += (call.keywords.empty() ? "" : " ") + tag;
            call.inputs = { { "function", kgraph::Value(name) } };
            presets.push_back(std::move(call));
        }
        return presets;
    }

    void Document::WithFunctions(kgraph::Graph& graph)
    {
        graph.functions.clear();
        const std::filesystem::path folder = FunctionsDirectory();
        std::vector<const kgraph::Graph*> look { &graph };
        while (!look.empty()) {
            const kgraph::Graph* of = look.back();
            look.pop_back();
            for (const auto& node : of->nodes) {
                if (node.type != "Function") continue;
                const std::string* name = TextOf(node, "function");
                if (!name || graph.functions.contains(*name)) continue;
                // The project's, or — when it has none so called — the engine's.
                const std::filesystem::path file = (IsEngineFunction(*name) ? EngineFunctionsDirectory() : folder) / (*name + FunctionEnding);
                std::error_code ec;
                const auto written = std::filesystem::last_write_time(file, ec);
                if (ec) continue;   // the call says there is no such function, on itself
                auto& cached = _functionFiles[*name];
                if (!cached.second || cached.first != written) {
                    kgraph::Graph read;
                    std::string error;
                    if (!ReadFunction(file, read, error, _assets.get())) { _notes.push_back(error); continue; }
                    cached = { written, std::make_shared<const kgraph::Graph>(std::move(read)) };
                }
                graph.functions[*name] = cached.second;
                look.push_back(cached.second.get());
            }
        }
    }

    void Document::ReloadFunctions()
    {
        _functionFiles.clear();
        Apply(kgraph::Graph(Graph()));
    }

    std::string Document::MakeFunction(const std::vector<std::string>& ids, const std::string& name, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return std::string(); };
        if (!IsFunctionName(name)) return fail("a function's name is letters, digits, spaces, '_' and '-'");
        const std::filesystem::path folder = FunctionsDirectory();
        if (folder.empty()) return fail("a function is kept in a project, and this scene is in none");
        const std::filesystem::path file = folder / (name + FunctionEnding);
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) return fail(std::format("there is a function called '{}' already", name));
        kgraph::Graph graph = Graph();
        // What it holds: the nodes picked — not a Reference, which is what another pass made, nor another's edge.
        std::set<std::string> chosen;
        for (const auto& id : ids)
            if (const kgraph::Node* node = graph.Find(id); node && node->type != "Reference" && !IsEdgeType(node->type)) chosen.insert(node->id);
        if (chosen.empty()) return fail("none of those can be a function's: pick the nodes it is to hold");
        const auto origin = [&](std::string id) {
            for (int hops = 0; hops < 16; ++hops) {
                const kgraph::Node* node = graph.Find(id);
                const std::string target = node ? StandsForOf(*node) : std::string();
                if (target.empty() || !graph.Find(target)) return id;
                id = target;
            }
            return id;
        };
        const auto kindOf = [&](const std::string& source, const std::string& output) -> std::string {
            for (const auto& port : _runtime->OutputsOf(origin(source)))
                if (port.name == output) {
                    if (port.kind == kgraph::PortKind::eResource)
                        return port.type == "Mesh" ? "eMesh" : port.type == "Buffer" || port.type == "BufferView" ? "eBuffer" : port.type == "Image" || port.type == "ImageView" ? "eImage" : "eResource";
                    return port.type == "entities" ? "eEntities" : "eValue";
                }
            return "eValue";
        };
        double left = 1e300, right = -1e300, top = 1e300, cx = 0, cy = 0;
        std::map<std::string, int> passes;
        for (const auto& node : graph.nodes)
            if (chosen.contains(node.id)) {
                left = std::min(left, node.x); right = std::max(right, node.x); top = std::min(top, node.y);
                cx += node.x; cy += node.y;
                ++passes[node.pass];
            }
        cx /= static_cast<double>(chosen.size());
        cy /= static_cast<double>(chosen.size());

        kgraph::Graph function;
        std::set<std::string> taken(chosen.begin(), chosen.end());
        // Its inputs: what its nodes take of the rest, an input for each output of something there.
        std::map<std::pair<std::string, std::string>, std::string> inputOf;     // what is taken, the edge it is taken through
        std::map<std::string, std::string> inputPort;                           // an edge, the call's port it is
        std::set<std::string> inputNames;
        for (const auto& node : graph.nodes) {
            if (!chosen.contains(node.id)) continue;
            kgraph::Node copy = node;
            copy.pass.clear();
            for (auto& [port, input] : copy.inputs) {
                if (!input.link || chosen.contains(input.link->node)) continue;
                const auto key = std::pair { input.link->node, input.link->output };
                auto it = inputOf.find(key);
                if (it == inputOf.end()) {
                    const std::string portName = Unique(EdgeName(port), [&](const std::string& n) { return inputNames.contains(n); });
                    inputNames.insert(portName);
                    const std::string edge = Unique("in " + portName, [&](const std::string& n) { return taken.contains(n); });
                    taken.insert(edge);
                    kgraph::Node& made = function.Add(edge, "FunctionInput");
                    made.Set("name", portName).Set("kind", kindOf(key.first, key.second));
                    made.x = left - 320.0;
                    made.y = top + 90.0 * static_cast<double>(inputOf.size());
                    it = inputOf.emplace(key, edge).first;
                    inputPort[edge] = portName;
                }
                input.link = kgraph::Link { it->second, "out" };
            }
            function.nodes.push_back(std::move(copy));
        }
        // Its outputs: what the rest takes of its nodes — straight, or through a Reference to one.
        std::map<std::pair<std::string, std::string>, std::string> outputOf;    // what is given, the call's port it is given at
        std::set<std::string> outputNames;
        for (const auto& node : graph.nodes) {
            if (chosen.contains(node.id)) continue;
            for (const auto& input : node.inputs | std::views::values) {
                if (!input.link) continue;
                const std::string source = origin(input.link->node);
                if (!chosen.contains(source)) continue;
                const auto key = std::pair { source, input.link->output };
                if (outputOf.contains(key)) continue;
                const std::string portName = Unique(EdgeName(key.second == "out" ? source : key.second), [&](const std::string& n) { return outputNames.contains(n); });
                outputNames.insert(portName);
                const std::string edge = Unique("out " + portName, [&](const std::string& n) { return taken.contains(n); });
                taken.insert(edge);
                kgraph::Node& made = function.Add(edge, "FunctionOutput");
                made.Set("name", portName).Set("kind", kindOf(key.first, key.second)).Wire("value", key.first, key.second);
                made.x = right + 320.0;
                made.y = top + 90.0 * static_cast<double>(outputOf.size());
                outputOf.emplace(key, portName);
            }
        }

        // The call, where they were: in the pass most of them were in, wired as they were.
        kgraph::Node call;
        call.id = Unique(name, [&](const std::string& n) { return graph.Find(n) && !chosen.contains(n); });
        call.type = "Function";
        call.pass = std::ranges::max_element(passes, {}, [](const auto& entry) { return entry.second; })->first;
        call.x = std::round(cx);
        call.y = std::round(cy);
        call.Set("function", name);
        for (const auto& [taken, edge] : inputOf) call.inputs[inputPort[edge]] = kgraph::Input { {}, kgraph::Link { taken.first, taken.second } };
        for (auto& node : graph.nodes) {
            if (chosen.contains(node.id)) continue;
            for (auto& input : node.inputs | std::views::values)
                if (input.link)
                    if (const auto at = outputOf.find({ origin(input.link->node), input.link->output }); at != outputOf.end()) input.link = kgraph::Link { call.id, at->second };
        }
        // What they were, and the References that brought them in: gone, the call in their place.
        std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return chosen.contains(node.id) || chosen.contains(StandsForOf(node)); });
        graph.nodes.push_back(std::move(call));
        const std::string made = graph.nodes.back().id;
        std::string why;
        if (!WriteFunction(file, function, {}, why, _assets.get())) return fail(why);
        _functionFiles.erase(name);
        WireAcrossPasses(graph, { made });
        Apply(std::move(graph));
        return made;
    }

    std::string Document::OpenFunction(const std::string_view call, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return std::string(); };
        if (_opened) return fail(std::format("'{}' is open already: close it first", _opened->function));
        kgraph::Graph graph = Graph();
        const kgraph::Node* found = graph.Find(call);
        if (!found || found->type != "Function") return fail(std::format("'{}' is no call of a function", call));
        // A copy: the function's nodes are added to the graph below, which moves the call.
        const kgraph::Node at = *found;
        const kgraph::Node* node = &at;
        const std::string* name = TextOf(*node, "function");
        const auto function = name ? graph.functions.find(*name) : graph.functions.end();
        if (function == graph.functions.end() || !function->second) return fail(name ? std::format("there is no function '{}' among the project's functions", *name) : std::string("it calls no function yet"));
        _beforeOpened = graph;
        OpenedFunction opened { *node, *name, {}, std::string(call) + "/" };
        // Its page: a pass of its own, after the call's.
        opened.pass = Unique(std::format("{} (function)", *name), [&](const std::string& p) { return std::ranges::find(graph.passes, p) != graph.passes.end(); });
        const auto after = std::ranges::find(graph.passes, node->pass);
        graph.passes.insert(after == graph.passes.end() ? graph.passes.end() : after + 1, opened.pass);
        // Run as often as the call is.
        if (graph.IsSetup(node->pass)) graph.setup.push_back(opened.pass);
        else if (graph.IsFixed(node->pass)) graph.fixed.push_back(opened.pass);
        RatesInOrder(graph);
        std::map<std::string, std::string> outputEdge;      // a port of the call, the FunctionOutput it is
        for (const auto& inner : function->second->nodes) {
            kgraph::Node copy = inner;
            copy.id = opened.prefix + inner.id;
            copy.pass = opened.pass;
            for (auto& input : copy.inputs | std::views::values)
                if (input.link) input.link->node = opened.prefix + input.link->node;
            RenameNamed(copy, [&](const std::string& id) { return std::optional(opened.prefix + id); });
            const std::string port = TextOf(inner, "name") ? *TextOf(inner, "name") : inner.id;
            // An input is given what the call was given there; an output is what was wired from the call.
            if (inner.type == "FunctionInput")
                if (const auto given = node->inputs.find(port); given != node->inputs.end()) copy.inputs["value"] = given->second;
            if (inner.type == "FunctionOutput") outputEdge[port] = copy.id;
            graph.nodes.push_back(std::move(copy));
        }
        for (auto& other : graph.nodes)
            for (auto& input : other.inputs | std::views::values)
                if (input.link && input.link->node == call)
                    if (const auto edge = outputEdge.find(input.link->output); edge != outputEdge.end()) input.link = kgraph::Link { edge->second, "out" };
        std::erase_if(graph.nodes, [&](const kgraph::Node& n) { return n.id == call; });
        _opened = std::move(opened);
        Apply(std::move(graph));
        return _opened->pass;
    }

    bool Document::CloseFunction(const bool keep, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        if (!_opened) return fail("no function is open");
        if (!keep) {
            kgraph::Graph before = std::move(_beforeOpened);
            _opened.reset();
            Apply(std::move(before));
            return true;
        }
        const OpenedFunction opened = std::move(*_opened);
        _opened.reset();
        kgraph::Graph graph = Graph();
        std::set<std::string> page;
        for (const auto& node : graph.nodes) if (node.pass == opened.pass) page.insert(node.id);
        // What a node of the page is called in the function: without the call's name before it, where that names none other.
        std::set<std::string> names;
        std::map<std::string, std::string> inFunction;
        for (const auto& id : page) {
            std::string stripped = id.starts_with(opened.prefix) ? id.substr(opened.prefix.size()) : id;
            if (names.contains(stripped)) stripped = id;
            names.insert(stripped);
            inFunction[id] = stripped;
        }
        kgraph::Graph function;
        kgraph::Node call = opened.call;
        for (auto it = call.inputs.begin(); it != call.inputs.end();) it = it->first == "function" ? std::next(it) : call.inputs.erase(it);
        std::set<std::string> ports;
        double left = 1e300, right = -1e300;
        for (const auto& node : graph.nodes) if (page.contains(node.id)) { left = std::min(left, node.x); right = std::max(right, node.x); }
        const auto edgeFor = [&](const char* type, const std::string& port, const std::string& kind, const double x) -> kgraph::Node& {
            const std::string id = Unique(std::format("{} {}", type == std::string("FunctionInput") ? "in" : "out", port), [&](const std::string& n) { return names.contains(n); });
            names.insert(id);
            kgraph::Node& made = function.Add(id, type);
            made.Set("name", port).Set("kind", kind);
            made.x = x;
            made.y = 90.0 * static_cast<double>(function.nodes.size());
            return made;
        };
        for (const auto& node : graph.nodes) {
            if (!page.contains(node.id)) continue;
            kgraph::Node copy = node;
            copy.id = inFunction[node.id];
            copy.pass.clear();
            if (copy.type == "FunctionInput") {
                // What it was given is what the call is given there.
                const std::string port = TextOf(copy, "name") ? *TextOf(copy, "name") : copy.id;
                ports.insert(port);
                if (const auto given = copy.inputs.find("value"); given != copy.inputs.end()) {
                    if (given->second.link && !page.contains(given->second.link->node)) call.inputs[port] = given->second;
                    else if (!given->second.link) call.inputs[port] = given->second;
                    copy.inputs.erase(given);
                }
            }
            for (auto& [port, input] : copy.inputs) {
                if (!input.link) continue;
                if (page.contains(input.link->node)) { input.link->node = inFunction[input.link->node]; continue; }
                // Wired straight from outside the page: an input more, which the call is given.
                const std::string name = Unique(EdgeName(port), [&](const std::string& n) { return ports.contains(n); });
                ports.insert(name);
                std::string kind = "eValue";
                for (const auto& out : _runtime->OutputsOf(input.link->node))
                    if (out.name == input.link->output) kind = out.kind == kgraph::PortKind::eResource ? "eResource" : out.type == "entities" ? "eEntities" : "eValue";
                const std::string id = edgeFor("FunctionInput", name, kind, left - 320.0).id;
                call.inputs[name] = input;
                input.link = kgraph::Link { id, "out" };
            }
            RenameNamed(copy, [&](const std::string& id) { return page.contains(id) ? std::optional(inFunction[id]) : std::nullopt; });
            function.nodes.push_back(std::move(copy));
        }
        // What the rest takes of the page is what the call gives: at an output, or — taken straight — at one made for it.
        std::map<std::pair<std::string, std::string>, std::string> made;
        for (auto& node : graph.nodes) {
            if (page.contains(node.id)) continue;
            for (auto& [port, input] : node.inputs) {
                if (!input.link || !page.contains(input.link->node)) continue;
                const kgraph::Node* from = graph.Find(input.link->node);
                if (from && from->type == "FunctionOutput") {
                    input.link = kgraph::Link { call.id, TextOf(*from, "name") ? *TextOf(*from, "name") : inFunction[from->id] };
                    continue;
                }
                const auto key = std::pair { input.link->node, input.link->output };
                auto at = made.find(key);
                if (at == made.end()) {
                    const std::string name = Unique(EdgeName(key.second == "out" ? inFunction[key.first] : key.second), [&](const std::string& n) { return ports.contains(n); });
                    ports.insert(name);
                    edgeFor("FunctionOutput", name, "eValue", right + 320.0).Wire("value", inFunction[key.first], key.second);
                    at = made.emplace(key, name).first;
                }
                input.link = kgraph::Link { call.id, at->second };
            }
        }
        std::erase_if(graph.nodes, [&](const kgraph::Node& node) { return page.contains(node.id); });
        std::erase(graph.passes, opened.pass);
        graph.nodes.push_back(call);
        std::string why;
        // Into the project, always: one of the engine's changed is the project's own from then on.
        if (FunctionsDirectory().empty()) return fail("a function is kept in a project, and this scene is in none");
        // What it says it is for goes with it: the engine's, for one of the engine's saved as the project's own.
        if (!WriteFunction(FunctionsDirectory() / (opened.function + FunctionEnding), function, AboutFunction(opened.function), why, _assets.get())) return fail(why);
        _functionFiles.erase(opened.function);
        WireAcrossPasses(graph, { call.id });
        Apply(std::move(graph));
        return true;
    }

    bool Document::WriteOut(const std::string_view call, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        kgraph::Graph graph = Graph();
        const kgraph::Node* node = graph.Find(call);
        if (!node || node->type != "Function") return fail(std::format("'{}' is no call of a function", call));
        const std::string* name = TextOf(*node, "function");
        const auto found = name ? graph.functions.find(*name) : graph.functions.end();
        if (found == graph.functions.end() || !found->second) return fail("there is no such function among the project's functions");
        const kgraph::Node at = *node;
        const std::string prefix = std::string(call) + "/";
        // What each edge stands for here: an input, what the call is given; an output, what it is wired from inside.
        std::map<std::string, kgraph::Input> stands;
        double left = 1e300, top = 1e300;
        for (const auto& inner : found->second->nodes) {
            if (!IsEdgeType(inner.type)) { left = std::min(left, inner.x); top = std::min(top, inner.y); }
            const std::string port = TextOf(inner, "name") ? *TextOf(inner, "name") : inner.id;
            if (inner.type == "FunctionInput") {
                const auto given = at.inputs.find(port);
                if (given != at.inputs.end()) stands[inner.id] = given->second;
                else if (const auto initial = inner.inputs.find("default"); initial != inner.inputs.end()) stands[inner.id] = kgraph::Input { initial->second.value, std::nullopt };
            }
        }
        std::set<std::string> written;
        for (const auto& inner : found->second->nodes) {
            if (IsEdgeType(inner.type)) continue;
            kgraph::Node copy = inner;
            copy.id = prefix + inner.id;
            copy.pass = at.pass;
            copy.x = at.x + (inner.x - left);
            copy.y = at.y + (inner.y - top);
            for (auto it = copy.inputs.begin(); it != copy.inputs.end();) {
                if (it->second.link) {
                    if (const auto edge = stands.find(it->second.link->node); edge != stands.end()) it->second = edge->second;
                    else if (const kgraph::Node* to = found->second->Find(it->second.link->node); to && IsEdgeType(to->type)) { it = copy.inputs.erase(it); continue; }
                    else it->second.link->node = prefix + it->second.link->node;
                }
                ++it;
            }
            RenameNamed(copy, [&](const std::string& id) { return std::optional(prefix + id); });
            written.insert(copy.id);
            graph.nodes.push_back(std::move(copy));
        }
        // What was wired from the call is wired from what its output was wired from inside.
        for (auto& other : graph.nodes)
            for (auto it = other.inputs.begin(); it != other.inputs.end();) {
                if (!it->second.link || it->second.link->node != call) { ++it; continue; }
                const kgraph::Node* edge = nullptr;
                for (const auto& inner : found->second->nodes)
                    if (inner.type == "FunctionOutput" && (TextOf(inner, "name") ? *TextOf(inner, "name") : inner.id) == it->second.link->output) edge = &inner;
                const auto value = edge ? edge->inputs.find("value") : decltype(edge->inputs.end()) {};
                if (!edge || value == edge->inputs.end() || !value->second.link) { it = other.inputs.erase(it); continue; }
                if (const auto through = stands.find(value->second.link->node); through != stands.end()) it->second = through->second;
                else it->second.link = kgraph::Link { prefix + value->second.link->node, value->second.link->output };
                ++it;
            }
        std::erase_if(graph.nodes, [&](const kgraph::Node& n) { return n.id == call; });
        WireAcrossPasses(graph, written);
        Apply(std::move(graph));
        return true;
    }

    // ---- prefabs ------------------------------------------------------------------------------------------------

    namespace {
        /** An instance's overrides, from the text its PrefabLink keeps them in: a key a line. */
        std::set<std::string> OverrideSet(const std::string& text)
        {
            std::set<std::string> keys;
            std::size_t at = 0;
            while (at < text.size()) {
                const std::size_t end = std::min(text.find('\n', at), text.size());
                if (end > at) keys.insert(text.substr(at, end - at));
                at = end + 1;
            }
            return keys;
        }
        std::string OverrideText(const std::set<std::string>& keys)
        {
            std::string text;
            for (const auto& key : keys) text += key + '\n';
            return text;
        }

        /** What a prefab's file holds: the entity over the rest, and all of it, each with the part it is. */
        bool ReadPrefab(const std::filesystem::path& file, Value& root, std::string& error, const kasset::Database* assets = nullptr)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) { error = std::format("'{}' cannot be read", file.generic_string()); return false; }
            std::stringstream read;
            read << in.rdbuf();
            // (kept: what the parse gives of it is views into it) Its files named by where they are now.
            const std::string text = assets ? kasset::FromIds(read.str(), *assets) : read.str();
            kecs::json::Value parsed;
            if (!kecs::json::Parse(text, parsed, error)) return false;
            const auto* version = parsed.Find("koral-prefab");
            const auto* top = parsed.Find("root");
            if (!version || version->number != 1.0 || !top) { error = std::format("'{}' is not a prefab: 'koral-prefab' 1, with a 'root'", file.generic_string()); return false; }
            root = Value::FromJson(*top);
            return true;
        }

        int PartOf(const Value& node)
        {
            const Value* part = node.Find("part");
            return part && part->Number() ? static_cast<int>(*part->Number()) : -1;
        }
    }

    std::filesystem::path Document::ProjectDirectory() const
    {
        const std::filesystem::path& assets = _runtime->AssetDirectory();
        return assets.empty() ? std::filesystem::path() : assets.parent_path();
    }

    kecs::Entity Document::PrefabRootOf(kecs::Entity entity) const
    {
        for (; _world.Alive(entity); entity = _world.ParentOf(entity))
            if (_world.Has<PrefabLink>(entity)) return entity;
        return kecs::NullEntity;
    }

    std::string Document::PrefabOf(const kecs::Entity root) const
    {
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        return link ? link->prefab : std::string();
    }

    std::vector<std::string> Document::OverridesOf(const kecs::Entity root) const
    {
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        if (!link) return {};
        const auto keys = OverrideSet(link->overrides);
        return { keys.begin(), keys.end() };
    }

    void Document::Keep(const kecs::Entity root, const int part, const std::string_view what)
    {
        PrefabLink& link = *_world.Mutable<PrefabLink>(root);
        auto keys = OverrideSet(link.overrides);
        // A component given back is no longer taken off, and the other way round.
        if (what.starts_with('+')) keys.erase(std::format("{}:-{}", part, what.substr(1)));
        if (what.starts_with('-')) keys.erase(std::format("{}:+{}", part, what.substr(1)));
        keys.insert(std::format("{}:{}", part, what));
        link.overrides = OverrideText(keys);
    }

    void Document::Override(const kecs::Entity entity, const std::string_view what)
    {
        if (_rebuilding > 0 || !_world.Alive(entity)) return;
        const kecs::Entity root = PrefabRootOf(entity);
        if (root.IsNull()) return;
        const PrefabLink& link = *_world.Get<PrefabLink>(root);
        const bool own = what == "position" || what == "rotation" || what == "scale" || what == "name" || what == "gone";
        // An instance another prefab placed: what is done to it is, to the instance it is in, a change of its own.
        if (link.outer >= 0)
            if (const kecs::Entity outer = PrefabRootOf(_world.ParentOf(root)); !outer.IsNull())
                Keep(outer, link.outer, entity == root && own ? what : std::string_view("nested"));
        // Where the instance is, what it is called and what it is under are its own: no override of anything.
        if (entity == root && own) return;
        if (const PrefabPart* part = _world.Get<PrefabPart>(entity)) Keep(root, part->part, what);
    }

    std::map<int, kecs::Entity> Document::PartsOf(const kecs::Entity root) const
    {
        std::map<int, kecs::Entity> parts;
        const std::function<void(kecs::Entity)> find = [&](const kecs::Entity entity) {
            if (entity != root)
                if (const PrefabLink* link = _world.Get<PrefabLink>(entity)) {
                    if (link->outer >= 0 && !parts.contains(link->outer)) parts[link->outer] = entity;
                    return;
                }
            if (const PrefabPart* part = _world.Get<PrefabPart>(entity); part && !parts.contains(part->part)) parts[part->part] = entity;
            for (const kecs::Entity child : _world.ChildrenOf(entity)) find(child);
        };
        find(root);
        return parts;
    }

    Value Document::WritePrefab(const kecs::Entity root)
    {
        // Which part each of the instance's own entities is — the one it was, or, for one put under it since, a new one
        // — and each instance of another prefab under it, which is one entity of this one.
        std::map<kecs::Entity, int> parts;
        std::set<int> taken;
        int next = 0;
        std::vector<kecs::Entity> fresh;
        const std::function<void(kecs::Entity)> number = [&](const kecs::Entity entity) {
            if (entity != root)
                if (const PrefabLink* link = _world.Get<PrefabLink>(entity)) {
                    if (link->outer >= 0 && taken.insert(link->outer).second) { parts[entity] = link->outer; next = std::max(next, link->outer + 1); }
                    else fresh.push_back(entity);
                    return;
                }
            const PrefabPart* part = _world.Get<PrefabPart>(entity);
            if (part && taken.insert(part->part).second) { parts[entity] = part->part; next = std::max(next, part->part + 1); }
            else fresh.push_back(entity);
            for (const kecs::Entity child : _world.ChildrenOf(entity)) number(child);
        };
        number(root);
        for (const kecs::Entity entity : fresh) {
            parts[entity] = next++;
            if (PrefabLink* link = entity != root ? _world.Mutable<PrefabLink>(entity) : nullptr) link->outer = parts[entity];
            else if (_world.Has<PrefabPart>(entity)) _world.Mutable<PrefabPart>(entity)->part = parts[entity];
            else _world.Add<PrefabPart>(entity, PrefabPart { parts[entity] });
        }
        const auto transform = [&](Value::Members& out, const kecs::Entity entity) {
            out.emplace_back("part", parts.at(entity));
            out.emplace_back("name", _world.NameOf(entity));
            out.emplace_back("position", Three(_world.PositionOf(entity)));
            out.emplace_back("rotation", Three(_world.RotationOf(entity)));
            out.emplace_back("scale", Three(_world.ScaleOf(entity)));
        };
        const std::function<Value(kecs::Entity)> write = [&](const kecs::Entity entity) {
            Value::Members out;
            transform(out, entity);
            // Another prefab's instance: which prefab, and the values of what it made its own — not its entities.
            if (entity != root)
                if (const PrefabLink* link = _world.Get<PrefabLink>(entity)) {
                    out.emplace_back("prefab", link->prefab);
                    const auto inner = PartsOf(entity);
                    Value::Members kept;
                    for (const auto& key : OverrideSet(link->overrides)) {
                        const auto colon = key.find(':');
                        const int part = std::atoi(key.substr(0, colon).c_str());
                        const std::string what = key.substr(colon + 1);
                        const auto at = inner.find(part);
                        Value value;
                        if (at != inner.end() && _world.Alive(at->second)) {
                            const kecs::Entity of = at->second;
                            if (what == "position") value = Three(_world.PositionOf(of));
                            else if (what == "rotation") value = Three(_world.RotationOf(of));
                            else if (what == "scale") value = Three(_world.ScaleOf(of));
                            else if (what == "name") value = _world.NameOf(of);
                            else if (what.starts_with('~')) value = IsEnabled(of, what.substr(1));
                            else if (const auto dot = what.find('.'); dot != std::string::npos && !what.starts_with('+') && !what.starts_with('-')) {
                                value = Tidied(Field(of, what.substr(0, dot), what.substr(dot + 1)));
                                if (EntityOfValue(value) && !value.IsNull()) value = {};     // an entity is not kept by number
                            }
                        }
                        kept.emplace_back(key, std::move(value));
                    }
                    out.emplace_back("overrides", Value(std::move(kept)));
                    return Value(std::move(out));
                }
            Value::Members components;
            Value::List disabled;
            for (const std::string& component : ComponentsOf(entity)) {
                Value::Members fields;
                for (const auto& field : _world.Describe(_world.Find(component)).fields) {
                    Value value = Tidied(Field(entity, component, field.name));
                    // Another of the prefab's entities, by which part it is; one outside it, none.
                    if (field.type == "entity") {
                        const auto other = EntityOfValue(value);
                        const auto it = other ? parts.find(*other) : parts.end();
                        value = it == parts.end() ? Value {} : Value(Value::Members { { "part", it->second } });
                    }
                    fields.emplace_back(field.name, std::move(value));
                }
                components.emplace_back(component, Value(std::move(fields)));
                if (!IsEnabled(entity, component)) disabled.emplace_back(component);
            }
            out.emplace_back("components", Value(std::move(components)));
            if (!disabled.empty()) out.emplace_back("disabled", Value(std::move(disabled)));
            Value::List children;
            for (const kecs::Entity child : _world.ChildrenOf(entity))
                if (parts.contains(child)) children.push_back(write(child));
            out.emplace_back("children", Value(std::move(children)));
            return Value(std::move(out));
        };
        return write(root);
    }

    namespace {
        bool WritePrefabFile(const std::filesystem::path& file, const Value& root, std::string& error, kasset::Database* assets = nullptr)
        {
            std::error_code ec;
            if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) { error = std::format("'{}' cannot be written", file.generic_string()); return false; }
            const std::string json = std::format("{{\n\"koral-prefab\": 1,\n\"root\": {}\n}}\n", root.ToJson());
            out << (assets ? kasset::ToIds(json, *assets) : json);
            if (!out.flush()) { error = std::format("'{}' could not be written whole", file.generic_string()); return false; }
            return true;
        }
    }

    bool Document::SaveAsPrefab(const kecs::Entity entity, const std::filesystem::path& file, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        if (!_world.Alive(entity)) return fail("there is no such entity");
        if (const kecs::Entity outer = PrefabRootOf(entity); !outer.IsNull() && outer != entity)
            return fail(std::format("{} is part of an instance of {}: Unlink that first, or make the prefab of it all", _world.NameOf(entity), PrefabOf(outer)));
        // What it was an instance of, it is not any more: it is the first of this one.
        if (_world.Has<PrefabLink>(entity)) Unlink(entity);
        const std::filesystem::path project = ProjectDirectory();
        std::error_code ec;
        const auto relative = project.empty() ? std::filesystem::path() : std::filesystem::relative(std::filesystem::absolute(file), project, ec);
        const std::string named = !ec && !relative.empty() && *relative.begin() != ".." ? relative.generic_string() : std::filesystem::absolute(file).generic_string();
        _world.Add<PrefabLink>(entity, PrefabLink { named, {}, -1 });
        const Value written = WritePrefab(entity);
        std::string why;
        if (!WritePrefabFile(file, written, why, _assets.get())) { Unlink(entity); return fail(why); }
        Changed();
        return true;
    }

    kecs::Entity Document::Instantiate(const std::filesystem::path& file, const kecs::Entity parent, std::string* error)
    {
        const std::filesystem::path whole = file.is_absolute() || ProjectDirectory().empty() ? file : ProjectDirectory() / file;
        Value prefab;
        std::string why;
        if (!ReadPrefab(whole, prefab, why, _assets.get())) { if (error) *error = why; return kecs::NullEntity; }
        const std::filesystem::path project = ProjectDirectory();
        std::error_code ec;
        const auto relative = project.empty() ? std::filesystem::path() : std::filesystem::relative(std::filesystem::absolute(whole), project, ec);
        const std::string named = !ec && !relative.empty() && *relative.begin() != ".." ? relative.generic_string() : std::filesystem::absolute(whole).generic_string();
        const auto& siblings = parent.IsNull() ? _world.Roots() : _world.ChildrenOf(parent);
        std::vector<std::string> names;
        for (const kecs::Entity sibling : siblings) names.push_back(_world.NameOf(sibling));
        const Value* name = prefab.Find("name");
        const kecs::Entity root = CreateEntity(UniqueName(name && name->String() ? *name->String() : whole.stem().string(), names), parent);
        kecs::Vec3 three;
        if (Three(prefab.Find("position"), three)) _world.SetPosition(root, three);
        if (Three(prefab.Find("rotation"), three)) _world.SetRotation(root, three);
        if (Three(prefab.Find("scale"), three)) _world.SetScale(root, three);
        _world.Add<PrefabLink>(root, PrefabLink { named, {}, -1 });
        _world.Add<PrefabPart>(root, PrefabPart { std::max(PartOf(prefab), 0) });
        Rebuild(root, prefab);
        Changed();
        return root;
    }

    void Document::Rebuild(const kecs::Entity root, const Value& prefab)
    {
        // A prefab that has an instance of itself in it, however far down, is made so far and no further.
        if (_rebuilding > 8) { _notes.push_back(std::format("{}: its prefab has itself in it: made eight deep, and no more", _world.NameOf(root))); return; }
        ++_rebuilding;
        const auto keys = OverrideSet(_world.Get<PrefabLink>(root)->overrides);
        const auto own = [&](const int part, const std::string& what) { return keys.contains(std::format("{}:{}", part, what)); };
        // Which of the instance's own entities is which part of the prefab. A part had twice (an entity of it copied
        // under it) is the first; the copy is the instance's own.
        std::map<int, kecs::Entity> parts;
        const std::function<void(kecs::Entity)> find = [&](const kecs::Entity entity) {
            if (entity != root)
                if (PrefabLink* link = _world.Mutable<PrefabLink>(entity)) {
                    if (link->outer >= 0 && !parts.contains(link->outer)) parts[link->outer] = entity;
                    else link->outer = -1;
                    return;
                }
            if (const PrefabPart* part = _world.Get<PrefabPart>(entity); part && entity != root) {
                if (!parts.contains(part->part)) parts[part->part] = entity;
                else _world.Remove<PrefabPart>(entity);
            }
            for (const kecs::Entity child : _world.ChildrenOf(entity)) find(child);
        };
        find(root);
        const int rootPart = std::max(PartOf(prefab), 0);
        _world.Mutable<PrefabPart>(root)->part = rootPart;
        parts[rootPart] = root;

        std::set<int> seen;
        struct Link { kecs::Entity entity; std::string component, field; int part; };
        std::vector<Link> links;
        const auto place = [&](const Value& node, const kecs::Entity entity, const int part) {
            const Value* name = node.Find("name");
            if (!own(part, "name") && name && name->String()) _world.Rename(entity, *name->String());
            kecs::Vec3 three;
            if (!own(part, "position") && Three(node.Find("position"), three)) _world.SetPosition(entity, three);
            if (!own(part, "rotation") && Three(node.Find("rotation"), three)) _world.SetRotation(entity, three);
            if (!own(part, "scale") && Three(node.Find("scale"), three)) _world.SetScale(entity, three);
        };
        // Another prefab's instance, placed by this one: made of its prefab, with what this one's says it made its own —
        // unless this instance changed something of it, which it keeps.
        const auto nested = [&](const Value& node, const kecs::Entity parent, const int part) {
            const std::string inner = node.Find("prefab")->String() ? *node.Find("prefab")->String() : std::string();
            const auto at = parts.find(part);
            kecs::Entity entity = at != parts.end() && _world.Alive(at->second) && PrefabOf(at->second) == inner ? at->second : kecs::NullEntity;
            if (!entity.IsNull() && at != parts.end() && at->second != entity) _world.Destroy(at->second);
            const bool made = entity.IsNull();
            if (made) {
                if (at != parts.end() && _world.Alive(at->second)) _world.Destroy(at->second);   // of another prefab, before
                const Value* name = node.Find("name");
                entity = CreateEntity(name && name->String() ? *name->String() : std::string("Entity"), parent);
                _world.Add<PrefabLink>(entity, PrefabLink { inner, {}, part });
                _world.Add<PrefabPart>(entity, PrefabPart { 0 });
                parts[part] = entity;
            }
            place(node, entity, part);
            const bool changedHere = own(part, "nested");
            const Value* kept = node.Find("overrides");
            if (!changedHere) {
                std::set<std::string> given;
                if (kept && kept->Named()) for (const auto& [key, value] : *kept->Named()) given.insert(key);
                _world.Mutable<PrefabLink>(entity)->overrides = OverrideText(given);
            }
            const std::filesystem::path file = std::filesystem::path(inner).is_absolute() || ProjectDirectory().empty() ? std::filesystem::path(inner) : ProjectDirectory() / inner;
            Value innerPrefab;
            std::string error;
            if (!ReadPrefab(file, innerPrefab, error, _assets.get())) { _notes.push_back(std::format("{}: {}", _world.NameOf(entity), error)); return; }
            Rebuild(entity, innerPrefab);
            if (changedHere || !kept || !kept->Named()) return;
            // What the outer prefab says the inner instance made its own, it has.
            const auto of = PartsOf(entity);
            for (const auto& [key, value] : *kept->Named()) {
                const auto colon = key.find(':');
                const auto it = of.find(std::atoi(key.substr(0, colon).c_str()));
                if (it == of.end() || !_world.Alive(it->second)) continue;
                const kecs::Entity target = it->second;
                const std::string what = key.substr(colon + 1);
                kecs::Vec3 three;
                if (what == "gone") { if (target != entity) _world.Destroy(target); }
                else if (what == "position" && Three(&value, three)) _world.SetPosition(target, three);
                else if (what == "rotation" && Three(&value, three)) _world.SetRotation(target, three);
                else if (what == "scale" && Three(&value, three)) _world.SetScale(target, three);
                else if (what == "name" && value.String()) _world.Rename(target, *value.String());
                else if (what.starts_with('+')) AddComponent(target, what.substr(1));
                else if (what.starts_with('-')) RemoveComponent(target, what.substr(1));
                else if (what.starts_with('~') && value.Bool()) SetEnabled(target, what.substr(1), *value.Bool());
                else if (const auto dot = what.find('.'); dot != std::string::npos) {
                    if (!std::ranges::contains(ComponentsOf(target), what.substr(0, dot))) AddComponent(target, what.substr(0, dot));
                    SetField(target, what.substr(0, dot), what.substr(dot + 1), value);
                }
            }
        };
        const std::function<void(const Value&, kecs::Entity)> make = [&](const Value& node, const kecs::Entity parent) {
            const int part = PartOf(node);
            if (part < 0) return;
            seen.insert(part);
            const auto at = parts.find(part);
            if (own(part, "gone")) {
                if (at != parts.end() && at->second != root) _world.Destroy(at->second);
                return;
            }
            if (const Value* inner = node.Find("prefab"); inner && inner->String() && part != rootPart) { nested(node, parent, part); return; }
            const Value* name = node.Find("name");
            kecs::Entity entity = at != parts.end() && _world.Alive(at->second) && !(at->second != root && _world.Has<PrefabLink>(at->second)) ? at->second : kecs::NullEntity;
            if (entity.IsNull()) {
                if (at != parts.end() && _world.Alive(at->second)) _world.Destroy(at->second);      // an instance, before
                entity = CreateEntity(name && name->String() ? *name->String() : std::string("Entity"), parent);
                _world.Add<PrefabPart>(entity, PrefabPart { part });
                parts[part] = entity;
            }
            if (entity != root) place(node, entity, part);
            std::set<std::string> has, off;
            if (const Value* disabled = node.Find("disabled"); disabled && disabled->Items())
                for (const auto& component : *disabled->Items()) if (component.String()) off.insert(*component.String());
            if (const Value* components = node.Find("components"); components && components->Named())
                for (const auto& [component, fields] : *components->Named()) {
                    has.insert(component);
                    if (own(part, "-" + component)) { RemoveComponent(entity, component); continue; }
                    const kecs::ComponentId id = _world.Find(component);
                    if (id == kecs::NoComponent) { _notes.push_back(std::format("{}: its prefab's '{}' is no component here: left out", _world.NameOf(root), component)); continue; }
                    if (!_world.Has(entity, id)) AddComponent(entity, component);
                    if (fields.Named())
                        for (const auto& [field, given] : *fields.Named()) {
                            if (own(part, component + "." + field)) continue;
                            if (const Value* other = given.Find("part"); other && other->Number() && given.Named() && given.Named()->size() == 1) {
                                links.push_back({ entity, component, field, static_cast<int>(*other->Number()) });
                                continue;
                            }
                            SetField(entity, component, field, given);
                        }
                    if (!own(part, "~" + component)) SetEnabled(entity, component, !off.contains(component));
                }
            // What the prefab does not have, the instance has not — unless it was given it.
            for (const std::string& component : ComponentsOf(entity))
                if (!has.contains(component) && !own(part, "+" + component)) RemoveComponent(entity, component);
            if (const Value* children = node.Find("children"); children && children->Items())
                for (const auto& child : *children->Items()) make(child, entity);
        };
        make(prefab, _world.ParentOf(root));
        // What the prefab no longer has, its instance no longer has either.
        for (const auto& [part, entity] : parts)
            if (!seen.contains(part) && entity != root && _world.Alive(entity)) _world.Destroy(entity);
        // And what one of its entities holds of another is that one, now that all of them are there.
        for (const auto& link : links) {
            const auto it = parts.find(link.part);
            SetField(link.entity, link.component, link.field, it != parts.end() && _world.Alive(it->second) ? EntityValue(it->second) : Value {});
        }
        --_rebuilding;
    }

    void Document::UpdatePrefabs(const std::filesystem::path& file)
    {
        const std::filesystem::path project = ProjectDirectory();
        const auto whole = [&](const std::filesystem::path& named) { return named.is_absolute() || project.empty() ? named : project / named; };
        std::vector<kecs::Entity> roots;
        for (const kecs::Entity entity : _world.All())
            if (_world.Has<PrefabLink>(entity)) roots.push_back(entity);
        std::map<std::string, std::optional<Value>> read;
        for (const kecs::Entity root : roots) {
            if (!_world.Alive(root)) continue;
            const std::filesystem::path of = whole(_world.Get<PrefabLink>(root)->prefab);
            std::error_code ec;
            if (!file.empty() && !std::filesystem::equivalent(of, whole(file), ec)) continue;
            auto& prefab = read[of.generic_string()];
            if (!prefab && !read.contains(of.generic_string() + "?")) {
                Value root_;
                std::string error;
                if (ReadPrefab(of, root_, error, _assets.get())) prefab = std::move(root_);
                else { read[of.generic_string() + "?"] = std::nullopt; _notes.push_back(std::format("{}: {}. It is as it was saved.", _world.NameOf(root), error)); }
            }
            if (prefab) Rebuild(root, *prefab);
        }
        Changed();
    }

    bool Document::ApplyPrefab(const kecs::Entity root, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        if (!link) return fail("it is no prefab's instance");
        const std::filesystem::path named = link->prefab;
        const std::filesystem::path file = named.is_absolute() || ProjectDirectory().empty() ? named : ProjectDirectory() / named;
        const Value written = WritePrefab(root);
        std::string why;
        if (!WritePrefabFile(file, written, why, _assets.get())) return fail(why);
        _world.Mutable<PrefabLink>(root)->overrides.clear();
        UpdatePrefabs(file);
        Changed();
        return true;
    }

    bool Document::RevertPrefab(const kecs::Entity root, std::string* error)
    {
        const auto fail = [&](std::string why) { if (error) *error = std::move(why); return false; };
        const PrefabLink* link = _world.Alive(root) ? _world.Get<PrefabLink>(root) : nullptr;
        if (!link) return fail("it is no prefab's instance");
        const std::filesystem::path named = link->prefab;
        const std::filesystem::path file = named.is_absolute() || ProjectDirectory().empty() ? named : ProjectDirectory() / named;
        Value prefab;
        std::string why;
        if (!ReadPrefab(file, prefab, why, _assets.get())) return fail(why);
        _world.Mutable<PrefabLink>(root)->overrides.clear();
        // What was put under it goes: entities no part of the prefab, and instances of others placed by hand. Those the
        // prefab places are its own, and made as it says.
        std::vector<kecs::Entity> extra;
        const std::function<void(kecs::Entity)> walk = [&](const kecs::Entity entity) {
            for (const kecs::Entity child : _world.ChildrenOf(entity)) {
                if (const PrefabLink* link = _world.Get<PrefabLink>(child)) {
                    if (link->outer < 0) extra.push_back(child);
                    else _world.Mutable<PrefabLink>(child)->overrides.clear();
                } else if (!_world.Has<PrefabPart>(child)) extra.push_back(child);
                else walk(child);
            }
        };
        walk(root);
        for (const kecs::Entity entity : extra) _world.Destroy(entity);
        Rebuild(root, prefab);
        Changed();
        return true;
    }

    bool Document::Unlink(const kecs::Entity root)
    {
        if (!_world.Alive(root) || !_world.Has<PrefabLink>(root)) return false;
        const std::function<void(kecs::Entity)> walk = [&](const kecs::Entity entity) {
            // An instance it placed stays one, of its own: placed by hand, now.
            if (entity != root) if (PrefabLink* link = _world.Mutable<PrefabLink>(entity)) { link->outer = -1; return; }
            _world.Remove<PrefabPart>(entity);
            for (const kecs::Entity child : _world.ChildrenOf(entity)) walk(child);
        };
        walk(root);
        // And itself, placed by another, is no longer that one's.
        if (const PrefabLink* link = _world.Get<PrefabLink>(root); link && link->outer >= 0)
            if (const kecs::Entity outer = PrefabRootOf(_world.ParentOf(root)); !outer.IsNull()) Keep(outer, link->outer, "gone");
        _world.Remove<PrefabLink>(root);
        Changed();
        return true;
    }

    bool Document::Load(const std::filesystem::path& path, std::string& error)
    {
        // A scene is in a project's `scenes` folder, beside its `assets`: where its entities' files are named from.
        const std::filesystem::path project = std::filesystem::absolute(path).parent_path().parent_path();
        SetAssetDirectory(project / "assets");
        // Its scripts' components, as their sources declare them: whether or not anything runs them, they are what the file's are.
        DeclareScripts(project / "scripts");
        std::ifstream in(path, std::ios::binary);
        if (!in) { error = std::format("'{}' cannot be read", path.string()); return false; }
        std::stringstream text;
        text << in.rdbuf();
        if (!_assets) return FromJson(text.str(), error);
        // Its files, kept by their IDs, named by where they are now.
        std::size_t missing = 0;
        const std::string named = kasset::FromIds(text.str(), *_assets, &missing);
        if (!FromJson(named, error)) return false;
        if (missing > 0) _notes.push_back(std::format("{} {} of the scene's {} no longer in the project", missing, missing == 1 ? "file" : "files", missing == 1 ? "is" : "are"));
        return true;
    }
}
