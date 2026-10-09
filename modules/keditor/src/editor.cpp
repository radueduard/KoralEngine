#include <keditor/editor.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
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

#include <kecs/json.h>
#include <kgraph/catalog.h>

namespace keditor
{
    using kgraph::Value;
    namespace kb = kor::builders;

    // ---- the session ------------------------------------------------------------------------------------------

    bool Session::NewProject(const std::filesystem::path& directory)
    {
        std::string error;
        Project project;
        if (!Project::Create(directory, directory.filename().string(), project, error)) { Say(error, true); return false; }
        _project = std::move(project);
        _hasProject = true;
        _scene = _project.start;
        _project.Use();
        WriteStandardObjects(_project.directory / "objects");
        LoadObjects();
        return Save();
    }

    bool Session::OpenProject(const std::filesystem::path& directory)
    {
        std::string error;
        Project project;
        if (!Project::Open(directory, project, error)) { Say(error, true); return false; }
        project.Use();
        if (!_play.Open(project.ScenePath(project.start), error)) { Say(error, true); return false; }
        _project = std::move(project);
        _hasProject = true;
        _scene = _project.start;
        selectedNode.clear();
        selectedEntity = kecs::NullEntity;
        _snapshot.clear();
        _folded.clear();
        LoadObjects();
        Say(std::format("Opened {}.", _project.name));
        return true;
    }

    bool Session::Save()
    {
        if (!_hasProject) { Say("There is no project to save it in yet: File, New project.", true); return false; }
        if (InPlayMode()) { Say("Stop playing to save: what is saved is the scene, not a moment of it.", true); return false; }
        std::string error;
        if (!Doc().Save(_project.ScenePath(_scene), error)) { Say(error, true); return false; }
        Say(std::format("Saved {}.", _project.ScenePath(_scene).filename().string()));
        return true;
    }

    std::string Session::RunCommand() const
    {
        if (!_hasProject) return {};
        // The player that was built with this editor: beside it. Failing that, whichever is found by name.
        std::filesystem::path player = "koral-play";
        std::error_code ec;
        if (const auto self = std::filesystem::read_symlink("/proc/self/exe", ec); !ec && std::filesystem::exists(self.parent_path() / "koral-play", ec))
            player = self.parent_path() / "koral-play";
        return std::format("\"{}\" \"{}\"", player.string(), _project.directory.string());
    }

    bool Session::Run()
    {
        if (!_hasProject) { Say("There is no project to run yet: File, New project.", true); return false; }
        if (!Save()) return false;
#ifdef _WIN32
        const std::string command = "start \"\" " + RunCommand();
#else
        const std::string command = RunCommand() + " &";
#endif
        if (std::system(command.c_str()) != 0) { Say("The player could not be started.", true); return false; }
        Say(std::format("Running {}.", _project.name));
        return true;
    }

    // ---- the editor's view ------------------------------------------------------------------------------------

    namespace {
        constexpr float Radians = 3.14159265358979323846f / 180.f;
    }

    kor::Vec3 EditorCamera::Position() const
    {
        const float y = yaw * Radians, p = pitch * Radians;
        return target + kor::Vec3(std::cos(p) * std::sin(y), -std::sin(p), std::cos(p) * std::cos(y)) * distance;
    }

    kgraph::View EditorCamera::View() const
    {
        kgraph::View view;
        view.view = kor::LookAt(Position(), target);
        view.fieldOfView = fieldOfView * Radians;
        view.near = near;
        view.far = far;
        return view;
    }

    void EditorCamera::Orbit(const float dx, const float dy)
    {
        yaw -= dx * 0.3f;
        pitch = std::clamp(pitch - dy * 0.3f, -89.f, 89.f);
    }

    void EditorCamera::Pan(const float dx, const float dy, const float height)
    {
        const float perPixel = 2.f * distance * std::tan(fieldOfView * Radians / 2.f) / std::max(height, 1.f);
        const kor::Vec3 forward = kor::Normalize(target - Position());
        const kor::Vec3 right = kor::Normalize(kor::Cross(forward, kor::Vec3(0.f, 1.f, 0.f)));
        const kor::Vec3 up = kor::Cross(right, forward);
        target = target - right * (dx * perPixel) + up * (dy * perPixel);
    }

    void EditorCamera::Zoom(const float steps) { distance = std::clamp(distance * std::pow(0.85f, steps), 0.05f, 5000.f); }

    void EditorCamera::Frame(const kecs::Mat4& world)
    {
        target = { world.At(3, 0), world.At(3, 1), world.At(3, 2) };
        float size = 0.f;
        for (int column = 0; column < 3; ++column)
            size = std::max(size, kor::Length(kor::Vec3(world.At(column, 0), world.At(column, 1), world.At(column, 2))));
        const float radius = std::max(size * 0.87f, 0.25f);    // half a unit cube's diagonal
        distance = radius / std::sin(fieldOfView * Radians / 2.f) * 1.2f;
    }

    void Session::Frame(const kecs::Entity entity)
    {
        if (!Doc().World().Alive(entity)) return;
        camera.Frame(Doc().World().WorldMatrix(entity));
        lookThroughCamera = false;
        ++_changes;
    }

    void Session::Tick(const kor::Input& input, const float seconds)
    {
        Document& doc = Doc();
        kecs::World& world = doc.World();
        kgraph::Runtime& runtime = doc.Runtime();
        kor::DebugDraw& debug = _play.SceneDebug();

        // Frames a second, over a quarter of one: steady enough to read.
        ++_framesCounted;
        _secondsCounted += seconds;
        if (_secondsCounted >= 0.25f) {
            const int now = static_cast<int>(std::lround(static_cast<float>(_framesCounted) / _secondsCounted));
            if (now != _fps) { _fps = now; ++_changes; }
            _framesCounted = 0;
            _secondsCounted = 0.f;
        }

        const kecs::Entity selected = world.Alive(selectedEntity) ? selectedEntity : kecs::NullEntity;
        runtime.SetSelected(selected);

        // The view, moved by the mouse over the scene's panel: how far it went since the last frame, from where it was.
        const kor::Vec2 mouse = input.MousePosition();
        const kor::Vec2 delta = _lastMouse ? mouse - *_lastMouse : kor::Vec2(0.f);
        _lastMouse = mouse;
        const auto pointer = viewport->PointerPosition();
        const kor::UVec2 size = viewport->Size();
        if (ThroughSceneCamera()) {
            _viewDrag.reset();
            runtime.SetView(std::nullopt);
        } else {
            if (_viewDrag && !input.IsMouseButtonHeld(*_viewDrag) && !input.IsMouseButtonPressed(*_viewDrag)) _viewDrag.reset();
            if (!_viewDrag && viewport->Hovered())
                for (const kor::MouseButton button : { kor::MouseButton::eRight, kor::MouseButton::eMiddle })
                    if (input.IsMouseButtonPressed(button)) { _viewDrag = button; break; }
            if (_viewDrag == kor::MouseButton::eRight) camera.Orbit(delta.x, delta.y);
            else if (_viewDrag == kor::MouseButton::eMiddle) camera.Pan(delta.x, delta.y, static_cast<float>(std::max(size.y, 1u)));
            if (viewport->Hovered())
                if (const float wheel = input.MouseScrollDelta().y; wheel != 0.f) camera.Zoom(wheel);
            runtime.SetView(camera.View());

            // What the editor's view shows of what is not drawn: each scene camera as the pyramid it sees through,
            // and the picked light's reach.
            const float aspect = runtime.Aspect();
            world.Query<const kgraph::Camera>().Each([&](const kecs::Entity entity, const kgraph::Camera&) {
                const kgraph::View lens = kgraph::ViewOf(world, entity);
                debug.Camera(lens.view, lens.Projection(aspect, false), 0.5f);
            });
            if (!selected.IsNull()) {
                kor::Vec3 position, scale;
                kor::Quat rotation;
                if (kor::Decompose(kgraph::ToKor(world.WorldMatrix(selected)), position, rotation, scale)) {
                    const kor::Vec3 forward = rotation * kor::Vec3(0.f, 0.f, -1.f);
                    if (world.Has<kgraph::DirectionalLight>(selected)) debug.DirectionalLight(position, forward);
                    if (const auto* light = world.Get<kgraph::PointLight>(selected)) debug.PointLight(position, light->range);
                    if (const auto* light = world.Get<kgraph::SpotLight>(selected))
                        debug.SpotLight(position, forward, light->range, light->outerAngle * Radians, light->innerAngle * Radians);
                }
            }
        }

        // The gizmo on what is picked, and what dragging it does to its transform.
        kor::GizmoPointer hand = viewport->GizmoPointer();
        if (!selected.IsNull() && gizmoEnabled && size.x > 0 && size.y > 0) {
            kor::Mat4 transform = kgraph::ToKor(world.WorldMatrix(selected));
            kor::GizmoOptions options;
            options.space = kor::GizmoSpace::eLocal;
            if (debug.Gizmo(gizmo, transform, runtime.ViewProjection(), hand, options)) {
                _gizmoMoved = true;
                // Only what this gizmo moves: taking the rest back out of the matrix would round it off a little each frame.
                world.SetWorldMatrix(selected, kgraph::ToKecs(transform), gizmo == kor::GizmoMode::eTranslate, gizmo == kor::GizmoMode::eRotate,
                                     gizmo == kor::GizmoMode::eScale);
                ++_changes;
            }
        }

        // A click on the picture — the button let go where it went down, the gizmo not dragged — picks what is under it.
        const bool down = hand.down;
        if (down && !_wasDown) { _pressAt = pointer; _gizmoMoved = false; }
        if (!down && _wasDown) {
            if (_pressAt && pointer && !_gizmoMoved && kor::Length(*pointer - *_pressAt) < 4.f && size.x > 0 && size.y > 0) {
                // The answer comes a few frames on: by then something else may have been picked, and that, being later, stands.
                const kecs::Entity before = selectedEntity;
                runtime.Pick(pointer->x / static_cast<float>(size.x), pointer->y / static_cast<float>(size.y), [this, before](const kecs::Entity picked) {
                    if (selectedEntity == before) Select(picked);
                });
            }
            _pressAt.reset();
        }
        _wasDown = down;
    }

    // ---- playing ------------------------------------------------------------------------------------------------

    void Session::StartPlaying()
    {
        if (_snapshot.empty()) {
            _snapshot = Doc().WorldJson();
            if (kgraph::FirstCamera(Doc().World()).IsNull()) Say("There is no camera in the scene: it plays seen from where the scene view's default is.", true);
            else Say("Playing.");
        }
        _play.SetPlaying(true);
        ++_changes;
    }

    void Session::Pause()
    {
        _play.SetPlaying(false);
        ++_changes;
    }

    void Session::Stop()
    {
        if (_snapshot.empty()) return;
        // Where what is picked is, by its place among its siblings up to the top: found again when the scene is put back.
        std::vector<std::size_t> path;
        const kecs::World& world = Doc().World();
        for (kecs::Entity at = world.Alive(selectedEntity) ? selectedEntity : kecs::NullEntity; !at.IsNull(); at = world.ParentOf(at)) {
            const kecs::Entity parent = world.ParentOf(at);
            const auto& siblings = parent.IsNull() ? world.Roots() : world.ChildrenOf(parent);
            path.insert(path.begin(), static_cast<std::size_t>(std::ranges::find(siblings, at) - siblings.begin()));
        }
        _play.SetPlaying(false);
        std::string error;
        if (!Doc().RestoreWorld(_snapshot, error)) Say(error, true);
        else Say("Stopped: the scene is as it was.");
        _snapshot.clear();
        _folded.clear();
        kecs::Entity found = kecs::NullEntity;
        const std::vector<kecs::Entity>* siblings = &world.Roots();
        for (const std::size_t index : path) {
            if (index >= siblings->size()) { found = kecs::NullEntity; break; }
            found = (*siblings)[index];
            siblings = &world.ChildrenOf(found);
        }
        selectedEntity = found;
        ++_changes;
    }

    // ---- entities -----------------------------------------------------------------------------------------------

    kecs::Entity Session::Duplicate(const kecs::Entity entity)
    {
        const kecs::Entity copy = Doc().Duplicate(entity);
        if (!copy.IsNull()) Select(copy);
        return copy;
    }

    void Session::Delete(const kecs::Entity entity)
    {
        const kecs::World& world = Doc().World();
        if (selectedEntity == entity || world.IsAncestor(entity, selectedEntity)) selectedEntity = kecs::NullEntity;
        Doc().DestroyEntity(entity);
        ++_changes;
    }

    void Session::LoadObjects()
    {
        _objects.clear();
        // An entity with nothing but a name and a place: there with or without a project.
        _objects.push_back({ "Empty", "An entity with nothing but a name and a place.", Value(Value::Members { { "name", "Empty" } }), {} });
        std::error_code ignored;
        std::vector<std::filesystem::path> files;
        if (_hasProject)
            for (const auto& entry : std::filesystem::directory_iterator(_project.directory / "objects", ignored))
                if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
        std::ranges::sort(files);
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            kecs::json::Value parsed;
            std::string error;
            if (!kecs::json::Parse(text, parsed, error)) { Say(std::format("{}: {}", file.filename().string(), error), true); continue; }
            ObjectTemplate object;
            object.data = Value::FromJson(parsed);
            const Value* name = object.data.Find("template");
            object.name = name && name->String() ? *name->String() : file.stem().string();
            if (const Value* description = object.data.Find("description"); description && description->String()) object.description = *description->String();
            object.file = file;
            std::erase_if(_objects, [&](const ObjectTemplate& other) { return other.name == object.name; });
            _objects.push_back(std::move(object));
        }
        ++_changes;
    }

    kecs::Entity Session::Create(const ObjectTemplate& object, const kecs::Entity parent)
    {
        Document& doc = Doc();
        const auto& siblings = parent.IsNull() ? doc.World().Roots() : doc.World().ChildrenOf(parent);
        std::vector<std::string> names;
        for (const kecs::Entity sibling : siblings) names.push_back(doc.World().NameOf(sibling));
        std::vector<std::string> problems;
        const kecs::Entity entity = doc.ReadEntity(object.data, parent, &problems);
        doc.Rename(entity, Document::UniqueName(doc.World().NameOf(entity), names));
        if (!problems.empty()) Say(std::format("{}: {}", object.name, problems.front()), true);
        if (!parent.IsNull()) Fold(parent, false);
        Select(entity);
        return entity;
    }

    bool Session::SaveAsObject(const kecs::Entity entity)
    {
        if (!_hasProject) { Say("There is no project to keep objects in yet: File, New project.", true); return false; }
        if (!Doc().World().Alive(entity)) return false;
        const std::string name = Doc().World().NameOf(entity);
        const Value written = Doc().WriteEntity(entity);
        Value::Members data { { "template", name } };
        // Made from it, a thing is wherever it is made: where this one was is no part of what it is.
        for (const auto& [key, value] : *written.Named()) data.emplace_back(key, key == "position" ? Value(Value::List { 0, 0, 0 }) : value);
        std::string file = name;
        for (char& c : file)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '_' && c != '-') c = '_';
        if (file.empty()) file = "Object";
        const auto path = _project.directory / "objects" / (file + ".json");
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
        {
            std::ofstream out(path, std::ios::binary);
            out << Value(std::move(data)).ToJson() << "\n";
            if (!out) { Say(std::format("{} could not be written.", path.string()), true); return false; }
        }   // closed, before it is read back among the objects
        LoadObjects();
        Say(std::format("Saved {} as an object: {}", name, path.filename().string()));
        return true;
    }

    bool Session::NewStandardScene()
    {
        if (!_hasProject) { Say("The standard scene's shaders go in a project: File, New project.", true); return false; }
        if (InPlayMode()) Stop();
        WriteStandardShaders(_project.ShaderDirectory());
        WriteStandardObjects(_project.directory / "objects");
        _play.New();
        selectedNode.clear();
        selectedEntity = kecs::NullEntity;
        _folded.clear();
        std::string error;
        if (!AddStandardGraph(Doc(), error)) { Say(error, true); return false; }
        Select(AddStandardScene(Doc()));
        LoadObjects();
        Say("A new scene: saving writes it to the project.");
        return true;
    }

    bool Session::Revert()
    {
        if (!_hasProject) return false;
        if (InPlayMode()) { _snapshot.clear(); _play.SetPlaying(false); }
        std::string error;
        if (!_play.Open(_project.ScenePath(_scene), error)) { Say(error, true); return false; }
        selectedNode.clear();
        selectedEntity = kecs::NullEntity;
        _folded.clear();
        Say(std::format("{} is as it was saved.", _scene));
        return true;
    }

    std::vector<std::string> Session::ShaderModules() const
    {
        std::vector<std::string> modules;
        std::error_code ec;
        if (!_hasProject) return modules;
        for (const auto& entry : std::filesystem::directory_iterator(_project.ShaderDirectory(), ec))
            if (entry.path().extension() == ".slang") modules.push_back(entry.path().stem().string());
        std::ranges::sort(modules);
        return modules;
    }

    std::vector<std::string> Session::EntryPoints(const std::string& module) const
    {
        std::vector<std::string> entries;
        if (!_hasProject) return entries;
        std::ifstream in(_project.ShaderDirectory() / (module + ".slang"));
        if (!in) return entries;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // [shader("stage")], whatever other attributes follow it, a return type, and the function's name.
        static const std::regex declared(R"re(\[\s*shader\s*\(\s*"\w+"\s*\)\s*\]\s*(?:\[[^\]]*\]\s*)*[\w:<>,\s]+?\s+(\w+)\s*\()re");
        for (std::sregex_iterator it(text.begin(), text.end(), declared), end; it != end; ++it) entries.push_back((*it)[1].str());
        return entries;
    }

    std::vector<const kgraph::NodeType*> FindNodeTypes(const std::string_view typed)
    {
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        std::vector<std::string> words;
        for (std::size_t at = 0; at < typed.size();) {
            const auto end = typed.find(' ', at);
            const auto word = typed.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
            if (!word.empty()) words.push_back(lower(std::string(word)));
            if (end == std::string_view::npos) break;
            at = end + 1;
        }
        // Whether a word starts one of the words of a text: of "vertex fragment", of "GraphicsPipeline" at its capitals.
        const auto startsAWord = [&](const std::string& text, const std::string& word) {
            for (std::size_t at = 0; at < text.size(); ++at) {
                const bool start = at == 0 || text[at - 1] == ' ' || (std::isupper(static_cast<unsigned char>(text[at])) && !std::isupper(static_cast<unsigned char>(text[at - 1])));
                if (start && lower(text.substr(at, word.size())) == word) return true;
            }
            return false;
        };
        std::vector<std::pair<int, const kgraph::NodeType*>> found;
        for (const auto& type : kgraph::Catalog()) {
            if (!type.listed) continue;
            const std::string name = lower(type.name), doc = lower(type.doc), category = lower(type.category);
            int score = 0;
            bool all = true;
            for (const auto& word : words) {
                int best = 0;
                if (name == word) best = 100;
                else if (name.starts_with(word)) best = 60;
                else if (startsAWord(type.name, word)) best = 45;
                else if (name.find(word) != std::string::npos) best = 30;
                else if (startsAWord(type.keywords, word)) best = 25;
                else if (category.starts_with(word)) best = 10;
                else if (doc.find(word) != std::string::npos) best = 5;
                if (best == 0) { all = false; break; }
                score += best;
            }
            if (all) found.emplace_back(score, &type);
        }
        // Best first; among equals, as the catalog has them — which with nothing typed is all of them, in its order.
        std::ranges::stable_sort(found, [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<const kgraph::NodeType*> types;
        for (const auto& [score, type] : found) types.push_back(type);
        return types;
    }

    namespace {
        constexpr float LabelWidth = 132.f;

        kui::Widget Small(std::string label, std::function<void()> onPressed)
        {
            return kui::Button(std::move(label), std::move(onPressed),
                               kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(8.f, 2.f)));
        }
        kui::Widget Heading(std::string text)
        {
            kui::TextStyle style;
            style.size = 15.f;
            style.weight = 700;
            return kui::Text(std::move(text), style);
        }
        kui::Widget Problem(std::string text)
        {
            return kui::Text(std::move(text), kui::TextStyle {}.SetSize(13.f).SetColor(kui::Color::Hex(0xFF6B6B)));
        }
        kui::Widget Rows(std::vector<kui::Widget> children, const float gap = 6.f)
        {
            return kui::Column(std::move(children), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(gap));
        }
        kui::Widget Panel(std::vector<kui::Widget> children)
        {
            return kui::ScrollView(kui::Padding(kui::EdgeInsets::All(10.f), Rows(std::move(children), 8.f)));
        }

        float NumberOf(const Value& value, const float fallback = 0.f)
        {
            if (const double* number = value.Number()) return static_cast<float>(*number);
            if (const bool* flag = value.Bool()) return *flag ? 1.f : 0.f;
            return fallback;
        }

        /** A value typed as JSON: for what has no editor of its own. Nothing typed is no value. */
        kui::Widget JsonField(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            const std::string text = value.IsNull() ? std::string() : value.ToJson();
            return kui::TextField(kui::TextFieldOptions { .text = text, .placeholder = "as JSON",
                .onSubmitted = [set](const std::string& typed) {
                    if (typed.empty()) { set(Value {}); return; }
                    kecs::json::Value parsed;
                    std::string error;
                    if (kecs::json::Parse(typed, parsed, error)) set(Value::FromJson(parsed));
                } }).Key(key + "=" + text);
        }

        /** An editor for a value of a builder's type, whatever it is: what a node's inspector is made of. */
        kui::Widget TypeEditor(const kb::Type& type, const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            using kb::Kind;
            switch (type.kind) {
            case Kind::eBool:
                return kui::Checkbox(value.Bool() && *value.Bool(), [set](const bool on) { set(Value(on)); });
            case Kind::eInt:
            case Kind::eUInt:
                return kui::DragValue(NumberOf(value), [set](const float v) { set(Value(std::round(v))); },
                                      kui::DragValueOptions {}.SetSpeed(1.f).SetDecimals(0).SetRange(type.kind == Kind::eUInt ? 0.f : -kui::Infinity, kui::Infinity));
            case Kind::eFloat:
                return kui::DragValue(NumberOf(value), [set](const float v) { set(Value(v)); });
            case Kind::eString: {
                const std::string text = value.String() ? *value.String() : std::string();
                return kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
            }
            case Kind::eEnum:
            case Kind::eChoice: {
                std::vector<std::string> names;
                int selected = -1;
                if (type.enumeration)
                    for (const auto& entry : type.enumeration->values) {
                        if ((value.String() && *value.String() == entry.name) || (value.Number() && static_cast<std::uint64_t>(*value.Number()) == entry.value))
                            selected = static_cast<int>(names.size());
                        names.emplace_back(entry.name);
                    }
                return kui::Dropdown(names, selected, [set, names](const int index) { set(Value(names[static_cast<std::size_t>(index)])); },
                                     kui::DropdownOptions {}.SetPlaceholder("(as it is by default)"));
            }
            case Kind::eFlags: {
                std::set<std::string> on;
                if (const auto* items = value.Items()) { for (const auto& item : *items) if (item.String()) on.insert(*item.String()); }
                else if (value.String()) on.insert(*value.String());
                std::vector<kui::Widget> boxes;
                if (type.enumeration)
                    for (const auto& entry : type.enumeration->values) {
                        const std::string name(entry.name);
                        boxes.push_back(kui::Checkbox(on.contains(name), [set, on, name](const bool checked) {
                            auto changed = on;
                            if (checked) changed.insert(name); else changed.erase(name);
                            set(Value(Value::List(changed.begin(), changed.end())));
                        }, name));
                    }
                return kui::Column(std::move(boxes), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(2.f));
            }
            case Kind::eArray: {
                if (!type.element || (type.element->kind != Kind::eFloat && type.element->kind != Kind::eInt && type.element->kind != Kind::eUInt)) break;
                std::vector<float> numbers(type.length, 0.f);
                if (const auto* items = value.Items())
                    for (std::size_t i = 0; i < numbers.size() && i < items->size(); ++i) numbers[i] = NumberOf((*items)[i]);
                return kgui::DragFloats(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); });
            }
            case Kind::eStruct: {
                // A colour an attachment is cleared to: picked, and given as the four floats it is.
                if (type.name == "KoralClearColor") {
                    kui::Color color(0.f, 0.f, 0.f, 1.f);
                    if (const Value* held = value.Find("value"))
                        if (const Value* floats = held->Find("f"); floats && floats->Items() && floats->Items()->size() == 4) {
                            const auto& c = *floats->Items();
                            color = kui::Color(NumberOf(c[0]), NumberOf(c[1]), NumberOf(c[2]), NumberOf(c[3]));
                        }
                    return kui::ColorEdit(color, [set](const kui::Color picked) {
                        set(Value(Value::Members { { "scalarType", "Float" }, { "components", 4 },
                                                   { "value", Value::Members { { "f", Value::List { picked.r, picked.g, picked.b, picked.a } } } } }));
                    });
                }
                std::vector<kui::Widget> rows;
                for (const auto& field : type.fields) {
                    const std::string name(field.name);
                    const Value* member = value.Find(name);
                    const auto setMember = [set, value, name](Value changed) {
                        Value::Members members = value.Named() ? *value.Named() : Value::Members {};
                        const auto known = std::ranges::find_if(members, [&](const auto& m) { return m.first == name; });
                        if (known != members.end()) known->second = std::move(changed);
                        else members.emplace_back(name, std::move(changed));
                        set(Value(std::move(members)));
                    };
                    rows.push_back(kgui::Labeled(name, TypeEditor(*field.type, member ? *member : Value {}, setMember, key + "." + name), 110.f));
                }
                return Rows(std::move(rows), 4.f);
            }
            case Kind::eList: {
                if (!type.element) break;
                const Value::List items = value.Items() ? *value.Items() : Value::List {};
                std::vector<kui::Widget> rows;
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const auto setItem = [set, items, i](Value changed) { auto list = items; list[i] = std::move(changed); set(Value(std::move(list))); };
                    rows.push_back(kui::Row({ kui::Expanded(TypeEditor(*type.element, items[i], setItem, std::format("{}[{}]", key, i))),
                                              Small("-", [set, items, i] { auto list = items; list.erase(list.begin() + static_cast<std::ptrdiff_t>(i)); set(Value(std::move(list))); }) },
                                            kui::FlexOptions {}.SetGap(4.f)));
                }
                rows.push_back(Small("+ one more", [set, items, element = type.element] {
                    auto list = items;
                    list.push_back(element->kind == Kind::eStruct ? Value(Value::Members {}) : element->kind == Kind::eString ? Value(std::string()) : Value(0));
                    set(Value(std::move(list)));
                }));
                return Rows(std::move(rows), 4.f);
            }
            default: break;
            }
            return JsonField(value, set, key);
        }

        /** An editor for an input of one of the engine's own nodes, which says only what its type is called. */
        kui::Widget NamedEditor(const std::string& type, const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            if (type == "number") return kui::DragValue(NumberOf(value), [set](const float v) { set(Value(v)); });
            if (type == "bool") return kui::Checkbox(value.Bool() && *value.Bool(), [set](const bool on) { set(Value(on)); });
            if (type == "string") {
                const std::string text = value.String() ? *value.String() : std::string();
                return kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
            }
            return JsonField(value, set, key);
        }

        /** A dropdown of names, showing the one that is the value: and the value itself when it is none of them. */
        kui::Widget Choice(std::vector<std::string> names, const Value& value, const std::function<void(Value)>& set, std::string placeholder = {})
        {
            const std::string now = value.String() ? *value.String() : std::string();
            if (!now.empty() && std::ranges::find(names, now) == names.end()) names.push_back(now);
            const auto at = std::ranges::find(names, now);
            return kui::Dropdown(names, at == names.end() ? -1 : static_cast<int>(at - names.begin()),
                                 [set, names](const int index) { set(Value(names[static_cast<std::size_t>(index)])); },
                                 kui::DropdownOptions {}.SetPlaceholder(std::move(placeholder)));
        }

        kui::Widget TextEdit(const Value& value, const std::function<void(Value)>& set, const std::string& key, std::string placeholder = {})
        {
            const std::string text = value.String() ? *value.String() : std::string();
            return kui::TextField(kui::TextFieldOptions { .text = text, .placeholder = std::move(placeholder),
                                                          .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
        }

        /** A list of names to tick: the ones there are to choose from, and any the value has besides. */
        kui::Widget Ticks(std::vector<std::string> names, const Value& value, const std::function<void(Value)>& set)
        {
            std::vector<std::string> on;
            if (const auto* items = value.Items())
                for (const auto& item : *items) if (item.String()) on.push_back(*item.String());
            for (const auto& name : on) if (std::ranges::find(names, name) == names.end()) names.push_back(name);
            if (names.empty()) return kgui::Muted("there are none yet");
            std::vector<kui::Widget> boxes;
            for (const auto& name : names)
                boxes.push_back(kui::Checkbox(std::ranges::find(on, name) != on.end(), [set, on, name](const bool checked) {
                    auto changed = on;
                    if (checked) changed.push_back(name); else std::erase(changed, name);
                    set(Value(Value::List(changed.begin(), changed.end())));
                }, name));
            return kui::Column(std::move(boxes), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(2.f));
        }

        /** What a vertex has, a line an attribute: what it is, of what, how many. */
        kui::Widget Attributes(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            static const std::vector<std::string> types { "eFloat", "eInt", "eUInt", "eShort", "eUShort", "eByte", "eUByte", "eDouble" };
            const Value::List items = value.Items() ? *value.Items() : Value::List {};
            std::vector<kui::Widget> rows;
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto with = [set, items, i](const std::string& name, Value changed) {
                    auto list = items;
                    Value::Members members = list[i].Named() ? *list[i].Named() : Value::Members {};
                    const auto known = std::ranges::find_if(members, [&](const auto& m) { return m.first == name; });
                    if (known != members.end()) known->second = std::move(changed); else members.emplace_back(name, std::move(changed));
                    list[i] = Value(std::move(members));
                    set(Value(std::move(list)));
                };
                const Value* semantic = items[i].Find("semantic");
                const Value* type = items[i].Find("type");
                const Value* count = items[i].Find("count");
                rows.push_back(kui::Row({
                    kui::Expanded(TextEdit(semantic ? *semantic : Value {}, [with](Value v) { with("semantic", std::move(v)); }, std::format("{}[{}]", key, i), "POSITION")),
                    Choice(types, type ? *type : Value("eFloat"), [with](Value v) { with("type", std::move(v)); }).Width(110.f),
                    kui::DragValue(count ? NumberOf(*count) : 3.f, [with](const float v) { with("count", Value(std::round(v))); },
                                   kui::DragValueOptions {}.SetSpeed(1.f).SetDecimals(0).SetRange(1.f, 4.f).SetWidth(48.f)),
                    Small("-", [set, items, i] { auto list = items; list.erase(list.begin() + static_cast<std::ptrdiff_t>(i)); set(Value(std::move(list))); }),
                }, kui::FlexOptions {}.SetGap(4.f)));
            }
            rows.push_back(Small("+ attribute", [set, items] {
                auto list = items;
                list.emplace_back(Value::Members { { "semantic", list.empty() ? "POSITION" : "" }, { "type", "eFloat" }, { "count", 3 } });
                set(Value(std::move(list)));
            }));
            return Rows(std::move(rows), 4.f);
        }

        kui::Widget ColorOf(const Value& value, const std::function<void(Value)>& set, const bool alpha = true)
        {
            kui::Color color(0.f, 0.f, 0.f, 1.f);
            if (const auto* c = value.Items(); c && c->size() >= 3) color = kui::Color(NumberOf((*c)[0]), NumberOf((*c)[1]), NumberOf((*c)[2]), c->size() > 3 ? NumberOf((*c)[3]) : 1.f);
            return kui::ColorEdit(color, [set, alpha](const kui::Color picked) {
                set(alpha ? Value(Value::List { picked.r, picked.g, picked.b, picked.a }) : Value(Value::List { picked.r, picked.g, picked.b }));
            }, {}, kui::ColorPickerOptions {}.SetAlpha(alpha));
        }

        /** Whether a name says a vector is a colour: what an entity's field is called is all there is to go by. */
        bool NamesAColor(std::string name)
        {
            std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // A sky's colours are named for where in the sky they are.
            return name.find("color") != std::string::npos || name.find("colour") != std::string::npos || name.find("tint") != std::string::npos
                || name.find("albedo") != std::string::npos || name.find("emission") != std::string::npos || name.find("emissive") != std::string::npos || name == "zenith" || name == "horizon" || name == "ground";
        }

        /** An input of a node, as it stands. */
        struct Shown {
            kgraph::Port port;      // its name is the input's: `color[1].view`, not `color[].view`
            bool given = false;     // the node has it: a value, a wire, or a port left open for one
        };

        /**
         * A node's inputs as they stand. Those it cannot do without, those it has been given, and those asked for
         * (@p asked): in the order its type lists them. One given more than once is there once for each it has, and
         * once more when it is one a node needs. The rest are in @p more, for whoever wants one.
         */
        std::vector<Shown> InputsOf(const kgraph::Runtime& runtime, const kgraph::Node& node, const std::function<bool(const std::string&)>& asked,
                                    std::vector<kgraph::Port>* more = nullptr)
        {
            std::vector<Shown> shown;
            std::set<std::string> listed;
            const auto consider = [&](kgraph::Port port, const bool always) {
                const bool given = node.inputs.contains(port.name);
                listed.insert(port.name);
                if (given || always || !port.optional || asked(port.name)) shown.push_back({ std::move(port), given });
                else if (more) more->push_back(std::move(port));
            };
            const kgraph::NodeType* type = kgraph::FindType(node.type);
            // An input given whole — `groups`, wired from a vector — is not also there in its parts.
            const auto givenWhole = [&](const std::string& name) {
                if (!type || name.size() < 3 || name[name.size() - 2] != '.') return false;
                const std::string whole = name.substr(0, name.size() - 2);
                return node.inputs.contains(whole) && std::ranges::contains(kgraph::PartsOf(*type, whole), name);
            };
            for (const auto& port : runtime.PortsOf(node.id)) {
                if (givenWhole(port.name)) { listed.insert(port.name); continue; }
                const auto at = port.name.find("[]");
                // What a shader declares is the pipeline's to be given, whatever else is: a parameter is never hidden.
                if (at == std::string::npos) { consider(port, port.name.starts_with("param.")); continue; }
                const std::string prefix = port.name.substr(0, at + 1);
                int count = 0;
                for (const auto& name : node.inputs | std::views::keys)
                    if (name.starts_with(prefix)) count = std::max(count, std::atoi(name.c_str() + prefix.size()) + 1);
                for (int i = 0; i <= count; ++i) {
                    kgraph::Port each = port;
                    each.name = std::format("{}{}]{}", prefix, i, port.name.substr(at + 2));
                    each.repeatable = false;
                    // The next one free is offered like any input that can be done without.
                    if (i == count) each.optional = port.optional || count > 0;
                    consider(std::move(each), false);
                }
            }
            // And what the node has that its type does not list: a shader wired in, an input of another way of drawing.
            for (const auto& [name, input] : node.inputs)
                if (!listed.contains(name))
                    shown.push_back({ kgraph::Port { name, input.link ? kgraph::PortKind::eResource : kgraph::PortKind::eValue, "value", false, {}, nullptr }, true });
            return shown;
        }

        /** Whether an input shows as a port on its node: what takes a resource, and a value that is wired or left open for a wire. */
        bool ShowsAsPort(const Shown& shown, const kgraph::Node& node)
        {
            if (shown.port.kind == kgraph::PortKind::eResource) return true;
            const auto input = node.inputs.find(shown.port.name);
            return input != node.inputs.end() && (input->second.link || input->second.value.IsNull());
        }

        kui::Color CategoryColor(const std::string& category)
        {
            if (category == "Pipelines") return kui::Color::Hex(0x7A4BC2);
            if (category == "Passes") return kui::Color::Hex(0xC2643B);
            if (category == "Values") return kui::Color::Hex(0x3B7FC2);
            return kui::Color::Hex(0x3B9C7A);
        }

        // ---- the root -------------------------------------------------------------------------------------------

        class Root final : public kui::StatefulWidget {
        public:
            explicit Root(std::shared_ptr<Session> session) : _session(std::move(session))
            {
                _layout = std::make_shared<kui::DockLayout>();
                _layout->Dock("graph")
                        .Dock("scene", kui::DockSide::eRight, "graph", 0.34f)
                        .Dock("inspector", kui::DockSide::eBottom, "scene", 0.55f)
                        .Dock("outliner", kui::DockSide::eLeft, "graph", 0.16f)
                        .Dock("problems", kui::DockSide::eBottom, "graph", 0.22f);
            }

            void InitState() override
            {
                // The document changes under the panels — an edit here, a shader saved elsewhere: built again when it has.
                Animate([this](float) {
                    if (_session->TakeAskForNodes()) SetState([&] { _dialog = Dialog::eAddNode; _addAt = { 80.f + 30.f * static_cast<float>(Doc().Graph().nodes.size() % 8), 80.f }; _filter.clear(); _newId.clear(); });
                    if (const auto version = _session->Version(); version != _shown) SetState([&] { _shown = version; });
                    return true;
                });
            }

            kui::Widget Build() override
            {
                _shown = _session->Version();
                auto body = kui::Column({
                    Menus(),
                    kui::Expanded(kui::DockSpace(_layout, {
                        { "graph", "Graph", Graph(), false },
                        { "scene", "Scene", ScenePanel(), false },
                        { "outliner", "Outliner", Outliner(), false },
                        { "inspector", "Inspector", Inspector(), false },
                        { "problems", "Problems", Problems(), false },
                    })),
                    kui::StatusBar(std::format("{}      {} {}", _session->Status(), Doc().World().EntityCount(), Doc().World().EntityCount() == 1 ? "entity" : "entities"),
                                   _session->Failed() ? kui::StatusLevel::eError : kui::StatusLevel::eInfo),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetMainAxisSize(kui::MainAxisSize::eMax));
                return kui::Modal(_dialog != Dialog::eNone, std::move(body), DialogBody(), [this] { SetState([&] { _dialog = Dialog::eNone; }); });
            }

        private:
            enum class Dialog { eNone, eAddNode, eNewProject, eOpenProject };

            Document& Doc() { return _session->Doc(); }

            // ---- the scene ---------------------------------------------------------------------------------------

            /** A button of the scene's toolbar: lit while what it stands for is on. */
            kui::Widget Tool(const std::string& label, const std::string& what, const bool active, std::function<void()> pressed)
            {
                return kui::Tooltip(what, Small(active ? "[" + label + "]" : label, std::move(pressed)));
            }

            /** The scene as it runs, under what looks at it and plays it: the gizmo, the view, play and stop. */
            kui::Widget ScenePanel()
            {
                const kecs::Entity selected = _session->selectedEntity;
                const bool playMode = _session->InPlayMode();
                std::vector<kui::Widget> tools;
                tools.push_back(Tool("Select", "Select only: no gizmo", !_session->gizmoEnabled, [this] { _session->SetGizmo(false, _session->gizmo); }));
                for (const auto& [mode, label, what] : { std::tuple { kor::GizmoMode::eTranslate, "Move", "Move what is picked" },
                                                         std::tuple { kor::GizmoMode::eRotate, "Rotate", "Rotate what is picked" },
                                                         std::tuple { kor::GizmoMode::eScale, "Scale", "Scale what is picked" } })
                    tools.push_back(Tool(label, what, _session->gizmoEnabled && _session->gizmo == mode, [this, mode] { _session->SetGizmo(true, mode); }));
                tools.push_back(kui::Tooltip("Frame what is picked (the view: right-drag turns, middle-drag moves, the wheel zooms)",
                                             Small("Frame", [this, selected] { _session->Frame(selected); })));
                tools.push_back(Tool("Camera", playMode ? "Playing: seen through the scene's camera" : "Look through the scene's camera", _session->ThroughSceneCamera(),
                                     [this, playMode] { if (!playMode) _session->SetLookThroughCamera(!_session->lookThroughCamera); }));
                tools.push_back(kui::Expanded(kui::Text("")));
                if (_session->IsPlaying()) tools.push_back(Tool("Pause", "Pause", false, [this] { _session->Pause(); }));
                else tools.push_back(Tool(playMode ? "Resume" : "Play", playMode ? "Resume" : "Play: scripts and the scene's systems run", playMode, [this] { _session->StartPlaying(); }));
                if (playMode) tools.push_back(Tool("Stop", "Stop, and put the scene back as it was", false, [this] { _session->Stop(); }));
                tools.push_back(kgui::Muted(std::format("{} fps", _session->Fps())));
                return kui::Column({
                    kui::Padding(kui::EdgeInsets::Symmetric(6.f, 3.f), kui::Row(std::move(tools), kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))),
                    kui::Expanded(kgui::SceneView(_session->Play(), _session->viewport)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetMainAxisSize(kui::MainAxisSize::eMax));
            }

            // ---- menus and dialogs ------------------------------------------------------------------------------

            kui::Widget Menus()
            {
                const kecs::Entity selected = Doc().World().Alive(_session->selectedEntity) ? _session->selectedEntity : kecs::NullEntity;
                const bool some = !selected.IsNull(), playMode = _session->InPlayMode();
                const auto needsProject = [this](std::function<void()> then) {
                    return [this, then = std::move(then)] { if (!_session->HasProject()) SetState([&] { _dialog = Dialog::eNewProject; }); else then(); };
                };
                // What there is to make: each object, at the top and — with something picked — under it.
                std::vector<kui::MenuItem> create;
                for (const auto& object : _session->Objects()) create.push_back({ object.name, [this, object] { _session->Create(object); } });
                if (some) {
                    create.push_back({ .separator = true });
                    for (const auto& object : _session->Objects())
                        create.push_back({ std::format("{} under {}", object.name, Doc().NameOf(selected)), [this, object, selected] { _session->Create(object, selected); } });
                }
                const auto ticked = [](const bool on, const char* label) { return std::string(on ? "* " : "  ") + label; };
                return kui::MenuBar({
                    { "File", {
                        { "New project...", [this] { SetState([&] { _dialog = Dialog::eNewProject; }); } },
                        { "Open project...", [this] { SetState([&] { _dialog = Dialog::eOpenProject; }); } },
                        { .separator = true },
                        { "New scene", needsProject([this] { _session->NewStandardScene(); }), !playMode },
                        { "Revert to saved", [this] { _session->Revert(); }, _session->HasProject() && !playMode },
                        { "Save scene", needsProject([this] { _session->Save(); }), !playMode },
                    } },
                    { "Edit", {
                        { "Duplicate", [this, selected] { _session->Duplicate(selected); }, some },
                        { "Move to the top", [this, selected] { Doc().SetParent(selected, kecs::NullEntity); }, some && !Doc().World().ParentOf(selected).IsNull() },
                        { "Save as object", [this, selected] { _session->SaveAsObject(selected); }, some },
                        { .separator = true },
                        { "Delete", [this, selected] { _session->Delete(selected); }, some },
                    } },
                    { "Create", std::move(create) },
                    { "View", {
                        { "Frame the selection", [this, selected] { _session->Frame(selected); }, some },
                        { ticked(_session->ThroughSceneCamera(), "Look through the scene's camera"), [this] { _session->SetLookThroughCamera(!_session->lookThroughCamera); }, !playMode },
                        { .separator = true },
                        { ticked(!_session->gizmoEnabled, "Select only"), [this] { _session->SetGizmo(false, _session->gizmo); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eTranslate, "Move"), [this] { _session->SetGizmo(true, kor::GizmoMode::eTranslate); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eRotate, "Rotate"), [this] { _session->SetGizmo(true, kor::GizmoMode::eRotate); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eScale, "Scale"), [this] { _session->SetGizmo(true, kor::GizmoMode::eScale); } },
                    } },
                    { "Scene", {
                        _session->IsPlaying() ? kui::MenuItem { "Pause", [this] { _session->Pause(); } }
                                              : kui::MenuItem { playMode ? "Resume" : "Play", [this] { _session->StartPlaying(); } },
                        { "Stop", [this] { _session->Stop(); }, playMode },
                    } },
                    { "Project", {
                        { "Run", needsProject([this] { _session->Run(); }) },
                    } },
                    { "Graph", {
                        { "Add node...   Ctrl+Space", [this] { SetState([&] { _dialog = Dialog::eAddNode; _addAt = { 80.f, 80.f }; _filter.clear(); _newId.clear(); }); } },
                    } },
                    { "Entity", {
                        { "New entity", [this] { _session->Select(Doc().CreateEntity()); } },
                        { "New tag...", [this] { SetState([&] { _dialog = Dialog::eNone; _session->Say("Give the tag a name in the inspector of an entity."); }); } },
                    } },
                });
            }

            kui::Widget DialogBody()
            {
                switch (_dialog) {
                case Dialog::eAddNode: return AddNodeDialog();
                case Dialog::eNewProject:
                    return kgui::FileBrowser({ .directories = true, .createDirectories = true, .confirm = "Make the project here" },
                        [this](const std::filesystem::path& chosen) { _session->NewProject(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                case Dialog::eOpenProject:
                    return kgui::FileBrowser({ .directories = true, .confirm = "Open" },
                        [this](const std::filesystem::path& chosen) { _session->OpenProject(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                default: return {};
                }
            }

            void AddOf(const std::string& type)
            {
                const std::string id = Doc().AddNode(type, _addAt.x, _addAt.y, _newId);
                _session->Select(id);
                _session->Say(std::format("Added {}.", id));
                SetState([&] { _dialog = Dialog::eNone; });
            }

            /**
             * The nodes there are to add. With nothing typed, every kind, under its category. As something is typed,
             * the kinds it finds, best first, each with what it does — and the nodes of this graph it names, to go to.
             * Enter adds the first.
             */
            kui::Widget AddNodeDialog()
            {
                const auto found = FindNodeTypes(_filter);
                kui::Widget results;
                if (_filter.empty()) {
                    std::vector<std::pair<std::string, std::vector<kui::Widget>>> byCategory;
                    for (const auto* type : found) {
                        auto column = std::ranges::find_if(byCategory, [&](const auto& c) { return c.first == type->category; });
                        if (column == byCategory.end()) column = byCategory.insert(byCategory.end(), { type->category, {} });
                        column->second.push_back(kui::Tooltip(type->doc, kui::Selectable(type->name, false, [this, name = type->name] { AddOf(name); })));
                    }
                    std::vector<kui::Widget> columns;
                    for (auto& [category, items] : byCategory) {
                        items.insert(items.begin(), kgui::Muted(category));
                        // A long category in two columns, so the list is not taller than the window.
                        const std::size_t per = items.size() > 26 ? (items.size() + 1) / 2 : items.size();
                        for (std::size_t at = 0; at < items.size(); at += per)
                            columns.push_back(kui::Column(std::vector<kui::Widget>(items.begin() + static_cast<std::ptrdiff_t>(at),
                                                                                    items.begin() + static_cast<std::ptrdiff_t>(std::min(at + per, items.size()))),
                                                          kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(0.f)).Width(170.f));
                    }
                    results = kui::Row(std::move(columns), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(10.f));
                } else {
                    std::vector<kui::Widget> rows;
                    constexpr std::size_t Most = 12;
                    for (std::size_t i = 0; i < found.size() && i < Most; ++i) {
                        const auto* type = found[i];
                        rows.push_back(kui::Selectable(std::format("{}   ·   {}", type->name, type->category), i == 0, [this, name = type->name] { AddOf(name); }));
                        rows.push_back(kui::Padding(kui::EdgeInsets { 14.f, 0.f, 0.f, 4.f }, kgui::Line(type->doc, kui::TextStyle {}.SetColor(kui::Theme::Current().textMuted).SetSize(12.f))));
                    }
                    if (found.size() > Most) rows.push_back(kgui::Muted(std::format("and {} more: type more of it", found.size() - Most)));
                    // The nodes this graph has, by name: picked, to go on working with.
                    std::string lowered = _filter;
                    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    std::size_t here = 0;
                    for (const auto& node : Doc().Graph().nodes) {
                        std::string id = node.id;
                        std::ranges::transform(id, id.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (id.find(lowered) == std::string::npos || ++here > 6) continue;
                        if (here == 1) rows.push_back(kgui::Muted("In this graph"));
                        rows.push_back(kui::Selectable(std::format("{}   ·   {}", node.id, node.type), false, [this, picked = node.id] {
                            _session->Select(picked);
                            SetState([&] { _dialog = Dialog::eNone; });
                        }));
                    }
                    if (rows.empty()) rows.push_back(kgui::Muted(std::format("Nothing is called, or does, '{}'.", _filter)));
                    results = kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(0.f)).Width(700.f);
                }
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading("Add a node"),
                    kui::Row({
                        kui::TextField(kui::TextFieldOptions { .text = _filter, .placeholder = "Type what you are after: image, blur, clamp, compare...",
                                                               .onChanged = [this](const std::string& typed) { SetState([&] { _filter = typed; }); },
                                                               .onSubmitted = [this](const std::string& typed) {
                                                                   if (const auto first = FindNodeTypes(typed); !typed.empty() && !first.empty()) AddOf(first.front()->name);
                                                               }, .width = 360.f, .focus = 1 }),
                        kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name (a pipeline's names its component)",
                                                               .onChanged = [this](const std::string& typed) { _newId = typed; }, .width = 320.f }),
                    }, kui::FlexOptions {}.SetGap(8.f)),
                    std::move(results),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(12.f)));
            }

            // ---- the graph --------------------------------------------------------------------------------------

            kui::Widget Graph()
            {
                const kgraph::Graph& graph = Doc().Graph();
                const kgraph::Runtime& runtime = Doc().Runtime();
                kui::NodeGraph shown;
                std::set<std::pair<std::string, std::string>> wiredFrom;
                for (const auto& node : graph.nodes)
                    for (const auto& input : node.inputs | std::views::values)
                        if (input.link) wiredFrom.insert({ input.link->node, input.link->output });
                for (const auto& node : graph.nodes) {
                    const kgraph::NodeType* type = kgraph::FindType(node.type);
                    kui::GraphNode card;
                    card.id = node.id;
                    card.title = node.title.empty() ? node.id : node.title;
                    card.position = { static_cast<float>(node.x), static_cast<float>(node.y) };
                    card.accent = CategoryColor(type ? type->category : std::string());
                    for (const auto& shown : InputsOf(runtime, node, [&](const std::string& input) { return _session->Asked(node.id, input); }))
                        if (ShowsAsPort(shown, node))
                            card.inputs.push_back({ shown.port.name, shown.port.name, (shown.port.kind == kgraph::PortKind::eResource ? "r:" : "v:") + shown.port.type });
                    // What it gives: what it makes, and whatever else of it something is wired to or was asked for.
                    if (type)
                        for (const auto& output : type->outputs)
                            if (!output.optional || wiredFrom.contains({ node.id, output.name }) || _session->Asked(node.id, "out:" + output.name))
                                card.outputs.push_back({ output.name, output.name == "out" ? output.type : output.name,
                                                         (output.kind == kgraph::PortKind::eResource ? "r:" : "v:") + output.type });
                    if (const auto* status = runtime.Status(node.id); status && !status->ok) card.error = status->error;
                    card.body = kui::Padding(kui::EdgeInsets::Symmetric(8.f, 2.f), kgui::Muted(node.type, 11.f));
                    shown.nodes.push_back(std::move(card));
                    for (const auto& [name, input] : node.inputs)
                        if (input.link) shown.wires.push_back({ { input.link->node, input.link->output }, { node.id, name } });
                }
                return kui::NodeEditor(std::move(shown), kui::NodeEditorOptions {}
                    .CanConnect([](const kui::NodePort& from, const kui::NodePort& to) {
                        // A resource to what takes one of its kind (a view of an image where an image goes), a value to a value.
                        if (from.type.substr(0, 2) != to.type.substr(0, 2)) return false;
                        if (from.type.starts_with("v:")) return true;
                        return from.type == to.type || from.type == "r:Resource" || to.type == "r:Resource" || (from.type == "r:ImageView" && to.type == "r:Image");
                    })
                    .OnConnect([this](const kui::GraphWire& wire) { Doc().Connect(wire.from.node, wire.from.port, wire.to.node, wire.to.port); })
                    .OnDisconnect([this](const kui::GraphWire& wire) { Doc().ClearInput(wire.to.node, wire.to.port); })
                    // Built again at once: a frame with the nodes back where they were is a frame of them jumping.
                    .OnMove([this](const std::vector<std::string>& nodes, const kor::Vec2 by) { Doc().MoveNodes(nodes, by.x, by.y); SetState(); })
                    .OnDelete([this](const std::vector<std::string>& nodes, const std::vector<kui::GraphWire>& wires, const std::vector<std::string>&) {
                        for (const auto& wire : wires) Doc().ClearInput(wire.to.node, wire.to.port);
                        if (!nodes.empty()) Doc().RemoveNodes(nodes);
                        if (std::ranges::find(nodes, _session->selectedNode) != nodes.end()) _session->Select(std::string());
                    })
                    .OnSelectionChanged([this](const std::vector<std::string>& nodes) { if (!nodes.empty()) _session->Select(nodes.front()); })
                    .OnContextMenu([this](const kor::Vec2 at, kor::Vec2) { SetState([&] { _dialog = Dialog::eAddNode; _addAt = at; _filter.clear(); _newId.clear(); }); }));
            }

            // ---- the outliner -----------------------------------------------------------------------------------

            /** An entity in the outliner, and under it — further in — everything that is under it. */
            kui::Widget EntityRow(const kecs::Entity entity)
            {
                const auto& children = Doc().World().ChildrenOf(entity);
                std::vector<kui::Widget> under;
                if (!_session->Folded(entity))
                    for (const kecs::Entity child : children) under.push_back(EntityRow(child));
                kui::Widget node = kui::TreeNode(Doc().NameOf(entity), !_session->Folded(entity), [this, entity](const bool open) { _session->Fold(entity, !open); },
                                                 std::move(under), kui::TreeNodeOptions {}.SetLeaf(children.empty()).SetSelected(entity == _session->selectedEntity)
                                                     .OnTap([this, entity] { _session->Select(entity); }));
                // Picked up and dropped on another, it goes under it: where it is in the world stays as it was.
                node = kui::Draggable({ "entity", entity }, std::move(node));
                node = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("entity").OnDrop([this, entity](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* moved = dragged.As<kecs::Entity>(); moved && !Doc().SetParent(*moved, entity) && *moved != entity)
                        _session->Say(std::format("'{}' is above '{}': it cannot go under it", Doc().NameOf(*moved), Doc().NameOf(entity)), true);
                }), std::move(node));
                std::vector<kui::MenuItem> menu;
                menu.push_back({ "Frame in the scene view", [this, entity] { _session->Select(entity); _session->Frame(entity); } });
                menu.push_back({ "Duplicate", [this, entity] { _session->Duplicate(entity); } });
                if (!Doc().World().ParentOf(entity).IsNull()) menu.push_back({ "Move to the top", [this, entity] { Doc().SetParent(entity, kecs::NullEntity); } });
                menu.push_back({ "Save as object", [this, entity] { _session->SaveAsObject(entity); } });
                menu.push_back({ .separator = true });
                for (const auto& object : _session->Objects())
                    menu.push_back({ std::format("New {} under it", object.name), [this, object, entity] { _session->Create(object, entity); } });
                menu.push_back({ .separator = true });
                menu.push_back({ "Delete, with what is under it", [this, entity] { _session->Delete(entity); } });
                return kui::ContextMenu(std::move(menu), std::move(node)).Key(std::format("entity {}.{}", entity.index, entity.generation));
            }

            /** The world's entities as a tree: picked by pressing, folded by their arrows, moved under one another by dragging. */
            kui::Widget Outliner()
            {
                std::vector<kui::Widget> rows;
                rows.push_back(Small("+ Entity", [this] { _session->Select(Doc().CreateEntity()); }));
                for (const kecs::Entity entity : Doc().World().Roots()) rows.push_back(EntityRow(entity));
                if (rows.size() == 1) rows.push_back(kgui::Muted("No entities yet."));
                // Dropped on the panel itself, past the tree, an entity goes to the top.
                return kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("entity").OnDrop([this](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* moved = dragged.As<kecs::Entity>()) Doc().SetParent(*moved, kecs::NullEntity);
                }), Panel(std::move(rows)));
            }

            // ---- the inspector ----------------------------------------------------------------------------------

            kui::Widget Inspector()
            {
                if (Doc().World().Alive(_session->selectedEntity)) return EntityInspector(_session->selectedEntity);
                if (const kgraph::Node* node = Doc().Graph().Find(_session->selectedNode)) return NodeInspector(*node);
                return Panel({ kgui::Muted("Pick a node or an entity to see what it is made of.") });
            }

            /** The control an input is set with: by what kind of thing it is, then by its type. */
            kui::Widget EditorFor(const kgraph::Node& node, const kgraph::Port& port, const Value& value, const std::function<void(Value)>& set, const std::string& key)
            {
                if (!port.choices.empty()) return Choice(port.choices, value, set, port.choices.front());
                if (port.editor == "color") return ColorOf(value, set);
                if (port.editor == "module") {
                    const auto modules = _session->ShaderModules();
                    return modules.empty() ? TextEdit(value, set, key, "a module's name") : Choice(modules, value, set, "a module");
                }
                if (port.editor == "entry") {
                    // The entry points of the module named beside it.
                    const std::string beside = port.name.substr(0, port.name.size() - 5) + "module";
                    const auto module = node.inputs.find(beside);
                    const std::string* named = module == node.inputs.end() ? nullptr : module->second.value.String();
                    const auto entries = named ? _session->EntryPoints(*named) : std::vector<std::string> {};
                    return entries.empty() ? TextEdit(value, set, key, "the function's name") : Choice(entries, value, set, "an entry point");
                }
                if (port.editor == "node") {
                    std::vector<std::string> ids;
                    for (const auto& other : Doc().Graph().nodes)
                        if (other.id != node.id && Doc().Runtime().ResourceOf(other.id)) ids.push_back(other.id);
                    return Choice(std::move(ids), value, set, "a node");
                }
                if (port.editor == "components") return Ticks(Doc().ComponentNames(), value, set);
                if (port.editor == "parameter") {
                    // The image parameters of the pipeline this pass runs.
                    std::vector<std::string> names;
                    if (const auto pipeline = node.inputs.find("pipeline"); pipeline != node.inputs.end() && pipeline->second.link)
                        for (const auto& each : Doc().Runtime().PortsOf(pipeline->second.link->node))
                            if (each.name.starts_with("param.") && each.type == "Image" && !each.name.ends_with(".sampler")) names.push_back(each.name.substr(6));
                    return Choice(std::move(names), value, set, "an image parameter");
                }
                if (port.editor == "attributes") return Attributes(value, set, key);
                if (port.editor == "numbers") {
                    // A few numbers that are one thing: a box (x, y, width, height), a depth bias's three.
                    std::vector<float> numbers(port.name == "depthBias" ? 3 : 4, 0.f);
                    if (const auto* items = value.Items())
                        for (std::size_t i = 0; i < numbers.size() && i < items->size(); ++i) numbers[i] = NumberOf((*items)[i]);
                    return kgui::DragFloats(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); },
                                            port.name == "depthBias" ? 0.01f : 1.f, port.name == "depthBias" ? 2 : 0);
                }
                return port.builder ? TypeEditor(*port.builder, value, set, key) : NamedEditor(port.type, value, set, key);
            }

            kui::Widget NodeInspector(const kgraph::Node& node)
            {
                const kgraph::Runtime& runtime = Doc().Runtime();
                const std::string id = node.id;
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(id));
                rows.push_back(kgui::Muted(node.type));
                if (const auto* status = runtime.Status(id); status && !status->ok) rows.push_back(Problem(status->error));
                if (const auto* report = runtime.ReportOf(id)) {
                    rows.push_back(kgui::Muted(std::format("Runs for {} {}.", report->ran, report->ran == 1 ? "entity" : "entities")));
                    if (report->skipped > 0) rows.push_back(Problem(std::format("{} passed by. {}", report->skipped, report->why)));
                    else if (!report->why.empty()) rows.push_back(Problem(report->why));
                }
                if (const kecs::ComponentId component = runtime.ComponentOf(id); component != kecs::NoComponent)
                    rows.push_back(kgui::Muted(std::format("Entities run it by having the component '{}'.", Doc().World().Describe(component).name)));
                rows.push_back(kui::Separator());

                std::vector<kgraph::Port> more;
                const auto inputs = InputsOf(runtime, node, [&](const std::string& input) { return _session->Asked(id, input); }, &more);
                const auto set = [this, id](const std::string& name) {
                    return [this, id, name](Value changed) {
                        if (changed.IsNull()) Doc().ClearInput(id, name);
                        else Doc().SetInput(id, name, std::move(changed));
                    };
                };
                // Back to nothing given; one that can be done without goes from the node with it.
                const auto remove = [this, id](const std::vector<std::string>& names) {
                    return [this, id, names] {
                        for (const auto& name : names) { Doc().ClearInput(id, name); _session->Hide(id, name); }
                    };
                };
                const auto plain = [&](const Shown& shown) {    // set here, with a control: not wired, and not left open for a wire
                    if (shown.port.kind != kgraph::PortKind::eValue) return false;
                    const auto input = node.inputs.find(shown.port.name);
                    return input == node.inputs.end() || (!input->second.link && !input->second.value.IsNull());
                };
                const auto whole = [](const kgraph::Port& port) {
                    return port.builder && (port.builder->kind == kb::Kind::eInt || port.builder->kind == kb::Kind::eUInt);
                };

                for (std::size_t at = 0; at < inputs.size(); ++at) {
                    const Shown& shown = inputs[at];
                    const kgraph::Port& port = shown.port;
                    const std::string name = port.name;
                    const auto input = node.inputs.find(name);

                    // Numbers that are one thing's parts — extent.x, extent.y, extent.z — on one line, under its name.
                    if (name.ends_with(".x") && plain(shown) && (port.type == "number")) {
                        const std::string prefix = name.substr(0, name.size() - 1);
                        std::vector<std::string> parts { name };
                        for (const char* axis : { "y", "z", "w" }) {
                            const std::size_t next = at + parts.size();
                            if (next < inputs.size() && inputs[next].port.name == prefix + axis && plain(inputs[next])) parts.push_back(prefix + axis);
                            else break;
                        }
                        if (parts.size() > 1) {
                            std::vector<float> numbers;
                            bool any = false;
                            for (const auto& part : parts) {
                                const auto each = node.inputs.find(part);
                                any = any || each != node.inputs.end();
                                const auto listed = std::ranges::find_if(inputs, [&](const Shown& other) { return other.port.name == part; });
                                numbers.push_back(each != node.inputs.end() ? NumberOf(each->second.value) : listed != inputs.end() ? NumberOf(listed->port.initial) : 0.f);
                            }
                            auto editor = kgui::DragFloats(numbers, [this, id, parts, numbers, round = whole(port)](const std::vector<float>& changed) {
                                for (std::size_t i = 0; i < parts.size(); ++i)
                                    if (changed[i] != numbers[i]) Doc().SetInput(id, parts[i], Value(round ? std::round(changed[i]) : changed[i]));
                            }, whole(port) ? 1.f : 0.01f, whole(port) ? 0 : 2);
                            std::vector<kui::Widget> line { kui::Expanded(std::move(editor)) };
                            if (any) line.push_back(kui::Tooltip("Back to nothing given", Small("x", remove(parts))));
                            rows.push_back(kui::Tooltip(port.doc.empty() ? port.type : port.doc,
                                                        kgui::Labeled(prefix.substr(0, prefix.size() - 1), kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)), LabelWidth)));
                            at += parts.size() - 1;
                            continue;
                        }
                    }

                    kui::Widget editor;
                    if (shown.given && input->second.link) {
                        editor = kgui::Line(std::format("from {}{}", input->second.link->node, input->second.link->output == "out" ? "" : "." + input->second.link->output));
                    } else if (port.kind == kgraph::PortKind::eResource) {
                        editor = kgui::Muted(port.type == "Resource" || port.type == "value" ? std::string("wire one in") : std::format("wire one in: {}", port.type));
                    } else if (shown.given && input->second.value.IsNull()) {
                        editor = kgui::Muted("a port: wire a value in");
                    } else {
                        // Not given, it shows what it is all the same: what the node type says it starts as.
                        editor = EditorFor(node, port, shown.given ? input->second.value : port.initial, set(name), id + "/" + name);
                    }
                    std::vector<kui::Widget> line { kui::Expanded(std::move(editor)) };
                    // A value a node can work out — a number, a vector, a colour — can be wired instead: a name chosen from a list cannot.
                    if (plain(shown) && port.choices.empty() && (port.editor.empty() || port.editor == "color" || port.editor == "numbers"))
                        line.push_back(kui::Tooltip("Make it a port, to wire a value to", Small("o", [this, id, name] { Doc().SetInput(id, name, Value {}); })));
                    if (shown.given || port.optional)
                        line.push_back(kui::Tooltip(shown.given ? "Back to nothing given" : "Not needed after all", Small("x", remove({ name }))));
                    rows.push_back(kui::Tooltip(port.doc.empty() ? port.type : port.doc + " (" + port.type + ")",
                                                kgui::Labeled(name, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)), LabelWidth)));
                }

                // What else it can be given: asked for here, it joins the inputs above — as a port on the node, when it takes a resource.
                if (!more.empty()) {
                    std::vector<std::string> names;
                    for (const auto& port : more) names.push_back(port.name);
                    rows.push_back(kui::Separator());
                    rows.push_back(kgui::Labeled("Add input", kui::Dropdown(names, -1, [this, id, more, kind = node.type](const int index) {
                        // Given what it starts as, when its type says what that is: what shows is then what is so.
                        const auto& port = more[static_cast<std::size_t>(index)];
                        // The whole of what is otherwise given in parts is asked for to be wired: a port, from the start.
                        if (const auto* type = kgraph::FindType(kind); type && !kgraph::PartsOf(*type, port.name).empty()) Doc().SetInput(id, port.name, Value {});
                        else if (port.kind == kgraph::PortKind::eValue && !port.initial.IsNull()) Doc().SetInput(id, port.name, port.initial);
                        else _session->Show(id, port.name);
                    }, kui::DropdownOptions {}.SetPlaceholder(std::format("{} more it can be given", names.size()))), LabelWidth));
                }
                // What it gives, besides what it makes: its size, its settings. Asked for, it is an output on the node to wire from.
                if (const kgraph::NodeType* type = kgraph::FindType(node.type)) {
                    std::vector<std::string> outputs;
                    bool first = true;
                    for (const auto& output : type->outputs) {
                        if (!output.optional) continue;
                        std::size_t wires = 0;
                        for (const auto& other : Doc().Graph().nodes)
                            for (const auto& input : other.inputs | std::views::values)
                                if (input.link && input.link->node == id && input.link->output == output.name) ++wires;
                        if (wires == 0 && !_session->Asked(id, "out:" + output.name)) { outputs.push_back(output.name); continue; }
                        // One the node shows: there until it is taken off again, and what is wired from it with it.
                        if (std::exchange(first, false)) rows.push_back(kui::Separator());
                        std::vector<kui::Widget> line { kui::Expanded(kgui::Muted(wires == 0 ? std::string("an output: wire from it")
                                                                                             : std::format("an output, wired to {} {}", wires, wires == 1 ? "input" : "inputs"))) };
                        line.push_back(kui::Tooltip(wires == 0 ? "Take the output off the node" : "Take the output off the node, and its wires with it",
                                                    Small("x", [this, id, name = output.name] { _session->Hide(id, "out:" + name); Doc().DisconnectOutput(id, name); })));
                        rows.push_back(kui::Tooltip(output.doc.empty() ? output.type : output.doc + " (" + output.type + ")",
                                                    kgui::Labeled(output.name, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)), LabelWidth)));
                    }
                    if (!outputs.empty())
                        rows.push_back(kgui::Labeled("Add output", kui::Dropdown(outputs, -1, [this, id, outputs](const int index) {
                            _session->Show(id, "out:" + outputs[static_cast<std::size_t>(index)]);
                        }, kui::DropdownOptions {}.SetPlaceholder(std::format("{} things of it to wire from", outputs.size()))), LabelWidth));
                }
                rows.push_back(kui::Separator());
                rows.push_back(Small("Remove the node", [this, id] { Doc().RemoveNodes({ id }); _session->Select(std::string()); }));
                return Panel(std::move(rows));
            }

            kui::Widget EntityInspector(const kecs::Entity entity)
            {
                Document& doc = Doc();
                std::vector<kui::Widget> rows;
                const std::string name = doc.NameOf(entity);
                rows.push_back(kgui::Labeled("Name", kui::TextField(kui::TextFieldOptions { .text = name,
                    .onSubmitted = [this, entity](const std::string& typed) { Doc().Rename(entity, typed); } }).Key("name=" + name), LabelWidth));

                // Where it is, relative to what it is under: every entity has that, and it is no component.
                const kecs::World& world = doc.World();
                const auto three = [&](const char* label, const kecs::Vec3 value, const float speed, const char* what, auto set) {
                    rows.push_back(kui::Tooltip(what, kgui::Labeled(label, kgui::DragFloats({ value.x, value.y, value.z }, [this, entity, set](const std::vector<float>& v) {
                        (Doc().*set)(entity, kecs::Vec3 { v[0], v[1], v[2] });
                    }, speed, 2), LabelWidth)));
                };
                if (const kecs::Entity parent = world.ParentOf(entity); !parent.IsNull()) rows.push_back(kgui::Muted(std::format("Under '{}'.", doc.NameOf(parent))));
                three("Position", world.PositionOf(entity), 0.01f, "Where it is, relative to what it is under.", &Document::SetPosition);
                three("Rotation", world.RotationOf(entity), 0.5f, "How it is turned, in degrees: about X, about Y, about Z.", &Document::SetRotation);
                three("Scale", world.ScaleOf(entity), 0.01f, "How big it is, each way.", &Document::SetScale);

                const auto has = doc.ComponentsOf(entity);
                for (const auto& component : has) {
                    rows.push_back(kui::Separator());
                    rows.push_back(kui::Row({
                        kui::Tooltip("Switched off, it has the component still and nothing runs for it",
                                     kui::Checkbox(doc.IsEnabled(entity, component), [this, entity, component](const bool on) { Doc().SetEnabled(entity, component, on); })),
                        kui::Expanded(Heading(component)),
                        Small("Remove", [this, entity, component] { Doc().RemoveComponent(entity, component); }),
                    }, kui::FlexOptions {}.SetGap(6.f)));
                    const auto& type = doc.World().Describe(doc.World().Find(component));
                    for (const auto& field : type.fields) {
                        const std::string fieldName = field.name;
                        const Value value = doc.Field(entity, component, fieldName);
                        const auto set = [this, entity, component, fieldName](const Value& changed) {
                            std::string error;
                            if (!Doc().SetField(entity, component, fieldName, changed, &error)) _session->Say(error, true);
                        };
                        kui::Widget editor;
                        if (HoldsResource(field.type)) {
                            std::vector<std::string> choices { "(nothing)" };
                            int selected = 0;
                            for (const auto& node : doc.NodesFor(field.type)) {
                                if (value.String() && *value.String() == node) selected = static_cast<int>(choices.size());
                                choices.push_back(node);
                            }
                            editor = kui::Dropdown(choices, selected, [set, choices](const int index) {
                                set(index <= 0 ? Value {} : Value(choices[static_cast<std::size_t>(index)]));
                            });
                        } else if (!Editable(field.type)) {
                            editor = kgui::Muted(field.type);
                        } else if (value.Bool()) {
                            editor = kui::Checkbox(*value.Bool(), [set](const bool on) { set(Value(on)); });
                        } else if (value.String()) {
                            const std::string text = *value.String();
                            editor = kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } })
                                .Key(component + "." + fieldName + "=" + text);
                        } else if ((field.type == "vec4" || field.type == "vec3") && NamesAColor(fieldName)) {
                            editor = ColorOf(value, set, field.type == "vec4");
                        } else if (const auto* items = value.Items()) {
                            std::vector<float> numbers;
                            for (const auto& item : *items) numbers.push_back(NumberOf(item));
                            const bool whole = field.type.starts_with("i") || field.type.starts_with("u") || field.type.starts_with("b");
                            editor = kgui::DragFloats(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); },
                                                      whole ? 1.f : 0.01f, whole ? 0 : 2);
                        } else {
                            const bool whole = field.type.starts_with("i") || field.type.starts_with("u");
                            editor = kui::DragValue(NumberOf(value), [set](const float v) { set(Value(v)); },
                                                    kui::DragValueOptions {}.SetSpeed(whole ? 1.f : 0.01f).SetDecimals(whole ? 0 : 2));
                        }
                        rows.push_back(kui::Tooltip(field.type, kgui::Labeled(fieldName, std::move(editor), LabelWidth)));
                    }
                }

                rows.push_back(kui::Separator());
                std::vector<std::string> more;
                for (const auto& component : doc.ComponentNames())
                    if (std::ranges::find(has, component) == has.end()) more.push_back(component);
                rows.push_back(kgui::Labeled("Add", kui::Dropdown(more, -1, [this, entity, more](const int index) {
                    Doc().AddComponent(entity, more[static_cast<std::size_t>(index)]);
                }, kui::DropdownOptions {}.SetPlaceholder("a component, or a tag")), LabelWidth));
                rows.push_back(kgui::Labeled("New tag", kui::TextField(kui::TextFieldOptions { .placeholder = "its name, then Enter",
                    .onSubmitted = [this, entity](const std::string& typed) {
                        if (typed.empty()) return;
                        Doc().AddTag(typed);
                        Doc().AddComponent(entity, typed);
                    } }).Key(std::format("tag{}", has.size())), LabelWidth));
                rows.push_back(Small("Destroy the entity", [this, entity] { Doc().DestroyEntity(entity); _session->Select(kecs::NullEntity); }));
                return Panel(std::move(rows));
            }

            // ---- what is wrong ----------------------------------------------------------------------------------

            kui::Widget Problems()
            {
                const kgraph::Runtime& runtime = Doc().Runtime();
                std::vector<kui::Widget> rows;
                for (const auto& problem : runtime.Problems()) {
                    const std::string node = problem.substr(0, problem.find(':'));
                    rows.push_back(Problem(problem).OnTap([this, node] { _session->Select(node); }));
                }
                for (const auto& node : Doc().Graph().nodes)
                    if (const auto* report = runtime.ReportOf(node.id); report && (report->skipped > 0 || (report->ran == 0 && !report->why.empty())))
                        rows.push_back(Problem(report->skipped > 0 ? std::format("{}: {} passed by. {}", node.id, report->skipped, report->why)
                                                                   : std::format("{}: {}", node.id, report->why)).OnTap([this, id = node.id] { _session->Select(id); }));
                for (const auto& note : Doc().Notes()) rows.push_back(kgui::Muted(note));
                if (rows.empty()) rows.push_back(kgui::Muted("Nothing is wrong."));
                return Panel(std::move(rows));
            }

            std::shared_ptr<Session> _session;
            std::shared_ptr<kui::DockLayout> _layout;
            std::uint64_t _shown = 0;
            Dialog _dialog = Dialog::eNone;
            kor::Vec2 _addAt {};
            std::string _filter, _newId;
        };
    }

    kui::Widget EditorRoot(std::shared_ptr<Session> session) { return kui::Make<Root>(std::move(session)); }

    // ---- the scene ----------------------------------------------------------------------------------------------

    EditorScene::EditorScene(std::filesystem::path project) : _projectPath(std::move(project)) {}
    EditorScene::EditorScene(const kor::SceneArgs& arguments) : _projectPath(arguments.String("project")) {}
    EditorScene::~EditorScene() = default;

    void EditorScene::Initialize()
    {
        // The scene being worked on runs as it would on its own, into an image a panel shows.
        _play = kor::App::Current().OpenOffscreen<PlayScene>({ .title = "Scene", .extent = { 640, 360 } });
        // It does not play until it is told to: what is edited is the scene, not a moment of it.
        _play->SetPlaying(false);
        _session = std::make_shared<Session>(*_play);
        _session->LoadObjects();
        // The editor's view, its gizmo and its picking, as the scene updates: in the frame being made.
        _play->OnFrame([this] { if (_session) _session->Tick(SceneInput(), static_cast<float>(SceneTime().FrameTime())); });
        Document::Alias(&Graph(), &_play->Graph());     // whoever is handed this scene is after the one it edits
        if (!_projectPath.empty()) _session->OpenProject(_projectPath);
        _ui = std::make_unique<kui::Ui>(EditorRoot(_session));
        Graph().Add<kui::UiPass>(*_ui);
    }

    void EditorScene::Update()
    {
        _ui->Update();
        // Control+S saves, Control+R runs — unless a field is being typed into.
        const kor::Input& input = SceneInput();
        if (!input.InterfaceWantsKeyboard() && (input.IsKeyHeld(kor::Key::eLeftControl) || input.IsKeyHeld(kor::Key::eRightControl))) {
            if (input.IsKeyPressed(kor::Key::eS)) _session->Save();
            if (input.IsKeyPressed(kor::Key::eSpace)) _session->AskForNodes();     // Control+Space: the nodes there are to add
            if (input.IsKeyPressed(kor::Key::eR)) _session->Run();
        }
    }

    void EditorScene::Shutdown()
    {
        if (_play) _play->OnFrame({});
        _ui.reset();
        _session.reset();
        Document::Alias(&Graph(), nullptr);
        if (_play) kor::App::Current().Close(*_play);
        _play = nullptr;
    }
}
