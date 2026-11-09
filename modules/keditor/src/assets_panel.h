#pragma once

//
// keditor: the Assets panel — the project's files as its folders have them, each a picture and a name — with what is
// done with them: picked and shown in the inspector, dragged where they are wanted, made, moved, thrown away, imported,
// compressed. A view of the session's assets (assets.cpp), in the editor's window.
//

#include "widgets.h"

#include <keditor/editor.h>

#include <kgraph/meshes.h>

#include <kui/icons.h>
#include <kgui/layout.h>
#include <koralUI.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <kgraph/meshes.h>

namespace keditor
{
    using namespace widgets;

    /** Which of the editor's dialogs is in front of everything: one at a time. */
    enum class EditorDialog { eNone, eAddNode, ePortChoice, eNewProject, eNewProjectFolder, eNewProjectModule, eImportModule, eImportModuleFolder, eOpenProject, eNewShader, eNewScript, eImportModel, eImportOptions, eConfirm, eNewFolder, eTrash, eNewFunction, eNewPrefab, eCompressTextures, eMoveAsset, eExportGame, eNewScene };

    class AssetsView {
    public:
        using Dialog = EditorDialog;
        using Value = kgraph::Value;
        /** What the window it is in does for it: draws it again, and gives what a field that holds a mesh or a picture is edited with. */
        struct Host {
            std::function<void(std::function<void()>)> setState;
            std::function<kui::Widget(const std::string& component, const std::string& field, const std::string& type, const Value& value,
                                      const std::function<void(const Value&)>& set)> heldField;
        };

        /**
         * Of the assets of the session @p session holds — the window's, so that the session goes when the window lets it
         * go; @p dialog, @p newId and @p newKind are the window's too, which its dialogs share.
         */
        AssetsView(std::shared_ptr<Session>& session, Host host, Dialog& dialog, std::string& newId, int& newKind)
            : _session(session), _host(std::move(host)), _dialog(dialog), _newId(newId), _newKind(newKind) {}

        static constexpr float ListSize = 28.f;     // the least the panel shows a thing at: a line of a list

        // ---- the project's files ----------------------------------------------------------------------------

        static const char* KindName(const Session::Asset::Kind kind)
        {
            switch (kind) {
            case Session::Asset::Kind::eModel: return "Models";
            case Session::Asset::Kind::eMesh: return "Meshes";
            case Session::Asset::Kind::eTexture: return "Textures";
            case Session::Asset::Kind::eShader: return "Shaders";
            case Session::Asset::Kind::eScene: return "Scenes";
            case Session::Asset::Kind::ePrefab: return "Prefabs";
            case Session::Asset::Kind::eMaterial: return "Materials";
            case Session::Asset::Kind::eGenerator: return "Generators";
            default: return "Scripts";
            }
        }

        /** A shader's entry by its stage: an icon and a colour each, the stages of a kind near in hue. */
        struct StageLook {
            const char* icon;           ///< Its icons' name: `vert` is shader_vert.svg, and shader_vert-small.svg in a list.
            std::uint32_t colour;
            const char* name;
        };
        static StageLook StageOf(const std::string_view stage)
        {
            static const std::map<std::string_view, StageLook> looks {
                { "vertex", { "vert", 0xD86A6A, "Vertex shader" } },
                { "hull", { "tesc", 0xE08A4F, "Hull shader" } },
                { "domain", { "tese", 0xD9B44A, "Domain shader" } },
                { "geometry", { "geom", 0xA9C25A, "Geometry shader" } },
                { "amplification", { "task", 0x5B9BD5, "Task shader" } },
                { "task", { "task", 0x5B9BD5, "Task shader" } },
                { "mesh", { "mesh", 0x9273E0, "Mesh shader" } },
                { "fragment", { "frag", 0x5DB37A, "Fragment shader" } },
                { "pixel", { "frag", 0x5DB37A, "Fragment shader" } },
                { "compute", { "comp", 0xC76B98, "Compute shader" } },
                { "raygeneration", { "rgen", 0x62C3E8, "Ray generation shader" } },
                { "closesthit", { "rchit", 0x3FB8AF, "Closest-hit shader" } },
                { "anyhit", { "rahit", 0x6FD3B5, "Any-hit shader" } },
                { "miss", { "rmiss", 0x8FA3B8, "Miss shader" } },
                { "intersection", { "rint", 0x7C8FE6, "Intersection shader" } },
                { "callable", { "rcall", 0xC9A0DC, "Callable shader" } },
            };
            const auto it = looks.find(stage);
            return it != looks.end() ? it->second : StageLook { "lib", 0x9AA4B0, stage.empty() ? "Shader module" : "Shader" };
        }

        /** A stage's icon at @p size, in its colour: on its page with its name, or by itself where that is too small to read. */
        static kui::Widget StageIcon(const std::string_view stage, const float size)
        {
            // Read once each, from the SVGs the library has of them.
            static std::map<std::string, std::shared_ptr<const kui::VectorImage>, std::less<>> read;
            const StageLook look = StageOf(stage);
            const std::string name = std::format("shader_{}{}", look.icon, size > 28.f ? "" : "-small");
            auto it = read.find(name);
            if (it == read.end()) {
                const auto svg = std::ranges::find(detail::ShaderIcons, std::string_view(name), &std::pair<std::string_view, std::string_view>::first);
                it = read.emplace(name, std::make_shared<const kui::VectorImage>(kui::VectorImage::FromSvg(svg == std::end(detail::ShaderIcons) ? std::string_view() : svg->second))).first;
            }
            return kui::SizedBox(size, size, kui::Icon(it->second, kui::Color::Hex(look.colour)));
        }

        /** What a script's component is (Session::Asset::group of a script), as an icon, a colour and a word. */
        struct ScriptLooks { const char* icon; std::uint32_t colour; const char* what; };
        static ScriptLooks ScriptLook(const std::string_view group)
        {
            if (group == "data") return { "data_object", 0x5B9BD5, "Data: fields and nothing else, which the graph reads" };
            if (group == "kind") return { "account_tree", 0x8E7CC3, "A kind: abstract Data that nothing has itself, and that finds every one of what derives from it" };
            if (group == "material") return { "palette", 0xD9703F, "A material kind: what a drawn thing is made of" };
            if (group == "resources") return { "memory", 0x4FB3BF, "A ResourceScript: what a Script node runs, to make resources and fill them" };
            if (group == "conditions") return { "filter_alt", 0xE0B040, "Conditions: tests a ForEach or a Filter keeps an entity by, each [Condition(N)] function one" };
            return { "code", 0x7DBB4B, "A script: run on each entity that has it" };
        }

        /** An asset by what kind of thing it is, as an icon of its colour: faint for one not loaded. */
        static kui::Widget KindIcon(const Session::Asset& asset, const float size)
        {
            if (asset.kind == Session::Asset::Kind::eShader) {
                return StageIcon(asset.stage, size);
            }
            if (asset.kind == Session::Asset::Kind::eScript) {
                // A component a script declares, by what it is: code that runs, data, a kind of data, a material, resources made.
                const ScriptLooks look = ScriptLook(asset.group);
                return kui::SizedBox(size, size, kui::Icon(look.icon, kui::IconStyle::eFilled, kui::Color::Hex(look.colour)));
            }
            const char* icon = asset.kind == Session::Asset::Kind::eModel ? "category" : asset.kind == Session::Asset::Kind::eMesh ? "view_in_ar"
                             : asset.kind == Session::Asset::Kind::eLight ? "lightbulb" : asset.kind == Session::Asset::Kind::eCamera ? "videocam"
                             : asset.kind == Session::Asset::Kind::eGenerator ? "auto_awesome" : asset.kind == Session::Asset::Kind::eFunction ? "functions"
                             : asset.kind == Session::Asset::Kind::eMaterial ? "palette"
                             : asset.kind == Session::Asset::Kind::eTexture ? "image"
                             : asset.kind == Session::Asset::Kind::eScene ? "landscape" : asset.kind == Session::Asset::Kind::ePrefab ? "widgets" : "description";
            const kui::Color tint = asset.kind == Session::Asset::Kind::eScript ? kui::Color::Hex(0x5B9BD5)
                                  : asset.kind == Session::Asset::Kind::eModel ? kui::Color::Hex(0x7DBB4B) : asset.kind == Session::Asset::Kind::eMesh ? kui::Color::Hex(0x6FA87A)
                                  : asset.kind == Session::Asset::Kind::eScene ? kui::Color::Hex(0xD9A441) : asset.kind == Session::Asset::Kind::ePrefab ? kui::Color::Hex(0x4FB3BF)
                                  : kui::Theme::Current().textMuted;
            return kui::SizedBox(size, size, kui::Icon(icon, kui::IconStyle::eFilled, asset.loaded || asset.kind >= Session::Asset::Kind::eShader ? tint : tint.WithAlpha(0.55f)));
        }

        /** What an asset is shown by: a texture that is loaded by itself; anything else by what kind of thing it is. */
        kui::Widget AssetPreview(const Session::Asset& asset, const float size, const bool pictured = true)
        {
            if (!pictured) {}
            else if (asset.kind == Session::Asset::Kind::eTexture) {
                // Its small picture, whether or not the scene has it: by its kind until that is read.
                return kui::Make<MeshThumbnail>(_session, [session = _session, file = asset.file] { return session->FilePicture(file); },
                    [size, kind = KindIcon(asset, size)](const kor::ResourceRef<const kor::Image> picture) {
                        return picture.Get() ? kui::SizedBox(size, size, kui::Image(picture, kui::ImageFit::eContain)) : kind;
                    });
            }
            else if (asset.kind == Session::Asset::Kind::eMesh) {
                // A mesh's small picture, once it is drawn; by its kind until then.
                return kui::Make<MeshThumbnail>(_session, [session = _session, id = asset.id] { return session->MeshPicture(id); },
                    [size, kind = KindIcon(asset, size)](const kor::ResourceRef<const kor::Image> picture) {
                        return picture.Get() ? kui::SizedBox(size, size, kui::Image(picture, kui::ImageFit::eContain)) : kind;
                    });
            }
            else if (asset.kind == Session::Asset::Kind::eModel) {
                // All of a model, once it is drawn: what it is by its kind until then.
                return kui::Make<MeshThumbnail>(_session, [session = _session, id = asset.id] { return session->ModelPicture(id); },
                    [size, kind = KindIcon(asset, size)](const kor::ResourceRef<const kor::Image> picture) {
                        return picture.Get() ? kui::SizedBox(size, size, kui::Image(picture, kui::ImageFit::eContain)) : kind;
                    });
            }
            else if (asset.kind == Session::Asset::Kind::eMaterial) {
                // A sphere of it, once the sphere is drawn; until then a disc of its colour — rimmed with the light it
                // gives when it gives any — or what it is.
                kui::Widget until = KindIcon(asset, size);
                if (const Session::Material* material = Session::MaterialOf(asset.id)) {
                    const auto channel = [](const float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
                    const kui::Color colour = kui::Color::Hex((channel(material->albedo[0]) << 16) | (channel(material->albedo[1]) << 8) | channel(material->albedo[2]));
                    const bool glows = material->emission[0] + material->emission[1] + material->emission[2] > 0.f;
                    until = kui::SizedBox(size, size, kui::Icon(glows ? "light_mode" : material->metallic > 0.5f ? "lens_blur" : "circle", kui::IconStyle::eFilled, colour));
                }
                return kui::Make<MeshThumbnail>(_session, [session = _session, id = asset.id] { return session->MaterialPicture(id); },
                    [size, until = std::move(until)](const kor::ResourceRef<const kor::Image> picture) {
                        return picture.Get() ? kui::SizedBox(size, size, kui::Image(picture, kui::ImageFit::eContain)) : until;
                    });
            }
            return KindIcon(asset, size);
        }

        /** A folder of the Assets panel, by its last name: `textures` for `assets/textures`. */
        static std::string FolderName(const std::string& folder)
        {
            const auto slash = folder.rfind('/');
            return slash == std::string::npos ? folder : folder.substr(slash + 1);
        }

        /** Goes to a folder of the Assets panel: what it shows is what is in it. */
        void ShowFolder(std::string folder)
        {
            SetState([&] { _session->assetFolder = std::move(folder); _session->assetModel.clear(); _session->assetFilter.clear(); _session->assetKind = -1; _folderPicked.clear(); });
        }

        /** Pressed once a thing is picked; pressed again at once, it is opened. */
        void Pressed(const std::string& key, const std::function<void()>& pick, const std::function<void()>& open)
        {
            const auto now = std::chrono::steady_clock::now();
            if (key == _tapped && now - _tappedAt < std::chrono::milliseconds(450)) {
                _tapped.clear();
                if (open) open();
            } else {
                _tapped = key;
                _tappedAt = now;
                if (pick) pick();
            }
        }

        /** One thing of the Assets panel: its picture over its name — or, at the smallest size, a line of a list. */
        kui::Widget Tile(const std::string& key, kui::Widget preview, const std::string& label, const bool picked, const bool faint, const float size,
                         std::function<void()> pressed, std::vector<kui::MenuItem> menu, const float lineWidth = 260.f, const std::string& under = {})
        {
            const kui::Theme& t = kui::Theme::Current();
            const kui::Color ink = picked ? t.text : faint ? t.textMuted : t.text.WithAlpha(0.9f);
            const kui::Decoration back = kui::Decoration {}.SetColor(picked ? t.primary.WithAlpha(0.32f) : kui::colors::Transparent).SetRadius(4.f);
            kui::Widget face;
            if (size <= ListSize) {
                // What it is of, after its name and fainter: a shader entry's module.
                kui::Widget name = kui::Text(label, kui::TextStyle {}.SetSize(13.f).SetColor(ink), kui::TextAlign::eStart, false, 1, true);
                if (!under.empty())
                    name = kui::Row({ kui::Flexible(std::move(name)), kui::Text(under, kui::TextStyle {}.SetSize(12.f).SetColor(t.textMuted), kui::TextAlign::eStart, false, 1, true) },
                                    kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                face = kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(6.f, 2.f)).SetDecoration(back),
                    kui::Row({ std::move(preview), kui::SizedBox(lineWidth, -1.f, std::move(name)) },
                             kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
            } else {
                // Its name over what it is of, a line each; or its name on two.
                kui::Widget name = under.empty()
                    ? kui::Text(label, kui::TextStyle {}.SetSize(11.f).SetColor(ink), kui::TextAlign::eCenter, true, 2, true)
                    : kui::Column({ kui::Text(label, kui::TextStyle {}.SetSize(11.f).SetColor(ink), kui::TextAlign::eCenter, false, 1, true),
                                    kui::Text(under, kui::TextStyle {}.SetSize(10.f).SetColor(t.textMuted), kui::TextAlign::eCenter, false, 1, true) },
                                  kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
                face = kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::All(4.f)).SetDecoration(back),
                    kui::Column({ std::move(preview), kui::SizedBox(size + 8.f, 28.f, std::move(name)) },
                                kui::FlexOptions {}.SetGap(3.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
            }
            kui::Widget tile = kui::Button(std::move(face), std::move(pressed), kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain));
            if (!menu.empty()) tile = kui::ContextMenu(std::move(menu), std::move(tile));
            return std::move(tile).Key(key);
        }

        /**
         * A pipeline's shader as a shader's file is shown in the panel: its stage's icon over its entry's name and its module;
         * with none in it, the icon faint over what it would be.
         */
        kui::Widget ShaderTile(const std::string& key, const std::string_view stage, const std::string& label, const std::string& module, const bool given,
                               std::function<void()> pressed, std::vector<kui::MenuItem> menu)
        {
            constexpr float Size = 64.f;
            kui::Widget icon = StageIcon(stage, Size);
            if (!given) icon = kui::Opacity(0.3f, std::move(icon));
            return Tile(key, std::move(icon), label, false, !given, Size, std::move(pressed), std::move(menu), 260.f, module);
        }

        /** What the right button offers of a file or a folder that is on disk. */
        std::vector<kui::MenuItem> FileMenu(const std::filesystem::path& path, const bool ofProject, const std::string& id = {})
        {
            std::vector<kui::MenuItem> menu {
                { "Show in the file manager", [this, path] { _session->ShowInFiles(path); } },
                { "Copy its path", [this, path] { kor::Window::SetClipboardText(path.generic_string()); _session->Say(std::format("Copied {}.", path.generic_string())); } },
                { {}, {}, true, true },
            };
            // An asset of the project's is known by its ID, not where it is: it can be put anywhere in it.
            if (!id.empty() && ofProject)
                menu.push_back({ "Rename or move...", [this, path, id] {
                    SetState([&] {
                        _dialog = Dialog::eMoveAsset;
                        _moving = id;
                        std::error_code ignored;
                        _newId = std::filesystem::relative(path, _session->CurrentProject().directory, ignored).generic_string();
                    });
                } });
            menu.push_back({ "Move to the trash...", [this, path] { SetState([&] { _dialog = Dialog::eTrash; _trash = path; }); }, ofProject });
            return menu;
        }

        /** What the right button offers where nothing is, in a folder: something made in it. */
        std::vector<kui::MenuItem> FolderMenu()
        {
            const std::filesystem::path here = _session->AssetFolderPath(_session->assetFolder);
            const bool project = !here.empty();
            return {
                { "New folder...", [this] { SetState([&] { _dialog = Dialog::eNewFolder; _newId.clear(); }); }, project },
                { "New shader...", [this] { SetState([&] { _dialog = Dialog::eNewShader; _newId.clear(); _newKind = 0; }); }, _session->HasProject() },
                { "New script...", [this] { SetState([&] { _dialog = Dialog::eNewScript; _newId.clear(); _newKind = 0; }); }, _session->HasProject() },
                { "Create prefab...", [this] { SetState([&] { _dialog = Dialog::eNewPrefab; _newId.clear(); }); }, _session->HasProject() },
                { "New material", [this] { _session->NewMaterial(); SetState(); }, _session->HasProject() },
                { "Import a model or a picture...", [this] { SetState([&] { _dialog = Dialog::eImportModel; }); } },
                { {}, {}, true, true },
                { "Show in the file manager", [this, here] { _session->ShowInFiles(here); }, project },
            };
        }

        /**
         * Everything the project is made of, as its folders have it: the folders down the side, what is in the
         * one shown beside them — each thing a picture and a name, as big as the slider says, a line each at its
         * least. Picked, the inspector says what a thing is; pressed twice, a folder is gone into, a shader or a
         * script is opened to be written, a model shows its meshes. A mesh or a texture is picked up and dropped
         * on the field of an entity that is to have it. Typed into, the search looks in every folder.
         */
        kui::Widget AssetsPanel()
        {
            using Kind = Session::Asset::Kind;
            const kui::Theme& t = kui::Theme::Current();
            auto assets = _session->Assets();
            const auto folders = _session->AssetFolders(assets);
            // A folder that has gone — thrown away, or of another project — is not shown: the project's own is.
            if (!_session->assetFolder.empty() && !std::ranges::contains(folders, _session->assetFolder)) _session->assetFolder.clear();
            const std::string& shown = _session->assetFolder;
            const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
            const std::string wanted = lower(_session->assetFilter);
            const bool searching = !wanted.empty() || _session->assetKind >= 0;
            const float size = std::clamp(_session->assetTileSize, ListSize, 128.f);
            const float preview = size <= ListSize ? 18.f : size;
            const std::string root = _session->HasProject() ? _session->CurrentProject().name : std::string("Project");

            // ---- what is shown: the folders in the one shown, then what is in it; or, looked for, whatever fits
            std::vector<kui::Widget> tiles;
            int pictures = 0;
            const auto tileOf = [&](const Session::Asset& asset) {
                // The interface shows 256 pictures at once and no more: the first of what is listed have theirs.
                const bool pictured = (asset.kind == Kind::eTexture || asset.kind == Kind::eMesh || asset.kind == Kind::eMaterial || asset.kind == Kind::eModel) && pictures < 160;
                if (pictured) ++pictures;
                const bool code = asset.kind == Kind::eShader || asset.kind == Kind::eScript;
                const bool ofProject = !asset.folder.starts_with(Session::OutsideFolder);
                std::string label = asset.label;
                if (asset.pending) label += " (reading)";
                std::vector<kui::MenuItem> menu;
                // (A module of entries, opened, shows them: its file is opened from here.)
                const bool module = asset.kind == Kind::eShader && asset.stage.empty();
                const bool entries = module && std::ranges::contains(assets, asset.id, &Session::Asset::parent);
                if (code) menu.push_back({ "Open in Visual Studio Code", [this, asset, module] { if (module) _session->OpenInCode(asset.file); else _session->OpenAsset(asset); } });
                if (entries) menu.push_back({ _session->assetsOpened.contains(asset.id) ? "Hide its entries" : "Show its entries", [this, asset] { _session->OpenAsset(asset); } });
                if (asset.kind == Kind::eModel) {
                    menu.push_back({ "Import into the scene...", [this, file = asset.file] { AskToImport(file); } });
                if (asset.kind == Kind::ePrefab) {
                    menu.push_back({ "Edit prefab", [this, id = asset.id] { _session->OpenPrefab(id); } });
                    menu.push_back({ "Make an instance", [this, id = asset.id] { _session->PlacePrefab(id); }, _session->PrefabOpen().empty() });
                }
                    menu.push_back({ "Open", [this, asset] { _session->OpenAsset(asset); } });
                }
                if (asset.kind != Kind::eMesh && !asset.builtIn) for (auto& item : FileMenu(asset.file, ofProject, asset.id)) menu.push_back(std::move(item));
                // A shader entry is named with its module: the one it is of, unless it is that module, of none.
                const std::string under = asset.kind == Kind::eShader && !asset.stage.empty() ? asset.group : std::string();
                kui::Widget tile = Tile("asset " + asset.id, AssetPreview(asset, preview, pictured), label, asset.id == _session->selectedAsset, !asset.loaded && !code && asset.kind != Kind::eScene, size,
                                        [this, asset] { Pressed("asset " + asset.id, [&] { _folderPicked.clear(); _session->SelectAsset(asset.id); }, [&] { _session->OpenAsset(asset); }); }, std::move(menu),
                                        260.f, under);
                // A shader module of entries: an arrow on it shows them after it, and hides them.
                if (entries && !searching) {
                    const bool open = _session->assetsOpened.contains(asset.id);
                    const char* what = open ? "Hide its entries" : "Show its entries";
                    tile = kui::Stack({ std::move(tile), kui::Positioned(kui::PositionedOptions { .top = size <= ListSize ? 0.f : size * 0.5f - 8.f, .right = 0.f },
                                                                         Tool(open ? "chevron_left" : "chevron_right", what, open, [this, asset] { _session->OpenAsset(asset); })) });
                }
                // A mesh or a texture is carried to the field that is to hold it, by the name it is found by.
                // (And a model to the scene or the outliner, to be imported; a script to an entity; a texture or a shader to the graph.)
                if (asset.kind != Kind::eScene) tile = kui::Draggable({ "asset", asset.id }, std::move(tile));
                tiles.push_back(std::move(tile));
            };
            // In a model: what it is made of, each kind under its heading.
            const auto within = std::ranges::find(assets, _session->assetModel, &Session::Asset::id);
            if (within == assets.end() || _session->assetModel.empty()) _session->assetModel.clear();
            const std::string& model = _session->assetModel;
            std::vector<kui::Widget> sections;
            if (searching) {
                for (const auto& asset : assets) {
                    if (_session->assetKind >= 0 && static_cast<int>(asset.kind) != _session->assetKind) continue;
                    // A mesh is found by what is typed, or asked for by its kind: not listed with everything else.
                    if (asset.kind == Kind::eMesh && wanted.empty() && _session->assetKind != static_cast<int>(Kind::eMesh)) continue;
                    if (!wanted.empty() && lower(asset.label).find(wanted) == std::string::npos && lower(asset.group).find(wanted) == std::string::npos) continue;
                    // The materials, a list of each kind the engine has and then the project's.
                    if (asset.kind == Kind::eMaterial && _session->assetKind == static_cast<int>(Kind::eMaterial) && asset.builtIn) continue;
                    tileOf(asset);
                }
                if (_session->assetKind == static_cast<int>(Kind::eMaterial)) {
                    auto ownTiles = std::exchange(tiles, {});
                    const auto listed = [&](const std::string& title, std::vector<kui::Widget> shownTiles) {
                        if (shownTiles.empty()) return;
                        const std::size_t count = shownTiles.size();
                        sections.push_back(ListHeading(std::format("{}  ·  {}", title, count)));
                        sections.push_back(size <= ListSize ? Rows(std::move(shownTiles), 1.f) : kui::Wrap(std::move(shownTiles), 6.f, 6.f));
                    };
                    for (const auto& [title, kind] : { std::pair<const char*, const char*> { "PBR", "PbrMaterial" }, { "Blinn-Phong", "BlinnPhongMaterial" } }) {
                        for (const auto& asset : assets)
                            if (asset.kind == Kind::eMaterial && asset.builtIn && asset.group == kind
                                && (wanted.empty() || lower(asset.label).find(wanted) != std::string::npos)) tileOf(asset);
                        listed(title, std::exchange(tiles, {}));
                    }
                    listed("The project's", std::move(ownTiles));
                }
            } else if (!model.empty()) {
                std::set<std::string> pictures_;
                if (const auto read = Doc().Runtime().ModelOf(within->file))
                    for (const auto& material : read->materials)
                        for (const std::string* map : { &material.albedoMap, &material.normalMap, &material.roughnessMap, &material.emissiveMap, &material.occlusionMap, &material.specularMap, &material.shininessMap, &material.alphaMap })
                            if (!map->empty()) pictures_.insert(*map);
                const auto section = [&](const char* title, const std::function<bool(const Session::Asset&)>& of) {
                    for (const auto& asset : assets) if (of(asset)) tileOf(asset);
                    if (tiles.empty()) return;
                    const std::size_t count = tiles.size();
                    sections.push_back(ListHeading(std::format("{}  ·  {}", title, count)));
                    sections.push_back(size <= ListSize ? Rows(std::exchange(tiles, {}), 1.f) : kui::Wrap(std::exchange(tiles, {}), 6.f, 6.f));
                };
                const auto part = [&](const Kind kind) { return [&, kind](const Session::Asset& asset) { return asset.kind == kind && asset.parent == model; }; };
                section("Meshes", part(Kind::eMesh));
                section("Materials", part(Kind::eMaterial));
                section("Textures", [&](const Session::Asset& asset) { return asset.kind == Kind::eTexture && pictures_.contains(asset.id); });
                section("Lights", part(Kind::eLight));
                section("Cameras", part(Kind::eCamera));
            } else {
                for (const auto& folder : folders) {
                    const auto slash = folder.rfind('/');
                    if ((slash == std::string::npos ? std::string() : folder.substr(0, slash)) != shown) continue;
                    const std::filesystem::path path = _session->AssetFolderPath(folder);
                    std::vector<kui::MenuItem> menu;
                    if (!path.empty()) menu = FileMenu(path, true);
                    menu.insert(menu.begin(), { "Open", [this, folder] { ShowFolder(folder); } });
                    tiles.push_back(Tile("folder " + folder, kui::SizedBox(preview, preview, kui::Icon("folder", kui::IconStyle::eFilled, kui::Color::Hex(0xC9A227))), FolderName(folder),
                                         folder == _folderPicked, false, size,
                                         [this, folder] { Pressed("folder " + folder, [&] { SetState([&] { _folderPicked = folder; }); }, [&] { ShowFolder(folder); }); },
                                         std::move(menu)));
                }
                // By name, as a folder is read; a model that is open has its meshes after it.
                // A shader's entries by their module's name, as it declares them.
                const auto named = [&](const Session::Asset& asset) { return lower(asset.kind == Kind::eShader ? asset.group : asset.label); };
                std::ranges::stable_sort(assets, [&](const Session::Asset& a, const Session::Asset& b) { return named(a) < named(b); });
                const auto entriesOf = [&](const Session::Asset& asset) {
                    if (asset.kind == Kind::eShader && _session->assetsOpened.contains(asset.id))
                        for (const auto& inner : assets)
                            if (inner.kind != Kind::eModel && inner.parent == asset.id) tileOf(inner);
                };
                if (_session->assetFiles && !_session->AssetFolderPath(shown).empty()) {
                    // As the folder is on disk: every file in it, by name. One that is an asset by itself — a model, a
                    // picture, a scene, a shader module — is shown as that asset; any other, a script's too, as a file.
                    std::map<std::string, const Session::Asset*> ofFile;
                    for (const auto& asset : assets)
                        if (!asset.file.empty() && asset.parent.empty() && !asset.builtIn && asset.kind != Kind::eScript && asset.kind != Kind::eMesh
                            && asset.kind != Kind::eLight && asset.kind != Kind::eCamera)
                            ofFile.try_emplace(asset.file.lexically_normal().generic_string(), &asset);
                    static const std::set<std::string> written { ".kt", ".kts", ".cs", ".csproj", ".json", ".md", ".txt", ".slang", ".toml", ".yaml", ".yml", ".xml",
                                                                 ".gradle", ".props", ".ini", ".cfg", ".glsl", ".hlsl", ".kmat", ".kgen", ".kfunc", ".prefab" };
                    for (const auto& file : _session->AssetFolderFiles(shown)) {
                        if (const auto it = ofFile.find(file.lexically_normal().generic_string()); it != ofFile.end()) {
                            tileOf(*it->second);
                            entriesOf(*it->second);
                            continue;
                        }
                        const std::string ending = lower(file.extension().string());
                        const bool source = ending == ".kt" || ending == ".kts" || ending == ".cs";
                        const bool text = written.contains(ending);
                        const std::string key = file.generic_string();
                        std::vector<kui::MenuItem> menu;
                        if (text) menu.push_back({ "Open in Visual Studio Code", [this, file] { _session->OpenInCode(file); } });
                        for (auto& item : FileMenu(file, true)) menu.push_back(std::move(item));
                        kui::Widget icon = kui::SizedBox(preview, preview, kui::Icon(source ? "code" : "description", kui::IconStyle::eFilled, source ? kui::Color::Hex(0x7DBB4B) : t.textMuted));
                        // Pressed twice, what is written is opened to be written; anything else shown where it is.
                        tiles.push_back(Tile("file " + key, std::move(icon), file.filename().string(), key == _folderPicked, false, size,
                                             [this, key, file, text] {
                                                 Pressed("file " + key, [&] { SetState([&] { _folderPicked = key; }); },
                                                         [&] { if (text) _session->OpenInCode(file); else _session->ShowInFiles(file); });
                                             }, std::move(menu)));
                    }
                }
                else for (const auto& asset : assets) {
                    // What is of a model is in the model; a module's entries, after it.
                    if (asset.folder != shown || asset.kind == Kind::eMesh || (asset.kind == Kind::eShader && !asset.parent.empty())
                        || (asset.kind == Kind::eMaterial && !asset.parent.empty())) continue;
                    tileOf(asset);
                    entriesOf(asset);
                }
            }
            kui::Widget content = !sections.empty() ? kui::Padding(kui::EdgeInsets::All(8.f), Rows(std::move(sections), 4.f))
                : !model.empty() ? kui::Padding(kui::EdgeInsets::All(12.f), kgui::Muted("Being read: what it is made of is shown here."))
                : tiles.empty()
                ? kui::Padding(kui::EdgeInsets::All(12.f), kgui::Muted(searching ? "Nothing of the project's is called that." : assets.empty() && folders.empty()
                    ? "Nothing yet. A model is imported with Assets, Import model; a project's shaders, scripts and the files in its folders are shown here." : "This folder is empty."))
                : kui::Padding(kui::EdgeInsets::All(8.f), size <= ListSize ? Rows(std::move(tiles), 1.f) : kui::Wrap(std::move(tiles), 6.f, 6.f));

            // ---- where it is: the folders to it, each to go back to; or what is looked for
            std::vector<kui::Widget> crumbs;
            const auto crumb = [&](const std::string& label, std::string folder, const bool last) {
                kui::TextStyle style = kui::TextStyle {}.SetSize(12.f).SetColor(last ? t.text : t.textMuted);
                crumbs.push_back(kui::Button(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kui::Text(label, style, kui::TextAlign::eStart, false)),
                                             [this, folder = std::move(folder)] { ShowFolder(folder); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)));
                if (!last) crumbs.push_back(kgui::Muted(">", 12.f));
            };
            if (searching) {
                const char* kinds[] { "models", "meshes", "textures", "shaders", "scripts", "scenes", "prefabs", "materials", "generators", "functions" };
                const std::string what = _session->assetKind >= 0 ? std::string("All ") + kinds[std::clamp(_session->assetKind, 0, 9)] : std::string("Everything");
                crumbs.push_back(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kgui::Muted(wanted.empty() ? std::format("{}, in every folder", what) : std::format("{} called '{}', in every folder", what, _session->assetFilter), 12.f)));
            } else {
                crumb(root, {}, shown.empty() && model.empty());
                for (std::size_t at = 0; at < shown.size();) {
                    const auto slash = shown.find('/', at);
                    const std::size_t end = slash == std::string::npos ? shown.size() : slash;
                    crumb(shown.substr(at, end - at), shown.substr(0, end), end == shown.size() && model.empty());
                    at = end + 1;
                }
                if (!model.empty()) crumb(within->label, shown, true);
            }

            // ---- the folders, down the side
            const std::function<kui::Widget(const std::string&)> branch = [&](const std::string& folder) -> kui::Widget {
                std::vector<kui::Widget> under;
                const bool open = !_foldersShut.contains(folder);
                bool any = false;
                for (const auto& other : folders) {
                    const auto slash = other.rfind('/');
                    if ((slash == std::string::npos ? std::string() : other.substr(0, slash)) != folder) continue;
                    if (other == "Materials" || other == "Meshes" || other.starts_with("Materials/") || other.starts_with("Meshes/")) continue;    // of no file: under Built in
                    any = true;
                    if (open) under.push_back(branch(other));
                }
                // The models in it, each gone into as a folder is.
                for (const auto& asset : assets) {
                    if (asset.kind != Kind::eModel || asset.folder != folder) continue;
                    any = true;
                    if (!open) continue;
                    const bool inside = !searching && asset.id == _session->assetModel;
                    kui::Widget line = kui::Row({ kui::SizedBox(14.f, 14.f, kui::Icon("category", kui::IconStyle::eFilled, kui::Color::Hex(0x7DBB4B))),
                                                  kui::Expanded(kui::Text(asset.label, kui::TextStyle {}.SetSize(13.f).SetColor(inside ? t.primary : t.text), kui::TextAlign::eStart, false, 1, true)) },
                                                kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                    under.push_back(kui::TreeNode({}, false, [](bool) {}, {}, kui::TreeNodeOptions {}.SetLeaf(true).SetSelected(inside).SetIndent(14.f)
                        .OnTap([this, asset] { SetState([&] { _session->OpenAsset(asset); }); }).SetTitle(std::move(line))));
                }
                const bool here = !searching && folder == shown && _session->assetModel.empty();
                kui::Widget title = kui::Row({ kui::SizedBox(14.f, 14.f, kui::Icon(open && any ? "folder_open" : "folder", kui::IconStyle::eFilled, kui::Color::Hex(0xC9A227))),
                                               kui::Expanded(kui::Text(folder.empty() ? root : FolderName(folder), kui::TextStyle {}.SetSize(13.f).SetColor(here ? t.primary : t.text), kui::TextAlign::eStart, false, 1, true)) },
                                             kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                return kui::TreeNode({}, open, [this, folder](const bool nowOpen) { SetState([&] { if (nowOpen) _foldersShut.erase(folder); else _foldersShut.insert(folder); }); }, std::move(under),
                                     kui::TreeNodeOptions {}.SetLeaf(!any).SetSelected(here).SetIndent(14.f).OnTap([this, folder] { ShowFolder(folder); }).SetTitle(std::move(title)));
            };
            // The project's folders first; under them, everything of one kind, wherever it is.
            std::vector<kui::Widget> side;
            const auto group = [&](const char* title, const char* shut, const std::vector<std::pair<const char*, Kind>>& members) {
                std::vector<kui::Widget> lines;
                for (const auto& [label, kind] : members) {
                    const bool on = _session->assetKind == static_cast<int>(kind);
                    const auto count = std::ranges::count(assets, kind, &Session::Asset::kind);
                    lines.push_back(kui::TreeNode(std::format("{}  ·  {}", label, count), false, [](bool) {}, {}, kui::TreeNodeOptions {}.SetLeaf(true).SetSelected(on).SetIndent(14.f).OnTap([this, kind, on] {
                        SetState([&] { _session->assetKind = on ? -1 : static_cast<int>(kind); });
                    })));
                }
                side.push_back(kui::TreeNode(title, !_foldersShut.contains(shut), [this, shut = std::string(shut)](const bool open) { SetState([&] { if (open) _foldersShut.erase(shut); else _foldersShut.insert(shut); }); },
                                             std::move(lines), kui::TreeNodeOptions {}.SetIndent(14.f)));
            };
            // What a pipeline uses as data, and what is opened and edited as a whole: wherever each is.
            group("Resources", "*resources", { { "Models", Kind::eModel }, { "Meshes", Kind::eMesh }, { "Materials", Kind::eMaterial }, { "Textures", Kind::eTexture },
                                               { "Shaders", Kind::eShader }, { "Generators", Kind::eGenerator } });
            group("Documents", "*documents", { { "Scenes", Kind::eScene }, { "Prefabs", Kind::ePrefab }, { "Functions", Kind::eFunction }, { "Scripts", Kind::eScript } });
            // Then where it all is on disk.
            side.push_back(ListHeading("FOLDERS"));
            side.push_back(branch({}));
            // And what every project has, of no file.
            side.push_back(ListHeading("BUILT IN"));
            for (const auto& [label, folder] : { std::pair<const char*, const char*> { "PBR materials", "Materials/PBR" }, { "Blinn-Phong materials", "Materials/Blinn-Phong" } }) {
                const bool here = !searching && _session->assetFolder == folder;
                side.push_back(kui::TreeNode(label, false, [](bool) {}, {}, kui::TreeNodeOptions {}.SetLeaf(true).SetSelected(here).SetIndent(14.f).OnTap([this, folder = std::string(folder)] { ShowFolder(folder); })));
            }

            // ---- over it all: something made, something looked for; under it: what is picked, and how big things are shown
            const std::vector<std::string> made { "Folder...", "Shader...", "Script...", "Import a model or a picture...", "Prefab of the picked entity", "Prefab...", "Material" };
            const std::vector<std::string> kindNames { "Every kind", "Models", "Meshes", "Textures", "Shaders", "Scripts", "Scenes", "Prefabs", "Materials", "Generators", "Functions" };
            kui::Widget bar = kui::Row({
                kui::Tooltip("Makes something in the project: a folder here, a shader, a script, a prefab of the picked entity; or brings a model's or a picture's file in.",
                    kui::Dropdown(made, -1, [this](const int index) {
                        if (index == 6) { _session->NewMaterial(); SetState(); return; }
                        if (index == 5) { SetState([&] { _dialog = Dialog::eNewPrefab; _newId.clear(); }); return; }
                        if (index == 4) {
                            // The entity picked, and what is under it: a prefab, and it its first instance.
                            if (Doc().World().Alive(_session->selectedEntity)) _session->SaveAsPrefab(_session->selectedEntity);
                            else _session->Say("Pick an entity in the scene tree first, or drag one here: it is saved as a prefab.", true);
                            SetState();
                            return;
                        }
                        SetState([&] {
                            _newId.clear();
                            _newKind = 0;
                            _dialog = index == 0 ? Dialog::eNewFolder : index == 1 ? Dialog::eNewShader : index == 2 ? Dialog::eNewScript : Dialog::eImportModel;
                        });
                    }, kui::DropdownOptions {}.SetPlaceholder("+  Create").SetWidth(120.f))),
                // What it is made of, or its files as they are on disk.
                kui::Row({
                    Tool("category", "Assets: what the project is made of — the scripts' components, Data, kinds, models, pictures — and only the folders they are in",
                         !_session->assetFiles, [this] { SetState([&] { _session->SetAssetFiles(false); _folderPicked.clear(); }); }),
                    Tool("folder_open", "Files: the folders as they are on disk, empty ones too, and every file in them",
                         _session->assetFiles, [this] { SetState([&] { _session->SetAssetFiles(true); _folderPicked.clear(); }); }),
                }, kui::FlexOptions {}.SetGap(2.f)),
                kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = _session->assetFilter, .placeholder = "Search: part of a name, in every folder",
                    .onChanged = [this](const std::string& typed) { SetState([&] { _session->assetFilter = typed; }); } }).Key("asset filter")),
                kui::Tooltip("Shows only one kind of thing, wherever it is.",
                    kui::Dropdown(kindNames, _session->assetKind + 1, [this](const int index) { SetState([&] { _session->assetKind = index - 1; }); }, kui::DropdownOptions {}.SetWidth(130.f))),
            }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));

            std::string picked;
            if (const auto it = std::ranges::find(assets, _session->selectedAsset, &Session::Asset::id); it != assets.end() && !_session->selectedAsset.empty())
                picked = it->builtIn ? std::format("{}, the editor's own", it->label) : it->kind == Kind::eMesh ? std::format("{}, of {}", it->label, it->file.generic_string()) : it->file.generic_string();
            else if (!_folderPicked.empty()) picked = _folderPicked;
            kui::Widget foot = kui::Row({
                kui::Expanded(kui::Text(picked, kui::TextStyle {}.SetSize(11.f).SetColor(t.textMuted), kui::TextAlign::eStart, false, 1, true)),
                kui::Tooltip("How big each thing is shown. At its least, a line each.",
                    kui::SizedBox(110.f, -1.f, kui::Slider(size, [this](const float value) { SetState([&] { _session->assetTileSize = value; }); }, ListSize, 128.f))),
            }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));

            // An entity dragged here from the outliner is saved as a prefab — it, and what is under it — and is its first instance.
            return kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("entity").OnDrop([this](const kui::DragData& dragged, kor::Vec2) {
                if (const auto* entity = dragged.As<kecs::Entity>()) _session->SaveAsPrefab(*entity);
                SetState();
            }), kui::Column({
                kui::Padding(kui::EdgeInsets { 8.f, 6.f, 8.f, 4.f }, std::move(bar)),
                kui::Separator(),
                kui::Expanded(kui::Row({
                    kui::ScrollView(kui::Padding(kui::EdgeInsets::All(6.f), Rows(std::move(side), 1.f))).Width(190.f),
                    kui::Separator(kui::Axis::eVertical),
                    kui::Expanded(kui::Column({
                        kui::Padding(kui::EdgeInsets { 6.f, 3.f, 6.f, 3.f }, kui::Row(std::move(crumbs), kui::FlexOptions {}.SetGap(2.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))),
                        kui::Separator(),
                        kui::Expanded(kui::ContextMenu(FolderMenu(), kui::ScrollView(std::move(content)))),
                    }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch))),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch))),
                kui::Separator(),
                kui::Padding(kui::EdgeInsets { 8.f, 2.f, 8.f, 2.f }, std::move(foot)),
            }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch)));
        }

        /** A name, for a folder to be made in the one the Assets panel shows. */
        kui::Widget NewFolderDialog()
        {
            const auto make = [this] {
                if (_session->NewAssetFolder(_newId)) SetState([&] { _dialog = Dialog::eNone; });
                else SetState();
            };
            std::vector<kui::Widget> rows;
            rows.push_back(Heading("A new folder"));
            rows.push_back(kgui::Muted(std::format("In {}.", _session->assetFolder.empty() ? std::string("the project's folder") : _session->assetFolder)));
            rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name",
                                                                  .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                  .onSubmitted = [this, make](const std::string& typed) { _newId = typed; make(); }, .width = 420.f, .focus = 1 }));
            if (_session->Failed()) rows.push_back(Problem(_session->Status()));
            rows.push_back(kui::Row({ kui::Button("Make it", make), kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                    kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(460.f));
        }

        /** A name, for another scene of the project's: made as the standard one is, or lit, and opened. */
        kui::Widget NewSceneDialog()
        {
            const auto make = [this](const bool lit) {
                if (_session->AddScene(_newId, lit)) SetState([&] { _dialog = Dialog::eNone; });
                else SetState();
            };
            std::vector<kui::Widget> rows;
            rows.push_back(Heading("Another scene"));
            rows.push_back(kgui::Muted("Of the project's, in scenes/: the one open is saved first."));
            rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name",
                                                                  .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                  .onSubmitted = [this, make](const std::string& typed) { _newId = typed; make(false); }, .width = 420.f, .focus = 1 }));
            if (_session->Failed()) rows.push_back(Problem(_session->Status()));
            rows.push_back(kui::Row({ kui::Button("Make it", [make] { make(false); }), kui::Button("Make it lit", [make] { make(true); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)),
                                      kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                    kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(460.f));
        }

        /** A name, for a prefab of one empty entity to be made and opened: what is put in it is made there. */
        kui::Widget NewPrefabDialog()
        {
            const auto make = [this] {
                if (_session->NewPrefab(_newId)) SetState([&] { _dialog = Dialog::eNone; });
                else SetState();
            };
            std::vector<kui::Widget> rows;
            rows.push_back(Heading("A new prefab"));
            rows.push_back(kgui::Muted("In prefabs. It opens by itself, one empty entity of its name: what is made under it is the prefab's."));
            rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name: Crate, Street lamp",
                                                                  .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                  .onSubmitted = [this, make](const std::string& typed) { _newId = typed; make(); }, .width = 420.f, .focus = 1 }));
            if (_session->Failed()) rows.push_back(Problem(_session->Status()));
            rows.push_back(kui::Row({ kui::Button("Make it", make), kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                    kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(460.f));
        }

        /** Asked before a file or a folder of the project's goes to the trash. */
        kui::Widget TrashDialog()
        {
            const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; }); };
            std::error_code ignored;
            const bool folder = std::filesystem::is_directory(_trash, ignored);
            std::vector<kui::Widget> rows {
                Heading(std::format("Move {} to the trash?", _trash.filename().string())),
                kgui::Line(folder ? "The folder and everything in it goes to the system's trash, and can be had back from there."
                                  : "It goes to the system's trash, and can be had back from there."),
            };
            // What names it has nothing once it is gone: said before, not found after.
            if (!folder)
                if (const auto users = _session->AssetUsers("@" + _trash.generic_string()); !users.empty()) {
                    rows.push_back(Problem(std::format("{} {} it:", users.size(), users.size() == 1 ? "file names" : "files name")));
                    for (std::size_t i = 0; i < users.size() && i < 8; ++i) rows.push_back(kgui::Muted(users[i]));
                    if (users.size() > 8) rows.push_back(kgui::Muted(std::format("and {} more", users.size() - 8)));
                }
            rows.push_back(kgui::Muted("Whatever of the scene is given it — a pipeline its shader, an entity its picture — says so among the problems until it is given something else."));
            rows.push_back(
                kui::Row({ kui::Button("Move it to the trash", [this, close] { const auto path = _trash; close(); _session->TrashAsset(path); }),
                           kui::Button("Cancel", close, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) }, kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(10.f)).Width(520.f));
        }

        /** Where the game is to be written: a folder, empty — out of the project, or in its build folder. */
        kui::Widget ExportGameDialog()
        {
            const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; }); };
            const auto build = [this] {
                if (_session->ExportGame(_newId)) SetState([&] { _dialog = Dialog::eNone; });
                else SetState();
            };
            std::vector<kui::Widget> rows;
            rows.push_back(Heading("Export the game"));
            rows.push_back(kgui::Muted("Every scene of the project and what they have — nothing else — with its shaders and scripts, each model read ahead. "
                                       "koral-play runs the folder."));
            rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "A folder: empty, out of the project or in its build folder",
                                                                  .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                  .onSubmitted = [this, build](const std::string& typed) { _newId = typed; build(); }, .width = 480.f, .focus = 1 }));
            if (_session->Failed()) rows.push_back(Problem(_session->Status()));
            rows.push_back(kui::Row({ kui::Button("Export", build), kui::Button("Cancel", close, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                    kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(520.f));
        }

        /** Where an asset is to be: its path from the project's folder, typed. What names it still does, by its ID. */
        kui::Widget MoveAssetDialog()
        {
            const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; }); };
            const auto move = [this] {
                if (_session->MoveAsset(_moving, _newId)) SetState([&] { _dialog = Dialog::eNone; });
                else SetState();
            };
            std::vector<kui::Widget> rows;
            rows.push_back(Heading("Rename or move"));
            rows.push_back(kgui::Muted("Its path from the project's folder. Its meta goes with it — a model's files with it too — and what names it still does."));
            rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "assets/textures/wall.png",
                                                                  .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                  .onSubmitted = [this, move](const std::string& typed) { _newId = typed; move(); }, .width = 480.f, .focus = 1 }));
            if (_session->Doc().Dirty()) rows.push_back(kgui::Muted("The scene is saved first."));
            if (_session->Failed()) rows.push_back(Problem(_session->Status()));
            rows.push_back(kui::Row({ kui::Button("Move it", move), kui::Button("Cancel", close, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                    kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(520.f));
        }

        /** What a picked asset is: where its file is, how big it is, and what is done with it. */
        /** Which of a model's attributes are read, and which buffer each read one is in (0, 1...): an import's settings, as chosen. */
        struct AttributeChoice {
            std::set<std::string> leaveOut;
            std::map<std::string, int> buffers;
        };
        static AttributeChoice ChoiceOf(const std::vector<kgraph::Model::Attribute>& attributes, const kgraph::MeshImport& import)
        {
            AttributeChoice choice { { import.leaveOut.begin(), import.leaveOut.end() }, {} };
            for (const auto& attribute : attributes) {
                choice.buffers[attribute.semantic] = static_cast<int>(import.buffers.size());     // what none names: in the one after them
                for (std::size_t b = 0; b < import.buffers.size(); ++b)
                    if (std::ranges::contains(import.buffers[b], attribute.semantic)) choice.buffers[attribute.semantic] = static_cast<int>(b);
            }
            return choice;
        }
        /** The choice as the graph keeps it: a buffer each, in order — nothing at all for every attribute in one. */
        static kgraph::MeshImport ImportOf(const std::vector<kgraph::Model::Attribute>& attributes, AttributeChoice choice)
        {
            kgraph::MeshImport import;
            choice.leaveOut.erase("POSITION");
            for (const auto& attribute : attributes) if (choice.leaveOut.contains(attribute.semantic)) import.leaveOut.push_back(attribute.semantic);
            int last = 0;
            for (const auto& attribute : attributes) if (!choice.leaveOut.contains(attribute.semantic)) last = std::max(last, choice.buffers[attribute.semantic]);
            if (last > 0)
                for (int b = 0; b <= last; ++b) {
                    std::vector<std::string> group;
                    for (const auto& attribute : attributes)
                        if (!choice.leaveOut.contains(attribute.semantic) && choice.buffers[attribute.semantic] == b) group.push_back(attribute.semantic);
                    if (!group.empty()) import.buffers.push_back(std::move(group));
                }
            return import;
        }

        /**
         * A model's attributes to choose from: each read or left out — then not held at all — and the vertex buffer
         * each read one is in; the usual ways of splitting them; and what a vertex then holds. @p changed hears each change.
         */
        std::vector<kui::Widget> AttributeChoices(const std::vector<kgraph::Model::Attribute>& attributes, const AttributeChoice& choice,
                                                  const std::function<void(AttributeChoice)>& changed)
        {
            std::vector<kui::Widget> rows;
            std::vector<std::vector<kui::Widget>> table;
            std::uint32_t bytes = 0;
            std::set<int> used;
            for (const auto& attribute : attributes) {
                const bool read = !choice.leaveOut.contains(attribute.semantic);
                const int buffer = choice.buffers.contains(attribute.semantic) ? choice.buffers.at(attribute.semantic) : 0;
                if (read) { bytes += attribute.count * 4; used.insert(buffer); }
                kui::Widget toggle = attribute.semantic == "POSITION" ? kgui::Muted("always")
                    : kui::Checkbox(read, [changed, choice = AttributeChoice(choice), semantic = attribute.semantic](const bool on) mutable {
                          if (on) choice.leaveOut.erase(semantic); else choice.leaveOut.insert(semantic);
                          changed(choice);
                      });
                kui::Widget in = !read ? kgui::Muted("not held")
                    : kui::Dropdown({ "Buffer 1", "Buffer 2", "Buffer 3", "Buffer 4" }, std::min(buffer, 3),
                                    [changed, choice = AttributeChoice(choice), semantic = attribute.semantic](const int b) mutable { choice.buffers[semantic] = b; changed(choice); });
                table.push_back({ kgui::Line(attribute.semantic), kgui::Line(std::format("{}{}", attribute.type, attribute.count == 1 ? std::string() : std::to_string(attribute.count))),
                                  std::move(toggle), std::move(in) });
            }
            rows.push_back(kui::Table({ { "attribute" }, { "as", 64.f }, { "read", 64.f }, { "in", 120.f } }, std::move(table), kui::TableOptions {}.SetRowHeight(30.f)));
            rows.push_back(kgui::Muted(std::format("Each vertex holds {} bytes, in {} {}. What is not read is not held at all.", bytes, used.size(), used.size() == 1 ? "buffer" : "buffers"), 12.f));
            // The usual ways of splitting them.
            const auto split = [&](const auto& placeOf) {
                AttributeChoice placed = choice;
                for (const auto& attribute : attributes) placed.buffers[attribute.semantic] = placeOf(attribute.semantic);
                return placed;
            };
            const auto bone = [](const std::string& s) { return s.starts_with("BONE"); };
            const auto surface = [](const std::string& s) { return s == "NORMAL" || s.starts_with("TANGENT") || s.starts_with("BITANGENT"); };
            std::vector<kui::Widget> presets {
                Chip("All in one buffer", [changed, all = split([](const std::string&) { return 0; })] { changed(all); }),
                Chip("Positions and UVs · surface · bones", [changed, three = split([&](const std::string& s) { return bone(s) ? 2 : surface(s) ? 1 : s == "POSITION" || s.starts_with("UV") ? 0 : 1; })] {
                    changed(three);
                }),
                Chip("Positions apart", [changed, apart = split([](const std::string& s) { return s == "POSITION" ? 0 : 1; })] { changed(apart); }),
            };
            if (!choice.leaveOut.empty()) presets.push_back(Chip("Read everything", [changed, choice = AttributeChoice(choice)]() mutable { choice.leaveOut.clear(); changed(choice); }));
            rows.push_back(kui::Wrap(std::move(presets), 6.f, 6.f));
            return rows;
        }

        /**
         * How a model's meshes are read: each attribute its file has, read or left out — not held at all — and which
         * vertex buffer it is in. Changed, the file is read again, and what holds its meshes holds the new ones.
         */
        std::vector<kui::Widget> MeshImportRows(const Session::Asset& asset)
        {
            std::vector<kui::Widget> rows;
            rows.push_back(kui::Separator());
            rows.push_back(Heading("How its meshes are read"));
            const std::string file = asset.id.starts_with('@') ? asset.id.substr(1) : asset.id;
            std::string error;
            const auto attributes = Doc().Runtime().ModelAttributes(asset.file, &error);
            if (!attributes) {
                rows.push_back(kgui::Muted(error.empty() ? "Reading it, to see what its meshes have." : error));
                return rows;
            }
            if (attributes->empty()) { rows.push_back(kgui::Muted("Its meshes have nothing to choose from.")); return rows; }
            for (auto& row : AttributeChoices(*attributes, ChoiceOf(*attributes, Doc().MeshImportOf(file)), [this, file, attributes = *attributes](AttributeChoice changed) {
                     Doc().SetMeshImport(file, ImportOf(attributes, std::move(changed)));
                     _session->Say(std::format("{} is read again, as asked.", std::filesystem::path(file).filename().string()));
                 }))
                rows.push_back(std::move(row));
            return rows;
        }

        // ---- importing a model: what of it is read, asked first ------------------------------------------------

        /** Asks how @p file is to be read — its attributes, and their buffers — before it is imported. */
        void AskToImport(std::filesystem::path file)
        {
            SetState([&] {
                _importFile = std::move(file);
                _importAttributes.reset();
                _importChoice = {};
                _dialog = Dialog::eImportOptions;
            });
        }

        // ---- compressing the scene's pictures ----------------------------------------------------------------------

        /** A picture as the Compress textures dialog has it: what it is, and the format chosen for it — none, as its kind is offered. */
        struct TextureChoice { Session::SceneTexture texture; std::optional<kgraph::TextureFormat> format; };
        /** A pack as the dialog has it: whether it is to be made, and whether it was only suggested. */
        struct PackChoice { std::string id; kgraph::TexturePack pack; bool on = true; bool suggested = false; };
        std::vector<TextureChoice> _textureChoices;
        std::vector<PackChoice> _packChoices;
        std::array<kgraph::TextureFormat, 4> _offered {};      // by kind
        kgraph::TextureQuality _textureQuality = kgraph::TextureQuality::eNormal;
        
        static constexpr std::array<kgraph::TextureKind, 4> TextureKinds { kgraph::TextureKind::eColor, kgraph::TextureKind::eNormal, kgraph::TextureKind::eMask, kgraph::TextureKind::eData };
        static const char* KindLabel(const kgraph::TextureKind kind)
        {
            switch (kind) {
            case kgraph::TextureKind::eNormal: return "Normal maps";
            case kgraph::TextureKind::eMask: return "Masks, one number";
            case kgraph::TextureKind::eData: return "Numbers together";
            default: return "Colours";
            }
        }
        static std::string FormatLabel(const kgraph::TextureFormat format)
        {
            switch (format) {
            case kgraph::TextureFormat::eBC1: return "BC1 · RGB, 4 bits";
            case kgraph::TextureFormat::eBC3: return "BC3 · RGBA, 8 bits";
            case kgraph::TextureFormat::eBC4: return "BC4 · one channel, 4 bits";
            case kgraph::TextureFormat::eBC5: return "BC5 · two channels, 8 bits";
            case kgraph::TextureFormat::eBC7: return "BC7 · RGBA, best, 8 bits";
            default: return "As read, not compressed";
            }
        }
        static std::vector<std::string> FormatLabels()
        {
            std::vector<std::string> labels;
            for (const auto format : kgraph::TextureFormats()) labels.push_back(FormatLabel(format));
            return labels;
        }
        static int FormatIndex(const kgraph::TextureFormat format)
        {
            const auto& all = kgraph::TextureFormats();
            return static_cast<int>(std::ranges::find(all, format) - all.begin());
        }
        /** A picture's own name, without its folder: `wall_Normal.png`. */
        static std::string PictureName(const std::string_view id)
        {
            const auto slash = id.find_last_of('/');
            return std::string(slash == std::string_view::npos ? id.substr(id.starts_with('@') ? 1 : 0) : id.substr(slash + 1));
        }

        /** Opens the dialog with the scene's pictures as the graph keeps them, and the packs it has and those their names suggest. */
        void OpenCompressTextures()
        {
            const kgraph::Graph& graph = Doc().Graph();
            _textureChoices.clear();
            std::vector<std::string> ids;
            for (auto& texture : _session->SceneTextures()) {
                ids.push_back(texture.id);
                const std::optional<kgraph::TextureFormat> format = texture.import.Default() ? std::nullopt : std::optional(texture.import.format);
                if (!texture.import.Default()) _textureQuality = texture.import.quality;
                _textureChoices.push_back({ std::move(texture), format });
            }
            _packChoices.clear();
            for (const auto& [id, pack] : graph.texturePacks) { _packChoices.push_back({ id, pack, true, false }); _textureQuality = pack.quality; }
            for (auto& [id, pack] : kgraph::SuggestTexturePacks(ids))
                if (!graph.texturePacks.contains(id)) _packChoices.push_back({ id, std::move(pack), true, true });
            for (std::size_t k = 0; k < TextureKinds.size(); ++k) _offered[k] = _session->OfferedFormat(TextureKinds[k]);
            SetState([&] { _dialog = Dialog::eCompressTextures; });
        }

        /** The pack a picture goes into, of those to be made; empty for none. */
        [[nodiscard]] std::string PackedIn(const std::string& id) const
        {
            for (const auto& choice : _packChoices)
                if (choice.on && std::ranges::any_of(choice.pack.channels, [&](const auto& channel) { return channel.from == id; })) return choice.id;
            return {};
        }
        [[nodiscard]] kgraph::TextureFormat FormatOf(const TextureChoice& choice) const
        {
            return choice.format.value_or(_offered[static_cast<std::size_t>(choice.texture.kind)]);
        }

        /** Keeps the dialog's choices in the graph and the project; with @p cook, compresses what is not yet. */
        void KeepTextureChoices(const bool cook)
        {
            std::map<std::string, kgraph::TextureImport> imports;
            for (const auto& choice : _textureChoices) {
                const kgraph::TextureFormat format = FormatOf(choice);
                if (choice.texture.compressible && format != kgraph::TextureFormat::eAsRead && PackedIn(choice.texture.id).empty())
                    imports[choice.texture.id] = { format, _textureQuality };
            }
            std::map<std::string, kgraph::TexturePack> packs;
            for (auto choice : _packChoices) {
                if (!choice.on) continue;
                choice.pack.quality = _textureQuality;
                packs[choice.id] = std::move(choice.pack);
            }
            for (std::size_t k = 0; k < TextureKinds.size(); ++k) _session->SetOfferedFormat(TextureKinds[k], _offered[k]);
            _session->KeepTextures(std::move(imports), std::move(packs), cook);
            SetState([&] { _dialog = Dialog::eNone; });
        }

        /** One pack's rows: whether it is made, its format, and what each of its channels is. */
        void PackRows(std::vector<kui::Widget>& rows, const std::size_t index)
        {
            PackChoice& choice = _packChoices[index];
            const std::string name = PictureName(choice.id);
            rows.push_back(kui::Row({
                kui::Expanded(kui::Checkbox(choice.on, [this, index](const bool on) { SetState([&] { _packChoices[index].on = on; }); },
                                            choice.suggested ? std::format("{}  (suggested)", name) : name)),
                kui::Dropdown(FormatLabels(), FormatIndex(choice.pack.format), [this, index](const int picked) {
                    SetState([&] { _packChoices[index].pack.format = kgraph::TextureFormats()[static_cast<std::size_t>(picked)]; });
                }, kui::DropdownOptions {}.SetWidth(220.f)),
            }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
            if (!choice.on) return;
            // What a channel can be: a number, or a channel of a picture of the scene's — of those whose names begin as the pack's, for one suggested.
            std::string base = PictureName(choice.id);
            if (const auto end = base.rfind("_ORM.pack"); end != std::string::npos) base = base.substr(0, end);
            else base.clear();
            std::vector<std::string> sources { "", "" }, labels { "0", "1" };
            for (const auto& texture : _textureChoices) {
                const std::string picture = PictureName(texture.texture.id);
                if (!base.empty() && !picture.starts_with(base)) continue;
                sources.push_back(texture.texture.id);
                labels.push_back(picture);
            }
            static constexpr std::array<const char*, 4> Names { "Red", "Green", "Blue", "Alpha" };
            for (std::size_t c = 0; c < 4; ++c) {
                const auto& channel = choice.pack.channels[c];
                int selected = channel.from.empty() ? (channel.constant >= 0.5f ? 1 : 0) : 0;
                if (!channel.from.empty()) {
                    if (const auto at = std::ranges::find(sources, channel.from); at != sources.end()) selected = static_cast<int>(at - sources.begin());
                    else { sources.push_back(channel.from); labels.push_back(PictureName(channel.from)); selected = static_cast<int>(sources.size()) - 1; }
                }
                std::vector<kui::Widget> line {
                    kui::Dropdown(labels, selected, [this, index, c, sources](const int picked) {
                        SetState([&] {
                            auto& into = _packChoices[index].pack.channels[c];
                            into.from = sources[static_cast<std::size_t>(picked)];
                            into.constant = picked == 1 ? 1.f : 0.f;
                            into.channel = 0;
                        });
                    }, kui::DropdownOptions {}.SetWidth(300.f)),
                };
                if (!channel.from.empty())
                    line.push_back(kui::Dropdown({ "its red", "its green", "its blue", "its alpha" }, std::clamp(channel.channel, 0, 3), [this, index, c](const int picked) {
                        SetState([&] { _packChoices[index].pack.channels[c].channel = picked; });
                    }, kui::DropdownOptions {}.SetWidth(110.f)));
                rows.push_back(kui::Padding(kui::EdgeInsets::Only(24.f, 0.f, 0.f, 0.f),
                    kgui::Labeled(Names[c], kui::Row(std::move(line), kui::FlexOptions {}.SetGap(6.f)), 60.f)));
            }
        }

        kui::Widget CompressTexturesDialog()
        {
            std::vector<kui::Widget> rows;
            rows.push_back(Heading("Compress textures"));
            const auto compressed = std::ranges::count_if(_textureChoices, [](const TextureChoice& choice) { return choice.texture.cooked; });
            const auto kept = std::ranges::count_if(_textureChoices, [](const TextureChoice& choice) { return !choice.texture.compressible; });
            rows.push_back(kgui::Muted(std::format(
                "{} textures in this scene, {} compressed{}. Each is compressed once, into the project's .koral/cache/textures — a few seconds a 4K texture, "
                "off the frame — and read from there from then on: smaller on the GPU, and much quicker to load. The scene still names the files; "
                "a file changed is compressed again.", _textureChoices.size() - kept, compressed,
                kept == 0 ? std::string() : std::format("; and {} HDR, kept as read — these formats hold eight bits a channel", kept))));
            if (const auto& cooking = _session->Cooking(); cooking.done < cooking.total)
                rows.push_back(kgui::Muted(std::format("Compressing {} of {}: {}", cooking.done + 1, cooking.total, PictureName(cooking.current))));

            rows.push_back(ListHeading("BY KIND"));
            std::array<int, 4> counts {};
            for (const auto& choice : _textureChoices) ++counts[static_cast<std::size_t>(choice.texture.kind)];
            for (std::size_t k = 0; k < TextureKinds.size(); ++k)
                rows.push_back(kgui::Labeled(std::format("{} ({})", KindLabel(TextureKinds[k]), counts[k]),
                    kui::Dropdown(FormatLabels(), FormatIndex(_offered[k]), [this, k](const int picked) {
                        SetState([&] { _offered[k] = kgraph::TextureFormats()[static_cast<std::size_t>(picked)]; });
                    }, kui::DropdownOptions {}.SetWidth(260.f)), 170.f));
            rows.push_back(kgui::Labeled("Quality", kui::Tooltip(
                "How hard the encoder works. Fast: about 4 s a 4K texture; normal: about 7 s; best: longer still, for a little more. "
                "Normal maps take longest.",
                kui::Dropdown({ "Fast", "Normal", "Best" }, static_cast<int>(_textureQuality), [this](const int picked) {
                    SetState([&] { _textureQuality = static_cast<kgraph::TextureQuality>(picked); });
                }, kui::DropdownOptions {}.SetWidth(260.f))), 170.f));
            rows.push_back(kgui::Muted("BC5 keeps a normal map's x and y; the shader works out z (the forward+ shaders do). BC4 keeps one channel and reads it in red, green and blue."));

            rows.push_back(ListHeading("PACKS", kui::Tooltip("A pack of your own: its channels taken from any of the scene's textures.", Small("Add a pack", [this] {
                std::string folder = "@";
                if (!_textureChoices.empty()) { const auto& first = _textureChoices.front().texture.id; folder = first.substr(0, first.find_last_of('/') + 1); }
                std::string id;
                for (int n = 1; id.empty() || std::ranges::any_of(_packChoices, [&](const PackChoice& choice) { return choice.id == id; }); ++n) id = std::format("{}pack{}.pack", folder, n);
                kgraph::TexturePack pack;
                pack.channels[3].constant = 1.f;
                SetState([&] { _packChoices.push_back({ id, pack, true, false }); });
            }))));
            rows.push_back(kgui::Muted("Several textures as the channels of one: one texture on the GPU in place of several. Each file in a pack still reads as itself — "
                                       "its channel, in red, green and blue — so the scene and its shaders need no change."));
            if (_packChoices.empty()) rows.push_back(kgui::Muted("None suggested: no material here has its occlusion, roughness and metalness in files of their own."));
            for (std::size_t i = 0; i < _packChoices.size(); ++i) PackRows(rows, i);

            rows.push_back(ListHeading("TEXTURES"));
            const kui::Theme& t = kui::Theme::Current();
            for (std::size_t i = 0; i < _textureChoices.size(); ++i) {
                const TextureChoice& choice = _textureChoices[i];
                // First, what its kind is offered as: what it follows until it is given one of its own.
                std::vector<std::string> labels { std::format("As its kind · {}", kgraph::TextureFormatName(_offered[static_cast<std::size_t>(choice.texture.kind)])) };
                for (const auto format : kgraph::TextureFormats()) labels.push_back(FormatLabel(format));
                const std::string packed = PackedIn(choice.texture.id);
                const kgraph::TextureFormat format = FormatOf(choice);
                std::string state = !choice.texture.compressible ? "kept as read"
                                  : !packed.empty() ? std::format("in {}", PictureName(packed))
                                  : format == kgraph::TextureFormat::eAsRead ? "not compressed"
                                  : choice.texture.cooked && choice.texture.import.format == format ? "compressed" : "to compress";
                kui::Widget pick = !packed.empty() || !choice.texture.compressible ? kui::SizedBox(240.f, 24.f)
                    : kui::Dropdown(labels, choice.format ? FormatIndex(*choice.format) + 1 : 0, [this, i](const int picked) {
                          SetState([&] { _textureChoices[i].format = picked == 0 ? std::nullopt : std::optional(kgraph::TextureFormats()[static_cast<std::size_t>(picked - 1)]); });
                      }, kui::DropdownOptions {}.SetWidth(240.f));
                rows.push_back(kui::Row({
                    kui::Expanded(kui::Tooltip(std::format("{}{}", choice.texture.id, choice.texture.field.empty() ? std::string() : "\nheld as " + choice.texture.field),
                                               kui::Text(PictureName(choice.texture.id), kui::TextStyle {}.SetSize(13.f), kui::TextAlign::eStart, false, 1, true))),
                    kui::SizedBox(110.f, 18.f, kgui::Muted(KindLabel(choice.texture.kind), 11.f)),
                    std::move(pick),
                    kui::SizedBox(110.f, 18.f, kui::Text(state, kui::TextStyle {}.SetSize(11.f).SetColor(state == "compressed" ? kui::Color::Hex(0x46A758) : t.textMuted)))
                }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
            }

            rows.push_back(kui::Row({
                kui::Button("Compress", [this] { KeepTextureChoices(true); }),
                kui::Tooltip("Keep these settings in the scene; nothing is compressed until you press Compress.",
                             kui::Button("Keep settings only", [this] { KeepTextureChoices(false); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary))),
                kui::Button("Cancel", [this] {
                    SetState([&] { _dialog = Dialog::eNone; });
                }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)),
            }, kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::ScrollView(Rows(std::move(rows), 8.f)).Width(820.f));
        }

        kui::Widget ImportOptionsDialog()
        {
            auto& runtime = Doc().Runtime();
            const std::string name = runtime.AssetId(_importFile).substr(1);
            std::vector<kui::Widget> rows;
            rows.push_back(Heading(std::format("Importing {}", _importFile.filename().string())));
            rows.push_back(kgui::Muted("What its meshes' vertices are read with. Each attribute left out is not read, and not held at all; each read one "
                                       "is in the vertex buffer chosen. It can be changed later, in the model's inspector."));
            std::string error;
            if (!_importAttributes) _importAttributes = runtime.ModelAttributes(_importFile, &error);
            if (_importAttributes) {
                // At first, as the scene reads it already (re-imported), or everything in one buffer.
                if (!_importChosen) { _importChoice = ChoiceOf(*_importAttributes, Doc().MeshImportOf(name)); _importChosen = true; }
                for (auto& row : AttributeChoices(*_importAttributes, _importChoice, [this](AttributeChoice changed) { SetState([&] { _importChoice = std::move(changed); }); }))
                    rows.push_back(std::move(row));
            } else {
                rows.push_back(error.empty() ? kgui::Muted("Reading the file, to see what its meshes have...") : Problem(error));
            }
            const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; _importChosen = false; }); };
            rows.push_back(kui::Row({
                kui::Button("Import", [this, name, close] {
                    if (_importAttributes) Doc().SetMeshImport(name, ImportOf(*_importAttributes, _importChoice));
                    const auto file = _importFile;
                    close();
                    _session->ImportModel(file);
                }),
                // (Not imported: where it was dropped is nothing to put the next thing placed at.)
                kui::Button("Cancel", [this, close] { _session->placeAt.reset(); close(); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)),
            }, kui::FlexOptions {}.SetGap(8.f)));
            return kui::Padding(kui::EdgeInsets::All(16.f), kui::ScrollView(Rows(std::move(rows), 8.f)).Width(620.f));
        }

        kui::Widget AssetInspector(const Session::Asset& asset)
        {
            std::vector<kui::Widget> rows;
            rows.push_back(Heading(asset.label));
            rows.push_back(kgui::Muted(asset.kind == Session::Asset::Kind::eModel ? "Model" : asset.kind == Session::Asset::Kind::eMesh ? "Mesh"
                                       : asset.kind == Session::Asset::Kind::eTexture ? "Texture" : asset.kind == Session::Asset::Kind::eShader ? StageOf(asset.stage).name
                                       : asset.kind == Session::Asset::Kind::eScene ? "Scene" : asset.kind == Session::Asset::Kind::ePrefab ? "Prefab"
                                       : asset.kind == Session::Asset::Kind::eMaterial ? "Material" : "Component"));
            if (asset.builtIn) rows.push_back(kgui::Labeled("File", kgui::Muted("none: the editor's own, in every project"), LabelWidth));
            else rows.push_back(kgui::Labeled("File", kgui::Muted(asset.file.generic_string()), LabelWidth));
            // What names it, as the project's files are saved.
            if (!asset.builtIn && asset.kind != Session::Asset::Kind::eMesh) {
                const auto users = _session->AssetUsers(asset.id);
                std::string said = users.empty() ? std::string("nothing saved names it") : users.front();
                for (std::size_t i = 1; i < users.size() && i < 4; ++i) said += ", " + users[i];
                if (users.size() > 4) said += std::format(" and {} more", users.size() - 4);
                rows.push_back(kgui::Labeled("Used by", kgui::Muted(said), LabelWidth));
            }
            const auto held = _session->AssetHeld(asset.id);
            switch (asset.kind) {
            case Session::Asset::Kind::eTexture:
                if (asset.pending) rows.push_back(kgui::Muted("Still being read."));
                else if (const auto image = PictureOf(held.get()); image.Get()) {
                    const auto extent = image->Extent();
                    rows.push_back(kgui::Labeled("Size", kgui::Line(std::format("{} x {}", extent.x, extent.y)), LabelWidth));
                    rows.push_back(kgui::Labeled("Levels", kgui::Line(std::to_string(image->MipLevels())), LabelWidth));
                    rows.push_back(kui::Image(kor::ResourceRef<const kor::Image>(image), kui::ImageFit::eContain, { -1.f, 220.f }));
                    rows.push_back(Small("Open in the viewer", [this, id = asset.id] { _session->View(id); }));
                } else rows.push_back(kgui::Muted("Not in the scene: it is read when something is given it."));
                rows.push_back(kgui::Labeled("Held as", kgui::Line(asset.id), LabelWidth));
                if (held) {
                    // How the scene keeps it: as read, or compressed — and whether what was compressed of it is what is read.
                    const kgraph::Graph& graph = Doc().Graph();
                    std::string kept = "as read, not compressed";
                    for (const auto& [pack, made] : graph.texturePacks)
                        if (std::ranges::any_of(made.channels, [&](const auto& channel) { return channel.from == asset.id; }))
                            kept = std::format("in {}, {}", PictureName(pack), Doc().Runtime().Cooked(pack) ? "compressed" : "not compressed yet");
                    if (!kgraph::TextureCompressible(asset.id)) kept = "as read: HDR, or compressed already";
                    else if (const auto it = graph.textureImports.find(asset.id); it != graph.textureImports.end())
                        kept = std::format("{}, {}", kgraph::TextureFormatName(it->second.format), Doc().Runtime().Cooked(asset.id) ? "compressed" : "not compressed yet");
                    rows.push_back(kgui::Labeled("Kept", kgui::Line(kept), LabelWidth));
                    rows.push_back(Small("Compress textures...", [this] { OpenCompressTextures(); }));
                }
                rows.push_back(kgui::Muted("Drag it from the Assets panel onto a picture field of an entity: an albedoMap, a normalMap."));
                break;
            case Session::Asset::Kind::eMesh:
                if (held && held->part) {
                    rows.push_back(kgui::Labeled("Triangles", kgui::Line(std::to_string(held->indexCount / 3)), LabelWidth));
                    rows.push_back(kgui::Labeled("Reaches", kgui::Line(std::format("{:.2f}, {:.2f}, {:.2f}  to  {:.2f}, {:.2f}, {:.2f}", held->bounds[0], held->bounds[1], held->bounds[2],
                                                                                   held->bounds[3], held->bounds[4], held->bounds[5])), LabelWidth));
                } else rows.push_back(kgui::Muted(asset.pending ? "Still being read." : "A mesh."));
                if (!asset.pending)
                    rows.push_back(kui::Make<MeshThumbnail>(_session, [session = _session, id = asset.id] { return session->MeshPicture(id); }, [](const kor::ResourceRef<const kor::Image> picture) {
                        return picture.Get() ? kui::Image(picture, kui::ImageFit::eContain, { -1.f, 160.f }) : kui::SizedBox(0.f, 0.f);
                    }));
                if (!asset.pending) rows.push_back(Small("Open in the viewer", [this, id = asset.id] { _session->View(id); }));
                rows.push_back(kgui::Labeled("Held as", kgui::Line(asset.id), LabelWidth));
                if (asset.builtIn) {
                    // What it is made from, as it starts: each entity that holds it tunes its own, in its Mesh.
                    const auto& parameters = kgraph::ShapeParameters(kgraph::ShapeOf(std::string_view(asset.id).substr(1)));
                    if (!parameters.empty()) rows.push_back(Heading("Parameters"));
                    for (const auto& parameter : parameters)
                        rows.push_back(kui::Tooltip(parameter.says, kgui::Labeled(parameter.name, kgui::Line(std::format("{}", parameter.initial)), LabelWidth)));
                    // Every default mesh alike, as the scene's graph says: what its shaders read of the surface's directions.
                    const std::vector<std::string> forms { "TANGENT, 4 numbers (w: the bitangent's side)", "TANGENT and BITANGENT, 3 numbers each", "Neither" };
                    const auto now = static_cast<int>(Doc().Graph().defaultMeshTangents);
                    rows.push_back(kgui::Labeled("Tangents", kui::Tooltip(
                        "What every default mesh has besides its position, normal and UV, for a normal map to be turned by: the way u grows along the surface "
                        "and the way v does. Four numbers is the tangent, w saying which side of it the bitangent is (bitangent = cross(normal, tangent.xyz) * w), "
                        "as glTF has it; three and three is how a model file's meshes are read, so that the two are laid out alike and can be drawn together. "
                        "A shader asks for TANGENT and BITANGENT by name. Saved with the scene's graph.",
                        kui::Dropdown(forms, now, [this](const int index) { Doc().SetDefaultMeshTangents(static_cast<kgraph::Tangents>(index)); SetState(); },
                                      kui::DropdownOptions {}.SetWidth(260.f))), LabelWidth));
                }
                rows.push_back(kgui::Muted(asset.builtIn ? "Drag it from the Assets panel onto an entity's Mesh: it starts as this, and the entity tunes it there. Entities with the same numbers share one mesh."
                                                         : "Drag it from the Assets panel onto an entity's Mesh."));
                break;
            case Session::Asset::Kind::eModel:
                rows.push_back(kgui::Muted("A model: imported, each thing in it is an entity with its mesh and its material's pictures."));
                rows.push_back(Small("Import into the scene...", [this, file = asset.file] { AskToImport(file); }));
                for (auto& row : MeshImportRows(asset)) rows.push_back(std::move(row));
                break;
            case Session::Asset::Kind::eShader: {
                const auto entries = _session->EntryPoints(asset.group);
                std::string list;
                for (const auto& entry : entries) list += (list.empty() ? "" : ", ") + entry;
                rows.push_back(kgui::Labeled("Module", kgui::Line(asset.group), LabelWidth));
                if (!asset.stage.empty()) {
                    const StageLook look = StageOf(asset.stage);
                    rows.push_back(kgui::Labeled("Entry", kgui::Line(asset.label), LabelWidth));
                    rows.push_back(kgui::Labeled("Stage", kui::Row({ StageIcon(asset.stage, 16.f), kgui::Line(look.name) },
                                                                   kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)), LabelWidth));
                }
                rows.push_back(kgui::Labeled(asset.stage.empty() ? "Entry points" : "In its module", kgui::Line(list.empty() ? std::string("none: a module others import") : list), LabelWidth));
                rows.push_back(Small("Open in Visual Studio Code", [this, asset] { if (asset.stage.empty()) _session->OpenInCode(asset.file); else _session->OpenAsset(asset); }));
                break;
            }
            case Session::Asset::Kind::eMaterial: {
                rows.push_back(AssetPreview(asset, 160.f));
                rows.push_back(Small("Open in the viewer", [this, id = asset.id] { _session->View(id); }));
                if (const auto toShow = _session->MaterialToShow(asset.id); toShow && !_session->HasShading(toShow->kind.name)) {
                    rows.push_back(kgui::Muted(std::format("{} has no shading: a module {}Shading, of the project's shaders, says how it is lit.", toShow->kind.name, toShow->kind.name)));
                    rows.push_back(Small("Write its shading", [this, kind = toShow->kind.name] { _session->WriteShadingModule(kind); SetState(); }));
                }
                if (const Session::Material* material = Session::MaterialOf(asset.id); material && !Doc().MaterialAssetOf(asset.id)) {
                    rows.push_back(kgui::Labeled("Kind", kgui::Line(material->kind), LabelWidth));
                    const auto said = [](const Value& value) {
                        if (const double* number = value.Number()) return std::format("{:.2f}", *number);
                        std::string text;
                        if (const auto* items = value.Items())
                            for (const auto& item : *items) if (const double* number = item.Number()) text += std::format("{}{:.2f}", text.empty() ? "" : ", ", *number);
                        return text;
                    };
                    for (const auto& [field, value] : Session::ValuesOf(*material)) rows.push_back(kgui::Labeled(field, kgui::Line(said(value)), LabelWidth));
                } else if (const MaterialAsset* material = Doc().MaterialAssetOf(asset.id)) {
                    // Its fields, as its kind has them: set here, they are set in every entity that follows it, but
                    // for what one made its own.
                    std::size_t following = 0;
                    for (const kecs::Entity entity : Doc().World().All()) following += Doc().MaterialAssetOfEntity(entity, material->kind) == material->id;
                    rows.push_back(kgui::Labeled("Kind", kgui::Line(material->kind), LabelWidth));
                    rows.push_back(kgui::Labeled("Followed by", kgui::Line(std::format("{} {}", following, following == 1 ? "entity" : "entities")), LabelWidth));
                    // What each field takes of what the file reports: of its own name, or — said here — another of its values.
                    if (const auto hash = asset.id.rfind("#material:"); hash != std::string::npos) {
                        const std::filesystem::path file = Doc().Runtime().AssetDirectory() / asset.id.substr(1, hash - 1);
                        const int index = std::atoi(asset.id.substr(hash + 10).c_str());
                        const auto model = Doc().Runtime().ModelOf(file);
                        const kecs::ComponentId kindId = Doc().World().Find(material->kind);
                        if (model && index >= 0 && index < static_cast<int>(model->materials.size()) && kindId != kecs::NoComponent) {
                            const kgraph::ReportedMaterial report = kgraph::ReportOf(model->materials[static_cast<std::size_t>(index)]);
                            std::vector<std::string> choices { "by its name" };
                            for (const auto& [name, value] : report.values) choices.push_back(name);
                            const auto mapped = Doc().MaterialFieldsOf(file, material->kind);
                            rows.push_back(Heading("From the file"));
                            for (const auto& field : Doc().World().Describe(kindId).fields) {
                                const auto at = mapped.find(field.name);
                                const auto chosen = at == mapped.end() ? choices.begin() : std::ranges::find(choices, at->second);
                                rows.push_back(kgui::Labeled(field.name, kui::Dropdown(choices, chosen == choices.end() ? 0 : static_cast<int>(chosen - choices.begin()),
                                    [this, file, kind = material->kind, name = field.name, choices](const int picked) {
                                        std::string error;
                                        if (!Doc().SetMaterialField(file, kind, name, picked <= 0 ? std::string() : choices[static_cast<std::size_t>(picked)], &error)) _session->Say(error, true);
                                        SetState();
                                    }), LabelWidth));
                            }
                            if (const auto unused = Doc().UnusedReported(asset.id); !unused.empty()) {
                                std::string said;
                                for (const auto& name : unused) said += (said.empty() ? "" : ", ") + name;
                                rows.push_back(kgui::Labeled("Not taken", kgui::Muted(said), LabelWidth));
                            }
                        }
                    }
                    // A model's is the file's to say until it is made the project's own: then the file read again leaves it.
                    if (asset.id.contains("#material:"))
                        rows.push_back(kui::Tooltip("Writes it to a material file of the project's own, which every entity that follows it follows: the model's file, read again, no longer changes it.",
                                                    kui::Button("Extract into the project", [this, id = asset.id] { _session->ExtractMaterial(id); SetState(); },
                                                                kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary))));
                    const kecs::ComponentId kind = Doc().World().Find(material->kind);
                    if (kind != kecs::NoComponent)
                        for (const auto& field : Doc().World().Describe(kind).fields) {
                            const std::string id = asset.id, name = field.name;
                            const auto set = [this, id, name](const Value& value) {
                                std::string error;
                                if (!Doc().SetMaterialAssetField(id, name, value, &error)) _session->Say(error, true);
                            };
                            const auto said = std::ranges::find(material->values, name, &std::pair<std::string, Value>::first);
                            const Value now = said != material->values.end() ? said->second : Value();
                            kui::Widget editor;
                            if (field.type == "Image") editor = HeldField(material->kind, name, field.type, now, set);
                            else editor = ValueField(field, now, set);
                            rows.push_back(kui::Tooltip(field.type, kgui::Labeled(name, std::move(editor), LabelWidth)));
                        }
                }
                rows.push_back(kgui::Muted("Drag it from the Assets panel onto an entity, in the scene tree or the inspector: the entity is given a material of "
                                           "its kind and follows it — changed here, it changes, but for what was changed on the entity itself."));
                break;
            }
            case Session::Asset::Kind::eGenerator: {
                // Its fields, as its script has them: set here, they are set in every entity that follows it, but for
                // what one made its own.
                const auto generator = Doc().GeneratorOf(asset.file);
                std::size_t following = 0;
                for (const kecs::Entity entity : Doc().World().All()) following += Doc().MaterialAssetOfEntity(entity, generator.script) == asset.id;
                rows.push_back(kgui::Labeled("Script", kgui::Line(generator.script), LabelWidth));
                rows.push_back(kgui::Labeled("Followed by", kgui::Line(std::format("{} {}", following, following == 1 ? "entity" : "entities")), LabelWidth));
                if (const kecs::ComponentId script = Doc().World().Find(generator.script); script != kecs::NoComponent)
                    for (const auto& field : Doc().World().Describe(script).fields) {
                        const auto set = [this, file = asset.file, name = field.name](const Value& value) {
                            std::string error;
                            if (!Doc().SetGeneratorField(file, name, value, &error)) _session->Say(error, true);
                        };
                        const auto said = std::ranges::find(generator.fields, field.name, &std::pair<std::string, Value>::first);
                        rows.push_back(kui::Tooltip(field.type, kgui::Labeled(field.name, ValueField(field, said != generator.fields.end() ? said->second : Value(), set), LabelWidth)));
                    }
                rows.push_back(kgui::Muted("Drop it on an entity: the entity makes what this does, and follows it — changed here, it changes, but for what was changed on the entity itself."));
                break;
            }
            case Session::Asset::Kind::eScene:
                rows.push_back(kgui::Muted("A scene of the project's: its graph and its entities, as they were saved."));
                break;
            case Session::Asset::Kind::ePrefab: {
                std::size_t instances = 0;
                for (const kecs::Entity entity : Doc().World().All())
                    if (std::filesystem::path(Doc().PrefabOf(entity)).filename() == asset.file.filename()) ++instances;
                rows.push_back(kgui::Muted("A prefab: entities that instances are made of. Dragged into the scene or onto an entity, it is an instance there; "
                                           "changed and applied from an instance, every instance follows."));
                rows.push_back(kgui::Labeled("In this scene", kgui::Line(std::format("{} {}", instances, instances == 1 ? "instance" : "instances")), LabelWidth));
                rows.push_back(kui::Tooltip("Opens it by itself, its entities alone in the scene tree and the scene view: as Unity's prefab mode does",
                                            Small("Edit prefab", [this, id = asset.id] { _session->OpenPrefab(id); })));
                rows.push_back(Small("Make an instance", [this, id = asset.id] { _session->PlacePrefab(id); }));
                break;
            }
            case Session::Asset::Kind::eFunction:
                rows.push_back(kgui::Muted("A function: nodes that a graph calls as one. Opened, it is a page of the graph, and what is done there is done for every call of it."));
                rows.push_back(Small("Open in the graph", [this, asset] { _session->OpenAsset(asset); SetState(); }));
                break;
            default:
                rows.push_back(kgui::Muted(std::format("{}, in {}: {}.", ScriptLook(asset.group).what, asset.file.extension() == ".kt" ? "Kotlin" : "C#",
                                                       asset.file.filename().string())));
                rows.push_back(Small("Open its file in Visual Studio Code", [this, file = asset.file] { _session->OpenInCode(file); }));
                break;
            }
            return Panel(std::move(rows));
        }

        /** The control a field of @p field 's kind is set with, by what it holds now: a choice, numbers, a box to tick. */
        // ---- what it keeps --------------------------------------------------------------------------------------

        std::string _folderPicked, _tapped;
        std::set<std::string> _foldersShut;
        std::chrono::steady_clock::time_point _tappedAt {};
        std::filesystem::path _trash;
        std::string _moving;                // the asset being renamed or moved, by its id
        std::filesystem::path _importFile;
        std::optional<std::vector<kgraph::Model::Attribute>> _importAttributes;
        AttributeChoice _importChoice;
        bool _importChosen = false;

    private:
        Document& Doc() { return _session->Doc(); }
        void SetState(std::function<void()> change = {}) { _host.setState(std::move(change)); }
        kui::Widget HeldField(const std::string& component, const std::string& fieldName, const std::string& type, const Value& value,
                              const std::function<void(const Value&)>& set) { return _host.heldField(component, fieldName, type, value, set); }

        std::shared_ptr<Session>& _session;
        Host _host;
        Dialog& _dialog;
        std::string& _newId;
        int& _newKind;
    };
}
