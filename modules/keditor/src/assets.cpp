//
// keditor: the project's assets as the editor has them — listed, found by what names them, imported, moved and
// opened — over the document's kasset::Database.
//

#include <keditor/editor.h>
#include <keditor/viewer.h>
#include <kasset/import.h>
#include <kasset/cache.h>
#include <kasset/manifest.h>
#include <kasset/parameters.h>
#include "shader_icons.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdlib>
#include <format>
#include <kui/icons.h>
#include <cctype>
#include <window.h>
#include <chrono>
#include <limits>
#include <tuple>
#include <cstring>
#include <optional>
#include <functional>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <app.h>
#include <builderDescriptions.h>
#include <koralUI.h>
#include <kui/dock.h>
#include <kui/nodes.h>
#include <kgui/layout.h>
#include <koralFileBrowser.h>
#include <keditor/starter.h>
#include <kgraph/scene.h>
#include <kmath/quaternion.h>
#include <kmath/transform.h>
#include <koralSceneView.h>
#include <sstream>
#include <fstream>
#include <capiInterop.h>
#include <kecs/json.h>
#include <kgraph/catalog.h>
#include <koralImageImport.h>
#include <context.h>
#include <deviceFeatures.h>
#include <kscript/declarations.h>
#include <kgraph/meshes.h>
#include <log.h>

namespace keditor
{
    namespace {
        /** The model asset @p named (`@models/ship.gltf`) is, of @p assets; null for one that is none of the project's. */
        const kasset::Entry* ModelAssetNamed(kasset::Database* assets, const std::string& named)
        {
            if (!assets) return nullptr;
            const auto reference = assets->FromLegacy(named);
            const kasset::Entry* entry = reference && !reference->part ? assets->Find(reference->asset) : nullptr;
            return entry && entry->meta.kind == kasset::Kind::eModel ? entry : nullptr;
        }
    }

    kasset::Database* Session::AssetDatabase()
    {
        kasset::Database* assets = Doc().Assets();
        // The editor's own materials, of no file: known by their old names as well as their IDs — given to each scene's
        // database, as the engine's meshes are by the document.
        if (assets)
            for (const auto& material : DefaultMaterials()) assets->AddBuiltIn(kasset::Kind::eMaterial, "material/" + material.name, "material:" + material.name);
        return assets;
    }

    bool Session::ScanAssets()
    {
        Doc().SetAssetDirectory(_project.directory / "assets");
        kasset::Database* assets = AssetDatabase();
        if (!assets) return true;
        // What one model's materials are imported as was the project's to say, before it was the model's own.
        bool moved = false;
        for (auto it = _project.importKinds.begin(); it != _project.importKinds.end();) {
            const kasset::Entry* asset = it->first.empty() ? nullptr : ModelAssetNamed(assets, it->first);
            if (!asset || !kasset::ParameterOf(asset->meta, "materialKinds").empty() || !assets->SetParameter(asset->meta.id, "materialKinds", kasset::ToJson(it->second))) { ++it; continue; }
            it = _project.importKinds.erase(it);
            moved = true;
        }
        if (std::string error; moved && !_project.Save(error)) Say(error, true);
        const auto& problems = Doc().AssetProblems();
        if (problems.empty()) return true;
        for (const auto& problem : problems) std::fprintf(stderr, "[assets] %s\n", problem.c_str());
        Say(problems.size() == 1 ? std::format("Of the project's assets: {}.", problems.front())
                                 : std::format("Of the project's assets, {} things were wrong; the first: {}.", problems.size(), problems.front()), true);
        return false;
    }

    std::vector<Session::Asset> Session::Assets()
    {
        static const auto ending = [](const std::filesystem::path& file) {
            std::string text = file.extension().string();
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        static const std::set<std::string> pictures { ".png", ".jpg", ".jpeg", ".hdr", ".tga", ".bmp", ".exr", ".dds", ".ktx", ".ktx2" };
        static const std::set<std::string> models { ".gltf", ".glb", ".obj", ".fbx", ".dae", ".ply" };
        kgraph::Runtime& runtime = Doc().Runtime();
        std::vector<Asset> assets;
        std::set<std::string> listed;
        std::error_code ignored;
        // What it shows is held until it has stopped showing it: a stand-in gives way to its file under the panel.
        _assetsHeldBefore = std::move(_assetsHeld);
        _assetsHeld.clear();
        const auto where = [&](const std::string_view id) {
            const auto mesh = id.rfind('#');
            const std::filesystem::path named(id.substr(1, mesh == std::string_view::npos ? std::string_view::npos : mesh - 1));
            return named.is_absolute() || runtime.AssetDirectory().empty() ? named : runtime.AssetDirectory() / named;
        };
        for (const auto& id : runtime.Assets()) {
            const auto held = runtime.ResourceOf(id);
            if (!held) continue;
            Asset asset;
            asset.id = id;
            asset.file = where(id);
            asset.kind = held->kind == "Mesh" ? Asset::Kind::eMesh : Asset::Kind::eTexture;
            asset.label = asset.file.filename().string() + (asset.kind == Asset::Kind::eMesh ? id.substr(id.rfind('#')) : std::string());
            if (asset.kind == Asset::Kind::eMesh) {
                asset.group = asset.file.filename().string();
                if (!held->label.empty()) asset.label = held->label;
            }
            asset.pending = held->pending;
            asset.loaded = true;
            _assetsHeld.emplace_back(id, held);
            if (asset.kind == Asset::Kind::eMesh && !asset.pending) {
                ThumbnailsHere().Want(id, held, MeshKey(id));
            }
            listed.insert(id);
            assets.push_back(std::move(asset));
        }
        // The editor's own, whatever the project: the procedural meshes, listed with the other meshes, and materials.
        for (const auto& name : kgraph::DefaultMeshNames()) {
            Asset asset;
            asset.kind = Asset::Kind::eMesh;
            asset.id = kgraph::DefaultMeshId(name);
            if (!listed.insert(asset.id).second) continue;
            asset.label = name;
            asset.builtIn = true;
            asset.loaded = true;
            if (const auto held = runtime.ResourceOf(asset.id)) {
                _assetsHeld.emplace_back(asset.id, held);
                ThumbnailsHere().Want(asset.id, held, MeshKey(asset.id));
            }
            assets.push_back(std::move(asset));
        }
        for (const auto& material : DefaultMaterials()) {
            Asset asset;
            asset.kind = Asset::Kind::eMaterial;
            asset.id = "material:" + material.name;
            asset.label = material.label;
            asset.group = material.kind;
            asset.folder = material.kind == "BlinnPhongMaterial" ? "Materials/Blinn-Phong" : "Materials/PBR";
            asset.builtIn = true;
            asset.loaded = true;
            assets.push_back(std::move(asset));
        }
        if (_hasProject) {
            // What is in the project's folders and not in the scene: there to be imported, or dragged where it is wanted.
            // Its own, and its modules': what they have is its too.
            std::vector<std::filesystem::path> roots { _project.directory };
            for (const auto& module : Doc().Modules()) roots.push_back(module);
            for (const auto& root : roots)
            if (const auto folder = root / "assets"; std::filesystem::is_directory(folder, ignored))
                for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ignored)) {
                    if (!entry.is_regular_file(ignored)) continue;
                    const std::string kind = ending(entry.path());
                    const bool material = kind == ".kmat";      // a project's own Material asset
                    const bool generator = kind == ".kgen";     // a generator, with its fields
                    if (!pictures.contains(kind) && !models.contains(kind) && !material && !generator) continue;
                    Asset asset;
                    asset.id = runtime.AssetId(entry.path());
                    if (!listed.insert(asset.id).second) continue;
                    asset.kind = generator ? Asset::Kind::eGenerator : material ? Asset::Kind::eMaterial : models.contains(kind) ? Asset::Kind::eModel : Asset::Kind::eTexture;
                    asset.file = entry.path();
                    asset.label = std::filesystem::relative(entry.path(), folder, ignored).generic_string();
                    assets.push_back(std::move(asset));
                }
            const auto files = [&](const std::filesystem::path& folder, const Asset::Kind kind, const char* prefix, const std::set<std::string>& endings) {
                if (!std::filesystem::is_directory(folder, ignored)) return;
                for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
                    if (!entry.is_regular_file(ignored) || !endings.contains(ending(entry.path()))) continue;
                    Asset asset;
                    asset.kind = kind;
                    asset.file = entry.path();
                    if (kind == Asset::Kind::eShader) {
                        // A shader module, and after it its entry points, each of it: opened, the panel shows them under it.
                        const std::string module = entry.path().stem().string();
                        asset.group = module;
                        asset.id = prefix + module;
                        asset.label = module;
                        for (const auto& [name, stage, line] : ShaderEntries(module)) {
                            Asset point = asset;
                            point.id = std::format("{}{}#{}", prefix, module, name);
                            point.label = name;
                            point.stage = stage;
                            point.parent = asset.id;
                            assets.push_back(std::move(point));
                        }
                        // (The module first of them: the sort keeps a module's assets in the order they are listed.)
                        assets.insert(assets.end() - static_cast<std::ptrdiff_t>(std::ranges::count(assets, asset.id, &Asset::parent)), asset);
                        continue;
                    } else {
                        // A function by its name, as a call knows it (`function:Scale`); anything else by its file's.
                        asset.id = prefix + (kind == Asset::Kind::eFunction ? entry.path().stem().string() : entry.path().filename().string());
                        asset.label = entry.path().filename().string();
                    }
                    assets.push_back(std::move(asset));
                }
            };
            for (const auto& root : roots) files(root / "shaders", Asset::Kind::eShader, "shader:", { ".slang" });
            // The scripts' components, not their files: each that a source declares — a script, Data, a kind, a material
            // kind, a ResourceScript — by its name, opened in the file it is in. A file of plain classes shows nothing.
            for (const auto& type : ProjectScriptTypes()) {
                Asset asset;
                asset.kind = Asset::Kind::eScript;
                asset.id = "script:" + type.name;
                asset.label = type.name;
                asset.file = type.file;
                asset.group = type.makesResources ? "resources" : type.material ? "material" : type.abstract ? "kind" : type.data ? "data" : "script";
                if (listed.insert(asset.id).second) assets.push_back(std::move(asset));
            }
            // And its classes of conditions: no component, but what a ForEach or a Filter keeps an entity by.
            for (const auto& conditions : ProjectConditions()) {
                Asset asset;
                asset.kind = Asset::Kind::eScript;
                asset.id = "script:" + conditions.name;
                asset.label = conditions.name;
                asset.file = conditions.file;
                asset.group = "conditions";
                if (listed.insert(asset.id).second) assets.push_back(std::move(asset));
            }
            for (const auto& root : roots) files(root / "prefabs", Asset::Kind::ePrefab, "prefab:", { ".prefab" });
            for (const auto& root : roots) files(root / "functions", Asset::Kind::eFunction, "function:", { ".kfunc" });
            // The scenes it has: listed, like everything else it is made of.
            if (const auto folder = _project.directory / "scenes"; std::filesystem::is_directory(folder, ignored))
                for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
                    const std::string name = entry.path().filename().string();
                    if (!entry.is_regular_file(ignored) || !name.ends_with(".scene.json")) continue;
                    Asset asset;
                    asset.kind = Asset::Kind::eScene;
                    asset.id = "scene:" + name.substr(0, name.size() - 11);
                    asset.file = entry.path();
                    asset.label = name.substr(0, name.size() - 11);
                    assets.push_back(std::move(asset));
                }
        }
        // A mesh is of a model: the model is listed too, with or without a file of the project's, and its meshes under it.
        std::map<std::string, std::size_t> models_;
        for (std::size_t i = 0; i < assets.size(); ++i)
            if (assets[i].kind == Asset::Kind::eModel) models_[assets[i].id] = i;
        for (std::size_t i = 0, count = assets.size(); i < count; ++i) {
            if (assets[i].kind != Asset::Kind::eMesh || assets[i].builtIn) continue;     // a default mesh is of no model
            const std::string parent = assets[i].id.substr(0, assets[i].id.rfind('#'));
            assets[i].parent = parent;
            const auto [at, fresh] = models_.try_emplace(parent, assets.size());
            if (fresh) {
                Asset model;
                model.kind = Asset::Kind::eModel;
                model.id = parent;
                model.file = assets[i].file;
                model.label = assets[i].file.filename().string();
                assets.push_back(std::move(model));
            }
            assets[at->second].loaded = true;
            assets[at->second].pending = assets[at->second].pending || assets[i].pending;
        }
        // Where each is shown: the folder its file is in, from the project's own — or, for a file that is somewhere
        // else, a folder named for the last two it is in, under the one for everything outside.
        for (auto& asset : assets) {
            if (asset.builtIn) continue;            // in the folder it was given
            const std::filesystem::path in = asset.file.parent_path();
            const auto relative = _hasProject ? in.lexically_relative(_project.directory) : std::filesystem::path("..");
            // A module's, under the one for the modules: in a folder of its name, as its own folders are.
            const auto module = std::ranges::find_if(Doc().Modules(), [&](const std::filesystem::path& folder) {
                const auto inside = in.lexically_normal().lexically_relative(folder.lexically_normal());
                return !inside.empty() && *inside.begin() != "..";
            });
            if (_hasProject && !relative.empty() && *relative.begin() != "..") asset.folder = relative == "." ? std::string() : relative.generic_string();
            else if (module != Doc().Modules().end()) {
                const auto inside = in.lexically_normal().lexically_relative(module->lexically_normal());
                asset.folder = std::format("{}/{}{}", ModulesFolder, module->filename().string(), inside == "." ? std::string() : "/" + inside.generic_string());
            } else {
                asset.folder = OutsideFolder;
                if (in.has_parent_path() && in.parent_path().has_filename()) asset.folder += "/" + in.parent_path().filename().string();
                if (in.has_filename()) asset.folder += "/" + in.filename().string();
            }
            // In a folder, a thing is called what its file is.
            if (asset.kind == Asset::Kind::eTexture || (asset.kind == Asset::Kind::eModel && !asset.file.empty())) asset.label = asset.file.filename().string();
        }
        // A model's materials — each an asset its things follow — shown beside its meshes, in its folder: of every model
        // the scene has (read), and of every asset an entity follows, whether it was imported now or the scene opened.
        {
            Document& doc = Doc();
            for (const kecs::Entity entity : doc.World().All())
                if (const auto* link = doc.World().Get<MaterialLink>(entity)) {
                    std::stringstream lines(link->assets);
                    for (std::string line; std::getline(lines, line);)
                        if (const auto space = line.find(' '); space != std::string::npos) (void)doc.MaterialAssetOf(line.substr(space + 1));
                }
            // (And the model the panel is in, read to show what it is made of.)
            for (const auto& asset : std::vector<Asset>(assets))
                if (asset.kind == Asset::Kind::eModel && (asset.loaded || asset.id == assetModel)) (void)doc.MaterialAssetOf(asset.id + "#material:0");
            if (const auto model = std::ranges::find(assets, assetModel, &Asset::id); !assetModel.empty() && model != assets.end())
                if (const auto read = doc.Runtime().ModelOf(model->file)) {
                    const Asset of = *model;
                    const auto part = [&](const Asset::Kind kind, const char* word, const std::size_t i, const std::string& name) {
                        Asset asset;
                        asset.kind = kind;
                        asset.id = std::format("{}#{}:{}", of.id, word, i);
                        asset.label = name.empty() ? std::format("{} {}", word, i) : name;
                        asset.parent = asset.group = of.id;
                        asset.folder = of.folder;
                        asset.file = of.file;
                        asset.loaded = true;
                        assets.push_back(std::move(asset));
                    };
                    for (std::size_t i = 0; i < read->lights.size(); ++i) part(Asset::Kind::eLight, "light", i, read->lights[i].name);
                    for (std::size_t i = 0; i < read->cameras.size(); ++i) part(Asset::Kind::eCamera, "camera", i, read->cameras[i].name);
                }
            for (const auto& id : doc.MaterialAssetIds()) {
                // The editor's own are listed by their kind; a project's file with its folder.
                if (id.starts_with("material:") || id.ends_with(".kmat") || !listed.insert(id).second) continue;
                const MaterialAsset* material = doc.MaterialAssetOf(id);
                if (!material) continue;
                Asset asset;
                asset.kind = Asset::Kind::eMaterial;
                asset.id = id;
                asset.label = material->name;
                asset.parent = asset.group = id.substr(0, id.find('#'));
                const auto model = std::ranges::find(assets, asset.parent, &Asset::id);
                asset.folder = model != assets.end() ? model->folder : std::string("Materials");
                asset.loaded = true;
                assets.push_back(std::move(asset));
            }
        }
        std::ranges::stable_sort(assets, [](const Asset& a, const Asset& b) { // A shader's entries as its module declares them.
            const auto label = [](const Asset& asset) -> const std::string& { static const std::string none; return asset.kind == Asset::Kind::eShader ? none : asset.label; };
            return std::tuple(a.kind, a.group, label(a)) < std::tuple(b.kind, b.group, label(b)); });
        return assets;
    }

    std::vector<std::string> Session::AssetFolders(const std::vector<Asset>& assets) const
    {
        std::set<std::string> folders;
        const auto add = [&](std::string folder) {
            // And every folder it is in.
            while (!folder.empty() && folders.insert(folder).second) {
                const auto slash = folder.rfind('/');
                folder = slash == std::string::npos ? std::string() : folder.substr(0, slash);
            }
        };
        for (const auto& asset : assets) add(asset.folder);
        if (_hasProject && assetFiles) {
            // As they are on disk: the project's own and its modules', whether or not anything is in them — but not
            // what a tool keeps for itself.
            static const std::set<std::string> skipped { "bin", "obj", "build", "node_modules", "out" };
            std::error_code ignored;
            const auto walk = [&](const std::filesystem::path& top, const std::string& under) {
                for (auto it = std::filesystem::recursive_directory_iterator(top, std::filesystem::directory_options::skip_permission_denied, ignored);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(ignored)) {
                    if (!it->is_directory(ignored)) continue;
                    const std::string name = it->path().filename().string();
                    // Nor the textures compressed into its cache: each the machine's own copy of a file shown elsewhere.
                    const bool cache = name == "cache" && it.depth() == 0;
                    if (name.starts_with(".") || skipped.contains(name) || cache) { it.disable_recursion_pending(); continue; }
                    const std::string relative = it->path().lexically_relative(top).generic_string();
                    add(under.empty() ? relative : under + "/" + relative);
                }
            };
            walk(_project.directory, {});
            for (const auto& module : const_cast<Session*>(this)->Doc().Modules()) {
                const std::string under = std::format("{}/{}", ModulesFolder, module.filename().string());
                add(under);
                walk(module, under);
            }
        }
        // The project's own first, by name; its modules' after them; what is outside it last.
        std::vector<std::string> sorted(folders.begin(), folders.end());
        const auto rank = [](const std::string& folder) { return folder.starts_with(OutsideFolder) ? 2 : folder.starts_with(ModulesFolder) ? 1 : 0; };
        std::ranges::stable_sort(sorted, [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        return sorted;
    }

    std::filesystem::path Session::AssetFolderPath(const std::string_view folder) const
    {
        if (!_hasProject || folder.starts_with(OutsideFolder)) return {};
        if (folder.empty()) return _project.directory;
        // A module's: its folder of that name, and what is in it.
        if (folder.starts_with(std::string(ModulesFolder) + "/")) {
            const std::filesystem::path within(std::string(folder.substr(std::string_view(ModulesFolder).size() + 1)));
            const std::string name = within.begin()->string();
            for (const auto& module : const_cast<Session*>(this)->Doc().Modules())
                if (module.filename() == name) return (module / within.lexically_relative(name)).lexically_normal();
            return {};
        }
        // The editor's own (Materials/PBR, Materials/Blinn-Phong) are of no folder on disk, unless the project has one so called.
        std::error_code ignored;
        if ((folder.starts_with("Meshes") || folder.starts_with("Materials")) && !std::filesystem::is_directory(_project.directory / folder, ignored)) return {};
        return _project.directory / folder;
    }

    std::vector<std::filesystem::path> Session::AssetFolderFiles(const std::string_view folder) const
    {
        std::vector<std::filesystem::path> files;
        const std::filesystem::path in = AssetFolderPath(folder);
        std::error_code ignored;
        if (in.empty() || !std::filesystem::is_directory(in, ignored)) return files;
        for (const auto& entry : std::filesystem::directory_iterator(in, std::filesystem::directory_options::skip_permission_denied, ignored)) {
            if (!entry.is_regular_file(ignored)) continue;
            if (entry.path().filename().string().starts_with(".") || entry.path().extension() == ".meta") continue;
            files.push_back(entry.path());
        }
        const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
        std::ranges::sort(files, {}, [&](const std::filesystem::path& file) { return lower(file.filename().string()); });
        return files;
    }

    bool Session::NewAssetFolder(const std::string& name)
    {
        const std::filesystem::path in = AssetFolderPath(assetFolder);
        if (in.empty()) { Say("A folder is made in a folder of the project's.", true); return false; }
        if (name.empty() || name.find_first_of("/\\") != std::string::npos || name.starts_with(".")) { Say("A folder's name is a plain one: no slashes, and not starting with a dot.", true); return false; }
        std::error_code error;
        if (std::filesystem::exists(in / name, error)) { Say(std::format("There is a '{}' here already.", name), true); return false; }
        if (!std::filesystem::create_directory(in / name, error)) { Say(std::format("The folder could not be made: {}.", error.message()), true); return false; }
        // Empty, it is shown in the files view and nowhere else.
        assetFiles = true;
        Say(std::format("Made the folder {}.", name));
        ++_changes;
        return true;
    }

    void Session::OpenAsset(const Asset& asset)
    {
        switch (asset.kind) {
        case Asset::Kind::eShader: {
            // A module of entries shows them under it, and hides them; one of none, and an entry, open where they are written.
            const auto entries = ShaderEntries(asset.group);
            if (asset.stage.empty() && !entries.empty()) {
                if (!assetsOpened.erase(asset.id)) assetsOpened.insert(asset.id);
                ++_changes;
                break;
            }
            const auto entry = std::ranges::find(entries, asset.label, &ShaderEntry::name);
            OpenInCode(asset.file, asset.stage.empty() || entry == entries.end() ? 0 : entry->line);
            break;
        }
        case Asset::Kind::eScript: OpenInCode(asset.file); break;
        case Asset::Kind::eFunction: OpenFunctionNamed(asset.file.stem().string()); break;
        case Asset::Kind::eModel:
            // Gone into, as a folder is: what it is made of, read if it has not been.
            assetModel = asset.id;
            assetFolder = asset.folder;
            assetFilter.clear();
            assetKind = -1;
            selectedAsset = asset.id;
            ++_changes;
            break;
        case Asset::Kind::eTexture:
        case Asset::Kind::eMesh:
        case Asset::Kind::eMaterial: View(asset.id); break;
        case Asset::Kind::ePrefab: OpenPrefab(asset.id); break;     // edited by itself; dragged into the scene, placed
        case Asset::Kind::eScene: OpenScene(asset.label); break;
        default: break;
        }
    }

    bool Session::TrashAsset(const std::filesystem::path& path)
    {
        std::error_code ignored;
        const auto relative = _hasProject ? path.lexically_relative(_project.directory) : std::filesystem::path("..");
        if (path.empty() || relative.empty() || relative == "." || *relative.begin() == "..") { Say("Only what is in the project's folder is thrown away from here.", true); return false; }
        if (!std::filesystem::exists(path, ignored)) { Say(std::format("There is no '{}'.", relative.generic_string()), true); return false; }
        // To the system's trash, where it can be had back from: never taken off the disk from here. Its meta goes with it.
        const auto trash = [&](const std::filesystem::path& file) {
            const std::string command = std::format("gio trash \"{}\"", file.string());
            return launch ? launch(command) : std::system((command + " >/dev/null 2>&1").c_str()) == 0;
        };
        if (!trash(path)) { Say(std::format("'{}' could not be moved to the trash (gio trash).", relative.generic_string()), true); return false; }
        // Its meta goes with it: where the project keeps it, or — not yet moved there — beside it.
        const auto metaOf = [&] { return Doc().Assets() ? Doc().Assets()->MetaOf(path) : kasset::MetaPathOf(_project.directory, path); };
        if (kasset::KindOfFile(path) != kasset::Kind::eGenerated)
            for (const auto& meta : { metaOf(), kasset::MetaBeside(path) }) if (std::filesystem::exists(meta, ignored)) trash(meta);
        if (Doc().Assets()) Doc().Assets()->Scan();
        if (!selectedAsset.empty()) selectedAsset.clear();
        Say(std::format("Moved {} to the trash.", relative.generic_string()));
        ++_changes;
        return true;
    }

    std::string Session::AssetLabel(const std::string& id) const
    {
        // A generator's: by the script that made it, not by what it was made of.
        if (id.starts_with("$script/gen/")) {
            const std::string_view rest = std::string_view(id).substr(12);
            return std::format("{}, generated", rest.substr(0, rest.find('/')));
        }
        // A material is no resource, and is not looked for as one: its file is no picture.
        if (const Material* material = MaterialOf(id)) return material->label;
        if (const MaterialAsset* material = _play.Doc().MaterialAssetOf(id)) return material->name;
        if (!id.starts_with('@')) return id;
        if (const auto held = _play.Doc().Runtime().ResourceOf(id); held && !held->label.empty()) return held->label;
        const auto mesh = id.rfind('#');
        const std::filesystem::path file(id.substr(1, mesh == std::string::npos ? std::string::npos : mesh - 1));
        return file.filename().string() + (mesh == std::string::npos ? std::string() : id.substr(mesh));
    }

    void Session::SetImportKind(const kgraph::ModelMaterial::Model model, const std::string& kind, const std::filesystem::path& file)
    {
        Doc().SetImportKind(model, kind, file);
        const std::string key = file.empty() ? std::string() : Doc().Runtime().AssetId(file);
        const char* reported = model == kgraph::ModelMaterial::Model::eClassic ? "classic" : "pbr";
        // One file's is its own, in its meta; every file's is the project's.
        if (const kasset::Entry* asset = key.empty() ? nullptr : ModelAssetNamed(AssetDatabase(), key)) {
            auto kinds = kasset::KindsFrom(kasset::ParameterOf(asset->meta, "materialKinds")).value_or(std::map<std::string, std::string> {});
            if (kind.empty()) kinds.erase(reported);
            else kinds[reported] = kind;
            if (auto set = AssetDatabase()->SetParameter(asset->meta.id, "materialKinds", kinds.empty() ? std::string() : kasset::ToJson(kinds)); !set) Say(set.error(), true);
            return;
        }
        if (kind.empty()) {
            if (const auto it = _project.importKinds.find(key); it != _project.importKinds.end()) {
                it->second.erase(reported);
                if (it->second.empty()) _project.importKinds.erase(it);
            }
        } else {
            _project.importKinds[key][reported] = kind;
        }
        std::string error;
        if (_hasProject && !_project.Save(error)) Say(error, true);
    }

    void Session::KeepTextures(std::map<std::string, kgraph::TextureImport> imports, std::map<std::string, kgraph::TexturePack> packs, const bool cook)
    {
        Doc().SetTextureSettings(std::move(imports), std::move(packs));
        ++_changes;
        if (!cook) return;
        // What is cooked is the machine's own, made again from the files: kept out of the project's history.
        if (_hasProject && (std::filesystem::exists(_project.directory / ".git") || std::filesystem::exists(_project.directory / ".gitignore"))) {
            std::ifstream in(_project.directory / ".gitignore");
            std::string line;
            bool ignored = false;
            while (std::getline(in, line)) ignored = ignored || line == "cache/" || line == "/cache/" || line == "cache";
            in.close();
            if (!ignored) {
                std::ifstream before(_project.directory / ".gitignore", std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
                before.close();
                std::ofstream(_project.directory / ".gitignore", std::ios::app) << (text.empty() || text.back() == '\n' ? "" : "\n") << "cache/\n";
            }
        }
        kgraph::Runtime& runtime = Doc().Runtime();
        std::vector<std::string> toCook;
        for (const auto& id : runtime.Pictures()) if (!runtime.Cooked(id)) toCook.push_back(id);
        for (const auto& pack : Doc().Graph().texturePacks | std::views::keys) if (!runtime.Cooked(pack) && std::ranges::find(toCook, pack) == toCook.end()) toCook.push_back(pack);
        runtime.Cook(toCook);
        _cookedShown = 0;
    }

    std::shared_ptr<kgraph::Resource> Session::AssetHeld(const std::string_view id) const
    {
        const auto it = std::ranges::find(_assetsHeld, id, [](const auto& held) -> const std::string& { return held.first; });
        return it == _assetsHeld.end() ? nullptr : it->second;
    }

    std::string Session::ImportAsset(const std::filesystem::path& file)
    {
        if (!_hasProject) { Say("A picture or a model is brought into a project, as one of its assets: File, New project.", true); return {}; }
        std::string ending = file.extension().string();
        std::ranges::transform(ending, ending.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::set<std::string> pictures { ".png", ".jpg", ".jpeg", ".hdr", ".tga", ".bmp", ".exr", ".dds", ".ktx", ".ktx2" };
        static const std::set<std::string> models { ".gltf", ".glb", ".obj", ".fbx", ".dae", ".ply" };
        const bool model = models.contains(ending);
        if (!model && !pictures.contains(ending)) { Say(std::format("{} is no picture or model the project can have.", file.filename().string()), true); return {}; }
        std::error_code failed;
        if (!std::filesystem::is_regular_file(file, failed)) { Say(std::format("{} is not there to be imported.", file.string()), true); return {}; }
        const std::filesystem::path assets = _project.directory / "assets";
        Doc().SetAssetDirectory(assets);
        std::filesystem::path into = std::filesystem::weakly_canonical(file, failed);
        // In the project already: it is one of its assets as it is.
        if (const auto relative = into.lexically_relative(std::filesystem::weakly_canonical(_project.directory, failed)); relative.empty() || *relative.begin() == "..") {
            const std::filesystem::path folder = model ? assets / "models" / file.stem() : assets / "textures";
            into = folder / file.filename();
            if (std::filesystem::exists(into, failed)) { Say(std::format("The project has {} already: it is picked in the Assets panel.", std::filesystem::relative(into, _project.directory, failed).generic_string())); }
            else {
                // A model with what it names beside it: copied as they are placed, or it would not be read.
                std::vector<std::filesystem::path> copied { file.filename() };
                if (model) for (auto& beside : kasset::NamedBeside(file)) copied.push_back(std::move(beside));
                std::size_t missing = 0;
                for (const auto& one : copied) {
                    const std::filesystem::path from = file.parent_path() / one, to = folder / one;
                    if (!std::filesystem::exists(from, failed)) { ++missing; continue; }
                    std::filesystem::create_directories(to.parent_path(), failed);
                    if (!std::filesystem::copy_file(from, to, std::filesystem::copy_options::skip_existing, failed) && failed) {
                        Say(std::format("{} could not be copied into the project: {}", from.string(), failed.message()), true);
                        return {};
                    }
                }
                Say(std::format("Imported {} into {}{}: drag it onto the scene, or onto the scene tree, to make entities of it.", file.filename().string(),
                                std::filesystem::relative(folder, _project.directory, failed).generic_string(),
                                missing ? std::format(" ({} of what it names were not beside it)", missing) : std::string()));
            }
        } else {
            Say(std::format("{} is the project's already: it is picked in the Assets panel.", file.filename().string()));
        }
        if (kasset::Database* assets = AssetDatabase())
            if (const auto added = assets->Add(into); !added) std::fprintf(stderr, "[assets] %s\n", added.error().c_str());
        const std::string id = Doc().Runtime().AssetId(into);
        assetFolder = std::filesystem::relative(into.parent_path(), _project.directory, failed).generic_string();
        assetFilter.clear();
        SelectAsset(id);
        return id;
    }

    bool Session::ImportModel(const std::filesystem::path& file)
    {
        if (_hasProject) Doc().SetAssetDirectory(_project.directory / "assets");
        std::string error;
        // What its materials are imported as: what the project says for every file, and what this one's meta says of it.
        const auto give = [&](const std::string& model, const std::map<std::string, std::string>& kinds) {
            for (const auto& [reported, kind] : kinds)
                Doc().SetImportKind(reported == "classic" ? kgraph::ModelMaterial::Model::eClassic : kgraph::ModelMaterial::Model::ePbr, kind,
                                    model.empty() ? std::filesystem::path() : std::filesystem::path(model.substr(1)));
        };
        for (const auto& [model, kinds] : _project.importKinds) give(model, kinds);
        const std::string named = Doc().Runtime().AssetId(file);
        if (const kasset::Entry* asset = ModelAssetNamed(AssetDatabase(), named))
            if (const auto kinds = kasset::KindsFrom(kasset::ParameterOf(asset->meta, "materialKinds"))) give(named, *kinds);
        const kecs::Entity root = Doc().ImportModel(file, &error);
        if (root.IsNull()) { Say(error, true); return false; }
        if (const auto at = std::exchange(placeAt, std::nullopt)) Doc().SetPosition(root, { at->x, at->y, at->z });
        Select(root);
        Say(std::format("Imported {}.", file.filename().string()));
        return true;
    }

    std::vector<std::string> Session::AssetUsers(const std::string& id)
    {
        kasset::Database* assets = AssetDatabase();
        if (!assets) return {};
        // Read again when anything may have been saved since: the files are what is looked at, not what is open.
        if (_usersIndexed != Version()) {
            (void)assets->Scan();    // and what was added since, as another program wrote it
            assets->IndexReferences();
            _usersIndexed = Version();
        }
        const auto reference = assets->Lookup(id);
        if (!reference) return {};
        std::vector<std::string> users;
        for (const kasset::Id user : assets->Users(reference->asset)) users.push_back(assets->Find(user)->file.generic_string());
        return users;
    }

    bool Session::MoveAsset(const std::string& id, const std::filesystem::path& to)
    {
        kasset::Database* assets = AssetDatabase();
        auto reference = assets ? assets->Lookup(id) : std::nullopt;
        // One made since the project's files were last looked at — a function just made — is found once they are.
        if (assets && !reference) { (void)assets->Scan(); reference = assets->Lookup(id); }
        if (!reference || reference->part || assets->BuiltInKind(reference->asset)) { Say("Only a file of the project's is moved: a model's mesh goes with its model.", true); return false; }
        if (InPlayMode()) Stop();
        if (_prefab) ClosePrefab(true);
        // What is open names the file by where it is: saved, it names it by its ID, and is read again from there after.
        if (Doc().Dirty() && !Save()) return false;
        (void)assets->Scan();
        assets->IndexReferences();
        if (const auto kept = assets->KeepUsersByIds(reference->asset); !kept) { Say(kept.error(), true); return false; }
        const std::filesystem::path from = assets->Find(reference->asset)->file;
        if (const auto moved = assets->Move(reference->asset, to); !moved) { Say(std::format("{} was not moved: {}", from.filename().string(), moved.error()), true); return false; }
        std::string error;
        if (!_play.Open(_project.ScenePath(_scene), error)) Say(error, true);
        selectedNode.clear();
        selectedEntity = kecs::NullEntity;
        _usersIndexed = ~std::uint64_t(0);
        const kasset::Entry* entry = assets->Find(reference->asset);
        if (!AssetDatabase() || !entry) return true;
        assetFolder = entry->file.parent_path().generic_string();
        assetFilter.clear();
        const std::string named = AssetDatabase()->ToLegacy({ reference->asset, {} });
        if (!named.empty()) SelectAsset(named);
        Say(std::format("Moved {} to {}.", from.generic_string(), entry->file.generic_string()));
        ++_changes;
        return true;
    }

    bool Session::OpenScene(const std::string& name)
    {
        if (!_hasProject) return false;
        if (_prefab) ClosePrefab(true);
        if (InPlayMode()) Stop();
        if (name == _scene) return true;
        // What was done to the one open is kept: saved before it is let go of.
        if (Doc().Dirty() && !Save()) return false;
        std::string error;
        if (!_play.Open(_project.ScenePath(name), error)) {
            Say(error, true);
            std::string again;
            (void)_play.Open(_project.ScenePath(_scene), again);
            return false;
        }
        _scene = name;
        selectedNode.clear();
        selectedEntity = kecs::NullEntity;
        _snapshot.clear();
        _folded.clear();
        Say(std::format("Opened {}.", name));
        return true;
    }

    std::string Session::ExtractMaterial(const std::string& id)
    {
        if (!_hasProject) { Say("A material of the project's own goes in a project: File, New project.", true); return {}; }
        const MaterialAsset* asset = Doc().MaterialAssetOf(id);
        std::string name = asset ? asset->name : std::string("Material");
        for (char& c : name) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        const std::filesystem::path folder = _project.directory / "assets" / "materials";
        std::filesystem::path file = folder / (name + ".kmat");
        std::error_code ec;
        for (int n = 2; std::filesystem::exists(file, ec); ++n) file = folder / std::format("{} {}.kmat", name, n);
        std::string error;
        const std::string made = Doc().ExtractMaterial(id, file, &error);
        if (made.empty()) { Say(error, true); return {}; }
        assetFolder = "assets/materials";
        assetFilter.clear();
        SelectAsset(made);
        Say(std::format("{} is the project's own now, in {}: the model's file read again no longer changes it.", name, std::filesystem::relative(file, _project.directory, ec).generic_string()));
        ++_changes;
        return made;
    }

    bool Session::ExportGame(const std::filesystem::path& folder)
    {
        if (!_hasProject) { Say("A game is exported from a project: File, New project.", true); return false; }
        if (InPlayMode()) Stop();
        if (_prefab) ClosePrefab(true);
        // What is open is what is exported: saved first.
        if (Doc().Dirty() && !Save()) return false;
        kasset::Database* assets = AssetDatabase();
        if (!assets) { Say("The project's assets could not be found.", true); return false; }
        std::error_code ec;
        const std::filesystem::path into = std::filesystem::weakly_canonical(folder.is_absolute() ? folder : _project.directory / folder, ec);
        const std::filesystem::path project = std::filesystem::weakly_canonical(_project.directory, ec);
        if (const auto inside = into.lexically_relative(project); !inside.empty() && *inside.begin() != ".." && !inside.string().starts_with("build")) {
            Say("A game is exported out of the project, or into its build folder: not among its assets.", true);
            return false;
        }
        if (std::filesystem::exists(into, ec) && !std::filesystem::is_empty(into, ec)) { Say(std::format("{} is not empty: a game is exported into an empty folder.", into.generic_string()), true); return false; }

        // Every scene of it, and what they reach.
        std::vector<kasset::Id> scenes;
        for (const auto& name : _project.scenes)
            if (const kasset::Entry* scene = assets->FindFile(_project.ScenePath(name))) scenes.push_back(scene->meta.id);
        const kasset::Manifest manifest = kasset::Reach(*assets, scenes);
        // Each model read into the cache as its meta says, so that the game never runs an importer.
        const kasset::Cache cache(kasset::CacheDirectoryOf(_project.directory));
        std::size_t models = 0;
        for (const kasset::Id id : manifest.assets)
            if (const kasset::Entry* entry = assets->Find(id); entry && entry->meta.kind == kasset::Kind::eModel) {
                const auto import = kasset::MeshImportFrom(kasset::ParameterOf(entry->meta, "mesh")).value_or(kasset::MeshImport {});
                if (const auto read = kasset::ImportModel(_project.directory / entry->file, import, &cache); !read) Say(read.error(), true);
                else ++models;
            }
        // Its files, and what was made of them.
        std::filesystem::create_directories(into, ec);
        for (const auto& file : manifest.files) {
            std::filesystem::create_directories((into / file).parent_path(), ec);
            std::filesystem::copy_file(_project.directory / file, into / file, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { Say(std::format("{} could not be copied: {}", file.generic_string(), ec.message()), true); return false; }
        }
        if (std::filesystem::exists(cache.Directory(), ec)) {
            std::filesystem::create_directories(kasset::CacheDirectoryOf(into), ec);
            std::filesystem::copy(cache.Directory(), kasset::CacheDirectoryOf(into), std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { Say(std::format("The cache could not be copied: {}", ec.message()), true); return false; }
        }
        std::ofstream(into / "manifest.json", std::ios::binary | std::ios::trunc) << kasset::Write(manifest);
        Say(std::format("Exported {} scenes, {} assets and {} models read ahead, in {} files, to {}: koral-play --project {} runs it.", scenes.size(), manifest.assets.size(), models,
                        manifest.files.size(), into.generic_string(), into.generic_string()));
        return true;
    }

    Thumbnails& Session::ThumbnailsHere()
    {
        if (!_thumbnails) _thumbnails = std::make_unique<Thumbnails>(_play.Graph());
        _thumbnails->SetCache(_hasProject ? kasset::CacheDirectoryOf(_project.directory) / "thumbnails" : std::filesystem::path());
        _thumbnails->SetAssetDirectory(_hasProject ? _project.directory / "assets" : std::filesystem::path());
        return *_thumbnails;
    }

    std::string Session::MeshKey(const std::string& id) const
    {
        kasset::Key key("thumbnail@1");
        const Document& doc = _play.Doc();
        if (id.starts_with('$')) {
            // The engine's own, as their tangents are; a generator's, as its id says what it was made of.
            key.Add(id).Add(kgraph::TangentsName(doc.Graph().defaultMeshTangents));
            return key.Text();
        }
        const auto hash = id.rfind('#');
        if (!id.starts_with('@') || hash == std::string::npos) return {};
        const std::string named = id.substr(1, hash - 1);
        const std::filesystem::path file = doc.Runtime().AssetDirectory().empty() ? std::filesystem::path(named) : doc.Runtime().AssetDirectory() / named;
        if (!key.AddFile(file)) return {};
        key.Add(id.substr(hash));
        if (const auto import = doc.Graph().meshImports.find(named); import != doc.Graph().meshImports.end()) key.Add(kasset::ToJson(import->second));
        return key.Text();
    }

    kor::ResourceRef<const kor::Image> Session::ModelPicture(const std::string& id)
    {
        if (!id.starts_with('@')) return {};
        const std::filesystem::path named(id.substr(1));
        const std::filesystem::path file = named.is_absolute() || Doc().Runtime().AssetDirectory().empty() ? named : Doc().Runtime().AssetDirectory() / named;
        kasset::Key key("model thumbnail@1");
        key.Add(kasset::ModelKey(file, {}).Text());
        if (!key.Valid()) return {};
        const std::string name = "model " + key.Text();
        Thumbnails& thumbnails = ThumbnailsHere();
        thumbnails.WantModel(name, file, key.Text());
        return thumbnails.Of(name);
    }

    kor::ResourceRef<const kor::Image> Session::FilePicture(const std::filesystem::path& file)
    {
        kasset::Key key("picture thumbnail@1");
        if (!key.AddFile(file)) return {};
        const std::string name = "file " + key.Text();
        Thumbnails& thumbnails = ThumbnailsHere();
        thumbnails.WantFile(name, file, key.Text());
        return thumbnails.Of(name);
    }

    kor::ResourceRef<const kor::Image> Session::MaterialPicture(const std::string& id)
    {
        auto shown = MaterialToShow(id);
        if (!shown) return {};
        const auto held = Doc().Runtime().ResourceOf(kgraph::DefaultMeshId("sphere"));
        if (!held) return {};
        // By what it looks like: its kind, its values, the pictures they name and what shades them. Another of any of
        // these is another picture; materials that look alike share one.
        kasset::Key key("material thumbnail@3");
        key.Add(shown->kind.name);
        for (const auto& [field, value] : shown->values) {
            key.Add(std::format("{}={}", field, value.ToJson()));
            if (const std::string* name = value.String(); name && name->starts_with('@')) (void)key.AddFile(Doc().Runtime().AssetDirectory() / name->substr(1));
        }
        const std::filesystem::path package = kscript::PackageDirectory() / "shaders";
        for (const char* file : { "koral_shading.slang", "koral_material_preview.slang" }) (void)key.AddFile(package / file);
        if (const auto own = package / (shown->kind.name + "Shading.slang"); !key.AddFile(own)) (void)key.AddFile(ShadingModuleOf(shown->kind.name));
        const std::string name = "material " + key.Text();
        Thumbnails& thumbnails = ThumbnailsHere();
        thumbnails.WantMaterial(name, held, std::move(*shown), key.Text());
        return thumbnails.Of(name);
    }

    std::string Session::SaveGenerator(const kecs::Entity entity)
    {
        if (!_hasProject) { Say("A generator is kept in a project: File, New project.", true); return {}; }
        // The component of it that makes something.
        std::string script;
        for (const char* piece : { "MeshGenerator", "ImageGenerator" })
            for (const auto& name : Doc().ScriptsTaking(piece))
                if (script.empty() && std::ranges::contains(Doc().ComponentsOf(entity), name)) script = name;
        if (script.empty()) { Say(std::format("{} has no generator: a script that makes its mesh or a picture.", Doc().NameOf(entity)), true); return {}; }
        const std::filesystem::path folder = _project.directory / "assets" / "generators";
        std::filesystem::path file = folder / (script + ".kgen");
        std::error_code ec;
        for (int n = 2; std::filesystem::exists(file, ec); ++n) file = folder / std::format("{} {}.kgen", script, n);
        std::string error;
        if (!Doc().SaveGenerator(entity, script, file, &error)) { Say(error, true); return {}; }
        // What it was saved from is the first to follow it.
        if (!Doc().GiveGenerator(entity, file, &error)) { Say(error, true); return {}; }
        const std::string id = Doc().Runtime().AssetId(file);
        assetFolder = "assets/generators";
        assetFilter.clear();
        SelectAsset(id);
        Say(std::format("Saved {}'s {} as {}: drop it on an entity for it to make the same.", Doc().NameOf(entity), script, file.filename().string()));
        ++_changes;
        return id;
    }
}
