#include <keditor/editor.h>
#include <keditor/viewer.h>

#include <algorithm>
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
#include <regex>
#include <set>

#include <app.h>
#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif
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
#include <kgraph/meshes.h>

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
        // A new project has nothing in it: not the scene of the project that was open when it was made. Only a scene
        // that was in no project yet — made here, and not saved anywhere — goes into it: this is where it is saved.
        const bool another = _hasProject;
        _project = std::move(project);
        _hasProject = true;
        _scene = _project.start;
        _project.Use();
        WriteStandardObjects(_project.directory / "objects");
        WriteEditorSetup(_project.directory, _project.name);
        LoadObjects();
        if (another) {
            Look({});
            _play.New();
            selectedNode.clear();
            selectedEntity = kecs::NullEntity;
            _pass.clear();
        }
        Doc().SetAssetDirectory(_project.directory / "assets");
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
        selectedAsset.clear();
        WriteEditorSetup(_project.directory, _project.name);
        LoadObjects();
        Say(std::format("Opened {}.", _project.name));
        _handingOver = handOver;    // over to its scripts' runner, if it has scripts: between frames, not in the middle of one
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

    namespace {
        /** A command's word, quoted for a shell. */
        std::string Quoted(const std::string& text) { return "\"" + text + "\""; }

        /** Whether two folders are the same one, however they are written. */
        bool Same(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            std::error_code ignored;
            return std::filesystem::equivalent(a, b, ignored);
        }

        /** What this user keeps of the editor's, from one run to the next: XDG_CONFIG_HOME's, or ~/.config's. */
        std::filesystem::path ConfigDirectory()
        {
            const char* home = std::getenv("HOME");
            const char* config = std::getenv("XDG_CONFIG_HOME");
            return (config && *config ? std::filesystem::path(config) : std::filesystem::path(home && *home ? home : ".") / ".config") / "koral-engine";
        }

        /** Says, to the editor about to be started, which project this one handed over: it does not hand it over again. */
        constexpr const char* HandedOver = "KORAL_ENGINE_HANDED_OVER";

        /** Which language a project's scripts are in: 'k', 'c', or 0 when it has none. */
        char ScriptLanguage(const Project& project) { return project.Language(); }
    }

    std::string ScriptRunner(const Project& project, const bool edit)
    {
        const std::string folder = project.directory.string();
        switch (ScriptLanguage(project)) {
        case 'k': {
            // The project's own build, which wants a JDK to start with: this build's, when none is said.
            const char* java = std::getenv("JAVA_HOME");
            const std::string home = (java && *java) || std::string_view(KEDITOR_JAVA_HOME).empty() ? std::string() : std::format("JAVA_HOME={} ", Quoted(KEDITOR_JAVA_HOME));
            return std::format("{}{} -p {} -q {}", home, Quoted((project.directory / "gradlew").string()), Quoted(folder), edit ? "edit" : "play");
        }
        case 'c': {
            const char* sdk = std::getenv("KORAL_SDK");
            const std::string where = sdk && *sdk ? std::string() : std::format("KORAL_SDK={} ", Quoted(KEDITOR_SDK));
            return std::format("{}{} {}{}", where, Quoted(KEDITOR_DOTNET_RUNNER), Quoted(folder), edit ? " --edit" : "");
        }
        default: return {};
        }
    }

    std::optional<int> RunThroughScripts(const Project& project)
    {
        const std::string command = ScriptRunner(project, true);
        if (command.empty()) return std::nullopt;
        // The runner that is started opens the editor with them, and says so: it is not handed over again.
        if (const char* was = std::getenv(HandedOver); was && Same(was, project.directory)) return std::nullopt;
        if (ScriptLanguage(project) == 'k') WriteKotlinBuild(project.directory, project.name);    // this build of the engine
#ifdef _WIN32
        _putenv_s(HandedOver, project.directory.string().c_str());
        return std::system(command.c_str());
#else
        setenv(HandedOver, project.directory.c_str(), 1);
        // A process of its own, in this one's terminal: what it says is said here, and this one ends when it does.
        const pid_t child = fork();
        if (child < 0) return std::nullopt;
        if (child == 0) {
            execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    }

    std::string Session::RunCommand() const
    {
        if (!_hasProject) return {};
        if (const std::string scripted = ScriptRunner(_project, false); !scripted.empty()) return scripted;
        // The player that was built with this editor: beside it. Failing that, whichever is found by name.
        std::filesystem::path player = "koral-play";
        std::error_code ec;
        if (const auto self = std::filesystem::read_symlink("/proc/self/exe", ec); !ec && std::filesystem::exists(self.parent_path() / "koral-play", ec))
            player = self.parent_path() / "koral-play";
        return std::format("\"{}\" \"{}\"", player.string(), _project.directory.string());
    }

    std::string Session::EditCommand() const { return _hasProject ? ScriptRunner(_project, true) : std::string(); }

    std::vector<std::string> Session::ScriptsNotRunning()
    {
        std::vector<std::string> idle;
        if (!_hasProject) return idle;
        const char* ending = ScriptLanguage(_project) == 'k' ? ".kt" : ".cs";   // those in another language run nowhere: a problem of their own
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(_project.directory / "scripts", ignored)) {
            if (entry.path().extension() != ending) continue;
            // A script is a component by its class's name, which the editor's own are named for their file.
            if (const std::string name = entry.path().stem().string(); !Doc().Scripts().Registered(name)) idle.push_back(name);
        }
        std::ranges::sort(idle);
        return idle;
    }

    bool Session::ReopenWithScripts()
    {
        const std::string command = EditCommand();
        if (command.empty()) { Say("The project has no scripts: there is nothing more to open it with.", true); return false; }
        if (InPlayMode()) Stop();
        if (Doc().Dirty() && !Save()) return false;
        if (!launch && handOverOnExit) {
            // The program goes to the runner once this window is closed, in its terminal: RunThroughScripts.
            handOverPending = _project.directory;
            Say("Opening again with the project's scripts...");
            if (quit) quit(); else kor::App::Current().Quit();
            return true;
        }
        if (ScriptLanguage(_project) == 'k') WriteKotlinBuild(_project.directory, _project.name);    // this build of the engine
#ifdef _WIN32
        _putenv_s(HandedOver, _project.directory.string().c_str());
        const std::string detached = "start \"\" " + command;
#else
        setenv(HandedOver, _project.directory.c_str(), 1);
        const std::string detached = "(" + command + ") >/dev/null 2>&1 &";
#endif
        if (!(launch ? launch(command) : std::system(detached.c_str()) == 0)) { Say("The editor could not be started again with the scripts.", true); return false; }
        Say("Opening again with the project's scripts...");
        if (quit) quit(); else kor::App::Current().Quit();
        return true;
    }

    bool Session::HostsScripts() const { return _hasProject && !_scriptsHost.empty() && Same(_scriptsHost, _project.directory); }

    void Session::UpdateHandOver()
    {
        if (!std::exchange(_handingOver, false) || !_hasProject || !ScriptLanguage(_project) || HostsScripts()) return;
        // Opened by the runner it was handed to, and that one did not say it runs them (one older than this editor):
        // not again, round and round. The project says nothing runs its scripts, and Project, Open with its scripts is there.
        if (const char* was = std::getenv(HandedOver); was && Same(was, _project.directory)) return;
        ReopenWithScripts();
    }

    std::filesystem::path Session::StartupProject()
    {
        std::ifstream in(ConfigDirectory() / "startup-project");
        std::string line;
        if (!std::getline(in, line) || line.empty()) return {};
        std::error_code ignored;
        return std::filesystem::exists(std::filesystem::path(line) / "project.json", ignored) ? std::filesystem::path(line) : std::filesystem::path();
    }

    bool Session::SetStartupProject(const std::filesystem::path& project)
    {
        const std::filesystem::path file = ConfigDirectory() / "startup-project";
        std::error_code ec;
        if (project.empty()) { std::filesystem::remove(file, ec); return !ec; }
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::trunc);
        out << std::filesystem::absolute(project).lexically_normal().string() << '\n';
        return static_cast<bool>(out);
    }

    bool Session::Run()
    {
        if (!_hasProject) { Say("There is no project to run yet: File, New project.", true); return false; }
        if (!Save()) return false;
        // Scripts in Kotlin are run by the project's build, which is told where this build of the engine is: the
        // one that is open, a debug build or a release one.
        if (ScriptLanguage(_project) == 'k') WriteKotlinBuild(_project.directory, _project.name);
#ifdef _WIN32
        const std::string command = "start \"\" " + RunCommand();
#else
        const std::string command = RunCommand() + " &";
#endif
        if (std::system(command.c_str()) != 0) { Say("The player could not be started.", true); return false; }
        Say(std::format("Running {}.", _project.name));
        return true;
    }

    void Session::GoTo(const std::string& id)
    {
        if (const kgraph::Node* node = Doc().Graph().Find(id); node && !_everyPass && node->pass != _pass) {
            if (node->pass.empty()) _everyPass = true;
            else _pass = node->pass;
        }
        Select(id);
        ++_went;
    }

    void Session::CopyNodes(const std::vector<std::string>& ids)
    {
        std::vector<kgraph::Node> nodes;
        for (const auto& id : ids)
            if (const kgraph::Node* node = Doc().Graph().Find(id)) nodes.push_back(*node);
        if (nodes.empty()) return;
        _copied = std::move(nodes);
        Say(_copied.size() == 1 ? std::format("Copied {}.", _copied.front().id) : std::format("Copied {} nodes.", _copied.size()));
    }

    std::vector<std::string> Session::PasteNodes(const double x, const double y)
    {
        if (_copied.empty()) { Say("Nothing has been copied.", true); return {}; }
        const std::string pass = _everyPass ? std::string() : _pass;
        const auto made = Doc().PasteNodes(_copied, x, y, _everyPass ? nullptr : &pass);
        if (!made.empty()) { Select(made.front()); Say(made.size() == 1 ? std::format("Pasted {}.", made.front()) : std::format("Pasted {} nodes.", made.size())); }
        return made;
    }

    std::string Session::MakeFunction(const std::vector<std::string>& ids, const std::string& name)
    {
        std::string error;
        const std::string call = Doc().MakeFunction(ids, name, &error);
        if (call.empty()) { Say(error, true); return call; }
        if (const kgraph::Node* node = Doc().Graph().Find(call); node && !node->pass.empty()) ShowPass(node->pass);
        Select(call);
        Say(std::format("Made functions/{}.kfunc of {} nodes: {} calls it. Pressed twice, it is opened.", name, ids.size(), call));
        return call;
    }

    bool Session::OpenFunction(const std::string& call)
    {
        std::string error;
        const std::string page = Doc().OpenFunction(call, &error);
        if (page.empty()) { Say(error, true); return false; }
        ShowPass(page);
        Say(std::format("{} is open: what is done on this page is the function's, for every call of it. Done keeps it; Discard puts it back.", Doc().Opened()->function));
        return true;
    }

    bool Session::CloseFunction(const bool keep)
    {
        const auto opened = Doc().Opened();
        if (!opened) return false;
        std::string error;
        if (!Doc().CloseFunction(keep, &error)) { Say(error, true); return false; }
        if (!opened->call.pass.empty()) ShowPass(opened->call.pass);
        Select(opened->call.id);
        Say(keep ? std::format("Kept: functions/{}.kfunc is what its page was, and every call follows.", opened->function)
                 : std::format("{} is as it was.", opened->function));
        return true;
    }

    bool Session::WriteOut(const std::string& call)
    {
        std::string error;
        if (!Doc().WriteOut(call, &error)) { Say(error, true); return false; }
        Select(std::string());
        Say(std::format("{} is its nodes now, in its place: no call of its function.", call));
        return true;
    }

    bool Session::MoveNodesToPass(const std::vector<std::string>& ids, std::string pass)
    {
        if (ids.empty()) return false;
        if (pass.empty()) {
            // A new pass: named for what it holds first, and after every other.
            const auto& passes = Doc().Passes();
            const std::string base = ids.front();
            pass = base;
            for (int n = 2; std::ranges::find(passes, pass) != passes.end(); ++n) pass = std::format("{} {}", base, n);
        }
        if (!Doc().MoveNodesToPass(ids, pass)) { Say("There is nothing of those to move.", true); return false; }
        ShowPass(pass);
        Select(ids.front());
        Say(ids.size() == 1 ? std::format("Moved {} to {}.", ids.front(), pass) : std::format("Moved {} nodes to {}.", ids.size(), pass));
        return true;
    }

    std::vector<std::string> Session::CopyNodesToPass(const std::vector<std::string>& ids, const std::string& pass)
    {
        std::vector<kgraph::Node> nodes;
        for (const auto& id : ids)
            if (const kgraph::Node* node = Doc().Graph().Find(id)) nodes.push_back(*node);
        if (nodes.empty()) return {};
        double left = nodes.front().x, top = nodes.front().y;
        for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }
        const auto made = Doc().PasteNodes(std::move(nodes), left, top, &pass);
        if (made.empty()) return made;
        ShowPass(pass);
        Select(made.front());
        Say(made.size() == 1 ? std::format("Copied {} to {}.", made.front(), pass) : std::format("Copied {} nodes to {}.", made.size(), pass));
        return made;
    }

    std::vector<std::string> Session::DuplicateNodes(const std::vector<std::string>& ids)
    {
        std::vector<std::string> made;
        if (Doc().MakesInOverview()) {
            // Every node seen at once: the copies beside the nodes where they are seen there.
            const auto places = Doc().OverviewPlaces();
            std::vector<kgraph::Node> nodes;
            for (const auto& id : ids)
                if (const kgraph::Node* node = Doc().Graph().Find(id)) {
                    nodes.push_back(*node);
                    if (const auto at = places.find(id); at != places.end()) std::tie(nodes.back().x, nodes.back().y) = at->second;
                }
            if (!nodes.empty()) {
                double left = nodes.front().x, top = nodes.front().y;
                for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }
                made = Doc().PasteNodes(std::move(nodes), left + 40, top + 40);
            }
        } else made = Doc().DuplicateNodes(ids);
        if (!made.empty()) { Select(made.front()); Say(made.size() == 1 ? std::format("Made {}.", made.front()) : std::format("Made {} nodes.", made.size())); }
        return made;
    }

    std::filesystem::path Session::ShaderOf(const std::string_view id)
    {
        const kgraph::Node* node = Doc().Graph().Find(id);
        if (!node || !_hasProject) return {};
        for (const auto& [name, input] : node->inputs) {
            if (name != "module" && !name.ends_with(".module")) continue;
            if (!input.value.String() || input.value.String()->empty()) continue;
            const std::filesystem::path file = _project.ShaderDirectory() / (*input.value.String() + ".slang");
            std::error_code ignored;
            if (std::filesystem::is_regular_file(file, ignored)) return file;
        }
        return {};
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
        TrackUndo(seconds, input.IsMouseButtonHeld(kor::MouseButton::eLeft));

        // Frames a second, over a quarter of one: steady enough to read.
        ++_framesCounted;
        _secondsCounted += seconds;
        if (_secondsCounted >= 0.25f) {
            const int now = static_cast<int>(std::lround(static_cast<float>(_framesCounted) / _secondsCounted));
            _fps = now;     // read by what shows it, every frame: nothing else is built again for it
            _framesCounted = 0;
            _secondsCounted = 0.f;
        }

        const kecs::Entity selected = world.Alive(selectedEntity) ? selectedEntity : kecs::NullEntity;
        runtime.SetSelected(selected);
        // Picking, the line round what is picked and the gizmo are the editor's own passes: no part of the scene's graph.
        Overlay* const over = _play.Over();
        if (over) over->SetSelected(selected);
        // Its picture is made again when the panel is another size: the interface is told, to show the one there is now.
        // The meshes the Assets panel lists are drawn small, a few frames each, until each has its picture.
        if (_thumbnails && _thumbnails->Update()) ++_changes;
        // A file read since: what the Assets panel shows of it is another thing now.
        if (const std::size_t reading = Doc().Reading(); reading != _reading) { _reading = reading; ++_changes; }
        // The one it shows meanwhile is kept until it has stopped showing it, a few frames on.
        std::erase_if(_overBefore, [](auto& kept) { return --kept.second <= 0; });
        if (auto held = over ? over->PictureHeld() : nullptr; held != _overHeld) {
            if (_overHeld) _overBefore.emplace_back(std::move(_overHeld), 6);
            _overHeld = std::move(held);
            ++_changes;
        }

        // The view, moved by the mouse over the scene's panel: how far it went since the last frame, from where it was.
        const kor::Vec2 mouse = input.MousePosition();
        const kor::Vec2 delta = _lastMouse ? mouse - *_lastMouse : kor::Vec2(0.f);
        _lastMouse = mouse;
        const auto pointer = viewport->PointerPosition();
        const kor::UVec2 size = viewport->Size();
        if (ThroughSceneCamera()) {
            _viewDrag.reset();
            runtime.SetView(std::nullopt);
            if (over) over->SetView(std::nullopt);
        } else {
            if (_viewDrag && !input.IsMouseButtonHeld(*_viewDrag) && !input.IsMouseButtonPressed(*_viewDrag)) _viewDrag.reset();
            if (!_viewDrag && viewport->Hovered())
                for (const kor::MouseButton button : { kor::MouseButton::eRight, kor::MouseButton::eMiddle })
                    if (input.IsMouseButtonPressed(button)) { _viewDrag = button; break; }
            if (_viewDrag == kor::MouseButton::eRight) {
                // Looked round from where it is, and flown with the keys while the button is held — as a scene's own
                // FlyCamera is: W A S D along and across the way it looks, E and Q up and down, Shift faster.
                const kor::Vec3 eye = camera.Position();
                camera.Orbit(delta.x, delta.y);
                camera.target += eye - camera.Position();
                const auto key = [&](const kor::Key held) { return !input.InterfaceWantsKeyboard() && input.IsKeyHeld(held) ? 1.f : 0.f; };
                const float along = key(kor::Key::eW) - key(kor::Key::eS), across = key(kor::Key::eD) - key(kor::Key::eA), up = key(kor::Key::eE) - key(kor::Key::eQ);
                if (along != 0.f || across != 0.f || up != 0.f) {
                    const kor::Vec3 forward = kor::Normalize(camera.target - camera.Position());
                    const kor::Vec3 right = kor::Normalize(kor::Cross(forward, kor::Vec3(0.f, 1.f, 0.f)));
                    const float step = std::max(camera.distance, 2.f) * 1.2f * seconds * (key(kor::Key::eLeftShift) > 0.f ? 4.f : 1.f);
                    // Nothing the panels show is where the view is: moving it is no change for them to be made again for.
                    camera.target += (forward * along + right * across + kor::Vec3(0.f, up, 0.f)) * step;
                }
            }
            else if (_viewDrag == kor::MouseButton::eMiddle) camera.Pan(delta.x, delta.y, static_cast<float>(std::max(size.y, 1u)));
            if (viewport->Hovered())
                if (const float wheel = input.MouseScrollDelta().y; wheel != 0.f) camera.Zoom(wheel);
            runtime.SetView(camera.View());
            if (over) over->SetView(camera.View());

            // What the editor's view shows of what is not drawn: each scene camera as the pyramid it sees through,
            // and the picked light's reach.
            const float aspect = over ? over->Aspect() : runtime.Aspect();
            // Which they are goes by what their components hold, as a View node goes: a field of view is a camera; a
            // range and a cone a spot; a range alone a point light; an intensity and no range, light from far away.
            const auto number = [&](const kecs::Entity entity, const kecs::ComponentId id, const char* name) -> std::optional<float> {
                const kecs::FieldDescription* field = world.Describe(id).FindField(name);
                const auto* bytes = static_cast<const std::byte*>(world.Get(entity, id));
                if (!field || !bytes || field->type != "f32") return std::nullopt;
                float v = 0.f;
                std::memcpy(&v, bytes + field->offset, 4);
                return v;
            };
            world.EachEntity([&](const kecs::Entity entity) {
                for (const kecs::ComponentId id : world.ComponentsOf(entity))
                    if (world.IsEnabled(entity, id) && number(entity, id, "fieldOfView")) {
                        const kgraph::View lens = kgraph::ViewOf(world, entity);
                        debug.Camera(lens.view, lens.Projection(aspect, false), 0.5f);
                        break;
                    }
            });
            if (!selected.IsNull()) {
                kor::Vec3 position, scale;
                kor::Quat rotation;
                if (kor::Decompose(kgraph::ToKor(world.WorldMatrix(selected)), position, rotation, scale)) {
                    const kor::Vec3 forward = rotation * kor::Vec3(0.f, 0.f, -1.f);
                    for (const kecs::ComponentId id : world.ComponentsOf(selected)) {
                        const auto range = number(selected, id, "range"), outer = number(selected, id, "outerAngle"), inner = number(selected, id, "innerAngle");
                        if (range && outer) debug.SpotLight(position, forward, *range, *outer * Radians, inner.value_or(*outer) * Radians);
                        else if (range && number(selected, id, "intensity")) debug.PointLight(position, *range);
                        else if (number(selected, id, "intensity") && !number(selected, id, "fieldOfView")) debug.DirectionalLight(position, forward);
                    }
                }
            }
        }

        // The gizmo on what is picked, and what dragging it does to its transform.
        kor::GizmoPointer hand = viewport->GizmoPointer();
        if (!selected.IsNull() && gizmoEnabled && size.x > 0 && size.y > 0) {
            kor::Mat4 transform = kgraph::ToKor(world.WorldMatrix(selected));
            kor::GizmoOptions options;
            options.space = kor::GizmoSpace::eLocal;
            if (debug.Gizmo(gizmo, transform, over ? over->ViewProjection() : runtime.ViewProjection(), hand, options)) {
                _gizmoMoved = true;
                // Only what this gizmo moves: taking the rest back out of the matrix would round it off a little each frame.
                world.SetWorldMatrix(selected, kgraph::ToKecs(transform), gizmo == kor::GizmoMode::eTranslate, gizmo == kor::GizmoMode::eRotate,
                                     gizmo == kor::GizmoMode::eScale);
                doc.Moved(selected, gizmo == kor::GizmoMode::eTranslate ? "position" : gizmo == kor::GizmoMode::eRotate ? "rotation" : "scale");
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
                const auto picked = [this, before](const kecs::Entity entity) { if (selectedEntity == before) Select(entity); };
                if (over) over->Pick(pointer->x / static_cast<float>(size.x), pointer->y / static_cast<float>(size.y), picked);
                else runtime.Pick(pointer->x / static_cast<float>(size.x), pointer->y / static_cast<float>(size.y), picked);
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
            // Scripts nothing is running: said, since a scene that plays with them idle looks like a script that does nothing.
            if (const auto idle = ScriptsNotRunning(); !idle.empty())
                Say(std::format("Playing, but nothing runs {}{}: this editor was started without the project's scripts. Project, Open with its scripts.",
                                idle.front(), idle.size() > 1 ? std::format(" and {} more", idle.size() - 1) : std::string()), true);
            else if (const kecs::ComponentId camera = Doc().World().Find("Camera");
                     camera == kecs::NoComponent || std::ranges::none_of(Doc().World().All(), [&](const kecs::Entity e) { return Doc().World().Has(e, camera); }))
                Say("There is no Camera in the scene: it plays seen from where its main View's defaults are.", true);
            else Say("Playing.");
            ++_playStarted;
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
        // The engine's own, as they are written into a new project's objects folder: there for a project made before
        // one of them was, and for none at all. The project's files, read after, are what they say instead.
        for (const auto& [file, source] : StandardObjects()) {
            kecs::json::Value parsed;
            std::string error;
            if (!kecs::json::Parse(source, parsed, error)) continue;
            ObjectTemplate object;
            object.data = Value::FromJson(parsed);
            const Value* name = object.data.Find("template");
            object.name = name && name->String() ? *name->String() : std::filesystem::path(file).stem().string();
            if (const Value* description = object.data.Find("description"); description && description->String()) object.description = *description->String();
            _objects.push_back(std::move(object));
        }
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

    bool Session::SaveAsPrefab(const kecs::Entity entity)
    {
        if (!_hasProject) { Say("There is no project to keep prefabs in yet: File, New project.", true); return false; }
        if (!Doc().World().Alive(entity)) return false;
        std::string file = Doc().World().NameOf(entity);
        for (char& c : file)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '_' && c != '-') c = '_';
        if (file.empty()) file = "Prefab";
        const auto path = _project.directory / "prefabs" / (file + ".prefab");
        std::error_code ignored;
        if (std::filesystem::exists(path, ignored)) {
            Say(std::format("There is a prefab called {} already: an instance of it changes it, with Apply in its inspector.", file), true);
            return false;
        }
        std::string error;
        if (!Doc().SaveAsPrefab(entity, path, &error)) { Say(error, true); return false; }
        Say(std::format("Saved prefabs/{}.prefab. {} is its instance: what changes in it is its own, until it is applied to the prefab.", file, Doc().NameOf(entity)));
        ++_changes;
        return true;
    }

    kecs::Entity Session::PlacePrefab(const std::string& id, const kecs::Entity parent)
    {
        if (!_hasProject || !id.starts_with("prefab:")) return kecs::NullEntity;
        std::string error;
        const kecs::Entity made = Doc().Instantiate(_project.directory / "prefabs" / id.substr(7), parent, &error);
        if (made.IsNull()) { Say(error, true); return made; }
        if (!parent.IsNull()) Fold(parent, false);
        Select(made);
        Say(std::format("Made {}, an instance of {}.", Doc().NameOf(made), id.substr(7)));
        return made;
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

    // ---- files ----------------------------------------------------------------------------------------------------

    namespace {
        /** Whether text is a name code can use: a letter or an underscore, then letters, digits and underscores. */
        bool IsIdentifier(const std::string& name)
        {
            if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')) return false;
            return std::ranges::all_of(name, [](const unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
        }

        /** `text` with every `$NAME` made @p name. */
        std::string Named(std::string text, const std::string& name)
        {
            for (std::size_t at = text.find("$NAME"); at != std::string::npos; at = text.find("$NAME", at + name.size())) text.replace(at, 5, name);
            return text;
        }

        constexpr const char* ComputeShader = R"slang(// $NAME: a compute shader. Wire an image to a ComputePipeline's `param.target`, and run it with a Dispatch
// whose `cover` is `target`.
module $NAME;

[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

// A value the pipeline's node sets: `constant.strength`.
[vk::constant_id(0)] const float strength = 1.0;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    target[id.xy] = float4(uv * strength, 0.0, 1.0);
}
)slang";

        constexpr const char* MeshShader = R"slang(// $NAME: draws a mesh for each entity. A Draw of its GraphicsPipeline is given each one's own — its mesh and the
// push constants below — from a Filter of an Entities node: `Mesh.mesh` and `matrix`, and for `color`, the field of the
// pipeline's component, named after its node, whose fields are what its Draws take from nobody else.
module $NAME;

import engineCommon;        // the frame: wire the frame pass's `frame data` to `param.frame`

struct Vertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct RasterInfo {
    float4 position : SV_Position;
    float3 normal;
};

struct DrawData {
    float4x4 model;         // where the entity is in the world: wire a Filter's `matrix` to the Draw's `push.model`
    float3 color;           // the entity's own: a field of the pipeline's component, wired from a Filter `with` it
};
[[vk::push_constant]] ConstantBuffer<DrawData> draw;

[shader("vertex")]
RasterInfo vertex(Vertex vertex)
{
    RasterInfo raster;
    const float4 world = mul(float4(vertex.position, 1.0), draw.model);
    raster.position = mul(world, frame.viewProjection);
    raster.normal = mul(float4(vertex.normal, 0.0), draw.model).xyz;
    return raster;
}

[shader("fragment")]
float4 fragment(RasterInfo raster) : SV_Target
{
    const float facing = saturate(dot(normalize(raster.normal), frame.sunDirection.xyz)) * 0.8 + 0.2;
    return float4(draw.color * facing, 1.0);
}
)slang";

        constexpr const char* FullscreenShader = R"slang(// $NAME: one triangle over the whole picture, and a fragment shader that reads another picture: what a pass that
// works on a picture is made of. Its pipeline needs no vertex layout, and its Draw no mesh. Wire the picture it reads
// to `param.source`, and draw into another.
module $NAME;

Texture2D<float4> source;

struct RasterInfo {
    float4 position : SV_Position;
};

[shader("vertex")]
RasterInfo vertex(uint index : SV_VertexID)
{
    const float2 corner = float2((index << 1) & 2, index & 2);
    RasterInfo raster;
    raster.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return raster;
}

[shader("fragment")]
float4 fragment(RasterInfo raster) : SV_Target
{
    const float4 color = source.Load(int3(int2(raster.position.xy), 0));
    return float4(1.0 - color.rgb, color.a);    // the picture, inverted: change this
}
)slang";

        constexpr const char* CSharpScript = R"cs(// $NAME: a script. Its public fields are a component — shown in the inspector, saved with the scene — and its
// methods are called for each entity that has it, while the scene plays. Add it to an entity in the inspector.
public sealed class $NAME : Script
{
    public float Speed;         // a field starts at zero: set it in the inspector, or in Start

    protected override void Start()
    {
        if (Speed == 0) Speed = 45;
    }

    protected override void Update(float delta)
    {
        // Turns its entity about the up axis, so many degrees a second.
        World.SetRotation(Entity, World.RotationOf(Entity) + new Vec3(0, Speed * delta, 0));
    }
}
)cs";

        constexpr const char* KotlinScript = R"kt(// $NAME: a script. Its public vars are a component — shown in the inspector, saved with the scene — and its
// functions are called for each entity that has it, while the scene plays. Add it to an entity in the inspector.
import koral.Vec3
import koral.engine.Script

class $NAME : Script() {
    var speed = 0f              // a var starts at zero: set it in the inspector, or in start

    override fun start() {
        if (speed == 0f) speed = 45f
    }

    override fun update(delta: Float) {
        // Turns its entity about the up axis, so many degrees a second.
        world.setRotation(entity, world.rotationOf(entity) + Vec3(0f, speed * delta, 0f))
    }
}
)kt";
    }

    const std::vector<std::pair<std::string, std::string>>& Session::ShaderKinds()
    {
        static const std::vector<std::pair<std::string, std::string>> kinds {
            { "Compute", "Works on an image, a group of pixels at a time: for a ComputePipeline and a Dispatch." },
            { "Mesh", "Draws a mesh for each entity that has the component it makes: for a GraphicsPipeline and a Draw." },
            { "Fullscreen", "One triangle over the whole picture, reading another picture: for a pass that works on one." },
        };
        return kinds;
    }

    std::vector<std::pair<std::string, std::string>> Session::ScriptKindsHere() const
    {
        const char language = _hasProject ? ScriptLanguage(_project) : 0;
        std::vector<std::pair<std::string, std::string>> kinds;
        for (const auto& kind : ScriptKinds()) {
            if (language == 0) kinds.emplace_back(kind.first, kind.second + " The project's scripts are all in the language its first is in.");
            else if ((kind.first == "Kotlin") == (language == 'k')) kinds.push_back(kind);
        }
        return kinds;
    }

    const std::vector<std::pair<std::string, std::string>>& Session::ScriptKinds()
    {
        static const std::vector<std::pair<std::string, std::string>> kinds {
            { "C#", "Compiled when the project starts, and again whenever it is saved. Run the project with koral-engine-dotnet." },
            { "Kotlin", "Compiled by a Gradle build the project is given: './gradlew edit' in its folder opens it with its scripts, built again as they are saved." },
        };
        return kinds;
    }

    bool Session::NewShader(const std::string& name, const std::string& kind)
    {
        if (!_hasProject) { Say("A shader goes in a project's shaders folder: File, New project.", true); return false; }
        if (!IsIdentifier(name)) { Say(std::format("'{}' is not a name a shader module can have: letters, digits and underscores, not starting with a digit.", name), true); return false; }
        const char* source = kind == "Compute" ? ComputeShader : kind == "Mesh" ? MeshShader : kind == "Fullscreen" ? FullscreenShader : nullptr;
        if (!source) { Say(std::format("'{}' is no kind of shader.", kind), true); return false; }
        const auto path = _project.ShaderDirectory() / (name + ".slang");
        std::error_code ignored;
        if (std::filesystem::exists(path, ignored)) { Say(std::format("There is a shader called {} already.", name), true); return false; }
        std::filesystem::create_directories(path.parent_path(), ignored);
        // A mesh shader reads the frame the standard shaders declare: they are the project's from here on.
        if (kind == "Mesh") WriteStandardShaders(_project.ShaderDirectory());
        { std::ofstream out(path, std::ios::binary); out << Named(source, name); if (!out) { Say(std::format("{} could not be written.", path.string()), true); return false; } }
        Say(std::format("Made shaders/{}.slang: a pipeline node names it as its module, '{}'.", name, name));
        return true;
    }

    bool Session::NewScript(const std::string& name, const std::string& language)
    {
        if (!_hasProject) { Say("A script goes in a project's scripts folder: File, New project.", true); return false; }
        if (!IsIdentifier(name)) { Say(std::format("'{}' is not a name a script's class can have: letters, digits and underscores, not starting with a digit.", name), true); return false; }
        const bool kotlin = language == "Kotlin";
        if (!kotlin && language != "C#") { Say(std::format("'{}' is not a language scripts are written in.", language), true); return false; }
        // A project's scripts are all in one language: the first one's, kept in project.json.
        if (const char said = ScriptLanguage(_project); said != 0 && said != (kotlin ? 'k' : 'c')) {
            Say(std::format("This project's scripts are in {}: a project has its scripts in one language.", said == 'k' ? "Kotlin" : "C#"), true);
            return false;
        }
        if (Doc().World().Find(name) != kecs::NoComponent) { Say(std::format("There is a component called {} already: a script is one, by its class's name.", name), true); return false; }
        const auto directory = _project.directory / "scripts";
        std::error_code ignored;
        for (const char* ending : { ".cs", ".kt" })
            if (std::filesystem::exists(directory / (name + ending), ignored)) { Say(std::format("There is a script called {} already.", name), true); return false; }
        std::filesystem::create_directories(directory, ignored);
        if (_project.scripts.empty()) {
            _project.scripts = Project::LanguageName(kotlin ? 'k' : 'c');
            std::string error;
            if (!_project.Save(error)) { Say(error, true); return false; }
        }
        const auto path = directory / (name + (kotlin ? ".kt" : ".cs"));
        { std::ofstream out(path, std::ios::binary); out << Named(kotlin ? KotlinScript : CSharpScript, name); if (!out) { Say(std::format("{} could not be written.", path.string()), true); return false; } }
        // Kotlin is compiled by a build: the project is given one, when it has none.
        if (kotlin) WriteKotlinBuild(_project.directory, _project.name);
        WriteEditorSetup(_project.directory, _project.name);
        // Its component is what its source declares, from now: to give entities, and as a node of the graph.
        Doc().DeclareScripts(path);
        Say(kotlin ? std::format("Made scripts/{}.kt, a component: './gradlew edit' in the project's folder opens it with its scripts running.", name)
                   : std::format("Made scripts/{}.cs, a component: koral-engine-dotnet runs it.", name));
        return true;
    }

    bool Session::NewStandardScene()
    {
        if (!_hasProject) { Say("The standard scene's shaders go in a project: File, New project.", true); return false; }
        if (InPlayMode()) Stop();
        WriteStandardShaders(_project.ShaderDirectory());
        WriteStandardObjects(_project.directory / "objects");
        _play.New();
        Doc().SetAssetDirectory(_project.directory / "assets");     // the project's: its files, and its prefabs, are named from it
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

    // What is done is counted by the document; a state is kept once the doing stops — the mouse let go, nothing changed
    // for a moment — so that a drag, or a number dragged through fifty values, is one thing to take back.
    void Session::TrackUndo(const float seconds, const bool busy)
    {
        Document& doc = Doc();
        if (&doc != _undoOf) {
            _undoOf = &doc;
            _undo.clear();
            _redo.clear();
            _now = { doc.Graph().ToJson(), doc.WorldJson() };
            _undoRevision = doc.Revision();
            _undoPending = false;
            return;
        }
        // Playing changes the scene and edits nothing; files being read make entities as they arrive, which is no edit either.
        if (InPlayMode() || doc.Reading() > 0) { _undoRevision = doc.Revision(); return; }
        if (doc.Revision() != _undoRevision) {
            _undoRevision = doc.Revision();
            _undoPending = true;
            _undoQuiet = 0.f;
            return;
        }
        if (!_undoPending || busy || (_undoQuiet += seconds) < 0.3f) return;
        Commit();
    }

    void Session::Commit()
    {
        if (!_undoPending) return;
        _undoPending = false;
        Was is { Doc().Graph().ToJson(), Doc().WorldJson() };
        if (is.graph == _now.graph && is.world == _now.world) return;
        _undo.push_back(std::move(_now));
        if (_undo.size() > 64) _undo.erase(_undo.begin());
        _redo.clear();
        _now = std::move(is);
        ++_changes;
    }

    void Session::PutBack(const Was& state)
    {
        Document& doc = Doc();
        const std::string picked = doc.World().Alive(selectedEntity) ? doc.NameOf(selectedEntity) : std::string();
        std::string error;
        if (state.graph != _now.graph) {
            kgraph::Graph graph;
            if (kgraph::Graph::FromJson(state.graph, graph, error)) doc.SetGraph(std::move(graph));
        }
        if (state.world != _now.world && !doc.RestoreWorld(state.world, error)) Say(error, true);
        _now = state;
        _undoRevision = doc.Revision();
        _undoPending = false;
        // What was picked is found again by its name: the entities are made anew.
        selectedEntity = picked.empty() ? kecs::NullEntity : doc.World().FindEntity(picked);
        if (!doc.Graph().Find(selectedNode)) selectedNode.clear();
        _folded.clear();
        ++_changes;
    }

    bool Session::Undo()
    {
        if (InPlayMode()) { Say("Stop playing to take something back: what a scene does as it plays is not an edit.", true); return false; }
        Commit();
        if (_undo.empty()) { Say("Nothing to take back."); return false; }
        Was before = std::move(_undo.back());
        _undo.pop_back();
        _redo.push_back(_now);
        PutBack(before);
        Say("Taken back.");
        return true;
    }

    bool Session::Redo()
    {
        if (InPlayMode() || _undoPending || _redo.empty()) { Say("Nothing to do again."); return false; }
        Was after = std::move(_redo.back());
        _redo.pop_back();
        _undo.push_back(_now);
        PutBack(after);
        Say("Done again.");
        return true;
    }

    std::vector<Session::Choice> Session::Choices() const
    {
        std::vector<Choice> choices;
        const kgraph::Graph& graph = _play.Doc().Graph();
        for (const auto& node : graph.nodes) {
            if (node.type != "Switch") continue;
            const auto index = node.inputs.find("index");
            const kgraph::Node* driver = index != node.inputs.end() && index->second.link ? graph.Find(index->second.link->node) : nullptr;
            if (!driver || (driver->type != "Number" && driver->type != "Bool")) continue;
            if (std::ranges::find(choices, driver->id, &Choice::driver) != choices.end()) continue;
            Choice choice;
            choice.driver = driver->id;
            for (int option = 0; option < 16; ++option) {
                const auto wired = node.inputs.find(std::format("option[{}]", option));
                if (wired == node.inputs.end() || !wired->second.link) break;
                // What a pass brought in is called what it stands for.
                const kgraph::Node* shown = graph.Find(wired->second.link->node);
                const auto target = shown && shown->type == "Reference" ? shown->inputs.find("node") : std::map<std::string, kgraph::Input>::const_iterator {};
                choice.options.push_back(shown && shown->type == "Reference" && target != shown->inputs.end() && target->second.value.String() ? *target->second.value.String() : wired->second.link->node);
            }
            if (choice.options.size() < 2) continue;
            const auto value = driver->inputs.find("value");
            if (value != driver->inputs.end()) choice.chosen = value->second.value.Bool() ? (*value->second.value.Bool() ? 1 : 0) : value->second.value.Number() ? static_cast<int>(*value->second.value.Number()) : 0;
            choices.push_back(std::move(choice));
        }
        return choices;
    }

    void Session::Choose(const std::string& driver, const int option)
    {
        const kgraph::Node* node = Doc().Graph().Find(driver);
        if (!node) return;
        Doc().SetInput(driver, "value", node->type == "Bool" ? Value(option != 0) : Value(option));
    }

    std::filesystem::path Session::ProjectsDirectory()
    {
        const char* said = std::getenv("KORAL_ENGINE_PROJECTS");
        std::filesystem::path directory = said && *said ? said : KEDITOR_PROJECTS;
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        return directory;
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
                if (!_thumbnails) _thumbnails = std::make_unique<Thumbnails>(_play.Graph());
                _thumbnails->Want(id, held);
            }
            listed.insert(id);
            assets.push_back(std::move(asset));
        }
        if (_hasProject) {
            // What is in the project's folders and not in the scene: there to be imported, or dragged where it is wanted.
            if (const auto folder = _project.directory / "assets"; std::filesystem::is_directory(folder, ignored))
                for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ignored)) {
                    if (!entry.is_regular_file(ignored)) continue;
                    const std::string kind = ending(entry.path());
                    if (!pictures.contains(kind) && !models.contains(kind)) continue;
                    Asset asset;
                    asset.id = runtime.AssetId(entry.path());
                    if (!listed.insert(asset.id).second) continue;
                    asset.kind = models.contains(kind) ? Asset::Kind::eModel : Asset::Kind::eTexture;
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
                    asset.id = std::string(prefix) + (kind == Asset::Kind::eShader ? entry.path().stem().string() : entry.path().filename().string());
                    asset.file = entry.path();
                    asset.label = entry.path().filename().string();
                    assets.push_back(std::move(asset));
                }
            };
            files(_project.ShaderDirectory(), Asset::Kind::eShader, "shader:", { ".slang" });
            files(_project.directory / "scripts", Asset::Kind::eScript, "script:", { ".cs", ".kt" });
            files(_project.directory / "prefabs", Asset::Kind::ePrefab, "prefab:", { ".prefab" });
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
            if (assets[i].kind != Asset::Kind::eMesh) continue;
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
            const std::filesystem::path in = asset.file.parent_path();
            const auto relative = _hasProject ? in.lexically_relative(_project.directory) : std::filesystem::path("..");
            if (_hasProject && !relative.empty() && *relative.begin() != "..") asset.folder = relative == "." ? std::string() : relative.generic_string();
            else {
                asset.folder = OutsideFolder;
                if (in.has_parent_path() && in.parent_path().has_filename()) asset.folder += "/" + in.parent_path().filename().string();
                if (in.has_filename()) asset.folder += "/" + in.filename().string();
            }
            // In a folder, a thing is called what its file is.
            if (asset.kind == Asset::Kind::eTexture || (asset.kind == Asset::Kind::eModel && !asset.file.empty())) asset.label = asset.file.filename().string();
        }
        std::ranges::stable_sort(assets, [](const Asset& a, const Asset& b) { return std::tuple(a.kind, a.group, a.label) < std::tuple(b.kind, b.group, b.label); });
        return assets;
    }

    // ---- what an entity is, and where it goes ------------------------------------------------------------------

    std::map<std::string, std::string> Session::PipelineComponents()
    {
        // Asked for by every line of the outliner: worked out once for the scene as it is, not once a line.
        if (_pipelinesAt == Doc().Version() + 1) return _pipelines;
        std::map<std::string, std::string> of;
        const kgraph::Runtime& runtime = Doc().Runtime();
        for (const auto& node : Doc().Graph().nodes)
            if (const kecs::ComponentId component = runtime.ComponentOf(node.id); component != kecs::NoComponent) of[Doc().World().Describe(component).name] = node.id;
        _pipelines = of;
        _pipelinesAt = Doc().Version() + 1;
        return of;
    }

    const std::pair<bool, bool>& Session::Drawn(const kecs::Entity entity)
    {
        // Whether each entity can be shown, and is: for every one of them at once, what is under each counted with
        // it, when the scene has changed — and looked up after.
        if (_drawnAt != Doc().Version() + 1) {
            _drawnAt = Doc().Version() + 1;
            _drawn.clear();
            const auto pipelines = PipelineComponents();
            const std::function<std::pair<bool, bool>(kecs::Entity)> each = [&](const kecs::Entity one) {
                std::pair<bool, bool> is { false, false };
                for (const auto& component : Doc().ComponentsOf(one))
                    if (pipelines.contains(component)) { is.first = true; is.second = is.second || Doc().IsEnabled(one, component); }
                for (const kecs::Entity child : Doc().World().ChildrenOf(one)) {
                    const auto under = each(child);
                    is.first = is.first || under.first;
                    is.second = is.second || under.second;
                }
                return _drawn[one] = is;
            };
            for (const kecs::Entity root : Doc().World().Roots()) each(root);
        }
        static const std::pair<bool, bool> none { false, false };
        const auto found = _drawn.find(entity);
        return found == _drawn.end() ? none : found->second;
    }

    std::string Session::EntityKind(const kecs::Entity entity)
    {
        const auto has = Doc().ComponentsOf(entity);
        const auto with = [&](const std::string_view name) { return std::ranges::find(has, name) != has.end(); };
        if (with("Camera")) return "camera";
        if (with("DirectionalLight")) return "sun";
        for (const auto& component : has)
            if (component.ends_with("Light")) return "light";
        if (with("Mesh")) return "mesh";
        const auto pipelines = PipelineComponents();
        static const std::set<std::string> own { "Mesh", "Camera", "Label" };
        for (const auto& component : has)
            if (!own.contains(component) && !pipelines.contains(component) && !Doc().World().Describe(Doc().World().Find(component)).fields.empty()) return "script";
        return Doc().World().ChildrenOf(entity).empty() ? "empty" : "group";
    }

    std::vector<std::string> Session::RunsFor(const kecs::Entity entity)
    {
        std::vector<std::string> passes;
        const auto pipelines = PipelineComponents();
        for (const auto& component : Doc().ComponentsOf(entity)) {
            const auto pipeline = pipelines.find(component);
            if (pipeline == pipelines.end() || !Doc().IsEnabled(entity, component)) continue;
            for (const auto& node : Doc().Graph().nodes)
                if (const auto input = node.inputs.find("pipeline"); input != node.inputs.end() && input->second.link && input->second.link->node == pipeline->second) passes.push_back(node.id);
        }
        return passes;
    }

    bool Session::CanShow(const kecs::Entity entity) { return Drawn(entity).first; }
    bool Session::IsShown(const kecs::Entity entity) { return Drawn(entity).second; }

    void Session::SetShown(const kecs::Entity entity, const bool shown)
    {
        const auto pipelines = PipelineComponents();
        const std::function<void(kecs::Entity)> each = [&](const kecs::Entity one) {
            for (const auto& component : Doc().ComponentsOf(one))
                if (pipelines.contains(component) && Doc().IsEnabled(one, component) != shown) Doc().SetEnabled(one, component, shown);
            // (A copy: switching a component moves nothing, but what is under it is asked for again each time.)
            const std::vector<kecs::Entity> under(Doc().World().ChildrenOf(one).begin(), Doc().World().ChildrenOf(one).end());
            for (const kecs::Entity child : under) each(child);
        };
        each(entity);
        ++_changes;
    }

    bool Session::MoveEntity(const kecs::Entity entity, const kecs::Entity target, const Beside where)
    {
        Document& doc = Doc();
        if (entity == target || !doc.World().Alive(entity) || !doc.World().Alive(target)) return false;
        bool moved = false;
        if (where == Beside::eUnder) moved = doc.SetParent(entity, target);
        else {
            const kecs::Entity parent = doc.World().ParentOf(target);
            // Where the target is among what it is with, as they are now: the world counts what is being moved, when
            // that is with it too, until it has moved.
            std::vector<kecs::Entity> beside;
            if (parent.IsNull()) { const auto roots = doc.World().Roots(); beside.assign(roots.begin(), roots.end()); }
            else beside.assign(doc.World().ChildrenOf(parent).begin(), doc.World().ChildrenOf(parent).end());
            const auto at = std::ranges::find(beside, target);
            const int index = static_cast<int>(at - beside.begin()) + (where == Beside::eAfter ? 1 : 0);
            moved = doc.SetParent(entity, parent, index);
        }
        if (!moved) Say(std::format("'{}' is above '{}': it cannot go under it", doc.NameOf(entity), doc.NameOf(target)), true);
        return moved;
    }

    std::vector<std::string> Session::FieldChoices(const std::string_view component, const std::string_view fieldType)
    {
        Document& doc = Doc();
        const auto& passes = doc.Passes();
        const auto passOf = [&](const std::string& pass) { return pass.empty() ? -1 : static_cast<int>(std::ranges::find(passes, pass) - passes.begin()); };
        // The pass the component's pipeline is run in: what a later pass makes is not there to be given to it.
        int last = static_cast<int>(passes.size());
        const auto pipelines = PipelineComponents();
        if (const auto pipeline = pipelines.find(std::string(component)); pipeline != pipelines.end()) {
            int ran = -1;
            for (const auto& node : doc.Graph().nodes)
                if (const auto input = node.inputs.find("pipeline"); input != node.inputs.end() && input->second.link && input->second.link->node == pipeline->second) ran = std::max(ran, passOf(node.pass));
            if (const kgraph::Node* node = doc.Graph().Find(pipeline->second); ran < 0 && node) ran = passOf(node->pass);
            if (ran >= 0) last = ran;
        }
        const bool mesh = fieldType == "Mesh", picture = fieldType == "Image";
        std::vector<std::string> choices;
        if (mesh)
            for (const auto& name : kgraph::DefaultMeshNames()) choices.push_back(kgraph::DefaultMeshId(name));
        if (picture) choices.insert(choices.end(), { "$white", "$black", "$normal" });
        if (mesh || picture) {
            for (const auto& asset : Assets())
                if (asset.kind == (mesh ? Asset::Kind::eMesh : Asset::Kind::eTexture)) choices.push_back(asset.id);
        }
        for (const auto& id : doc.NodesFor(fieldType)) {
            const kgraph::Node* node = doc.Graph().Find(id);
            if (!node) continue;        // (a mesh there is without a node: listed already)
            const int pass = passOf(node->pass);
            // A picture a pass draws into is not given to what that pass draws: one from a file is, wherever it is.
            if (picture ? (pass < last || node->type == "ImageFile") : pass <= last) choices.push_back(id);
        }
        return choices;
    }

    bool Session::GiveAsset(const std::string& id, const kecs::Entity entity)
    {
        Document& doc = Doc();
        if (!doc.World().Alive(entity)) return false;
        const auto assets = Assets();
        const auto asset = std::ranges::find(assets, id, &Asset::id);
        if (asset == assets.end()) { Say("That is nothing an entity can be given.", true); return false; }
        const std::string name = doc.NameOf(entity);
        std::string error;
        switch (asset->kind) {
        case Asset::Kind::eMesh:
            if (!std::ranges::contains(doc.ComponentsOf(entity), std::string("Mesh"))) doc.AddComponent(entity, "Mesh");
            if (!doc.SetField(entity, "Mesh", "mesh", kgraph::Value(id), &error)) { Say(error, true); return false; }
            Say(std::format("{} is {}'s mesh.", asset->label, name));
            return true;
        case Asset::Kind::eTexture: {
            // Its albedo's, before any other; one with nothing in it, before one that has something.
            std::optional<std::pair<std::string, std::string>> best;
            int bestScore = -1;
            for (const auto& component : doc.ComponentsOf(entity))
                for (const auto& field : doc.World().Describe(doc.World().Find(component)).fields) {
                    if (field.type != "Image") continue;
                    const kgraph::Value held = doc.Field(entity, component, field.name);
                    const bool free = !held.String() || held.String()->empty() || held.String()->starts_with("$");
                    const int score = (field.name.find("albedo") != std::string::npos ? 2 : 0) + (free ? 1 : 0);
                    if (score > bestScore) { bestScore = score; best = { component, field.name }; }
                }
            if (!best) { Say(std::format("{} has no field that holds a picture: a component of a pipeline that samples one has.", name), true); return false; }
            if (!doc.SetField(entity, best->first, best->second, kgraph::Value(id), &error)) { Say(error, true); return false; }
            Say(std::format("{} is {}'s {}.", asset->label, name, best->second));
            return true;
        }
        case Asset::Kind::ePrefab:
            // Dropped on an entity, a prefab's instance is made under it.
            return !PlacePrefab(id, entity).IsNull();
        case Asset::Kind::eScript: {
            const std::string component = asset->file.stem().string();
            if (std::ranges::contains(doc.ComponentsOf(entity), component)) { Say(std::format("{} has {} already.", name, component)); return true; }
            if (!doc.AddComponent(entity, component)) {
                Say(std::format("There is no component '{}' yet: a script's is there once the editor runs the project's scripts.", component), true);
                return false;
            }
            Say(std::format("{} has {}.", name, component));
            return true;
        }
        default:
            Say(std::format("{} is not something an entity is given: a mesh, a texture or a script is.", asset->label), true);
            return false;
        }
    }

    std::string Session::AssetToNode(const std::string& id, const double x, const double y)
    {
        const auto assets = Assets();
        const auto asset = std::ranges::find(assets, id, &Asset::id);
        if (asset == assets.end()) return {};
        Document& doc = Doc();
        const std::string pass = _everyPass ? std::string() : _pass;
        std::string made;
        if (asset->kind == Asset::Kind::eTexture) {
            made = doc.AddNode("ImageFile", x, y, asset->file.stem().string(), pass);
            if (!made.empty()) doc.SetInput(made, "file", kgraph::Value(id.substr(1)));
        } else if (asset->kind == Asset::Kind::eShader) {
            const std::string module = asset->file.stem().string();
            const auto entries = EntryPoints(module);
            const auto entry = [&](const std::string_view wanted) { return std::ranges::find_if(entries, [&](const std::string& e) { return e.starts_with(wanted); }); };
            if (entries.empty()) { Say(std::format("{} has no entry point: it is a module others import, and no pipeline's.", asset->label), true); return {}; }
            if (const auto compute = entry("compute"); compute != entries.end()) {
                made = doc.AddNode("ComputePipeline", x, y, module, pass);
                if (!made.empty()) { doc.SetInput(made, "compute.module", kgraph::Value(module)); doc.SetInput(made, "compute.entry", kgraph::Value(*compute)); }
            } else {
                made = doc.AddNode("GraphicsPipeline", x, y, module, pass);
                if (!made.empty())
                    for (const char* stage : { "vertex", "fragment" })
                        if (const auto found = entry(stage); found != entries.end()) {
                            doc.SetInput(made, std::string(stage) + ".module", kgraph::Value(module));
                            doc.SetInput(made, std::string(stage) + ".entry", kgraph::Value(*found));
                        }
            }
        } else {
            Say(std::format("{} is not a node: a texture is an ImageFile, a shader a pipeline.", asset->label), true);
            return {};
        }
        if (!made.empty()) { Select(made); Say(std::format("Made {} of {}.", made, asset->label)); }
        return made;
    }

    std::vector<std::string> Session::WireChoices(const std::string_view id, const std::string_view input)
    {
        Document& doc = Doc();
        const kgraph::Runtime& runtime = doc.Runtime();
        const kgraph::Node* node = doc.Graph().Find(id);
        std::vector<std::string> choices;
        if (!node) return choices;
        // What the input takes: by the name it has on the node, or the one its type lists it by (`color[].view`).
        std::string wanted;
        for (const auto& port : runtime.PortsOf(id)) {
            std::string listed = port.name;
            if (const auto at = listed.find("[]"); at != std::string::npos) {
                const auto open = input.find('[');
                const auto close = input.find(']');
                if (open == at && close != std::string_view::npos) listed = std::string(input.substr(0, close)) + listed.substr(at + 1);
            }
            if (listed == input && port.kind == kgraph::PortKind::eResource) wanted = port.type;
        }
        if (wanted.empty()) return choices;
        const auto& passes = doc.Passes();
        const auto passOf = [&](const std::string& pass) { return pass.empty() ? -1 : static_cast<int>(std::ranges::find(passes, pass) - passes.begin()); };
        const int here = passOf(node->pass);
        // Whatever is wired from this node, straight or through others, cannot be wired back into it.
        std::set<std::string> after { node->id };
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& other : doc.Graph().nodes)
                if (!after.contains(other.id) && std::ranges::any_of(other.inputs | std::views::values, [&](const kgraph::Input& in) { return in.link && after.contains(in.link->node); })) { after.insert(other.id); grew = true; }
        }
        for (const auto& other : doc.Graph().nodes) {
            if (after.contains(other.id)) continue;
            if (other.type == "Reference" && other.pass != node->pass) continue;       // another page's stand-in for something
            if (const int pass = passOf(other.pass); pass > here && here >= 0) continue;
            // What stands for another node gives what that node gives.
            const kgraph::Node* stood = &other;
            for (int hops = 0; stood && stood->type == "Reference" && hops < 8; ++hops) {
                const auto target = stood->inputs.find("node");
                stood = target != stood->inputs.end() && target->second.value.String() ? doc.Graph().Find(*target->second.value.String()) : nullptr;
            }
            if (!stood) continue;
            // (And a node this page already has a stand-in for is offered as that, once.)
            if (other.type != "Reference" && other.pass != node->pass && !other.pass.empty()
                && std::ranges::any_of(doc.Graph().nodes, [&](const kgraph::Node& each) {
                       const auto target = each.inputs.find("node");
                       return each.type == "Reference" && each.pass == node->pass && target != each.inputs.end() && target->second.value.String() && *target->second.value.String() == other.id;
                   })) continue;
            std::string gives;
            for (const auto& output : runtime.OutputsOf(stood->id))
                if (output.name == "out" && output.kind == kgraph::PortKind::eResource) gives = output.type;
            if (gives.empty()) continue;
            const bool fits = wanted == "Resource" || wanted == "value" || gives == wanted || gives == "Resource" || (gives == "ImageView" && wanted == "Image");
            if (fits) choices.push_back(other.id);
        }
        return choices;
    }

    bool Session::WireFrom(const std::string_view id, const std::string_view input, const std::string& from)
    {
        Document& doc = Doc();
        const kgraph::Node* node = doc.Graph().Find(id);
        const kgraph::Node* source = doc.Graph().Find(from);
        if (!node || !source) return false;
        std::string wired = from;
        // Of another pass, it is brought into this one: a node here that stands for it.
        if (!node->pass.empty() && !source->pass.empty() && source->pass != node->pass) wired = doc.Import(from, node->pass, node->x - 260., node->y);
        if (wired.empty() || !doc.Connect(wired, "out", id, input)) { Say(std::format("{} could not be wired to {}.", from, input), true); return false; }
        return true;
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
        if (_hasProject) {
            // The project's own, whether or not anything is in them — but not what a tool keeps for itself.
            static const std::set<std::string> skipped { "bin", "obj", "build", "node_modules", "out" };
            std::error_code ignored;
            for (auto it = std::filesystem::recursive_directory_iterator(_project.directory, std::filesystem::directory_options::skip_permission_denied, ignored);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ignored)) {
                if (!it->is_directory(ignored)) continue;
                const std::string name = it->path().filename().string();
                if (name.starts_with(".") || skipped.contains(name)) { it.disable_recursion_pending(); continue; }
                add(it->path().lexically_relative(_project.directory).generic_string());
            }
        }
        // The project's own first, by name; what is outside it after them.
        std::vector<std::string> sorted(folders.begin(), folders.end());
        std::ranges::stable_sort(sorted, [](const std::string& a, const std::string& b) { return a.starts_with(OutsideFolder) < b.starts_with(OutsideFolder); });
        return sorted;
    }

    std::filesystem::path Session::AssetFolderPath(const std::string_view folder) const
    {
        if (!_hasProject || folder.starts_with(OutsideFolder)) return {};
        return folder.empty() ? _project.directory : _project.directory / folder;
    }

    bool Session::NewAssetFolder(const std::string& name)
    {
        const std::filesystem::path in = AssetFolderPath(assetFolder);
        if (in.empty()) { Say("A folder is made in a folder of the project's.", true); return false; }
        if (name.empty() || name.find_first_of("/\\") != std::string::npos || name.starts_with(".")) { Say("A folder's name is a plain one: no slashes, and not starting with a dot.", true); return false; }
        std::error_code error;
        if (std::filesystem::exists(in / name, error)) { Say(std::format("There is a '{}' here already.", name), true); return false; }
        if (!std::filesystem::create_directory(in / name, error)) { Say(std::format("The folder could not be made: {}.", error.message()), true); return false; }
        Say(std::format("Made the folder {}.", name));
        ++_changes;
        return true;
    }

    void Session::OpenAsset(const Asset& asset)
    {
        switch (asset.kind) {
        case Asset::Kind::eShader:
        case Asset::Kind::eScript: OpenInCode(asset.file); break;
        case Asset::Kind::eModel:
            if (!assetsOpened.erase(asset.id)) assetsOpened.insert(asset.id);
            ++_changes;
            break;
        case Asset::Kind::eTexture:
        case Asset::Kind::eMesh: View(asset.id); break;
        case Asset::Kind::ePrefab: PlacePrefab(asset.id); break;
        default: break;
        }
    }

    bool Session::ShowInFiles(const std::filesystem::path& path)
    {
        std::error_code ignored;
        const std::filesystem::path folder = std::filesystem::is_directory(path, ignored) ? path : path.parent_path();
        const std::string command = std::format("xdg-open \"{}\"", folder.string());
        const bool started = launch ? launch(command) : std::system((command + " >/dev/null 2>&1 &").c_str()) == 0;
        if (!started) Say("The file manager could not be started.", true);
        return started;
    }

    bool Session::TrashAsset(const std::filesystem::path& path)
    {
        std::error_code ignored;
        const auto relative = _hasProject ? path.lexically_relative(_project.directory) : std::filesystem::path("..");
        if (path.empty() || relative.empty() || relative == "." || *relative.begin() == "..") { Say("Only what is in the project's folder is thrown away from here.", true); return false; }
        if (!std::filesystem::exists(path, ignored)) { Say(std::format("There is no '{}'.", relative.generic_string()), true); return false; }
        // To the system's trash, where it can be had back from: never taken off the disk from here.
        const std::string command = std::format("gio trash \"{}\"", path.string());
        const bool done = launch ? launch(command) : std::system((command + " >/dev/null 2>&1").c_str()) == 0;
        if (!done) { Say(std::format("'{}' could not be moved to the trash (gio trash).", relative.generic_string()), true); return false; }
        if (!selectedAsset.empty()) selectedAsset.clear();
        Say(std::format("Moved {} to the trash.", relative.generic_string()));
        ++_changes;
        return true;
    }

    std::string Session::AssetLabel(const std::string& id) const
    {
        if (!id.starts_with('@')) return id;
        if (const auto held = _play.Doc().Runtime().ResourceOf(id); held && !held->label.empty()) return held->label;
        const auto mesh = id.rfind('#');
        const std::filesystem::path file(id.substr(1, mesh == std::string::npos ? std::string::npos : mesh - 1));
        return file.filename().string() + (mesh == std::string::npos ? std::string() : id.substr(mesh));
    }

    kor::ResourceRef<const kor::Image> Session::MeshPicture(const std::string_view id) const
    {
        return _thumbnails ? _thumbnails->Of(id) : kor::ResourceRef<const kor::Image>();
    }

    std::shared_ptr<kgraph::Resource> Session::AssetHeld(const std::string_view id) const
    {
        const auto it = std::ranges::find(_assetsHeld, id, [](const auto& held) -> const std::string& { return held.first; });
        return it == _assetsHeld.end() ? nullptr : it->second;
    }

    bool Session::OpenInCode(const std::filesystem::path& file)
    {
        if (!_hasProject) { Say("There is a folder to open once there is a project: File, New project.", true); return false; }
        WriteEditorSetup(_project.directory, _project.name);
        std::error_code ignored;
        bool hasKotlin = false;
        if (std::filesystem::is_directory(_project.directory / "scripts", ignored))
            for (const auto& entry : std::filesystem::directory_iterator(_project.directory / "scripts", ignored)) hasKotlin = hasKotlin || entry.path().extension() == ".kt";
        if (hasKotlin) { WriteKotlinBuild(_project.directory, _project.name); }
        const char* said = std::getenv("KORAL_ENGINE_CODE");
        std::string command = std::format("{} \"{}\"", said && *said ? said : "code", _project.directory.string());
        if (!file.empty()) command += std::format(" --goto \"{}\"", file.string());
        // The shell's, and not waited for: the editor of code is a program of its own.
        const bool started = launch ? launch(command) : std::system((command + " >/dev/null 2>&1 &").c_str()) == 0;
        if (started) Say(file.empty() ? std::string("Opened the project in the editor of code.") : std::format("Opened {} in the editor of code.", file.filename().string()));
        else Say(std::format("'{}' could not be run: set KORAL_ENGINE_CODE to the editor's command.", command), true);
        return started;
    }

    bool Session::ImportModel(const std::filesystem::path& file)
    {
        if (_hasProject) Doc().SetAssetDirectory(_project.directory / "assets");
        std::string error;
        const kecs::Entity root = Doc().ImportModel(file, &error);
        if (root.IsNull()) { Say(error, true); return false; }
        Select(root);
        Say(std::format("Imported {}.", file.filename().string()));
        return true;
    }

    kor::ResourceRef<const kor::Image> Session::OverPicture() const
    {
        return _overHeld && _overHeld->handle ? kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(_overHeld->handle)) : kor::ResourceRef<const kor::Image>();
    }

    void Session::Look(std::string id)
    {
        // Nothing named: none is looked at any more.
        if (id.empty()) {
            for (auto& [looked, made] : _looks) {
                if (looked != _viewing) Doc().Runtime().Unwatch(looked);    // what the viewer has, it goes on copying out
                if (made.image.Valid()) _lookBefore.push_back(std::move(made.image));
            }
            _looks.clear();
            _looking.clear();
            ++_changes;
            return;
        }
        _looking = id;
        if (_looks.try_emplace(id).second) {
            Doc().Runtime().Watch(id);
            Doc().Runtime().WatchEvery(id, 1);      // closely: every frame, whatever its thumbnail asked
            _looks[id].says = "Not copied out yet.";
            _lookTimer = 0.f;
        }
        lookSlice = 0;
        ++_changes;
    }

    void Session::Unlook(const std::string& id)
    {
        const auto it = _looks.find(id);
        if (it == _looks.end()) return;
        if (id != _viewing) Doc().Runtime().Unwatch(id);
        if (it->second.image.Valid()) _lookBefore.push_back(std::move(it->second.image));
        _looks.erase(it);
        if (_looking == id) _looking = _looks.empty() ? std::string() : _looks.begin()->first;
        ++_changes;
    }

    kor::ResourceRef<const kor::Image> Session::LookPictureOf(const std::string_view id) const
    {
        const auto it = _looks.find(id);
        return it != _looks.end() && it->second.image.Valid() ? kor::ResourceRef<const kor::Image>(it->second.image) : kor::ResourceRef<const kor::Image>();
    }
    const std::string& Session::LookSaysOf(const std::string_view id) const
    {
        static const std::string nothing;
        const auto it = _looks.find(id);
        return it == _looks.end() ? nothing : it->second.says;
    }
    const std::vector<std::string>& Session::LookLines() const
    {
        static const std::vector<std::string> none;
        const auto it = _looks.find(_looking);
        return it == _looks.end() ? none : it->second.lines;
    }

    bool Session::View(std::string id)
    {
        if (id.empty()) { StopViewing(); return false; }
        kgraph::Runtime& runtime = Doc().Runtime();
        std::shared_ptr<kgraph::Resource> held = runtime.ResourceOf(id);
        if (!held && id.starts_with('@')) held = AssetHeld(id);
        ViewKind kind = ViewKind::eNone;
        if (held) {
            if (held->kind == "Mesh") kind = ViewKind::eMesh;
            else if (held->kind == "Image" || held->kind == "ImageView" || runtime.ImageOf(id).Get()) kind = ViewKind::eImage;
            else if (held->kind == "Buffer" || held->kind == "BufferView") kind = ViewKind::eBuffer;
        }
        if (kind == ViewKind::eNone) { Say(std::format("'{}' holds no image, buffer or mesh to view.", id), true); return false; }
        if (id != _viewing) {
            // Another thing: looked at from its first level and layer, its first row, and seen whole.
            StopViewing();
            imageView.mip = imageView.layer = 0;
            imageView.look.slice = 0;
            imageView.look.black.reset();
            imageView.look.white.reset();
            bufferFirst = 0;
            if (_meshViewer) _meshViewer->Frame();
        }
        _viewing = std::move(id);
        _viewKind = kind;
        _viewSays = kind == ViewKind::eMesh ? std::string() : std::string("Not copied out yet.");
        _viewTimer = 0.f;
        if (kind != ViewKind::eMesh) runtime.Watch(_viewing, imageView.mip, imageView.layer);
        ++_viewAsked;
        ++_changes;
        return true;
    }

    void Session::StopViewing()
    {
        if (_viewing.empty()) return;
        if (_viewKind != ViewKind::eMesh && !_looks.contains(_viewing)) Doc().Runtime().Unwatch(_viewing);
        if (_viewImage.Valid()) _viewBefore.emplace_back(std::move(_viewImage), 4);
        if (_viewPictureHeld) _overBefore.emplace_back(std::move(_viewPictureHeld), 4);
        _viewPictureHeld.reset();
        if (_meshViewer) _meshViewer->Show(nullptr);
        _viewPeek.reset();
        _viewPicture = {};
        _viewHistogram = {};
        _viewSays.clear();
        _viewing.clear();
        _viewKind = ViewKind::eNone;
        ++_changes;
    }

    kor::ResourceRef<const kor::Image> Session::ViewedPicture() const
    {
        if (_viewKind == ViewKind::eMesh) return _meshViewer ? _meshViewer->Picture() : kor::ResourceRef<const kor::Image>();
        return _viewImage.Valid() ? kor::ResourceRef<const kor::Image>(_viewImage) : kor::ResourceRef<const kor::Image>();
    }

    std::shared_ptr<kgraph::Resource> Session::ViewedMeshHeld() const
    {
        if (_viewKind != ViewKind::eMesh) return nullptr;
        if (auto held = _play.Doc().Runtime().ResourceOf(_viewing)) return held;
        return AssetHeld(_viewing);
    }

    void Session::SetBufferLayout(std::string text)
    {
        _bufferLayoutText = std::move(text);
        _bufferLayoutError.clear();
        _bufferLayout.reset();
        const bool blank = std::ranges::all_of(_bufferLayoutText, [](const char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
        if (!blank) _bufferLayout = ParseBufferLayout(_bufferLayoutText, &_bufferLayoutError);
        bufferFirst = 0;
        ++_changes;
    }

    void Session::UpdateView(const float seconds)
    {
        std::erase_if(_viewBefore, [](auto& kept) { return --kept.second <= 0; });
        // A mesh no longer viewed: its passes go here, at the start of a frame (StopViewing may have been pressed in one).
        if (_meshViewer && _viewKind != ViewKind::eMesh) _meshViewer->Update();
        if (_viewKind == ViewKind::eNone) return;
        kgraph::Runtime& runtime = Doc().Runtime();
        if (_viewKind == ViewKind::eMesh) {
            const auto held = ViewedMeshHeld();
            if (!_meshViewer) _meshViewer = std::make_unique<MeshViewer>(_play.Graph());
            _meshViewer->Show(held && !held->pending ? held : nullptr);
            _meshViewer->Update();
            const std::string says = !held ? std::string("It is not there any more.") : held->pending ? std::string("Still being read.") : _meshViewer->Problem();
            if (says != _viewSays) { _viewSays = says; ++_changes; }
            // Its picture is made again when it is another size: held until the interface has stopped showing the last.
            if (auto picture = _meshViewer->PictureHeld(); picture != _viewPictureHeld) {
                if (_viewPictureHeld) _overBefore.emplace_back(std::move(_viewPictureHeld), 4);
                _viewPictureHeld = std::move(picture);
                ++_changes;
            }
            return;
        }
        runtime.Watch(_viewing, imageView.mip, imageView.layer);
        if ((_viewTimer -= seconds) > 0.f) return;
        _viewTimer = 0.25f;
        auto peek = runtime.Watched(_viewing);
        if (!peek) {
            std::string says = runtime.WatchProblem(_viewing);
            if (says.empty()) says = "Not copied out yet.";
            if (says != _viewSays) { _viewSays = std::move(says); ++_changes; }
            return;
        }
        _viewSays.clear();
        if (peek->image) {
            LookOptions options = imageView.look;
            options.most = 2048;
            options.slice = std::min(options.slice, std::max(peek->extent.z, 1u) - 1);
            _viewPicture = ToPicture(*peek, options);
            _viewHistogram = HistogramOf(*peek, options.slice);
            if (_viewPicture.pixels.empty()) {
                _viewSays = std::format("{} has no such channel to show.", peek->format);
            } else {
                auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(_viewPicture.size)
                    .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                    .SetData(std::span<const kor::U8Vec4>(_viewPicture.pixels)).Build();
                if (image.Valid()) {
                    if (_viewImage.Valid()) _viewBefore.emplace_back(std::move(_viewImage), 4);
                    _viewImage = std::move(image);
                }
            }
        }
        _viewPeek = std::move(peek);
        ++_changes;
    }

    void Session::UpdateLook(const float seconds)
    {
        if (_looks.empty()) { _lookBefore.clear(); return; }
        kgraph::Runtime& runtime = Doc().Runtime();
        // A scene opened or made anew is another runtime, which was not asked to watch.
        for (const auto& id : _looks | std::views::keys) runtime.Watch(id);
        if ((_lookTimer -= seconds) > 0.f) return;
        _lookTimer = 0.25f;
        _lookBefore.clear();    // those are several frames old by now
        for (auto& [id, looked] : _looks) {
            const auto peek = runtime.Watched(id);
            std::string says;
            std::vector<std::string> lines;
            if (!peek) {
                says = runtime.WatchProblem(id);
                if (says.empty()) says = "Not copied out yet.";
            } else if (peek->image) {
                const Picture picture = ToPicture(*peek, lookMode, id == _looking ? lookSlice : 0);
                if (picture.pixels.empty()) {
                    says = std::format("{} has no such channel to show.", peek->format);
                } else {
                    auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(picture.size)
                        .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                        .SetData(std::span<const kor::U8Vec4>(picture.pixels)).Build();
                    if (image.Valid()) {
                        if (looked.image.Valid()) _lookBefore.push_back(std::move(looked.image));
                        looked.image = std::move(image);
                    }
                    says = std::format("{}, {} x {}{}. Its values run from {:.4g} to {:.4g}.", peek->format, peek->extent.x, peek->extent.y,
                                       peek->extent.z > 1 ? std::format(" x {}", peek->extent.z) : std::string(), picture.least, picture.greatest);
                }
            } else {
                lines = ToLines(*peek);
                says = peek->stride > 0 ? std::format("{} bytes: {} of {} each.", peek->bytes.size(), peek->count == 1 ? std::string("1 element") : std::format("{} elements", peek->count), peek->stride)
                                        : std::format("{} bytes, as whole numbers of 32 bits.", peek->bytes.size());
            }
            // A picture made again, and nothing else said of it: only what shows the picture is built again. What is
            // said of it is the interface's, which is.
            if (says != looked.says || lines != looked.lines) ++_changes;
            else if (peek && peek->image) ++looked.version;
            looked.says = std::move(says);
            looked.lines = std::move(lines);
        }
    }

    std::uint64_t Session::PictureVersionOf(const std::string_view id, const bool looking) const
    {
        if (looking) { const auto it = _looks.find(id); return it == _looks.end() ? 0 : it->second.version; }
        const auto it = _thumbs.find(id);
        return it == _thumbs.end() ? 0 : it->second.version;
    }

    kor::ResourceRef<const kor::Image> Session::ThumbnailOf(const std::string_view id) const
    {
        const auto it = _thumbs.find(id);
        return it != _thumbs.end() && it->second.image.Valid() ? kor::ResourceRef<const kor::Image>(it->second.image) : kor::ResourceRef<const kor::Image>();
    }

    const std::string& Session::ThumbnailSaysOf(const std::string_view id) const
    {
        static const std::string nothing;
        const auto it = _thumbs.find(id);
        return it == _thumbs.end() ? nothing : it->second.says;
    }

    void Session::UpdateThumbnails(const float seconds)
    {
        ++_thumbFrame;
        std::erase_if(_thumbBefore, [&](const auto& old) { return old.first + 4 < _thumbFrame; });
        kgraph::Runtime& runtime = Doc().Runtime();
        // The image nodes of the page shown, but those looked at closely: their pictures are made every few frames already.
        const std::string shown = ShowsEveryPass() ? std::string() : Pass();
        std::set<std::string, std::less<>> images;
        for (const auto& node : Doc().Graph().nodes)
            if ((shown.empty() || node.pass == shown) && node.type != "Screen" && !_looks.contains(node.id) && runtime.ImageOf(node.id).Get()) images.insert(node.id);
        // Each is watched for as long as it is on the page, and copied out once in so many frames — each on frames of its
        // own — so that a page of images costs about a copy a frame, and nothing is built again while it is shown.
        for (auto it = _thumbWatched.begin(); it != _thumbWatched.end();) {
            if (images.contains(it->first)) { ++it; continue; }
            if (!_looks.contains(it->first) && it->first != _viewing) runtime.Unwatch(it->first);
            it = _thumbWatched.erase(it);
        }
        for (const auto& id : images) {
            if (!_thumbWatched.contains(id)) {
                // Of an image with smaller levels, the first no wider than a thumbnail needs: much less to copy.
                const auto image = runtime.ImageOf(id);
                std::uint32_t mip = 0;
                for (kor::u32 width = image->Extent().x; mip + 1 < image->MipLevels() && width > 512; width /= 2) ++mip;
                _thumbWatched[id] = _thumbFrame;
                if (mip > 0) runtime.Watch(id, mip, 0);
            }
            runtime.Watch(id);              // (another runtime — a scene opened anew — was not asked to)
            runtime.WatchEvery(id, 30);
        }
        // Pictures made now and then, a few at a time — each makes the interface be built again — of those made longest
        // ago whose first copy has had time to come.
        // One at a time, every eighth of a second: as many a second as four every half, without four in one frame.
        if ((_thumbTimer -= seconds) > 0.f) return;
        _thumbTimer = 0.125f;
        std::vector<std::pair<std::uint64_t, std::string>> due;
        for (const auto& [id, since] : _thumbWatched) {
            if (_thumbFrame - since < 4) continue;
            const auto it = _thumbs.find(id);
            due.emplace_back(it == _thumbs.end() ? 0 : it->second.made, id);
        }
        std::ranges::sort(due);
        bool changed = false;
        for (std::size_t n = 0; n < due.size() && n < 1; ++n) {
            const std::string& id = due[n].second;
            Thumbnail& thumb = _thumbs[id];
            thumb.made = _thumbFrame;
            const auto peek = runtime.Watched(id);
            if (peek && peek->image) {
                const Picture picture = ToPicture(*peek, LookMode::eAuto, 0, 256);
                if (picture.pixels.empty()) { thumb.says = std::format("{} does not read back as a picture.", peek->format); changed = true; continue; }
                auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(picture.size)
                    .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                    .SetData(std::span<const kor::U8Vec4>(picture.pixels)).Build();
                if (!image.Valid()) continue;
                // A picture where there was one, and nothing said: the card is as big as it was, and only its picture is
                // built again. A first one, or one in place of what was said, makes the card another size.
                if (!thumb.image.Valid() || !thumb.says.empty()) changed = true;
                else ++thumb.version;
                if (thumb.image.Valid()) _thumbBefore.emplace_back(_thumbFrame, std::move(thumb.image));
                thumb.image = std::move(image);
                thumb.says.clear();
            } else if (_thumbFrame - _thumbWatched[id] > 90 && !thumb.image.Valid() && thumb.says.empty()) {
                // Nothing comes of it: an image that cannot be copied, or that no pass draws into.
                thumb.says = runtime.WatchProblem(id);
                if (thumb.says.empty()) thumb.says = "Nothing has drawn into it yet.";
                changed = true;
            }
        }
        if (changed) ++_changes;
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

    std::vector<const kgraph::NodeType*> FindNodeTypes(const std::string_view typed, const std::vector<kgraph::Preset>& presets)
    {
        // A component's node as a type of its own: kept while the editor runs, so what is found stays where it is.
        static std::map<std::string, std::unique_ptr<kgraph::NodeType>, std::less<>> components;
        std::vector<const kgraph::NodeType*> offered;
        for (const auto& preset : presets) {
            const kgraph::NodeType* base = kgraph::FindType(preset.type);
            if (!base) continue;
            auto& type = components[preset.name];
            if (!type) type = std::make_unique<kgraph::NodeType>();
            *type = *base;
            type->name = preset.name;
            // A call of one of the project's functions is offered among the functions; a component's node, among the components.
            const bool call = preset.type == "Function";
            type->category = call ? "Functions" : "Components";
            type->doc = preset.doc;
            type->keywords = call ? std::format("function call {}", base->keywords) : std::format("{} {}", preset.component, base->keywords);
            offered.push_back(type.get());
        }
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
        std::vector<const kgraph::NodeType*> all;
        for (const auto& type : kgraph::Catalog()) all.push_back(&type);
        all.insert(all.end(), offered.begin(), offered.end());
        for (const kgraph::NodeType* each : all) {
            const kgraph::NodeType& type = *each;
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
        /** A small button that is a picture of what it does: said by what is shown when the pointer rests on it. */
        kui::Widget SmallIcon(const std::string_view icon, std::function<void()> onPressed)
        {
            return kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(icon, kui::IconStyle::eFilled, kui::Theme::Current().textMuted)), std::move(onPressed),
                               kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)));
        }
        kui::Widget Heading(std::string text)
        {
            kui::TextStyle style;
            style.size = 15.f;
            style.weight = 700;
            return kui::Text(std::move(text), style);
        }
        /** What to do about a problem, when what it says is one of those there is something plain to do about: said after it. */
        std::string HintFor(const std::string& problem)
        {
            static const std::pair<const char*, const char*> hints[] {
                { "is still being read", "Wait: its file is being read, and it is drawn when it has arrived." },
                { "has nothing in it", "Give the field something: drag a mesh or a texture onto it from the Assets panel, or choose $white for a picture it has no use for." },
                { "has no mesh in it", "Give its Mesh a mesh: a default one from the list, or one dragged from the Assets panel." },
                { ".slang among the shaders", "Make the shader (Assets, New shader) or put its file in the project's shaders folder; or name another module on the node." },
                { "Failed to load Slang module", "The shader does not compile. Open it from the Assets panel and mend what the message names: it is tried again when the file is saved." },
                { "which failed", "Mend the node it names first: this one only waits for it." },
                { "which is not in the graph", "Wire the input to a node that is there, or take the input off." },
                { "wired, through other nodes, to itself", "Take out one wire of the circle: a node cannot wait for itself." },
                { "among its usage", "Add the usage it names to the image's or the buffer's 'usage' input." },
                { "has no input", "Take the input off the node: its kind, or its shader, no longer has it." },
                { "has no field", "Name one of the fields listed, or take the input off: the shader's struct no longer has it." },
                { "uses no buffer", "Set 'parameter' to one of the buffers listed." },
                { "has no output", "Wire from an output the node has: its inspector lists them under Add output." },
                { "needs an image wired in", "Wire an Image or an ImageView to it." },
                { "there is no component", "Add the component to an entity, or build the pipeline or the script that makes it." },
                { "more than one list of entities", "Wire every field from the same Entities node: an element of the buffer is one entity's." },
                { "there is no turn", "Give an index the loop has: its turns count from 0." },
                { "'first'", "Name, in 'first', the node it is on the first turn." },
                { "takes a ", "Wire it to a node that makes that kind of thing." },
            };
            for (const auto& [has, hint] : hints)
                if (problem.find(has) != std::string::npos) return hint;
            return {};
        }

        kui::Widget Problem(std::string text)
        {
            const std::string hint = HintFor(text);
            kui::Widget said = kui::Text(std::move(text), kui::TextStyle {}.SetSize(13.f).SetColor(kui::Color::Hex(0xFF6B6B)));
            if (hint.empty()) return said;
            // What is wrong, and under it what to do about it.
            return kui::Column({ std::move(said), kui::Text("→ " + hint, kui::TextStyle {}.SetSize(12.f).SetColor(kui::Theme::Current().textMuted)) },
                               kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(2.f));
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

        // ---- a vertex layout, as a picture and as what it is made of ------------------------------------------

        /** One attribute of a vertex layout, as VertexLayout's `attributes` hold it. */
        struct LayoutAttribute {
            std::string semantic, type = "eFloat";
            int count = 3, buffer = 0;
            bool instanced = false;
        };
        std::vector<LayoutAttribute> AttributesOf(const Value& value)
        {
            std::vector<LayoutAttribute> out;
            if (const auto* items = value.Items())
                for (const auto& item : *items) {
                    LayoutAttribute attribute;
                    if (const Value* v = item.Find("semantic"); v && v->String()) attribute.semantic = *v->String();
                    if (const Value* v = item.Find("type"); v && v->String()) attribute.type = *v->String();
                    if (const Value* v = item.Find("count"); v && v->Number()) attribute.count = static_cast<int>(*v->Number());
                    if (const Value* v = item.Find("binding"); v && v->Number()) attribute.buffer = static_cast<int>(*v->Number());
                    if (const Value* v = item.Find("instanced"); v && v->Bool()) attribute.instanced = *v->Bool();
                    out.push_back(std::move(attribute));
                }
            return out;
        }
        Value ValueOf(const std::vector<LayoutAttribute>& attributes)
        {
            Value::List list;
            for (const auto& a : attributes) {
                Value::Members members { { "semantic", a.semantic }, { "type", a.type }, { "count", a.count } };
                if (a.buffer != 0) members.emplace_back("binding", a.buffer);
                if (a.instanced) members.emplace_back("instanced", true);
                list.emplace_back(std::move(members));
            }
            return Value(std::move(list));
        }
        std::size_t BytesOf(const LayoutAttribute& a)
        {
            static const std::map<std::string, std::size_t, std::less<>> sizes {
                { "eFloat", 4 }, { "eInt", 4 }, { "eUInt", 4 }, { "eShort", 2 }, { "eUShort", 2 }, { "eByte", 1 }, { "eUByte", 1 }, { "eDouble", 8 } };
            const auto it = sizes.find(a.type);
            return (it == sizes.end() ? 4 : it->second) * static_cast<std::size_t>(std::clamp(a.count, 1, 4));
        }
        /** An attribute as a shader would write its type: float3, ubyte4. */
        std::string ShortType(const LayoutAttribute& a)
        {
            std::string type = a.type.size() > 1 ? a.type.substr(1) : a.type;
            std::ranges::transform(type, type.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return a.count > 1 ? std::format("{}{}", type, a.count) : type;
        }
        /** The colour an attribute is drawn in: the same for the same semantic, wherever it is. */
        kui::Color SemanticColor(const std::string& semantic)
        {
            static const std::map<std::string, std::uint32_t, std::less<>> known {
                { "POSITION", 0x4C8DD6 }, { "NORMAL", 0x52B788 }, { "UV", 0xE0A23B }, { "TEXCOORD", 0xE0A23B }, { "COLOR", 0xC0637E },
                { "TANGENT", 0x8E6CD9 }, { "BITANGENT", 0x3FB8AF }, { "BONE_IDS", 0xD9703F }, { "BONE_WEIGHTS", 0xB08C5A } };
            std::string base = semantic;
            while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();     // UV1 is a UV
            if (const auto it = known.find(base); it != known.end()) return kui::Color::Hex(it->second);
            static const std::uint32_t palette[] { 0x6FA8DC, 0x93C47D, 0xF6B26B, 0xE06666, 0xB4A7D6, 0x76A5AF, 0xD5A6BD, 0x7A8B99 };
            std::size_t hash = 0;
            for (const char c : semantic) if (!std::isdigit(static_cast<unsigned char>(c))) hash = hash * 31 + static_cast<unsigned char>(c);
            return kui::Color::Hex(palette[hash % std::size(palette)]);
        }

        /**
         * What a vertex is made of, as a picture: a strip for each buffer the mesh's vertices are in, its attributes one
         * after another along it, each as wide as its bytes; and what each buffer is — how many bytes a vertex (or an
         * instance) takes of it.
         */
        kui::Widget LayoutPicture(const std::vector<LayoutAttribute>& attributes, const float perByte = 4.f)
        {
            std::map<int, std::vector<const LayoutAttribute*>> buffers;
            for (const auto& a : attributes) buffers[a.buffer].push_back(&a);
            if (buffers.empty()) return kgui::Muted("No attributes yet.", 11.f);
            std::vector<kui::Widget> rows;
            kui::TextStyle inside;
            inside.size = 10.f;
            inside.color = kui::Color::Hex(0x101010);
            for (const auto& [index, members] : buffers) {
                std::size_t stride = 0;
                bool instanced = false;
                for (const auto* a : members) { stride += BytesOf(*a); instanced = instanced || a->instanced; }
                kui::TextStyle header;
                header.size = 10.f;
                header.color = kui::Theme::Current().textMuted;
                rows.push_back(kui::Text(std::format("buffer {}  ·  {}  ·  {} bytes", index, instanced ? "per instance" : "per vertex", stride), header, kui::TextAlign::eStart, false));
                std::vector<kui::Widget> blocks;
                for (const auto* a : members) {
                    // As wide as its bytes, and never too narrow for its name.
                    const std::string label = a->semantic.empty() ? std::string("?") : a->semantic;
                    const float width = std::max(static_cast<float>(BytesOf(*a)) * perByte, static_cast<float>(label.size()) * 6.4f + 10.f);
                    blocks.push_back(kui::Tooltip(std::format("{}: {}, {} bytes", a->semantic.empty() ? "(no name)" : a->semantic, ShortType(*a), BytesOf(*a)),
                        kui::Container(kui::ContainerOptions { .width = width, .height = 20.f, .padding = kui::EdgeInsets::Symmetric(4.f, 0.f),
                                                               .decoration = { .color = SemanticColor(a->semantic), .radius = 3.f } },
                                       kui::Align(kui::Alignment::CenterLeft(), kui::Text(label, inside, kui::TextAlign::eStart, false)))));
                }
                rows.push_back(kui::Row(std::move(blocks), kui::FlexOptions {}.SetGap(2.f)));
            }
            return kui::Column(std::move(rows), kui::FlexOptions {}.SetGap(3.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart));
        }

        /**
         * What a vertex has, a buffer at a time: each buffer's attributes under it — what each is, of what, how many,
         * in which buffer, moved up and down — whether the buffer goes a vertex or an instance at a time, and the
         * picture of it all on top.
         */
        kui::Widget Attributes(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            static const std::vector<std::string> types { "eFloat", "eInt", "eUInt", "eShort", "eUShort", "eByte", "eUByte", "eDouble" };
            const auto attributes = AttributesOf(value);
            const auto change = [set, attributes](std::function<void(std::vector<LayoutAttribute>&)> how) {
                return [set, attributes, how = std::move(how)] { auto now = attributes; how(now); set(ValueOf(now)); };
            };
            std::set<int> buffers;
            for (const auto& a : attributes) buffers.insert(a.buffer);
            if (buffers.empty()) buffers.insert(0);
            std::vector<std::string> bufferNames;
            const int most = *buffers.rbegin() + 1;
            for (int b = 0; b <= most; ++b) bufferNames.push_back(b == most ? std::format("{} new", b) : std::to_string(b));

            std::vector<kui::Widget> rows { LayoutPicture(attributes, 5.f), kui::Separator() };
            for (const int buffer : buffers) {
                bool instanced = false;
                for (const auto& a : attributes) if (a.buffer == buffer && a.instanced) instanced = true;
                rows.push_back(kui::Row({
                    kui::Expanded(kgui::Muted(std::format("Buffer {}", buffer))),
                    kui::Tooltip("Its attributes go an instance at a time, not a vertex at a time: what each copy of a mesh drawn many times has of its own",
                                 kui::Checkbox(instanced, [set, attributes, buffer](const bool on) {
                                     auto now = attributes;
                                     for (auto& a : now) if (a.buffer == buffer) a.instanced = on;
                                     set(ValueOf(now));
                                 }, "per instance")),
                    Small("+ attribute", change([buffer](auto& now) {
                        bool instanced = false;
                        for (const auto& a : now) if (a.buffer == buffer && a.instanced) instanced = true;
                        now.push_back({ now.empty() ? "POSITION" : "", "eFloat", now.empty() ? 3 : 4, buffer, instanced });
                    })),
                }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                for (std::size_t i = 0; i < attributes.size(); ++i) {
                    if (attributes[i].buffer != buffer) continue;
                    const auto& a = attributes[i];
                    rows.push_back(kui::Row({
                        kui::SizedBox(4.f, 22.f, kui::Container(kui::ContainerOptions { .decoration = { .color = SemanticColor(a.semantic), .radius = 2.f } }, kui::SizedBox(4.f, 22.f))),
                        kui::Expanded(TextEdit(Value(a.semantic), [set, attributes, i](Value v) { auto now = attributes; now[i].semantic = v.String() ? *v.String() : std::string(); set(ValueOf(now)); },
                                               std::format("{}[{}]", key, i), "SEMANTIC")),
                        Choice(types, Value(a.type), [set, attributes, i](Value v) { auto now = attributes; if (v.String()) now[i].type = *v.String(); set(ValueOf(now)); }).Width(100.f),
                        kui::DragValue(static_cast<float>(a.count), [set, attributes, i](const float v) { auto now = attributes; now[i].count = std::clamp(static_cast<int>(std::round(v)), 1, 4); set(ValueOf(now)); },
                                       kui::DragValueOptions {}.SetSpeed(0.1f).SetDecimals(0).SetRange(1.f, 4.f).SetWidth(44.f)),
                        kui::Tooltip("Which buffer it is in", kui::Dropdown(bufferNames, a.buffer, [set, attributes, i](const int b) {
                            auto now = attributes;
                            now[i].buffer = b;
                            // A buffer goes a vertex or an instance at a time: it takes the way of the one it joins.
                            bool joined = false, instanced = false;
                            for (std::size_t j = 0; j < now.size(); ++j) if (j != i && now[j].buffer == b) { joined = true; instanced = instanced || now[j].instanced; }
                            if (joined) now[i].instanced = instanced;
                            set(ValueOf(now));
                        }, kui::DropdownOptions {}.SetWidth(64.f))),
                        kui::Tooltip("Earlier", SmallIcon("arrow_upward", change([i](auto& now) { if (i > 0) std::swap(now[i], now[i - 1]); }))),
                        kui::Tooltip("Later", SmallIcon("arrow_downward", change([i](auto& now) { if (i + 1 < now.size()) std::swap(now[i], now[i + 1]); }))),
                        kui::Tooltip("Remove it", SmallIcon("close", change([i](auto& now) { now.erase(now.begin() + static_cast<std::ptrdiff_t>(i)); }))),
                    }, kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                }
            }
            rows.push_back(Small("+ buffer", change([most](auto& now) { now.push_back({ "", "eFloat", 4, most, false }); })));
            return Rows(std::move(rows), 4.f);
        }

        /** An element's fields, a line a field: its name, what it is, and how many of it — 1 for one, more for a fixed array. */
        kui::Widget Fields(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            static const std::vector<std::string> types { "eFloat", "eInt", "eUInt", "eBool", "eVec2", "eVec3", "eVec4", "eIVec2", "eIVec3", "eIVec4",
                                                          "eUVec2", "eUVec3", "eUVec4", "eMat3", "eMat4" };
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
                const Value* name = items[i].Find("name");
                const Value* type = items[i].Find("type");
                const Value* count = items[i].Find("count");
                rows.push_back(kui::Row({
                    kui::Expanded(TextEdit(name ? *name : Value {}, [with](Value v) { with("name", std::move(v)); }, std::format("{}[{}]", key, i), "position")),
                    Choice(types, type ? *type : Value("eFloat"), [with](Value v) { with("type", std::move(v)); }).Width(96.f),
                    kui::DragValue(count ? NumberOf(*count) : 1.f, [with](const float v) { with("count", Value(std::round(v))); },
                                   kui::DragValueOptions {}.SetSpeed(1.f).SetDecimals(0).SetRange(1.f, 256.f).SetWidth(48.f)),
                    Small("-", [set, items, i] { auto list = items; list.erase(list.begin() + static_cast<std::ptrdiff_t>(i)); set(Value(std::move(list))); }),
                }, kui::FlexOptions {}.SetGap(4.f)));
            }
            rows.push_back(Small("+ field", [set, items] {
                auto list = items;
                list.emplace_back(Value::Members { { "name", std::format("field{}", list.size()) }, { "type", "eFloat" } });
                set(Value(std::move(list)));
            }));
            return Rows(std::move(rows), 4.f);
        }

        /**
         * What a field that holds another entity is shown as: which of the scene's it is — each under what it is under —
         * or none; and an entity dragged here from the outliner is the one it holds from then on.
         */
        kui::Widget EntityPicker(const kecs::World& world, const Value& value, std::function<void(Value)> set)
        {
            std::vector<kecs::Entity> choices { kecs::NullEntity };
            std::vector<std::string> labels { "(nothing)" };
            const std::function<void(kecs::Entity, int)> add = [&](const kecs::Entity entity, const int depth) {
                choices.push_back(entity);
                labels.push_back(std::string(static_cast<std::size_t>(depth) * 3, ' ') + world.NameOf(entity));
                for (const kecs::Entity child : world.ChildrenOf(entity)) add(child, depth + 1);
            };
            for (const kecs::Entity root : world.Roots()) add(root, 0);
            const kecs::Entity held = EntityOfValue(value).value_or(kecs::NullEntity);
            int selected = 0;
            if (const auto at = std::ranges::find(choices, held); at != choices.end()) selected = static_cast<int>(at - choices.begin());
            else if (!held.IsNull()) { selected = static_cast<int>(choices.size()); choices.push_back(held); labels.push_back("(an entity that is gone)"); }
            kui::Widget picker = kui::Dropdown(std::move(labels), selected, [set, choices](const int index) {
                set(EntityValue(choices[static_cast<std::size_t>(std::max(index, 0))]));
            });
            return kui::Tooltip("An entity of the scene: chosen here, or dragged here from the outliner.",
                kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("entity").OnDrop([set](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* entity = dragged.As<kecs::Entity>()) set(EntityValue(*entity));
                }), std::move(picker)));
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

        /** What an output of a kind is called on a node: a vertex layout for `struct KoralVertexLayout`, and other types as they are. */
        std::string TypeLabel(const std::string& type)
        {
            if (!type.starts_with("struct Koral")) return type;
            std::string words;
            for (const char c : type.substr(12)) {
                if (std::isupper(static_cast<unsigned char>(c)) && !words.empty()) words += ' ';
                words += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return words;
        }

        /** What a port is called on a node: its name, without what kind of input it is — `position` for `field.position`. */
        std::string PortLabel(const std::string& name)
        {
            for (const std::string_view kind : { "field.", "param.", "push.", "constant." })
                if (name.starts_with(kind) && name.size() > kind.size()) return name.substr(kind.size());
            return name;
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
            const auto consider = [&](kgraph::Port port, bool always) {
                const bool given = node.inputs.contains(port.name);
                // One set on the node is there, given or not: its control is what it is.
                if (kgraph::PlacementOf(port, node) == kgraph::Placement::eNode) always = true;
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
                // What a shader declares is the node's to be given, whatever else is: a pipeline's parameter, or a
                // field of a ShaderBuffer's struct, is never hidden.
                if (at == std::string::npos) { consider(port, port.name.starts_with("param.") || port.name.starts_with("field.")); continue; }
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
            // A field of a shader's struct is there to be wired to, from an entity's: a port from the start.
            if ((node.type == "ShaderBuffer" || node.type == "BufferBuilder") && shown.port.name.starts_with("field.")) return true;
            // One set on the node is a port with its control; one wired only, a port to wire.
            if (kgraph::PlacementOf(shown.port, node) != kgraph::Placement::eInspector) return true;
            const auto input = node.inputs.find(shown.port.name);
            return input != node.inputs.end() && (input->second.link || input->second.value.IsNull());
        }

        /** What a port carries, as the graph's view tells kinds apart: `r:Image`, `v:number`. */
        std::string WireType(const kgraph::Port& port) { return (port.kind == kgraph::PortKind::eResource ? "r:" : "v:") + port.type; }

        /** Whether a wire may go from an output of kind @p from to an input of kind @p to: a resource to what takes one of its kind (a view of an image where an image goes), a value to a value. */
        bool WireFits(const std::string_view from, const std::string_view to)
        {
            if (from.substr(0, 2) != to.substr(0, 2)) return false;
            if (from.starts_with("v:")) return true;
            return from == to || from == "r:Resource" || to == "r:Resource" || (from == "r:ImageView" && to == "r:Image");
        }

        /** A port's colour, and its wires': by what it carries, so that an image's wire is told from a buffer's at a glance. */
        kui::Color PortColor(const std::string_view kind)
        {
            const std::string_view type = kind.size() > 2 ? kind.substr(2) : kind;
            if (kind.starts_with("r:")) {
                if (type == "Image" || type == "ImageView") return kui::Color::Hex(0x3FB8AF);
                if (type == "Buffer" || type == "BufferView") return kui::Color::Hex(0xD9A441);
                if (type == "Sampler") return kui::Color::Hex(0xA77BD8);
                if (type == "Mesh") return kui::Color::Hex(0x7DBB4B);
                if (type == "Shader") return kui::Color::Hex(0xC76B98);
                if (type.ends_with("Pipeline")) return kui::Color::Hex(0xD9703F);
                return kui::Color::Hex(0x9AA4B0);
            }
            if (type == "bool" || type == "Bool") return kui::Color::Hex(0xD86A6A);
            if (type == "number" || type == "Number") return kui::Color::Hex(0x5B9BD5);
            if (type == "string" || type == "String") return kui::Color::Hex(0xC9A0DC);
            if (type == "vector" || type == "Color") return kui::Color::Hex(0x62C3E8);
            return kui::Color::Hex(0x8FA3B8);
        }

        /** A value in a few characters, to be said on a node. */
        std::string Brief(const kgraph::Value& value, const std::size_t most = 28)
        {
            std::string text;
            if (const bool* flag = value.Bool()) text = *flag ? "yes" : "no";
            else if (const double* number = value.Number()) text = *number == std::floor(*number) && std::abs(*number) < 1e9 ? std::format("{}", static_cast<long long>(*number)) : std::format("{:.3g}", *number);
            else if (const std::string* string = value.String()) text = *string;
            else if (const auto* items = value.Items()) {
                for (const auto& item : *items) text += (text.empty() ? "" : ", ") + Brief(item, most);
            } else if (!value.IsNull()) text = "...";
            // A file is its name, not where it is.
            if (text.size() > most && text.find('/') != std::string::npos) text = text.substr(text.rfind('/') + 1);
            if (text.size() > most) text = text.substr(0, most - 3) + "...";
            return text;
        }

        kui::Color CategoryColor(const std::string& category)
        {
            // A colour a kind: what is made on the device warm, what is worked out cool, what the scene holds green.
            static const std::map<std::string, std::uint32_t, std::less<>> colors {
                { "Pipelines", 0x7A4BC2 }, { "Passes", 0xC2643B }, { "Resources", 0xB0892F }, { "Values", 0x3B7FC2 }, { "Math", 0x2F8FA8 },
                { "Vectors", 0x4C6FCF }, { "Logic", 0x8A5BA8 }, { "Cameras", 0x5A8F3A }, { "Scene", 0x3B9C7A }, { "Components", 0x4E9E4E }, { "Flow", 0x6B7685 },
                { "Functions", 0x3E9FA8 } };
            const auto it = colors.find(category);
            return kui::Color::Hex(it == colors.end() ? 0x3B9C7A : it->second);
        }

        // ---- a node's picture ----------------------------------------------------------------------------------------

        /**
         * A node's picture on its card — its thumbnail, or as it is looked at closely — built again on its own when a new
         * one is made: a picture every few frames costs that much, not the whole interface built again.
         */
        class NodePicture final : public kui::StatefulWidget {
        public:
            NodePicture(std::shared_ptr<Session> session, std::string id, const bool looking) : _session(std::move(session)), _id(std::move(id)), _looking(looking) {}

            void InitState() override
            {
                _seen = _session->PictureVersionOf(_id, _looking);
                Animate([this](float) {
                    if (const auto version = _session->PictureVersionOf(_id, _looking); version != _seen) SetState([&] { _seen = version; });
                    return true;
                });
            }
            void DidUpdateWidget(const kui::StatefulWidget& newer) override
            {
                const auto& given = static_cast<const NodePicture&>(newer);
                _id = given._id;
                _looking = given._looking;
                _seen = _session->PictureVersionOf(_id, _looking);
            }
            kui::Widget Build() override
            {
                const auto picture = _looking ? _session->LookPictureOf(_id) : _session->ThumbnailOf(_id);
                return kui::Container(kui::ContainerOptions { .padding = kui::EdgeInsets::All(2.f), .decoration = { .color = kui::Theme::Current().background, .radius = 4.f } },
                                      kui::Image(picture, kui::ImageFit::eContain, { 236.f, 146.f }));
            }

        private:
            std::shared_ptr<Session> _session;
            std::string _id;
            bool _looking = false;
            std::uint64_t _seen = 0;
        };

        /** How many frames a second, built again on its own as it changes: four times a second, it was the whole interface. */
        class FrameRate final : public kui::StatefulWidget {
        public:
            explicit FrameRate(std::shared_ptr<Session> session) : _session(std::move(session)) {}

            void InitState() override
            {
                _shown = _session->Fps();
                Animate([this](float) {
                    if (const int now = _session->Fps(); now != _shown) SetState([&] { _shown = now; });
                    return true;
                });
            }
            kui::Widget Build() override
            {
                return kui::Padding(kui::EdgeInsets::Symmetric(4.f, 0.f), kui::Text(std::format("{} fps", _shown), kui::TextStyle {}.SetColor(kui::Color::Hex(0xE0E0E0)).SetSize(12.f)));
            }

        private:
            std::shared_ptr<Session> _session;
            int _shown = 0;
        };

        // ---- the root -------------------------------------------------------------------------------------------

        class Root final : public kui::StatefulWidget {
        public:
            explicit Root(std::shared_ptr<Session> session) : _session(std::move(session))
            {
                _layout = StartingLayout();
                // Where they were left, the run before.
                if (!std::getenv("KORAL_ENGINE_NO_LAYOUT")) {
                    std::ifstream saved(LayoutFile());
                    std::stringstream text;
                    text << saved.rdbuf();
                    if (saved && !text.str().empty() && _layout->Load(text.str()) && text.str().find("\"viewer\"") == std::string::npos)
                        _layout->Close("viewer");      // a layout from before there was a viewer: it opens when something is viewed
                    for (const char* panel : { "graph", "scene", "game", "inspector", "outliner", "problems", "assets" })
                        if (!_layout->IsOpen(panel)) _layout->Open(panel);      // a layout from before there was such a panel
                }
            }

            /** Where the panels are before anybody has moved them. */
            static std::shared_ptr<kui::DockLayout> StartingLayout()
            {
                auto layout = std::make_shared<kui::DockLayout>();
                // The graph has the middle, and most of the window: it is what is worked on. The scene it makes is
                // beside it with the inspector under that, and what is wrong is a strip along the bottom.
                layout->Dock("graph", kui::DockArea::eCenter, 0)
                        // The scene the graph makes is beside it, the middle shared between them: a line to drag. What it
                        // looks like played is a tab beside that, brought forward by Play — or dragged to a part of its own.
                        .Dock("scene", kui::DockArea::eCenter, 1)
                        .Dock("game", kui::DockSide::eCenter, "scene")
                        .Dock("inspector", kui::DockSide::eRight, "graph", 0.2f)
                        .Dock("outliner", kui::DockSide::eLeft, "graph", 0.14f)
                        // Along the bottom, the project's files, with room for their folders beside them; what is
                        // wrong is behind them, a press away.
                        .Dock("problems", kui::DockSide::eBottom, "graph", 0.27f)
                        .Dock("assets", kui::DockSide::eCenter, "problems")
                        // What is looked at closely is a tab beside the scene, there once something is.
                        .Dock("viewer", kui::DockSide::eCenter, "scene");
                layout->Close("viewer");
                layout->Activate("assets");
                layout->Activate("scene");
                return layout;
            }

            void InitState() override
            {
                // The document changes under the panels — an edit here, a shader saved elsewhere: built again when it has.
                Animate([this](float) {
                    if (_session->TakeAskForNodes()) AskForNode(NewNodeAt());
                    if (_session->TakeAskToArrange()) Arrange();
                    // A node gone to — found, or pressed among the problems — is brought into the graph's view.
                    if (const auto went = _session->Went(); went != _went) { _went = went; _view->Reveal(_session->selectedNode); SetState(); }
                    // A model to be imported, while what its meshes have is read: shown when it is known.
                    if (_dialog == Dialog::eImportOptions && !_importAttributes && Doc().Runtime().ModelAttributes(_importFile)) SetState();
                    // Something opened in the viewer: the viewer is brought forward.
                    // Played, the Game panel is brought forward; and it is drawn only while it is seen.
                    if (const auto played = _session->PlayStarted(); played != _played) {
                        _played = played;
                        _layout->Open("game");
                        _layout->Activate("game");
                        SetState();
                    }
                    if (GameScene* game = _session->Game()) game->SetShown(_layout->IsShown("game"));
                    if (const auto asked = _session->ViewAsked(); asked != _viewAsked) {
                        _viewAsked = asked;
                        _layout->Open("viewer");
                        _layout->Activate("viewer");
                        SetState();
                    }
                    if (const auto version = _session->Version(); version != _shown) SetState([&] { _shown = version; });
                    // Played, the picked entity's inspector follows what scripts do to it — where it is, what its
                    // components hold — built again ten times a second at most, so a field set every frame is seen
                    // without the panels being built every frame.
                    if (_session->IsPlaying() && Doc().World().Alive(_session->selectedEntity)) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - _followedAt >= std::chrono::milliseconds(100)) {
                            const kecs::World& world = Doc().World();
                            std::uint64_t changes = world.SceneVersion();
                            for (const kecs::ComponentId id : world.ComponentsOf(_session->selectedEntity)) changes += world.ChangesOf(id);
                            if (changes != _followed) { _followed = changes; _followedAt = now; SetState(); }
                        }
                    }
                    return true;
                });
            }

            kui::Widget Build() override
            {
                _shown = _session->Version();
                // The window's own title bar, drawn here in place of the system's: the menus at its start, then which
                // project is open, room to move the window by, and the window's buttons.
                const std::string title = std::format("{}{} — Koral Engine", _session->HasProject() ? _session->CurrentProject().name : std::string("No project"),
                                                      Doc().Dirty() ? " (not saved)" : "");
                auto body = kui::Column({
                    kui::TitleBar(title, kui::TitleBarOptions {}.SetLeading(Menus())),
                    kui::Expanded(kui::DockSpace(_layout, {
                        Docked("graph", "Graph", "account_tree", Graph()),
                        ScenePanelDocked(),
                        GamePanelDocked(),
                        Docked("outliner", "Outliner", "list", Outliner()),
                        Docked("inspector", "Inspector", "tune", Inspector()),
                        Docked("problems", "Problems", "warning", Problems()),
                        Docked("assets", "Assets", "folder", AssetsPanel()),
                        ViewerDocked(),
                    }, kui::DockOptions {}.OnChanged([this] { SaveLayout(); }).OnClosed([this](const std::string& panel) {
                        if (panel == "viewer") _session->StopViewing();     // closed, what it copied out and drew stops
                        SaveLayout();
                    }))),
                    kui::StatusBar(std::format("{}      {} {}{}", _session->Status(), Doc().World().EntityCount(), Doc().World().EntityCount() == 1 ? "entity" : "entities",
                                               Doc().Reading() == 0 ? std::string() : std::format("      reading {} {}...", Doc().Reading(), Doc().Reading() == 1 ? "file" : "files")),
                                   _session->Failed() ? kui::StatusLevel::eError : kui::StatusLevel::eInfo),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetMainAxisSize(kui::MainAxisSize::eMax));
                return kui::Modal(_dialog != Dialog::eNone, std::move(body), DialogBody(), [this] { SetState([&] { _dialog = Dialog::eNone; }); });
            }

        private:
            enum class Dialog { eNone, eAddNode, eNewProject, eOpenProject, eNewShader, eNewScript, eImportModel, eImportOptions, eConfirm, eNewFolder, eTrash, eNewFunction };
            static constexpr float ListSize = 28.f;     // the least the Assets panel shows a thing at: a line of a list

            /** Where the panels are, kept from one run to the next: this user's, not a project's. */
            static std::filesystem::path LayoutFile() { return ConfigDirectory() / "editor.layout"; }
            /** The project that is open, as the one the editor opens when given none; already that, the engine's own again. */
            void ToggleStartup()
            {
                const std::filesystem::path open = _session->CurrentProject().directory;
                const bool already = Same(Session::StartupProject(), open);
                if (!Session::SetStartupProject(already ? std::filesystem::path() : open)) { _session->Say("The project to start with could not be kept.", true); return; }
                _session->Say(already ? "The editor opens its own project when it is given none, again."
                                      : std::format("The editor opens {} when it is given none.", _session->CurrentProject().name));
                SetState();
            }
            void SaveLayout() const
            {
                if (std::getenv("KORAL_ENGINE_NO_LAYOUT")) return;
                std::error_code ignored;
                std::filesystem::create_directories(LayoutFile().parent_path(), ignored);
                std::ofstream(LayoutFile(), std::ios::trunc) << _layout->Save();
            }

            /**
             * What throws the scene away — another project, a scene made anew, the saved one put back — asks first
             * when there is something not saved in it.
             */
            std::function<void()> Asking(std::string what, std::function<void()> then)
            {
                return [this, what = std::move(what), then = std::move(then)] {
                    if (!Doc().Dirty()) { then(); return; }
                    SetState([&] { _dialog = Dialog::eConfirm; _confirmWhat = what; _confirmed = then; });
                };
            }
            kui::Widget ConfirmDialog()
            {
                const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; }); };
                const auto secondary = kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary);
                std::vector<kui::Widget> buttons;
                if (_session->HasProject() && !_session->InPlayMode())
                    buttons.push_back(kui::Button("Save, then go on", [this, close] { const auto then = _confirmed; close(); if (_session->Save() && then) then(); }));
                buttons.push_back(kui::Button("Go on without saving", [this, close] { const auto then = _confirmed; close(); if (then) then(); }, secondary));
                buttons.push_back(kui::Button("Cancel", close, secondary));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading("The scene has changes that are not saved"),
                    kgui::Line(std::format("{} leaves this scene: what was done to it since it was saved would be lost.", _confirmWhat)),
                    kui::Row(std::move(buttons), kui::FlexOptions {}.SetGap(8.f)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(10.f)).Width(520.f));
            }
            std::string _confirmWhat;
            std::function<void()> _confirmed;

            Document& Doc() { return _session->Doc(); }

            // ---- the scene ---------------------------------------------------------------------------------------

            /** A button of the scene's toolbar: lit while what it stands for is on. */
            /** A panel, with the picture its button in the stripe shows. */
            static kui::DockPanel Docked(const char* id, const char* title, const char* icon, kui::Widget content)
            {
                kui::DockPanel panel { id, title, std::move(content), false };
                panel.icon = kui::MaterialIcon(icon);
                return panel;
            }

            kui::DockPanel ViewerDocked()
            {
                kui::DockPanel panel { "viewer", "Viewer", ViewerPanel(_session), true };
                panel.icon = kui::MaterialIcon("image_search");
                return panel;
            }

            kui::DockPanel ScenePanelDocked()
            {
                // A model dropped on the scene from the Assets panel is imported into it.
                kui::Widget scene = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this](const kui::DragData& dragged, kor::Vec2) {
                    const auto* id = dragged.As<std::string>();
                    const auto asset = id ? AssetOf(*id) : std::nullopt;
                    if (asset && asset->kind == Session::Asset::Kind::eModel) AskToImport(asset->file);
                    else if (asset && asset->kind == Session::Asset::Kind::ePrefab) _session->PlacePrefab(asset->id);
                    else _session->Say("A model dropped on the scene is imported and a prefab placed; a mesh, a texture or a script is dropped on the entity that is to have it.", true);
                }), ScenePanel());
                kui::DockPanel panel { "scene", "Scene", std::move(scene), false };
                panel.icon = kui::MaterialIcon("videocam");
                panel.titleBar = TitleTools(true);
                return panel;
            }

            /**
             * The scene as it plays, filling its panel: seen from its own camera, its scripts steered from here — and
             * nothing of the editor's over it. A click here picks nothing.
             */
            kui::DockPanel GamePanelDocked()
            {
                kui::Widget view = _session->Game() ? kgui::SceneView(*_session->Game()) : kui::Text("");
                kui::DockPanel panel { "game", "Game", std::move(view), false };
                panel.icon = kui::MaterialIcon("sports_esports");
                panel.titleBar = TitleTools(false);
                return panel;
            }

            /** A square button with an icon: lit while what it stands for is on, dim while it cannot be pressed. */
            kui::Widget Tool(const std::string_view icon, const std::string& what, const bool active, std::function<void()> pressed, const bool enabled = true)
            {
                const kui::Color tint = enabled ? kui::Color::Hex(0xE0E0E0) : kui::Color::Hex(0x707070);
                kui::Widget face = kui::Container(kui::ContainerOptions {}.SetSize(24.f, 24.f).SetAlignment(kui::Alignment::Center())
                                                      .SetDecoration(kui::Decoration {}.SetColor(active ? kui::Theme::Current().primary.WithAlpha(0.35f) : kui::colors::Transparent).SetRadius(3.f)),
                                                  kui::SizedBox(16.f, 16.f, kui::Icon(icon, kui::IconStyle::eFilled, tint)));
                return kui::Tooltip(what, kui::Button(std::move(face), std::move(pressed), kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain).SetEnabled(enabled)));
            }
            static kui::Widget ToolSeparator() { return kui::SizedBox(1.f, 16.f, kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x606060))))); }

            /**
             * What the Scene and Game panels' title bars have, at their end: play, pause and stop — and, the Scene's,
             * whose eyes it is seen through.
             */
            kui::Widget TitleTools(const bool scene)
            {
                const bool playMode = _session->InPlayMode();
                std::vector<kui::Widget> tools;
                tools.push_back(kui::Expanded(kui::Text("")));
                if (scene) {
                    tools.push_back(Tool("videocam", "Look through the scene's camera", _session->ThroughSceneCamera(),
                                         [this] { _session->SetLookThroughCamera(!_session->lookThroughCamera); }));
                    tools.push_back(ToolSeparator());
                }
                if (_session->IsPlaying()) tools.push_back(Tool("pause", "Pause", false, [this] { _session->Pause(); }));
                else tools.push_back(Tool("play_arrow", playMode ? "Resume" : "Play: scripts and the scene's systems run", playMode, [this] { _session->StartPlaying(); }));
                tools.push_back(Tool("stop", "Stop, and put the scene back as it was", false, [this] { _session->Stop(); }, playMode));
                return kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f),
                                    kui::Row(std::move(tools), kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
            }

            /**
             * The scene as it runs, filling its panel: over it what the editor draws of its own — the line round what
             * is picked, the gizmo — and, in a corner, the gizmo's tools on a backing that reads over whatever the scene
             * shows behind it.
             */
            kui::Widget ScenePanel()
            {
                const kecs::Entity selected = _session->selectedEntity;
                std::vector<kui::Widget> tools;
                tools.push_back(Tool("near_me", "Select only: no gizmo", !_session->gizmoEnabled, [this] { _session->SetGizmo(false, _session->gizmo); }));
                for (const auto& [mode, icon, what] : { std::tuple { kor::GizmoMode::eTranslate, "open_with", "Move what is picked" },
                                                        std::tuple { kor::GizmoMode::eRotate, "3d_rotation", "Rotate what is picked" },
                                                        std::tuple { kor::GizmoMode::eScale, "open_in_full", "Scale what is picked" } })
                    tools.push_back(Tool(icon, what, _session->gizmoEnabled && _session->gizmo == mode, [this, mode] { _session->SetGizmo(true, mode); }));
                tools.push_back(ToolSeparator());
                tools.push_back(Tool("center_focus_strong", "Frame what is picked (the view: right-drag turns, middle-drag moves, the wheel zooms)", false,
                                     [this, selected] { _session->Frame(selected); }, !selected.IsNull()));
                tools.push_back(ToolSeparator());
                tools.push_back(kui::Make<FrameRate>(_session));
                // What the graph can be switched between — its picture, a debug view of it — without going to the node that says which.
                for (const auto& choice : _session->Choices()) {
                    tools.push_back(ToolSeparator());
                    tools.push_back(kui::Tooltip(std::format("{}: what the graph's Switch shows", choice.driver),
                                                 kui::SizedBox(150.f, 24.f, kui::Dropdown(choice.options, std::clamp(choice.chosen, 0, static_cast<int>(choice.options.size()) - 1),
                                                                                          [this, driver = choice.driver](const int index) { _session->Choose(driver, index); }))));
                }

                std::vector<kui::Widget> layers;
                layers.push_back(kgui::SceneView(_session->Play(), _session->viewport));
                if (_session->Play().Over())
                    if (const auto picture = _session->OverPicture(); picture.Get())
                        layers.push_back(kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = 0.f, .right = 0.f, .bottom = 0.f }, kui::IgnorePointer(kui::Image(picture, kui::ImageFit::eFill))));
                layers.push_back(kui::Positioned(kui::PositionedOptions { .left = 8.f, .top = 8.f },
                    kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(5.f, 4.f))
                                       .SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x101012).WithAlpha(0.69f)).SetRadius(6.f)),
                                   kui::Row(std::move(tools), kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter).SetMainAxisSize(kui::MainAxisSize::eMin)))));
                return kui::Stack(std::move(layers));
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
                std::vector<kui::MenuItem> create = ObjectMenu(kecs::NullEntity);
                if (some) {
                    create.push_back({ .separator = true });
                    create.push_back({ .label = std::format("Under {}", Doc().NameOf(selected)), .items = ObjectMenu(selected) });
                }
                const auto ticked = [](const bool on, const char* label) { return std::string(on ? "✓  " : "     ") + label; };
                return kui::MenuBar({
                    { "File", {
                        { "New project...", Asking("Making another project", [this] { SetState([&] { _dialog = Dialog::eNewProject; }); }) },
                        { "Open project...", Asking("Opening another project", [this] { SetState([&] { _dialog = Dialog::eOpenProject; }); }) },
                        { .separator = true },
                        { "New scene", Asking("Making the scene anew", needsProject([this] { _session->NewStandardScene(); })), !playMode },
                        { "Revert to saved", Asking("Putting the saved scene back", [this] { _session->Revert(); }), _session->HasProject() && !playMode },
                        { "Save scene", needsProject([this] { _session->Save(); }), !playMode },
                    } },
                    { "Edit", {
                        { "Undo   Ctrl+Z", [this] { _session->Undo(); }, _session->CanUndo() && !playMode },
                        { "Redo   Ctrl+Y", [this] { _session->Redo(); }, _session->CanRedo() && !playMode },
                        { .separator = true },
                        { "Duplicate", [this, selected] { _session->Duplicate(selected); }, some },
                        { "Move to the top", [this, selected] { Doc().SetParent(selected, kecs::NullEntity); }, some && !Doc().World().ParentOf(selected).IsNull() },
                        { "Save as object", [this, selected] { _session->SaveAsObject(selected); }, some },
                        { "Save as prefab", [this, selected] { _session->SaveAsPrefab(selected); }, some },
                        { .separator = true },
                        { "Delete", [this, selected] { _session->Delete(selected); }, some },
                    } },
                    { "Create", std::move(create) },
                    { "View", {
                        { "Frame the selection", [this, selected] { _session->Frame(selected); }, some },
                        { ticked(_session->ThroughSceneCamera(), "Look through the scene's camera"), [this] { _session->SetLookThroughCamera(!_session->lookThroughCamera); } },
                        { .separator = true },
                        { ticked(!_session->gizmoEnabled, "Select only"), [this] { _session->SetGizmo(false, _session->gizmo); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eTranslate, "Move"), [this] { _session->SetGizmo(true, kor::GizmoMode::eTranslate); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eRotate, "Rotate"), [this] { _session->SetGizmo(true, kor::GizmoMode::eRotate); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eScale, "Scale"), [this] { _session->SetGizmo(true, kor::GizmoMode::eScale); } },
                        { .separator = true },
                        { "Put the panels back where they start", [this] { SetState([&] { _layout = StartingLayout(); }); SaveLayout(); } },
                    } },
                    { "Assets", {
                        { "Import model...", [this] { SetState([&] { _dialog = Dialog::eImportModel; }); } },
                        { "New shader...", needsProject([this] { SetState([&] { _dialog = Dialog::eNewShader; _newId.clear(); _newKind = 0; }); }) },
                        { "New script...", needsProject([this] { SetState([&] { _dialog = Dialog::eNewScript; _newId.clear(); _newKind = 0; }); }) },
                    } },
                    { "Scene", {
                        _session->IsPlaying() ? kui::MenuItem { "Pause", [this] { _session->Pause(); } }
                                              : kui::MenuItem { playMode ? "Resume" : "Play", [this] { _session->StartPlaying(); } },
                        { "Stop", [this] { _session->Stop(); }, playMode },
                    } },
                    { "Project", {
                        { "Run", needsProject([this] { _session->Run(); }) },
                        { "Open with its scripts", [this] { _session->ReopenWithScripts(); }, !_session->ScriptsNotRunning().empty() },
                        { ticked(_session->HasProject() && Same(Session::StartupProject(), _session->CurrentProject().directory), "Open at startup"),
                          [this] { ToggleStartup(); }, _session->HasProject() },
                        { "Open in Visual Studio Code", needsProject([this] { _session->OpenInCode(); }) },
                    } },
                    { "Graph", {
                        { "Add node...   Ctrl+Space", [this] { AskForNode(NewNodeAt()); } },
                        { "Paste   Ctrl+V", [this] { const kor::Vec2 at = NewNodeAt(); _session->PasteNodes(at.x, at.y); }, _session->HasCopiedNodes() },
                        { "Duplicate   Ctrl+D", [this] { if (!_session->selectedNode.empty()) _session->DuplicateNodes({ _session->selectedNode }); }, !_session->selectedNode.empty() },
                        { "Arrange this page", [this] { Arrange(); } },
                        { "Frame everything   F", [this] { _view->Frame(); SetState(); } },
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
                    return kgui::FileBrowser({ .start = Session::ProjectsDirectory(), .directories = true, .createDirectories = true, .confirm = "Make the project here" },
                        [this](const std::filesystem::path& chosen) { _session->NewProject(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                case Dialog::eOpenProject:
                    return kgui::FileBrowser({ .start = Session::ProjectsDirectory(), .directories = true, .confirm = "Open" },
                        [this](const std::filesystem::path& chosen) { _session->OpenProject(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                case Dialog::eConfirm: return ConfirmDialog();
                case Dialog::eImportOptions: return ImportOptionsDialog();
                case Dialog::eNewFolder: return NewFolderDialog();
                case Dialog::eTrash: return TrashDialog();
                case Dialog::eImportModel:
                    return kgui::FileBrowser({ .confirm = "Import" },
                        [this](const std::filesystem::path& chosen) { AskToImport(chosen); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                case Dialog::eNewShader: return NewFileDialog("A new shader", "Its name: the module a pipeline names", Session::ShaderKinds(), true);
                case Dialog::eNewScript: return NewFileDialog("A new script", "Its name: the class, and the component", _session->ScriptKindsHere(), false);
                case Dialog::eNewFunction: return FunctionDialog();
                default: return {};
                }
            }

            /** The name of a function to make of the nodes picked: they become it, and one node in their place calls it. */
            kui::Widget FunctionDialog()
            {
                const auto make = [this] {
                    if (!_session->MakeFunction(_functionOf, _newId).empty()) SetState([&] { _dialog = Dialog::eNone; });
                    else SetState();
                };
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(std::format("A function of {} nodes", _functionOf.size())));
                rows.push_back(kgui::Line("They become a function of the project's — functions/<name>.kfunc — and one node calls it where they were. Each wire into them "
                                          "is an input of it, each wire out of them an output, and the call is wired as they were.",
                                          kui::TextStyle {}.SetColor(kui::Theme::Current().textMuted).SetSize(12.f)));
                rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name: Instances",
                                                                      .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                      .onSubmitted = [this, make](const std::string& typed) { _newId = typed; make(); }, .width = 420.f }));
                if (_session->Failed()) rows.push_back(Problem(_session->Status()));
                rows.push_back(kui::Row({ kui::Button("Make it", make), kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                        kui::FlexOptions {}.SetGap(8.f)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(460.f));
            }

            /** A name and one of a few kinds, for a file to be made from: a shader, or a script. */
            kui::Widget NewFileDialog(const char* title, const char* placeholder, const std::vector<std::pair<std::string, std::string>>& kinds, const bool shader)
            {
                if (_newKind >= static_cast<int>(kinds.size())) _newKind = 0;
                const auto make = [this, kinds, shader] {
                    const std::string& kind = kinds[static_cast<std::size_t>(_newKind)].first;
                    if (shader ? _session->NewShader(_newId, kind) : _session->NewScript(_newId, kind)) SetState([&] { _dialog = Dialog::eNone; });
                    else SetState();
                };
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(title));
                rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = placeholder,
                                                                      .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                      .onSubmitted = [this, make](const std::string& typed) { _newId = typed; make(); }, .width = 420.f }));
                for (std::size_t i = 0; i < kinds.size(); ++i) {
                    rows.push_back(kui::RadioButton(static_cast<int>(i) == _newKind, [this, i] { SetState([&] { _newKind = static_cast<int>(i); }); }, kinds[i].first));
                    rows.push_back(kui::Padding(kui::EdgeInsets { 28.f, 0.f, 0.f, 4.f }, kgui::Line(kinds[i].second, kui::TextStyle {}.SetColor(kui::Theme::Current().textMuted).SetSize(12.f))));
                }
                if (_session->Failed()) rows.push_back(Problem(_session->Status()));
                rows.push_back(kui::Row({ kui::Button("Make it", make), kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                        kui::FlexOptions {}.SetGap(8.f)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(460.f));
            }

            void AddOf(const std::string& type)
            {
                const std::string id = Doc().AddNode(type, _addAt.x, _addAt.y, _newId, ShownPass());
                _session->Select(id);
                _session->Say(std::format("Added {}.", id));
                if (_wire) WireTo(id);
                SetState([&] { _dialog = Dialog::eNone; });
            }

            /** A wire let go over nothing: where it came from, and what it carries. */
            struct LooseWire {
                kui::PortRef from;
                bool fromOutput = true;
                std::string type;
            };

            /** Where a node asked for from a menu or a key goes: the middle of what the graph shows, each a little off the last. */
            kor::Vec2 NewNodeAt()
            {
                const float off = 24.f * static_cast<float>(_made++ % 6);
                return { std::round(_view->centre.x - 90.f + off), std::round(_view->centre.y - 60.f + off) };
            }

            /** Opens the list of nodes there are to add, for one at @p at — wired to @p wire, when it comes of a wire let go. */
            void AskForNode(const kor::Vec2 at, std::optional<LooseWire> wire = std::nullopt)
            {
                SetState([&] { _dialog = Dialog::eAddNode; _addAt = at; _filter.clear(); _newId.clear(); _wire = std::move(wire); });
            }

            /** Whether a node of kind @p type has somewhere for @p wire to go. */
            static bool Takes(const kgraph::NodeType& type, const LooseWire& wire)
            {
                const bool value = wire.type.starts_with("v:");
                // A value goes into anything that has a setting: what is offered is what works on values, and what has one of its kind.
                if (value && wire.fromOutput && (type.category == "Values" || type.category == "Flow")) return true;
                for (const auto& port : wire.fromOutput ? type.inputs : type.outputs) {
                    const std::string kind = WireType(port);
                    if (value ? kind == wire.type : wire.fromOutput ? WireFits(wire.type, kind) : WireFits(kind, wire.type)) return true;
                }
                return false;
            }

            /** Wires the node just made to the wire it was made for: the first of its ports the wire fits, one of its own kind before any other. */
            void WireTo(const std::string& id)
            {
                const LooseWire wire = *std::exchange(_wire, std::nullopt);
                const kgraph::Node* node = Doc().Graph().Find(id);
                if (!node) return;
                std::string best;
                int bestScore = -1;
                const auto consider = [&](const kgraph::Port& port, const bool fits) {
                    if (!fits) return;
                    const int score = (WireType(port) == wire.type ? 2 : 0) + (port.optional ? 0 : 1);
                    if (score > bestScore) { bestScore = score; best = port.name; }
                };
                if (wire.fromOutput) {
                    std::vector<kgraph::Port> more;
                    for (const auto& shown : InputsOf(Doc().Runtime(), *node, [](const std::string&) { return false; }, &more)) consider(shown.port, WireFits(wire.type, WireType(shown.port)));
                    for (const auto& port : more) if (port.name.find("[]") == std::string::npos) consider(port, WireFits(wire.type, WireType(port)));
                    if (!best.empty()) Doc().Connect(wire.from.node, wire.from.port, id, best);
                } else {
                    for (const auto& port : Doc().Runtime().OutputsOf(id)) consider(port, WireFits(WireType(port), wire.type));
                    if (!best.empty()) Doc().Connect(id, best, wire.from.node, wire.from.port);
                }
                if (best.empty()) _session->Say(std::format("Added {}. Nothing of it takes that wire: it is not wired.", id), true);
            }

            /** What a Reference stands for, however many References away; the node itself, when it is not one. */
            const kgraph::Node* StandsFor(const kgraph::Node& node)
            {
                const kgraph::Node* at = &node;
                for (int hops = 0; at && at->type == "Reference" && hops < 8; ++hops) {
                    const auto target = at->inputs.find("node");
                    at = target != at->inputs.end() && target->second.value.String() ? Doc().Graph().Find(*target->second.value.String()) : nullptr;
                }
                return at;
            }

            /** Puts the page's nodes in order, left to right by what is wired to what. */
            void Arrange()
            {
                // As the page is shaped: as wide, for how tall, as the panel it is in.
                const float aspect = _view->size.y > 1.f ? std::clamp(_view->size.x / _view->size.y, 0.4f, 4.f) : 1.6f;
                const auto where = ArrangeNodes(_placed, _wires, aspect);
                if (where.empty()) return;
                if (_overview) Doc().PlaceInOverview(where);
                else Doc().PlaceNodes(where);
                _view->Frame();
                _session->Say("The page's nodes are in the order they are wired in. Ctrl+Z puts them back.");
                SetState();
            }

            /**
             * The nodes there are to add. With nothing typed, every kind, under its category. As something is typed,
             * the kinds it finds, best first, each with what it does — and the nodes of this graph it names, to go to.
             * Enter adds the first.
             */
            kui::Widget AddNodeDialog()
            {
                auto found = FindNodeTypes(_filter, Doc().Presets());
                // For a wire let go over nothing: only what has somewhere for it to go.
                if (_wire) std::erase_if(found, [&](const kgraph::NodeType* type) { return !Takes(*type, *_wire); });
                // The categories, in the order work goes: what the scene holds and what is worked out of it, then what is
                // made on the device, and what runs.
                static const std::vector<std::string> order { "Components", "Scene", "Values", "Math", "Vectors", "Cameras", "Logic", "Resources", "Pipelines", "Passes", "Flow", "Functions" };
                std::map<std::string, std::size_t> counts;
                for (const auto* type : found) ++counts[type->category];
                std::vector<std::string> categories;
                for (const auto& category : order) if (counts.contains(category)) categories.push_back(category);
                for (const auto& [category, count] : counts) if (!std::ranges::contains(categories, category)) categories.push_back(category);
                const std::string category = std::ranges::contains(categories, _menuCategory) ? _menuCategory : std::string();

                // Left: every category, with how many it has of what is found.
                std::vector<kui::Widget> left;
                const auto categoryRow = [&](const std::string& name, const std::string& label, const std::size_t count) {
                    return kui::Selectable(std::format("{}   {}", label, count), category == name, [this, name] { SetState([&] { _menuCategory = name; }); });
                };
                left.push_back(categoryRow({}, _filter.empty() ? "Everything" : "All that is found", found.size()));
                for (const auto& name : categories)
                    left.push_back(kui::Row({ kui::Container(kui::ContainerOptions { .width = 4.f, .height = 18.f, .decoration = { .color = CategoryColor(name), .radius = 2.f } }),
                                              kui::Expanded(categoryRow(name, name, counts[name])) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));

                // Middle: the nodes of the category — or, as something is typed, what it finds, best first.
                std::vector<const kgraph::NodeType*> listed;
                for (const auto* type : found) if (category.empty() || type->category == category) listed.push_back(type);
                if (_filter.empty() && category.empty())
                    std::ranges::stable_sort(listed, [&](const auto* a, const auto* b) {
                        const auto at = [&](const std::string& c) { const auto it = std::ranges::find(categories, c); return it - categories.begin(); };
                        return at(a->category) < at(b->category);
                    });
                const std::string preview = std::ranges::any_of(listed, [&](const auto* t) { return t->name == _menuPreview; }) ? _menuPreview
                                          : listed.empty() ? std::string() : listed.front()->name;
                std::vector<kui::Widget> middle;
                std::string under;
                for (std::size_t i = 0; i < listed.size(); ++i) {
                    const auto* type = listed[i];
                    if (category.empty() && _filter.empty() && type->category != under) { under = type->category; middle.push_back(kgui::Muted(under, 11.f)); }
                    const std::string first = type->doc.substr(0, std::min(type->doc.find(". "), type->doc.size()));
                    middle.push_back(kui::GestureDetector(kui::GestureOptions {}.OnEnter([this, name = type->name] { if (_menuPreview != name) SetState([&] { _menuPreview = name; }); }), kui::Column({
                            // Found among every kind, a node says which kind it is.
                            kui::Selectable(_filter.empty() || !category.empty() ? type->name : std::format("{}   ·   {}", type->name, type->category),
                                            type->name == preview && !_filter.empty() && i == 0, [this, name = type->name] { AddOf(name); }),
                            kui::Padding(kui::EdgeInsets { 12.f, 0.f, 0.f, 4.f }, kgui::Line(first, kui::TextStyle {}.SetColor(kui::Theme::Current().textMuted).SetSize(11.f))),
                        }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(0.f))));
                }
                if (middle.empty()) middle.push_back(kgui::Muted(std::format("Nothing is called, or does, '{}'.", _filter)));
                // The nodes this graph has, by name: picked, to go on working with.
                if (!_filter.empty()) {
                    std::string lowered = _filter;
                    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    std::size_t here = 0;
                    for (const auto& node : Doc().Graph().nodes) {
                        std::string id = node.id;
                        std::ranges::transform(id, id.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (id.find(lowered) == std::string::npos || ++here > 6) continue;
                        if (here == 1) middle.push_back(kgui::Muted("In this graph", 11.f));
                        middle.push_back(kui::Selectable(std::format("{}   ·   {}", node.id, node.type), false, [this, picked = node.id] {
                            _session->GoTo(picked);
                            SetState([&] { _dialog = Dialog::eNone; });
                        }));
                    }
                }

                // Right: the node pointed at, as it will be on the graph, and what it is.
                std::vector<kui::Widget> right;
                const auto* shown = std::ranges::find_if(listed, [&](const auto* t) { return t->name == preview; }) != listed.end()
                    ? *std::ranges::find_if(listed, [&](const auto* t) { return t->name == preview; }) : nullptr;
                if (shown) {
                    kui::NodeGraph one;
                    one.nodes.push_back(PreviewCard(*shown));
                    auto view = std::make_shared<kui::NodeEditorView>();
                    view->page = "preview " + shown->name;
                    view->Frame();
                    right.push_back(kui::Container(kui::ContainerOptions { .height = 230.f, .decoration = { .color = kui::Theme::Current().background, .radius = 6.f } },
                                                   kui::IgnorePointer(kui::NodeEditor(std::move(one), kui::NodeEditorOptions {}.SetView(view).SetGridSize(20.f)))));
                    right.push_back(kui::Row({ kui::Container(kui::ContainerOptions { .width = 4.f, .height = 16.f, .decoration = { .color = CategoryColor(shown->category), .radius = 2.f } }),
                                               Heading(shown->name), kgui::Muted(shown->category, 11.f) }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                    right.push_back(kui::Text(shown->doc, kui::TextStyle {}.SetSize(12.f)));
                    const auto names = [&](const std::vector<kgraph::Port>& ports, const bool outputs) {
                        std::string text;
                        for (const auto& port : ports)
                            if (!port.optional || outputs) text += (text.empty() ? "" : ", ") + (outputs && port.name == "out" ? TypeLabel(port.type) : PortLabel(port.name));
                        return text;
                    };
                    if (const std::string in = names(shown->inputs, false); !in.empty()) right.push_back(kgui::Labeled("Takes", kgui::Line(in, kui::TextStyle {}.SetSize(11.f)), 70.f));
                    if (const std::string out = names(shown->outputs, true); !out.empty()) right.push_back(kgui::Labeled("Gives", kgui::Line(out, kui::TextStyle {}.SetSize(11.f)), 70.f));
                    right.push_back(Small(std::format("Add {}", shown->name), [this, name = shown->name] { AddOf(name); }));
                }

                const auto column = [](std::vector<kui::Widget> children, const float width, const float gap) {
                    return kui::SizedBox(width, 460.f, kui::ScrollView(kui::Column(std::move(children), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(gap))));
                };
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading(_wire ? (_wire->fromOutput ? std::format("Add a node, wired from {} of {}", PortLabel(_wire->from.port), _wire->from.node)
                                                       : std::format("Add a node, wired to {} of {}", PortLabel(_wire->from.port), _wire->from.node))
                                  : std::string("Add a node")),
                    kui::Row({
                        kui::TextField(kui::TextFieldOptions { .text = _filter, .placeholder = "Type what you are after: image, blur, clamp, light...",
                                                               .onChanged = [this](const std::string& typed) { SetState([&] { _filter = typed; }); },
                                                               .onSubmitted = [this](const std::string& typed) {
                                                                   auto first = FindNodeTypes(typed, Doc().Presets());
                                                                   if (_wire) std::erase_if(first, [&](const kgraph::NodeType* type) { return !Takes(*type, *_wire); });
                                                                   if (!_menuCategory.empty()) std::erase_if(first, [&](const kgraph::NodeType* type) { return type->category != _menuCategory; });
                                                                   if (!typed.empty() && !first.empty()) AddOf(first.front()->name);
                                                               }, .width = 420.f, .focus = 1 }),
                        kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name (a pipeline's names its component)",
                                                               .onChanged = [this](const std::string& typed) { _newId = typed; }, .width = 320.f }),
                    }, kui::FlexOptions {}.SetGap(8.f)),
                    kui::Row({ column(std::move(left), 180.f, 2.f), column(std::move(middle), 330.f, 0.f),
                               kui::SizedBox(380.f, 460.f, kui::Column(std::move(right), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(8.f))) },
                             kui::FlexOptions {}.SetGap(14.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(12.f)));
            }

            /**
             * A node of a type as it is when it is made: what the menu shows of a node before it is added. Its inputs
             * that are set on the node, with their controls as they start; the rest it cannot do without, as ports.
             */
            kui::GraphNode PreviewCard(const kgraph::NodeType& type)
            {
                kgraph::Node node;
                node.id = "preview";
                node.type = type.name;
                kui::GraphNode card;
                card.id = "preview";
                card.title = type.name;
                card.icon = type.icon;
                card.accent = CategoryColor(type.category);
                for (const auto& port : type.inputs) {
                    if (port.name.find("[]") != std::string::npos) {
                        if (port.optional) continue;
                    }
                    const kgraph::Placement placement = kgraph::PlacementOf(port, node);
                    const bool asPort = port.kind == kgraph::PortKind::eResource || placement != kgraph::Placement::eInspector;
                    if (!asPort || (port.optional && placement != kgraph::Placement::eNode)) continue;
                    kgraph::Port shown = port;
                    if (const auto open = shown.name.find("[]"); open != std::string::npos) shown.name.insert(open + 1, "0");
                    kui::NodePort row { shown.name, PortLabel(shown.name), WireType(shown) };
                    row.color = PortColor(row.type);
                    if (placement == kgraph::Placement::eNode) row.control = NodeControl(node, shown, shown.initial);
                    card.inputs.push_back(std::move(row));
                }
                for (const auto& port : type.outputs)
                    if (!port.optional) {
                        kui::NodePort row { port.name, port.name == "out" ? TypeLabel(port.type) : PortLabel(port.name), WireType(port) };
                        row.color = PortColor(row.type);
                        card.outputs.push_back(std::move(row));
                    }
                std::vector<kui::Widget> body { kgui::Muted(type.name, 11.f) };
                if (type.name == "VertexLayout") {
                    const auto initial = std::ranges::find(type.inputs, "attributes", &kgraph::Port::name);
                    if (initial != type.inputs.end()) body.push_back(LayoutPicture(AttributesOf(initial->initial)));
                }
                card.body = kui::Column(std::move(body), kui::FlexOptions {}.SetGap(6.f));
                return card;
            }

            // ---- the graph --------------------------------------------------------------------------------------

            /** The pass whose page is shown: none when every node is, or when the graph has no passes. */
            std::string ShownPass()
            {
                const auto& passes = Doc().Passes();
                if (_session->ShowsEveryPass() || passes.empty()) return {};
                if (std::ranges::find(passes, _session->Pass()) == passes.end()) _session->ShowPass(passes.front());
                return _session->Pass();
            }

            /**
             * The passes a frame is made in, in the order they are drawn: pressed, a pass's page is shown. Under them,
             * for the one shown: something to make in it, and what the passes before it made, to bring in.
             */
            kui::Widget PassList()
            {
                const kui::Theme& t = kui::Theme::Current();
                const auto passes = Doc().Passes();
                const std::string shown = ShownPass();
                const kgraph::Runtime& runtime = Doc().Runtime();
                // A heading over a part of the list, and what it says, small and quiet.
                const auto section = [&](std::string title, kui::Widget trailing = {}) {
                    std::vector<kui::Widget> line { kui::Expanded(kui::Text(std::move(title), kui::TextStyle {}.SetSize(11.f).SetColor(t.textMuted).SetLetterSpacing(0.6f), kui::TextAlign::eStart, false, 1, true)) };
                    if (trailing) line.push_back(std::move(trailing));
                    return kui::Padding(kui::EdgeInsets::Only(4.f, 10.f, 0.f, 2.f), kui::SizedBox(-1.f, 22.f, kui::Row(std::move(line), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));
                };
                // A number in a pill: how many, or how many wrong.
                const auto chip = [](std::string text, const kui::Color& tint) {
                    return kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(6.f, 1.f)).SetDecoration(kui::Decoration {}.SetColor(tint.WithAlpha(0.16f)).SetRadius(8.f)),
                                          kui::Text(std::move(text), kui::TextStyle {}.SetSize(11.f).SetColor(tint), kui::TextAlign::eStart, false));
                };
                // A line of the list: picked, it is lit and marked down its left; under the pointer, lit a little.
                const auto entry = [&](const std::string& key, const bool picked, std::vector<kui::Widget> line, std::function<void()> onTap) {
                    const bool over = _passHover == key;
                    kui::Widget row = kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Only(10.f, 0.f, 6.f, 0.f))
                                                         .SetDecoration(kui::Decoration {}.SetColor(picked ? t.primary.WithAlpha(0.16f) : over ? t.surfaceHover : kui::colors::Transparent).SetRadius(5.f)),
                        kui::SizedBox(-1.f, 28.f, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));
                    if (picked)
                        row = kui::Stack({ std::move(row), kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = 6.f, .bottom = 6.f, .width = 3.f },
                            kui::IgnorePointer(kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary).SetRadius(2.f))))) });
                    return kui::GestureDetector(kui::GestureOptions {}.OnTap(std::move(onTap))
                                                    .OnEnter([this, key] { if (_passHover != key) SetState([&] { _passHover = key; }); })
                                                    .OnExit([this, key] { if (_passHover == key) SetState([&] { _passHover.clear(); }); }), std::move(row));
                };
                const auto icon = [](const std::string_view name, const kui::Color& tint) { return kui::SizedBox(15.f, 15.f, kui::Icon(name, kui::IconStyle::eFilled, tint)); };
                const auto name = [&](std::string text, const bool picked) {
                    return kui::Expanded(kui::Text(std::move(text), kui::TextStyle {}.SetSize(13.f).SetColor(picked ? t.text : t.text.WithAlpha(0.86f)), kui::TextAlign::eStart, false, 1, true));
                };

                std::vector<kui::Widget> rows;
                // A function open in place: what its page is, and the way out — keeping what was done, or not.
                if (const auto& opened = Doc().Opened()) {
                    const kui::Color teal = kui::Color::Hex(0x3E9FA8);
                    rows.push_back(kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::All(10.f))
                                                      .SetDecoration(kui::Decoration {}.SetColor(teal.WithAlpha(0.14f)).SetBorderWidth(1.f).SetBorderColor(teal.WithAlpha(0.45f)).SetRadius(6.f)),
                        Rows({ kui::Row({ icon("functions", teal), kui::Expanded(kui::Text(std::format("Function {} is open", opened->function), kui::TextStyle {}.SetSize(13.f).SetColor(t.text), kui::TextAlign::eStart, false, 1, true)) },
                                        kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)),
                               kgui::Muted(std::format("What is done on its page is the function's, for every call. Its inputs are what {} was given.", opened->call.id), 11.f),
                               kui::Row({ Small("Done", [this] { _session->CloseFunction(true); }), Small("Discard", [this] { _session->CloseFunction(false); }),
                                          kui::Expanded(kui::Text("")), SmallIcon("open_in_new", [this, pass = opened->pass] { _session->ShowPass(pass); }) },
                                        kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)) }, 6.f)));
                }
                rows.push_back(kui::TextField(kui::TextFieldOptions { .placeholder = "Find a node",
                    .onSubmitted = [this](const std::string& typed) {
                        if (typed.empty()) return;
                        const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                        const std::string wanted = lower(typed);
                        // The one so named, or else the first whose name or kind has it: after the one that is picked, so that Enter again goes on to the next.
                        const auto& nodes = Doc().Graph().nodes;
                        const auto picked = std::ranges::find(nodes, _session->selectedNode, &kgraph::Node::id);
                        const std::size_t from = picked == nodes.end() ? 0 : static_cast<std::size_t>(picked - nodes.begin()) + 1;
                        for (std::size_t i = 0; i < nodes.size(); ++i) {
                            const kgraph::Node& node = nodes[(from + i) % nodes.size()];
                            if (lower(node.id).find(wanted) == std::string::npos && lower(node.type).find(wanted) == std::string::npos) continue;
                            _session->GoTo(node.id);
                            _session->Say(std::format("Found '{}'{}.", node.id, node.pass.empty() ? std::string() : std::format(", in {}", node.pass)));
                            return;
                        }
                        _session->Say(std::format("No node's name or kind has '{}' in it.", typed), true);
                    } }).Key("find node"));

                // Every node at once: a page of its own, laid out apart from the passes' pages.
                rows.push_back(kui::Tooltip("Every node of every pass, on one page laid out by itself: moving a node here does not move it on its pass's page.",
                    entry("*", shown.empty(), { icon("grid_view", shown.empty() ? t.primary : t.textMuted), name("Every node at once", shown.empty()),
                                                chip(std::to_string(Doc().Graph().nodes.size()), t.textMuted) }, [this] { _session->ShowEveryPass(); })));

                rows.push_back(section("PASSES, AS THEY ARE DRAWN", kui::Tooltip("A new pass, drawn after the rest", SmallIcon("add", [this] { SetState([&] { _addingPass = !_addingPass; ++_passFocus; }); }))));
                for (std::size_t i = 0; i < passes.size(); ++i) {
                    const std::string& pass = passes[i];
                    std::size_t nodes = 0, failed = 0;
                    for (const auto& node : Doc().Graph().nodes) {
                        if (node.pass != pass) continue;
                        ++nodes;
                        if (const auto* status = runtime.Status(node.id); status && !status->ok) ++failed;
                    }
                    const bool picked = pass == shown;
                    const int index = static_cast<int>(i);
                    std::vector<kui::Widget> line;
                    line.push_back(kui::SizedBox(18.f, -1.f, kui::Text(std::to_string(i + 1), kui::TextStyle {}.SetSize(11.f).SetColor(picked ? t.primary : t.textMuted), kui::TextAlign::eEnd, false)));
                    line.push_back(name(pass, picked));
                    if (failed) line.push_back(kui::Tooltip(std::format("{} of its nodes could not be made", failed), chip(std::format("{} wrong", failed), kui::Color::Hex(0xE5484D))));
                    line.push_back(chip(std::to_string(nodes), t.textMuted));
                    kui::Widget row = kui::ContextMenu({
                        { "Earlier", [this, pass, index] { Doc().MovePass(pass, index - 1); }, i > 0 },
                        { "Later", [this, pass, index] { Doc().MovePass(pass, index + 1); }, i + 1 < passes.size() },
                        { "First", [this, pass] { Doc().MovePass(pass, 0); }, i > 0 },
                        { "Last", [this, pass, count = static_cast<int>(passes.size())] { Doc().MovePass(pass, count - 1); }, i + 1 < passes.size() },
                        { .separator = true },
                        { nodes ? std::format("Delete, with its {} nodes", nodes) : std::string("Delete"), [this, pass] { Doc().RemovePass(pass); } },
                        { "Delete the pass, keep its nodes", [this, pass] { Doc().RemovePass(pass, true); }, nodes > 0 },
                    }, kui::Tooltip(std::format("{}: {} nodes. Drag it up or down to draw it earlier or later; right-click for more.", pass, nodes),
                                    entry("pass " + pass, picked, std::move(line), [this, pass] { _session->ShowPass(pass); })));
                    // Picked up and let go on another, it takes that one's place: drawn then, and the rest move along.
                    row = kui::Draggable({ "pass", pass }, std::move(row));
                    row = kui::DropTarget(kui::DropTargetOptions {}
                        .Accepts([pass](const kui::DragData& dragged) { const auto* other = dragged.As<std::string>(); return dragged.type == "pass" && other && *other != pass; })
                        .OnMove([this, index](const kui::DragData&, kor::Vec2) { if (_passDrop != index) SetState([&] { _passDrop = index; }); })
                        .OnLeave([this, index] { if (_passDrop == index) SetState([&] { _passDrop = -1; }); })
                        .OnDrop([this, index](const kui::DragData& dragged, kor::Vec2) {
                            _passDrop = -1;
                            if (const auto* moved = dragged.As<std::string>()) {
                                Doc().MovePass(*moved, index);
                                _session->Say(std::format("{} is pass {} now, of {}.", *moved, index + 1, Doc().Passes().size()));
                            }
                        }), std::move(row));
                    // Where it would go: a line over the one it is held over.
                    if (_passDrop == index)
                        row = kui::Stack({ std::move(row), kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = -1.f, .right = 0.f, .height = 2.f },
                            kui::IgnorePointer(kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary))))) });
                    rows.push_back(std::move(row));
                }
                if (passes.empty()) rows.push_back(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kgui::Muted("None yet: every node is on one page. + makes the first.", 11.f)));
                if (_addingPass || passes.empty())
                    rows.push_back(kui::TextField(kui::TextFieldOptions { .placeholder = "Its name, then Enter",
                        .onSubmitted = [this](const std::string& typed) {
                            if (typed.empty()) { SetState([&] { _addingPass = false; }); return; }
                            if (Doc().AddPass(typed)) { _session->ShowPass(typed); SetState([&] { _addingPass = false; }); }
                            else _session->Say(std::format("There is a pass called '{}' already.", typed), true);
                        }, .focus = _passFocus, .onEscape = [this] { SetState([&] { _addingPass = false; }); } }).Key(std::format("new pass {}", passes.size())));

                if (!shown.empty()) {
                    rows.push_back(section("THIS PASS"));
                    rows.push_back(kui::Tooltip("Its name: typed over, then Enter",
                        kui::TextField(kui::TextFieldOptions { .text = shown,
                            .onSubmitted = [this, shown](const std::string& typed) { if (typed != shown && Doc().RenamePass(shown, typed)) _session->ShowPass(typed); } }).Key("rename " + shown)));
                    // Something to make in it: the kinds of thing a pass has of its own.
                    std::vector<std::string> kinds;
                    for (const auto& type : kgraph::Catalog())
                        if (type.listed && (type.category == "Resources" || type.category == "Pipelines" || type.category == "Passes" || type.category == "Scene" || type.category == "Flow") && type.name != "Reference")
                            kinds.push_back(type.name);
                    rows.push_back(kui::Tooltip("Makes a node in this pass. Right-click the graph, or Control+Space, for every kind there is.",
                        kui::Dropdown(kinds, -1, [this, kinds, shown](const int index) {
                            const kor::Vec2 at = NewNodeAt();
                            _session->Select(Doc().AddNode(kinds[static_cast<std::size_t>(index)], at.x, at.y, {}, shown));
                        }, kui::DropdownOptions {}.SetPlaceholder("Create a node"))));
                    // What the passes before it made: brought in, it is a node here to wire from.
                    const auto importable = Doc().Importable(shown);
                    std::vector<std::string> labels;
                    for (const auto& id : importable) {
                        const kgraph::Node* node = Doc().Graph().Find(id);
                        labels.push_back(node && !node->pass.empty() ? std::format("{}  · {}", id, node->pass) : id);
                    }
                    rows.push_back(kui::Tooltip("Brings what an earlier pass made into this one: a node here that stands for it, to wire from.",
                        kui::Dropdown(labels, -1, [this, importable, shown](const int index) {
                            const kor::Vec2 at = NewNodeAt();
                            _session->Select(Doc().Import(importable[static_cast<std::size_t>(index)], shown, at.x, at.y));
                        }, kui::DropdownOptions {}.SetPlaceholder(importable.empty() ? "Nothing before it to bring in" : "Bring in from before"))));
                }
                // As wide as it is, whatever is in it: what is longer ends in an ellipsis, not past the edge.
                return kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.surface)),
                    kui::ScrollView(kui::Padding(kui::EdgeInsets::Symmetric(10.f, 10.f),
                        kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(3.f))))).Width(236.f);
            }

            /** The graph: the passes down its side, and the page of the one that is shown. */
            kui::Widget Graph()
            {
                return kui::Row({ PassList(), kui::Separator(kui::Axis::eVertical), kui::Expanded(GraphPage()) },
                                kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
            }

            kui::Widget GraphPage()
            {
                const kgraph::Graph& graph = Doc().Graph();
                const kgraph::Runtime& runtime = Doc().Runtime();
                const std::string page = ShownPass();
                const bool everything = page.empty();
                // Every node at once, of passes: a layout of its own, not the passes' pages over one another.
                const bool overview = everything && !Doc().Passes().empty();
                Doc().MakeInOverview(overview);
                _overview = overview;
                const auto places = overview ? Doc().OverviewPlaces() : std::map<std::string, std::pair<double, double>> {};
                const auto here = [&](const kgraph::Node& node) { return everything || node.pass == page; };
                kui::NodeGraph shown;
                std::set<std::pair<std::string, std::string>> wiredFrom;
                for (const auto& node : graph.nodes)
                    for (const auto& input : node.inputs | std::views::values)
                        if (input.link) wiredFrom.insert({ input.link->node, input.link->output });
                // What @p of gives — its type's outputs, and an Entities node's fields — as outputs of @p node.
                const auto outputsOf = [&](const kgraph::Node& node, const kgraph::Node& of, kui::GraphNode& card) {
                    for (const auto& output : runtime.OutputsOf(of.id))
                        if (!output.optional || wiredFrom.contains({ node.id, output.name }) || _session->Asked(node.id, "out:" + output.name)) {
                            kui::NodePort port { output.name, output.name == "out" ? TypeLabel(output.type) : PortLabel(output.name), WireType(output) };
                            port.color = PortColor(port.type);
                            card.outputs.push_back(std::move(port));
                        }
                };
                // About how big a card comes out, from what is on it: for putting the page in order.
                const auto wide = [](const std::string& text, const float each) { return static_cast<float>(text.size()) * each; };
                const auto measured = [&](const kui::GraphNode& card, const float bodyWidth, const float bodyHeight) {
                    PlacedNode placed { card.id, card.position.x, card.position.y };
                    const std::size_t rows = std::max(card.inputs.size(), card.outputs.size());
                    placed.width = std::max({ kui::nodes::MinWidth, wide(card.title, 7.6f) + 24.f + (card.icon.empty() ? 0.f : 21.f), bodyWidth + 16.f });
                    for (std::size_t i = 0; i < rows; ++i) {
                        float row = 40.f;
                        if (i < card.inputs.size())
                            row += wide(card.inputs[i].label, 6.6f) + (card.inputs[i].control ? 120.f : card.inputs[i].value.empty() ? 0.f : 8.f + wide(card.inputs[i].value, 6.6f));
                        if (i < card.outputs.size()) row += wide(card.outputs[i].label, 6.6f);
                        placed.width = std::max(placed.width, row);
                    }
                    placed.height = kui::nodes::TitleHeight + kui::nodes::RowHeight * static_cast<float>(rows) + bodyHeight + 16.f + (card.error.empty() ? 0.f : 44.f);
                    return placed;
                };
                _placed.clear();
                _wires.clear();
                std::set<std::string> visible;
                // The loops: each Repeat, and with it every node that is there once a turn because of it. And each ForEach,
                // with what is worked out once an element of it.
                std::map<std::string, std::vector<std::string>> loops, eaches;
                const std::function<std::string(const kgraph::Node&, int)> loopOf = [&](const kgraph::Node& node, const int depth) -> std::string {
                    if (node.type == "Repeat") return node.id;
                    if (depth > 24) return {};
                    for (const auto& input : node.inputs | std::views::values)
                        if (input.link)
                            if (const kgraph::Node* from = graph.Find(input.link->node); from && (from->type == "Repeat" || !runtime.TurnsOf(from->id).empty()))
                                if (std::string found = loopOf(*from, depth + 1); !found.empty()) return found;
                    return {};
                };
                for (const auto& node : graph.nodes) {
                    if (!here(node)) continue;
                    visible.insert(node.id);
                    const kgraph::NodeType* type = kgraph::FindType(node.type);
                    kui::GraphNode card;
                    card.id = node.id;
                    card.title = node.title.empty() ? node.id : node.title;
                    // One of a loop is there once a turn: its node says how many it stands for.
                    const std::size_t turns = runtime.TurnsOf(node.id).size();
                    if (turns > 0) {
                        card.title += std::format("   x{}", turns);
                        if (const std::string loop = loopOf(node, 0); !loop.empty()) loops[loop].push_back(node.id);
                    } else if (node.type == "Repeat") {
                        if (const auto count = node.inputs.find("count"); count != node.inputs.end() && count->second.value.Number()) card.title += std::format("   {} turns", static_cast<int>(*count->second.value.Number()));
                        loops[node.id];
                    }
                    if (const std::string each = runtime.EachOf(node.id); !each.empty()) {
                        // Worked out once an element of a ForEach (or a Filter), a buffer of an element each, a draw of
                        // each: in the node that holds them, with it.
                        eaches[each];
                        if (each != node.id) eaches[each].push_back(node.id);
                    }
                    card.position = { static_cast<float>(node.x), static_cast<float>(node.y) };
                    if (const auto at = places.find(node.id); at != places.end())
                        card.position = { static_cast<float>(at->second.first), static_cast<float>(at->second.second) };
                    card.accent = CategoryColor(type ? type->category : std::string());
                    if (type) card.icon = type->icon;
                    for (const auto& input : InputsOf(runtime, node, [&](const std::string& name) { return _session->Asked(node.id, name); }))
                        if (ShowsAsPort(input, node)) {
                            kui::NodePort port { input.port.name, PortLabel(input.port.name), WireType(input.port) };
                            port.color = PortColor(port.type);
                            const auto given = node.inputs.find(input.port.name);
                            const bool wired = given != node.inputs.end() && given->second.link;
                            // Set on the node: its control, while nothing is wired to it — what it is, or what it starts as.
                            if (!wired && kgraph::PlacementOf(input.port, node) == kgraph::Placement::eNode)
                                port.control = NodeControl(node, input.port, given != node.inputs.end() && !given->second.value.IsNull() ? given->second.value : input.port.initial);
                            // A field given a value where a wire could go says the value.
                            else if (given != node.inputs.end() && !wired && !given->second.value.IsNull())
                                port.value = Brief(given->second.value, 18);
                            card.inputs.push_back(std::move(port));
                        }
                    std::string kind = node.type;
                    if (node.type == "Reference") {
                        // What was brought in from another pass: it gives what the node it stands for gives.
                        const kgraph::Node* target = StandsFor(node);
                        card.title = target ? target->id : card.title;
                        card.accent = kui::Color::Hex(0x5A6472);
                        if (target) {
                            outputsOf(node, *target, card);
                            // Its main output is the thing itself, whatever else of it is wired from.
                            kind = target->pass.empty() ? std::format("{}, imported", target->type) : std::format("{}, from {}", target->type, target->pass);
                        }
                        if (card.outputs.empty()) card.outputs.push_back({ "out", "Resource", "r:Resource", PortColor("r:Resource") });
                    } else {
                        // What it gives: what it makes, and whatever else of it something is wired to or was asked for.
                        outputsOf(node, node, card);
                    }
                    if (const auto* status = runtime.Status(node.id); status && !status->ok) card.error = status->error;
                    if (turns > 0) kind += ", once a turn";
                    float bodyWidth = wide(kind, 6.f) + 16.f, bodyHeight = 18.f;
                    if (node.type == "Note") {
                        // Words: what it says is all it is.
                        const auto text = node.inputs.find("text");
                        const std::string words = text != node.inputs.end() && text->second.value.String() ? *text->second.value.String() : std::string();
                        card.accent = kui::Color::Hex(0x8A7A3A);
                        card.body = kui::SizedBox(220.f, -1.f, kui::Text(words, kui::TextStyle {}.SetSize(12.f)));
                        bodyWidth = 236.f;
                        bodyHeight = 12.f + 16.f * std::ceil((wide(words, 6.6f) + 1.f) / 220.f);
                    } else {
                        std::vector<kui::Widget> said;
                        // What it holds is looked at from the node itself: an eye on it, and while it is on, the picture.
                        const auto held = runtime.ResourceOf(node.id);
                        const bool looks = held && (runtime.ImageOf(node.id).Get() || held->kind == "Buffer" || held->kind == "BufferView");
                        const bool looking = looks && _session->IsLooking(node.id);
                        if (looks) {
                            said.push_back(kui::Row({ kui::Expanded(kgui::Muted(kind, 11.f)),
                                                      Tool(looking ? "visibility" : "visibility_off",
                                                           looking ? "Stop looking at it closely" : runtime.ImageOf(node.id).Get() ? "Look at it closely: its picture made again every few frames, and the inspector's controls for how it is shown" : "Look at what it holds",
                                                           looking,
                                                           [this, id = node.id, looking] { if (looking) _session->Unlook(id); else _session->Look(id); }) },
                                                    kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                            bodyWidth += 36.f;
                            bodyHeight = 28.f;
                        } else {
                            said.push_back(kgui::Muted(kind, 11.f));
                        }
                        // A vertex layout is drawn: its buffers, and the attributes along each.
                        if (node.type == "VertexLayout") {
                            const auto attributes = node.inputs.find("attributes");
                            const auto read = AttributesOf(attributes != node.inputs.end() ? attributes->second.value : Value {});
                            said.push_back(LayoutPicture(read));
                            std::set<int> buffers;
                            float widest = 0.f;
                            std::map<int, float> along;
                            for (const auto& a : read) { buffers.insert(a.buffer); widest = std::max(widest, along[a.buffer] += std::max(static_cast<float>(BytesOf(a)) * 4.f, 30.f) + 2.f); }
                            bodyWidth = std::max(bodyWidth, widest + 16.f);
                            bodyHeight += 40.f * static_cast<float>(std::max<std::size_t>(buffers.size(), 1));
                        }
                        // What it is set to, said on it in a line: what it is most for someone reading the graph — a
                        // shader, a format, a size, a number, which entities it runs for.
                        if (node.type != "Reference") {
                            std::vector<std::string> parts;
                            for (const auto& [name, input] : node.inputs) {
                                if (input.link || input.value.IsNull() || input.value.Bool()) continue;
                                if (std::ranges::contains(card.inputs, name, &kui::NodePort::id)) continue;
                                const std::string value = Brief(input.value, 20);
                                if (value.empty() || value == "...") continue;
                                const std::string label = PortLabel(name);
                                // Which entities it runs for is said as that: with these, not those.
                                if (label == "with") parts.push_back("with " + value);
                                else if (label == "without") parts.push_back("not " + value);
                                else if (std::ranges::any_of(std::initializer_list<std::string_view> { "module", "format", "extent", "ize", "value", "count", "component", "file", "text", "parameter" },
                                                             [&](const std::string_view word) { return label.find(word) != std::string::npos; })) parts.push_back(value);
                            }
                            std::string line;
                            for (const auto& part : parts) {
                                if (line.size() + part.size() > 36 && !line.empty()) { line += " ..."; break; }
                                line += (line.empty() ? "" : "  ·  ") + part;
                            }
                            if (!line.empty()) {
                                said.push_back(kui::Text(line, kui::TextStyle {}.SetSize(11.f)));
                                bodyWidth = std::max(bodyWidth, wide(line, 6.f) + 16.f);
                                bodyHeight += 17.f;
                            }
                        }
                        // An image is always seen on its node: as it is now while it is looked at, or as it was when its
                        // turn last came to be copied out.
                        const bool image = node.type != "Screen" && runtime.ImageOf(node.id).Get();
                        if (looking || image) {
                            const auto picture = looking ? _session->LookPictureOf(node.id) : _session->ThumbnailOf(node.id);
                            const std::string& says = looking ? _session->LookSaysOf(node.id) : _session->ThumbnailSaysOf(node.id);
                            if (picture.Get()) {
                                said.push_back(kui::Make<NodePicture>(_session, node.id, looking));
                                bodyWidth = std::max(bodyWidth, 256.f);
                                bodyHeight += 156.f;
                                card.width = 264.f;     // as wide as its picture: a card is otherwise as narrow as its words
                            } else {
                                said.push_back(kgui::Muted(says.empty() ? std::string("Its picture is on its way.") : says, 11.f));
                                bodyHeight += 17.f;
                            }
                        }
                        card.body = kui::Column(std::move(said), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
                    }
                    _placed.push_back(measured(card, bodyWidth, bodyHeight));
                    shown.nodes.push_back(std::move(card));
                }
                // A wire from another page's node, not brought in by a Reference: the node is shown beside what it
                // is wired to, as what it is — a thing of another pass.
                if (!everything) {
                    std::map<std::string, kor::Vec2> outside;
                    for (const auto& node : graph.nodes) {
                        if (!here(node)) continue;
                        for (const auto& input : node.inputs | std::views::values)
                            if (input.link && !visible.contains(input.link->node) && graph.Find(input.link->node)) {
                                const auto [at, fresh] = outside.try_emplace(input.link->node, kor::Vec2(static_cast<float>(node.x) - 260.f, static_cast<float>(node.y)));
                                if (!fresh) at->second = { std::min(at->second.x, static_cast<float>(node.x) - 260.f), std::min(at->second.y, static_cast<float>(node.y)) };
                            }
                    }
                    for (const auto& [id, at] : outside) {
                        const kgraph::Node& node = *graph.Find(id);
                        const kgraph::Node* target = StandsFor(node);
                        kui::GraphNode card;
                        card.id = id;
                        card.title = node.title.empty() ? id : node.title;
                        card.position = at;
                        card.accent = kui::Color::Hex(0x5A6472);
                        card.icon = "link";
                        outputsOf(node, target ? *target : node, card);
                        card.body = kui::Padding(kui::EdgeInsets::Symmetric(0.f, 0.f),
                                                 kgui::Muted(node.pass.empty() ? std::format("{}, in no pass", node.type) : std::format("{}, in {}", node.type, node.pass), 11.f));
                        shown.nodes.push_back(std::move(card));
                    }
                }
                std::set<std::string> drawn;
                for (const auto& card : shown.nodes) drawn.insert(card.id);
                for (const auto& node : graph.nodes)
                    if (drawn.contains(node.id) && visible.contains(node.id))
                        for (const auto& [name, input] : node.inputs)
                            if (input.link && drawn.contains(input.link->node)) {
                                shown.wires.push_back({ { input.link->node, input.link->output }, { node.id, name } });
                                _wires.emplace_back(input.link->node, node.id);
                            }
                for (const auto& [repeat, members] : loops)
                    for (auto& placed : _placed)
                        if (placed.id == repeat || std::ranges::contains(members, placed.id)) placed.group = repeat;
                // What a Filter keeps some of — an Entities node, another Filter — is in its node too, when it is that
                // Filter's alone: one that feeds several stands by itself.
                std::map<std::string, std::vector<std::string>> fedTo;
                for (const auto& node : graph.nodes)
                    if (node.type == "Filter")
                        if (const auto from = node.inputs.find("entities"); from != node.inputs.end() && from->second.link) fedTo[from->second.link->node].push_back(node.id);
                for (auto& [each, members] : eaches) {
                    std::string at = each;
                    for (int hops = 0; hops < 16; ++hops) {
                        const kgraph::Node* node = graph.Find(at);
                        const auto from = node && node->type == "Filter" ? node->inputs.find("entities") : decltype(node->inputs.end()) {};
                        if (!node || node->type != "Filter" || from == node->inputs.end() || !from->second.link) break;
                        const std::string& upstream = from->second.link->node;
                        if (fedTo[upstream].size() != 1 || (eaches.contains(upstream) && !eaches[upstream].empty())) break;
                        members.push_back(upstream);
                        at = upstream;
                    }
                }
                // What is of one loop is arranged together, as a Repeat's is.
                for (const auto& [each, members] : eaches)
                    if (!members.empty())
                        for (auto& placed : _placed)
                            if (placed.group.empty() && (placed.id == each || std::ranges::contains(members, placed.id))) placed.group = "each " + each;
                // Each loop is a node that holds its nodes: a ForEach's, a Filter's, a Repeat's.
                std::vector<kui::GraphComment> frames;
                for (auto& [each, members] : eaches) {
                    if (members.empty()) continue;
                    const kgraph::Node* node = graph.Find(each);
                    const auto read = [&](const char* input) -> const kgraph::Value* {
                        if (!node) return nullptr;
                        const auto it = node->inputs.find(input);
                        return it == node->inputs.end() ? nullptr : &it->second.value;
                    };
                    std::string what = "entity";
                    if (const kgraph::Value* name = read("component"); name && name->String() && !name->String()->empty()) what = *name->String();
                    else if (const kgraph::Value* with = read("with"); with && with->Items() && !with->Items()->empty()) {
                        what = "entity with";
                        for (const auto& one : *with->Items()) if (one.String()) what += (what.ends_with("with") ? " " : ", ") + *one.String();
                    }
                    const auto count = runtime.ValueOf(each, "count");
                    kui::GraphComment frame;
                    frame.id = "each " + each;
                    frame.text = std::format("{}: for each {}{}", node && !node->title.empty() ? node->title : each, what,
                                             count.Number() ? std::format("  ·  {}", static_cast<int>(*count.Number())) : std::string());
                    frame.color = kui::Color::Hex(0x3BA0C2);
                    frame.container = true;
                    frame.around = std::move(members);
                    if (visible.contains(each)) frame.around.push_back(each);
                    std::erase_if(frame.around, [&](const std::string& id) { return !visible.contains(id); });
                    frames.push_back(std::move(frame));
                }
                // A loop of turns: its Repeat, and what is there once a turn because of it.
                for (auto& [repeat, members] : loops) {
                    if (members.empty()) continue;
                    const kgraph::Node* node = graph.Find(repeat);
                    const auto count = node ? node->inputs.find("count") : decltype(node->inputs.end()) {};
                    kui::GraphComment frame;
                    frame.id = "loop " + repeat;
                    frame.text = node && count != node->inputs.end() && count->second.value.Number()
                        ? std::format("{}: {} turns", repeat, static_cast<int>(*count->second.value.Number())) : std::format("{}: a loop", repeat);
                    frame.color = kui::Color::Hex(0x9C6ADE);
                    frame.container = true;
                    frame.around = std::move(members);
                    if (visible.contains(repeat)) frame.around.push_back(repeat);
                    frames.push_back(std::move(frame));
                }
                // One that holds all of another's nodes, and more, is round it: further out, and drawn first.
                for (bool grew = true; grew;) {
                    grew = false;
                    for (auto& outer : frames)
                        for (const auto& inner : frames)
                            if (&outer != &inner && outer.around.size() > inner.around.size() && outer.depth <= inner.depth
                                && std::ranges::all_of(inner.around, [&](const std::string& id) { return std::ranges::contains(outer.around, id); })) {
                                outer.depth = inner.depth + 1;
                                grew = true;
                            }
                }
                std::ranges::stable_sort(frames, std::greater {}, &kui::GraphComment::depth);
                for (auto& frame : frames) shown.comments.push_back(std::move(frame));
                // What each port carries, for a wire let go over nothing.
                auto kinds = std::make_shared<std::map<std::tuple<std::string, std::string, bool>, std::string>>();
                for (const auto& card : shown.nodes) {
                    for (const auto& port : card.inputs) (*kinds)[{ card.id, port.id, false }] = port.type;
                    for (const auto& port : card.outputs) (*kinds)[{ card.id, port.id, true }] = port.type;
                }
                // What is here to be moved and deleted: the page's own nodes, not those shown from other pages.
                const auto own = [visible](const std::vector<std::string>& nodes) {
                    std::vector<std::string> kept;
                    for (const auto& id : nodes)
                        if (visible.contains(id)) kept.push_back(id);
                    return kept;
                };
                _view->page = everything ? std::string("*") : page;
                // A texture or a shader dropped on the graph from the Assets panel is the node it makes, there.
                return kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this](const kui::DragData& dragged, const kor::Vec2 local) {
                    const auto* id = dragged.As<std::string>();
                    if (!id) return;
                    const kor::Vec2 at = _view->centre + (local - _view->size * 0.5f) / std::max(_view->zoom, 0.01f);
                    _session->AssetToNode(*id, std::round(at.x), std::round(at.y));
                }), kui::NodeEditor(std::move(shown), kui::NodeEditorOptions {}
                    .SetView(_view)
                    .CanConnect([](const kui::NodePort& from, const kui::NodePort& to) { return WireFits(from.type, to.type); })
                    .OnConnect([this](const kui::GraphWire& wire) { Doc().Connect(wire.from.node, wire.from.port, wire.to.node, wire.to.port); })
                    .OnDisconnect([this](const kui::GraphWire& wire) { Doc().ClearInput(wire.to.node, wire.to.port); })
                    // Built again at once: a frame with the nodes back where they were is a frame of them jumping.
                    .OnMove([this, own, overview](const std::vector<std::string>& nodes, const kor::Vec2 by) {
                        if (overview) Doc().MoveInOverview(own(nodes), by.x, by.y);
                        else Doc().MoveNodes(own(nodes), by.x, by.y);
                        SetState();
                    })
                    .OnDelete([this, own](const std::vector<std::string>& picked, const std::vector<kui::GraphWire>& wires, const std::vector<std::string>&) {
                        const std::vector<std::string> nodes = own(picked);
                        for (const auto& wire : wires) Doc().ClearInput(wire.to.node, wire.to.port);
                        if (!nodes.empty()) Doc().RemoveNodes(nodes);
                        if (std::ranges::find(nodes, _session->selectedNode) != nodes.end()) _session->Select(std::string());
                    })
                    .OnCopy([this, own](const std::vector<std::string>& picked) { _session->CopyNodes(own(picked)); })
                    .OnPaste([this](const kor::Vec2 at) { _session->PasteNodes(std::round(at.x), std::round(at.y)); })
                    .OnDuplicate([this, own](const std::vector<std::string>& picked) { _session->DuplicateNodes(own(picked)); })
                    .OnSelectionChanged([this](const std::vector<std::string>& nodes) { if (!nodes.empty()) _session->Select(nodes.front()); })
                    // A wire let go over nothing asks for what it should go to: a node made there, and wired.
                    .OnWireDropped([this, kinds](const kui::PortRef& from, const bool fromOutput, const kor::Vec2 at) {
                        const auto kind = kinds->find({ from.node, from.port, fromOutput });
                        if (kind == kinds->end()) return;
                        AskForNode({ std::round(fromOutput ? at.x : at.x - 180.f), std::round(at.y - 40.f) }, LooseWire { from, fromOutput, kind->second });
                    })
                    // Pressed twice: gone to where it is made, when it is of another page; its shader opened, when it has one; looked at, when it holds something.
                    .OnActivate([this, visible](const std::string& id) { Activate(id, visible.contains(id)); })
                    .Menu([this, own](const std::vector<std::string>& picked, const kor::Vec2 at) { return picked.empty() ? PageMenu(at) : NodeMenu(picked, own(picked)); })
                    .OnContextMenu([this](const kor::Vec2 at, kor::Vec2) { AskForNode(at); })));
            }

            /** A node pressed twice. */
            void Activate(const std::string& id, const bool ownPage)
            {
                const kgraph::Node* node = Doc().Graph().Find(id);
                if (!node) return;
                if (!ownPage) { _session->GoTo(id); return; }
                if (node->type == "Reference") {
                    if (const kgraph::Node* target = StandsFor(*node); target && target != node) _session->GoTo(target->id);
                    return;
                }
                if (node->type == "Function") { _session->OpenFunction(id); return; }
                if (const auto shader = _session->ShaderOf(id); !shader.empty()) { _session->OpenInCode(shader); return; }
                if (const auto held = Doc().Runtime().ResourceOf(id); held && (Doc().Runtime().ImageOf(id).Get() || held->kind == "Buffer" || held->kind == "BufferView")) {
                    if (_session->IsLooking(id)) _session->Unlook(id); else _session->Look(id);
                }
            }

            static kui::MenuItem MenuLine() { return { {}, {}, true, true }; }

            /** The menu of the graph itself, where nothing is: what can be put there, and the page put in order. */
            std::vector<kui::MenuItem> PageMenu(const kor::Vec2 at)
            {
                return {
                    { "Add node...   Ctrl+Space", [this, at] { AskForNode(at); } },
                    { "Add a note", [this, at] { _session->Select(Doc().AddNode("Note", std::round(at.x), std::round(at.y), {}, ShownPass())); } },
                    { "Paste   Ctrl+V", [this, at] { _session->PasteNodes(std::round(at.x), std::round(at.y)); }, _session->HasCopiedNodes() },
                    MenuLine(),
                    { "Arrange this page", [this] { Arrange(); } },
                    { "Frame everything   F", [this] { _view->Frame(); SetState(); } },
                };
            }

            /** The menu of what is picked: @p picked, of which @p own are this page's to change. */
            std::vector<kui::MenuItem> NodeMenu(const std::vector<std::string>& picked, const std::vector<std::string>& own)
            {
                std::vector<kui::MenuItem> items;
                if (picked.size() == 1) {
                    const std::string id = picked.front();
                    const kgraph::Node* node = Doc().Graph().Find(id);
                    if (!node) return items;
                    if (const auto held = Doc().Runtime().ResourceOf(id); held && (Doc().Runtime().ImageOf(id).Get() || held->kind == "Buffer" || held->kind == "BufferView")) {
                        const bool looking = _session->IsLooking(id);
                        items.push_back({ looking ? "Stop looking at what it holds" : "Look at what it holds", [this, id, looking] { if (looking) _session->Unlook(id); else _session->Look(id); } });
                    }
                    if (const auto held = Doc().Runtime().ResourceOf(id); held && (Doc().Runtime().ImageOf(id).Get() || held->kind == "Buffer" || held->kind == "BufferView" || held->kind == "Mesh"))
                        items.push_back({ "Open in the viewer", [this, id] { _session->View(id); } });
                    if (const auto shader = _session->ShaderOf(id); !shader.empty())
                        items.push_back({ std::format("Open {} in Visual Studio Code", shader.filename().string()), [this, shader] { _session->OpenInCode(shader); } });
                    if (const kgraph::Node* target = StandsFor(*node); own.empty() || (target && target != node)) {
                        const std::string to = own.empty() || !target ? id : target->id;
                        const kgraph::Node* made = Doc().Graph().Find(to);
                        items.push_back({ made && !made->pass.empty() ? std::format("Go to where it is made, in {}", made->pass) : std::string("Go to where it is made"), [this, to] { _session->GoTo(to); } });
                    }
                    if (!items.empty()) items.push_back(MenuLine());
                }
                const bool some = !own.empty();
                // A call: its function opened, to be changed for every call; or written out, its nodes in its place.
                if (picked.size() == 1)
                    if (const kgraph::Node* node = Doc().Graph().Find(picked.front()); node && node->type == "Function" && some) {
                        const std::string id = node->id;
                        items.push_back({ "Open the function", [this, id] { _session->OpenFunction(id); }, !Doc().Opened() });
                        items.push_back({ "Write it out here", [this, id] { _session->WriteOut(id); } });
                        items.push_back(MenuLine());
                    }
                items.push_back({ own.size() > 1 ? std::format("Make a function of {} nodes...", own.size()) : std::string("Make a function of it..."), [this, own] {
                    SetState([&] { _functionOf = own; _newId.clear(); _dialog = Dialog::eNewFunction; });
                }, some && _session->HasProject() });
                items.push_back({ "Copy   Ctrl+C", [this, own] { _session->CopyNodes(own); }, some });
                items.push_back({ "Duplicate   Ctrl+D", [this, own] { _session->DuplicateNodes(own); }, some });
                // To another pass: moved there (what they take of others, and what others take of them, brought in through
                // References), or copied there.
                if (some) {
                    const std::string here = ShownPass();
                    std::vector<kui::MenuItem> moveTo, copyTo;
                    for (const auto& pass : Doc().Passes()) {
                        moveTo.push_back({ pass, [this, own, pass] { _session->MoveNodesToPass(own, pass); }, pass != here });
                        copyTo.push_back({ pass, [this, own, pass] { _session->CopyNodesToPass(own, pass); } });
                    }
                    moveTo.push_back({ "A new pass", [this, own] { _session->MoveNodesToPass(own, {}); } });
                    const std::string what = own.size() == 1 ? std::string("it") : std::format("{} nodes", own.size());
                    items.push_back({ .label = std::format("Move {} to a pass", what), .items = std::move(moveTo) });
                    items.push_back({ .label = std::format("Copy {} to a pass", what), .items = std::move(copyTo) });
                }
                items.push_back({ "Take its wires off", [this, own] {
                    for (const auto& id : own) {
                        const kgraph::Node* node = Doc().Graph().Find(id);
                        if (!node) continue;
                        std::vector<std::string> wired;
                        for (const auto& [name, input] : node->inputs) if (input.link) wired.push_back(name);
                        for (const auto& name : wired) Doc().ClearInput(id, name);
                        for (const auto& output : Doc().Runtime().OutputsOf(id)) Doc().DisconnectOutput(id, output.name);
                    }
                }, some });
                items.push_back(MenuLine());
                items.push_back({ picked.size() > 1 ? std::format("Delete {} nodes   Del", own.size()) : std::string("Delete   Del"), [this, own] {
                    Doc().RemoveNodes(own);
                    if (std::ranges::find(own, _session->selectedNode) != own.end()) _session->Select(std::string());
                }, some });
                return items;
            }

            // ---- the outliner -----------------------------------------------------------------------------------

            /** The picture an entity is shown by, for what it is. */
            static std::pair<const char*, kui::Color> EntityIcon(const std::string& kind)
            {
                if (kind == "camera") return { "videocam", kui::Color::Hex(0x62C3E8) };
                if (kind == "sun") return { "wb_sunny", kui::Color::Hex(0xE8C562) };
                if (kind == "light") return { "lightbulb", kui::Color::Hex(0xE8C562) };
                if (kind == "mesh") return { "view_in_ar", kui::Color::Hex(0x7DBB4B) };
                if (kind == "script") return { "data_object", kui::Color::Hex(0x5B9BD5) };
                if (kind == "group") return { "folder", kui::Color::Hex(0xC9A227) };
                return { "radio_button_unchecked", kui::Color::Hex(0x9AA4B0) };
            }

            /** Something of the Assets panel, by its id: what was dropped is looked up for what kind of thing it is. */
            std::optional<Session::Asset> AssetOf(const std::string& id)
            {
                const auto assets = _session->Assets();
                const auto it = std::ranges::find(assets, id, &Session::Asset::id);
                return it == assets.end() ? std::nullopt : std::optional(*it);
            }

            /** Where on an entity's line something is held, as where it would go: over its top edge, before it; its foot, after it; its middle, under it. */
            static Session::Beside BesideAt(const kor::Vec2 local, const bool openWithChildren)
            {
                constexpr float Line = 26.f;
                if (local.y < 7.f) return Session::Beside::eBefore;
                if (local.y > Line - 7.f && local.y <= Line + 2.f && !openWithChildren) return Session::Beside::eAfter;
                return Session::Beside::eUnder;
            }

            /**
             * An entity in the outliner — what it is, its name, what is wrong with it, whether it is drawn — and under
             * it, further in, everything that is under it. @p flat: by itself, as something found.
             */
            kui::Widget EntityRow(const kecs::Entity entity, const bool flat = false)
            {
                const kui::Theme& t = kui::Theme::Current();
                const auto& children = Doc().World().ChildrenOf(entity);
                const bool open = !flat && !_session->Folded(entity);
                std::vector<kui::Widget> under;
                if (open)
                    for (const kecs::Entity child : children) under.push_back(EntityRow(child));
                const std::string name = Doc().NameOf(entity);
                const std::string key = std::format("entity {}.{}", entity.index, entity.generation);
                const bool picked = entity == _session->selectedEntity;
                const bool can = _session->CanShow(entity), shown = !can || _session->IsShown(entity);
                const auto [icon, tint] = EntityIcon(_session->EntityKind(entity));

                std::vector<kui::Widget> line;
                line.push_back(kui::SizedBox(15.f, 15.f, kui::Icon(icon, kui::IconStyle::eFilled, shown ? tint : tint.WithAlpha(0.45f))));
                if (_renaming == entity) {
                    line.push_back(kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = name,
                        .onSubmitted = [this, entity](const std::string& typed) { if (!typed.empty()) Doc().Rename(entity, typed); SetState([&] { _renaming = kecs::NullEntity; }); }, .focus = 1 }).Key("rename " + key)));
                } else {
                    line.push_back(kui::Expanded(kui::Text(name, kui::TextStyle {}.SetSize(13.f).SetColor(picked ? t.primary : shown ? t.text : t.textMuted), kui::TextAlign::eStart, false, 1, true)));
                }
                // Shut, it says how many are under it; one a pass could not run for says so, and why to the pointer.
                if (!flat && !open && !children.empty()) line.push_back(kgui::Muted(std::to_string(children.size()), 11.f));
                // A prefab's instance: which prefab, and whether anything of it is its own.
                if (const std::string prefab = Doc().PrefabOf(entity); !prefab.empty()) {
                    const std::size_t own = Doc().OverridesOf(entity).size();
                    line.push_back(kui::Tooltip(std::format("An instance of {}{}", std::filesystem::path(prefab).stem().string(), own ? std::format(", {} of it its own", own) : std::string()),
                                                kui::SizedBox(13.f, 13.f, kui::Icon("widgets", kui::IconStyle::eFilled, kui::Color::Hex(0x4FB3BF).WithAlpha(own ? 1.f : 0.7f)))));
                }
                if (flat)
                    if (const kecs::Entity parent = Doc().World().ParentOf(entity); !parent.IsNull()) line.push_back(kgui::Muted("in " + Doc().NameOf(parent), 11.f));
                if (const auto passed = _passedBy.find(entity); passed != _passedBy.end()) {
                    std::string why;
                    for (const auto& each : passed->second) why += (why.empty() ? "Not drawn by " : "\n") + each;
                    line.push_back(kui::Tooltip(why, kui::SizedBox(14.f, 14.f, kui::Icon("warning", kui::IconStyle::eFilled, kui::Color::Hex(0xE5484D)))));
                }
                if (can)
                    line.push_back(kui::Tooltip(shown ? "Drawn. Pressed: it and what is under it are not." : "Not drawn. Pressed: it and what is under it are.",
                        kui::Button(kui::SizedBox(14.f, 14.f, kui::Icon(shown ? "visibility" : "visibility_off", kui::IconStyle::eFilled, shown ? t.textMuted : t.textMuted.WithAlpha(0.6f))),
                                    [this, entity, shown] { _session->SetShown(entity, !shown); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain))));
                kui::Widget title = kui::Row(std::move(line), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));

                kui::Widget node = kui::TreeNode({}, open, [this, entity](const bool nowOpen) { _session->Fold(entity, !nowOpen); }, std::move(under),
                                                 kui::TreeNodeOptions {}.SetLeaf(flat || children.empty()).SetSelected(picked).SetTitle(std::move(title))
                                                     // Pressed, it is picked; pressed again at once, its name is there to be typed over.
                                                     .OnTap([this, entity, key] { Pressed(key, [&] { _session->Select(entity); }, [&] { SetState([&] { _renaming = entity; }); }); }));
                // Where what is held over it would go: a line over it or under it, or all of it lit.
                if (_dropOn == entity) {
                    const kui::Widget mark = _dropWhere == Session::Beside::eUnder
                        ? kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = 0.f, .right = 0.f, .height = 26.f }, kui::IgnorePointer(kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary.WithAlpha(0.22f)).SetRadius(4.f)))))
                        : kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = _dropWhere == Session::Beside::eBefore ? -1.f : 25.f, .right = 0.f, .height = 2.f },
                                          kui::IgnorePointer(kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary)))));
                    node = kui::Stack({ std::move(node), mark });
                }
                // Picked up and dropped on another, it goes before it, after it or under it: where it is in the world
                // stays as it was. And what is dropped on it from the Assets panel — a mesh, a texture, a script — is its.
                node = kui::Draggable({ "entity", entity }, std::move(node));
                const bool openWithChildren = open && !children.empty();
                node = kui::DropTarget(kui::DropTargetOptions {}
                    .Accepts([entity](const kui::DragData& dragged) {
                        if (const auto* moved = dragged.As<kecs::Entity>()) return dragged.type == "entity" && *moved != entity;
                        return dragged.type == "asset";
                    })
                    .OnMove([this, entity, openWithChildren, flat](const kui::DragData& dragged, const kor::Vec2 local) {
                        const Session::Beside where = dragged.type == "entity" && !flat ? BesideAt(local, openWithChildren) : Session::Beside::eUnder;
                        if (_dropOn != entity || _dropWhere != where) SetState([&] { _dropOn = entity; _dropWhere = where; });
                    })
                    .OnLeave([this, entity] { if (_dropOn == entity) SetState([&] { _dropOn = kecs::NullEntity; }); })
                    .OnDrop([this, entity, openWithChildren, flat](const kui::DragData& dragged, const kor::Vec2 local) {
                        _dropOn = kecs::NullEntity;
                        if (const auto* moved = dragged.As<kecs::Entity>()) {
                            _session->MoveEntity(*moved, entity, flat ? Session::Beside::eUnder : BesideAt(local, openWithChildren));
                            if (!flat) _session->Fold(entity, false);
                        } else if (const auto* asset = dragged.As<std::string>()) {
                            if (_session->GiveAsset(*asset, entity) && !asset->starts_with("prefab:")) _session->Select(entity);
                        }
                        SetState();
                    }), std::move(node));
                std::vector<kui::MenuItem> menu;
                menu.push_back({ "Rename", [this, entity] { _session->Select(entity); SetState([&] { _renaming = entity; }); } });
                menu.push_back({ "Frame in the scene view", [this, entity] { _session->Select(entity); _session->Frame(entity); } });
                menu.push_back({ "Duplicate", [this, entity] { _session->Duplicate(entity); } });
                if (can) menu.push_back({ shown ? "Hide, with what is under it" : "Show, with what is under it", [this, entity, shown] { _session->SetShown(entity, !shown); } });
                if (!Doc().World().ParentOf(entity).IsNull()) menu.push_back({ "Move to the top", [this, entity] { Doc().SetParent(entity, kecs::NullEntity); } });
                menu.push_back({ "Save as object", [this, entity] { _session->SaveAsObject(entity); } });
                menu.push_back({ "Save as prefab", [this, entity] { _session->SaveAsPrefab(entity); } });
                menu.push_back({ .separator = true });
                menu.push_back({ "New entity under it", [this, entity] { _session->Fold(entity, false); _session->Select(Doc().CreateEntity("Entity", entity)); } });
                for (const auto& object : _session->Objects())
                    menu.push_back({ std::format("New {} under it", object.name), [this, object, entity] { _session->Create(object, entity); } });
                menu.push_back({ .separator = true });
                menu.push_back({ "Delete, with what is under it", [this, entity] { _session->Delete(entity); } });
                return kui::ContextMenu(std::move(menu), std::move(node)).Key(key);
            }

            std::map<kecs::Entity, std::vector<std::string>> _passedBy;     // which entities a pass could not run for, and why: as of this build

            /**
             * What there is to make an entity of, as a menu: an empty one; the meshes, the lights and the cameras each in a
             * submenu of their own — by what each holds — and the rest as they are. Made under @p parent, when it is one.
             */
            std::vector<kui::MenuItem> ObjectMenu(const kecs::Entity parent)
            {
                std::vector<kui::MenuItem> meshes, lights, cameras, others;
                std::vector<kui::MenuItem> menu;
                for (const auto& object : _session->Objects()) {
                    kui::MenuItem item { object.name, [this, object, parent] { _session->Create(object, parent); } };
                    if (object.name == "Empty") {
                        item.label = "Empty entity";
                        if (parent.IsNull()) item.onSelected = [this] { _session->Select(Doc().CreateEntity()); };     // called what any new entity is
                        menu.insert(menu.begin(), std::move(item));
                        continue;
                    }
                    const Value* components = object.data.Find("components");
                    const auto has = [&](const auto& test) {
                        if (components && components->Named())
                            for (const auto& name : *components->Named() | std::views::keys) if (test(name)) return true;
                        return false;
                    };
                    if (has([](const std::string& name) { return name == "Camera"; })) cameras.push_back(std::move(item));
                    else if (has([](const std::string& name) { return name.ends_with("Light"); })) lights.push_back(std::move(item));
                    else if (has([](const std::string& name) { return name == "Mesh"; })) meshes.push_back(std::move(item));
                    else others.push_back(std::move(item));
                }
                if (!meshes.empty()) menu.push_back({ .label = "Mesh", .items = std::move(meshes) });
                if (!lights.empty()) menu.push_back({ .label = "Light", .items = std::move(lights) });
                if (!cameras.empty()) menu.push_back({ .label = "Camera", .items = std::move(cameras) });
                for (auto& item : others) menu.push_back(std::move(item));
                return menu;
            }

            /**
             * The world's entities as a tree: each by what it is and its name; picked by pressing, renamed by pressing
             * twice, folded by their arrows; moved before, after or under one another by dragging; shown and hidden by
             * their eyes. Typed into, the line over it finds them by name, or by a component they have.
             */
            kui::Widget Outliner()
            {
                _passedBy.clear();
                for (auto& [entity, why] : Doc().Runtime().PassedBy()) _passedBy[entity].push_back(std::move(why));
                kui::Widget bar = kui::Row({
                    kui::Tooltip("Makes an entity: an empty one, or one of the objects there are — meshes, lights and cameras each in a list of their own.",
                        kui::MenuBar({ { "+", ObjectMenu(kecs::NullEntity) } })),
                    kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = _outlinerFilter, .placeholder = "Find: a name, or a component",
                        .onChanged = [this](const std::string& typed) { SetState([&] { _outlinerFilter = typed; }); } }).Key("outliner filter")),
                }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                std::vector<kui::Widget> rows;
                if (_outlinerFilter.empty()) {
                    for (const kecs::Entity entity : Doc().World().Roots()) rows.push_back(EntityRow(entity));
                    if (rows.empty()) rows.push_back(kgui::Muted("No entities yet."));
                } else {
                    const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                    const std::string wanted = lower(_outlinerFilter);
                    for (const kecs::Entity entity : Doc().Entities()) {
                        bool found = lower(Doc().NameOf(entity)).find(wanted) != std::string::npos;
                        for (const auto& component : Doc().ComponentsOf(entity)) found = found || lower(component).find(wanted) != std::string::npos;
                        if (found) rows.push_back(EntityRow(entity, true));
                    }
                    if (rows.empty()) rows.push_back(kgui::Muted(std::format("No entity is called '{}', or has such a component.", _outlinerFilter)));
                }
                // Dropped on the panel itself, past the tree, an entity goes to the top — and a model is imported.
                kui::Widget tree = kui::DropTarget(kui::DropTargetOptions {}
                    .Accepts([](const kui::DragData& dragged) { return dragged.type == "entity" || dragged.type == "asset"; })
                    .OnDrop([this](const kui::DragData& dragged, kor::Vec2) {
                        if (const auto* moved = dragged.As<kecs::Entity>()) { Doc().SetParent(*moved, kecs::NullEntity); return; }
                        const auto* id = dragged.As<std::string>();
                        const auto asset = id ? AssetOf(*id) : std::nullopt;
                        if (asset && asset->kind == Session::Asset::Kind::eModel) AskToImport(asset->file);
                        else if (asset && asset->kind == Session::Asset::Kind::ePrefab) _session->PlacePrefab(asset->id);
                        else _session->Say("Dropped on an entity, a mesh, a texture, a script or a prefab is its; dropped here, a model is imported and a prefab placed.", true);
                    }), kui::ScrollView(kui::Padding(kui::EdgeInsets { 8.f, 4.f, 8.f, 8.f }, Rows(std::move(rows), 2.f))));
                return kui::Column({ kui::Padding(kui::EdgeInsets { 8.f, 6.f, 8.f, 4.f }, std::move(bar)), kui::Separator(), kui::Expanded(std::move(tree)) },
                                   kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
            }

            // ---- the inspector ----------------------------------------------------------------------------------

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
                default: return "Scripts";
                }
            }

            /** What an asset is shown by: a texture that is loaded by itself; anything else by what kind of thing it is. */
            kui::Widget AssetPreview(const Session::Asset& asset, const float size, const bool pictured = true)
            {
                if (!pictured) {}
                else if (asset.kind == Session::Asset::Kind::eTexture) {
                    if (const auto held = _session->AssetHeld(asset.id); held && held->handle)
                        return kui::SizedBox(size, size, kui::Image(kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(held->handle)), kui::ImageFit::eContain));
                }
                else if (asset.kind == Session::Asset::Kind::eMesh) {
                    // A mesh's small picture, once it is drawn; by its kind until then.
                    if (const auto picture = _session->MeshPicture(asset.id); picture.Get()) return kui::SizedBox(size, size, kui::Image(picture, kui::ImageFit::eContain));
                }
                const char* icon = asset.kind == Session::Asset::Kind::eModel ? "category" : asset.kind == Session::Asset::Kind::eMesh ? "view_in_ar"
                                 : asset.kind == Session::Asset::Kind::eTexture ? "image" : asset.kind == Session::Asset::Kind::eShader ? "code"
                                 : asset.kind == Session::Asset::Kind::eScene ? "landscape" : asset.kind == Session::Asset::Kind::ePrefab ? "widgets" : "description";
                const kui::Color tint = asset.kind == Session::Asset::Kind::eShader ? kui::Color::Hex(0x9C6ADE) : asset.kind == Session::Asset::Kind::eScript ? kui::Color::Hex(0x5B9BD5)
                                      : asset.kind == Session::Asset::Kind::eModel ? kui::Color::Hex(0x7DBB4B) : asset.kind == Session::Asset::Kind::eMesh ? kui::Color::Hex(0x6FA87A)
                                      : asset.kind == Session::Asset::Kind::eScene ? kui::Color::Hex(0xD9A441) : asset.kind == Session::Asset::Kind::ePrefab ? kui::Color::Hex(0x4FB3BF)
                                      : kui::Theme::Current().textMuted;
                return kui::SizedBox(size, size, kui::Icon(icon, kui::IconStyle::eFilled, asset.loaded || asset.kind >= Session::Asset::Kind::eShader ? tint : tint.WithAlpha(0.55f)));
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
                SetState([&] { _session->assetFolder = std::move(folder); _session->assetFilter.clear(); _session->assetKind = -1; _folderPicked.clear(); });
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
                             std::function<void()> pressed, std::vector<kui::MenuItem> menu)
            {
                const kui::Theme& t = kui::Theme::Current();
                const kui::Color ink = picked ? t.text : faint ? t.textMuted : t.text.WithAlpha(0.9f);
                const kui::Decoration back = kui::Decoration {}.SetColor(picked ? t.primary.WithAlpha(0.32f) : kui::colors::Transparent).SetRadius(4.f);
                kui::Widget face;
                if (size <= ListSize) {
                    face = kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(6.f, 2.f)).SetDecoration(back),
                        kui::Row({ std::move(preview), kui::SizedBox(260.f, -1.f, kui::Text(label, kui::TextStyle {}.SetSize(13.f).SetColor(ink), kui::TextAlign::eStart, false, 1, true)) },
                                 kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                } else {
                    face = kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::All(4.f)).SetDecoration(back),
                        kui::Column({ std::move(preview), kui::SizedBox(size + 8.f, 28.f, kui::Text(label, kui::TextStyle {}.SetSize(11.f).SetColor(ink), kui::TextAlign::eCenter, true, 2, true)) },
                                    kui::FlexOptions {}.SetGap(3.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                }
                kui::Widget tile = kui::Button(std::move(face), std::move(pressed), kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain));
                if (!menu.empty()) tile = kui::ContextMenu(std::move(menu), std::move(tile));
                return std::move(tile).Key(key);
            }

            /** What the right button offers of a file or a folder that is on disk. */
            std::vector<kui::MenuItem> FileMenu(const std::filesystem::path& path, const bool ofProject)
            {
                return {
                    { "Show in the file manager", [this, path] { _session->ShowInFiles(path); } },
                    { "Copy its path", [this, path] { kor::Window::SetClipboardText(path.generic_string()); _session->Say(std::format("Copied {}.", path.generic_string())); } },
                    { {}, {}, true, true },
                    { "Move to the trash...", [this, path] { SetState([&] { _dialog = Dialog::eTrash; _trash = path; }); }, ofProject },
                };
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
                    { "Import model...", [this] { SetState([&] { _dialog = Dialog::eImportModel; }); } },
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
                    const bool pictured = (asset.kind == Kind::eTexture || asset.kind == Kind::eMesh) && pictures < 160;
                    if (pictured) ++pictures;
                    const bool code = asset.kind == Kind::eShader || asset.kind == Kind::eScript;
                    const bool ofProject = !asset.folder.starts_with(Session::OutsideFolder);
                    std::string label = asset.label;
                    if (asset.pending) label += " (reading)";
                    std::vector<kui::MenuItem> menu;
                    if (code) menu.push_back({ "Open in Visual Studio Code", [this, file = asset.file] { _session->OpenInCode(file); } });
                    if (asset.kind == Kind::eModel) {
                        menu.push_back({ "Import into the scene...", [this, file = asset.file] { AskToImport(file); } });
                        menu.push_back({ _session->assetsOpened.contains(asset.id) ? "Hide its meshes" : "Show its meshes", [this, asset] { _session->OpenAsset(asset); }, asset.loaded });
                    }
                    if (asset.kind != Kind::eMesh) for (auto& item : FileMenu(asset.file, ofProject)) menu.push_back(std::move(item));
                    kui::Widget tile = Tile("asset " + asset.id, AssetPreview(asset, preview, pictured), label, asset.id == _session->selectedAsset, !asset.loaded && !code && asset.kind != Kind::eScene, size,
                                            [this, asset] { Pressed("asset " + asset.id, [&] { _folderPicked.clear(); _session->SelectAsset(asset.id); }, [&] { _session->OpenAsset(asset); }); }, std::move(menu));
                    // A model that is in the scene has its meshes: an arrow on it shows them after it, and hides them.
                    if (asset.kind == Kind::eModel && asset.loaded && !searching) {
                        const bool open = _session->assetsOpened.contains(asset.id);
                        tile = kui::Stack({ std::move(tile), kui::Positioned(kui::PositionedOptions { .top = size <= ListSize ? 0.f : size * 0.5f - 8.f, .right = 0.f },
                                                                             Tool(open ? "chevron_left" : "chevron_right", open ? "Hide its meshes" : "Show its meshes", open, [this, asset] { _session->OpenAsset(asset); })) });
                    }
                    // A mesh or a texture is carried to the field that is to hold it, by the name it is found by.
                    // (And a model to the scene or the outliner, to be imported; a script to an entity; a texture or a shader to the graph.)
                    if (asset.kind != Kind::eScene) tile = kui::Draggable({ "asset", asset.id }, std::move(tile));
                    tiles.push_back(std::move(tile));
                };
                if (searching) {
                    for (const auto& asset : assets) {
                        if (_session->assetKind >= 0 && static_cast<int>(asset.kind) != _session->assetKind) continue;
                        // A mesh is found by what is typed, or asked for by its kind: not listed with everything else.
                        if (asset.kind == Kind::eMesh && wanted.empty() && _session->assetKind != static_cast<int>(Kind::eMesh)) continue;
                        if (!wanted.empty() && lower(asset.label).find(wanted) == std::string::npos && lower(asset.group).find(wanted) == std::string::npos) continue;
                        tileOf(asset);
                    }
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
                    std::ranges::stable_sort(assets, [&](const Session::Asset& a, const Session::Asset& b) { return lower(a.label) < lower(b.label); });
                    for (const auto& asset : assets) {
                        if (asset.folder != shown || asset.kind == Kind::eMesh) continue;
                        tileOf(asset);
                        if (asset.kind == Kind::eModel && _session->assetsOpened.contains(asset.id))
                            for (const auto& mesh : assets)
                                if (mesh.kind == Kind::eMesh && mesh.parent == asset.id) tileOf(mesh);
                    }
                }
                kui::Widget content = tiles.empty()
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
                    const char* kinds[] { "models", "meshes", "textures", "shaders", "scripts", "scenes" };
                    const std::string what = _session->assetKind >= 0 ? std::string("All ") + kinds[std::clamp(_session->assetKind, 0, 5)] : std::string("Everything");
                    crumbs.push_back(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kgui::Muted(wanted.empty() ? std::format("{}, in every folder", what) : std::format("{} called '{}', in every folder", what, _session->assetFilter), 12.f)));
                } else {
                    crumb(root, {}, shown.empty());
                    for (std::size_t at = 0; at < shown.size();) {
                        const auto slash = shown.find('/', at);
                        const std::size_t end = slash == std::string::npos ? shown.size() : slash;
                        crumb(shown.substr(at, end - at), shown.substr(0, end), end == shown.size());
                        at = end + 1;
                    }
                }

                // ---- the folders, down the side
                const std::function<kui::Widget(const std::string&)> branch = [&](const std::string& folder) -> kui::Widget {
                    std::vector<kui::Widget> under;
                    const bool open = !_foldersShut.contains(folder);
                    bool any = false;
                    for (const auto& other : folders) {
                        const auto slash = other.rfind('/');
                        if ((slash == std::string::npos ? std::string() : other.substr(0, slash)) != folder) continue;
                        any = true;
                        if (open) under.push_back(branch(other));
                    }
                    const bool here = !searching && folder == shown;
                    kui::Widget title = kui::Row({ kui::SizedBox(14.f, 14.f, kui::Icon(open && any ? "folder_open" : "folder", kui::IconStyle::eFilled, kui::Color::Hex(0xC9A227))),
                                                   kui::Expanded(kui::Text(folder.empty() ? root : FolderName(folder), kui::TextStyle {}.SetSize(13.f).SetColor(here ? t.primary : t.text), kui::TextAlign::eStart, false, 1, true)) },
                                                 kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                    return kui::TreeNode({}, open, [this, folder](const bool nowOpen) { SetState([&] { if (nowOpen) _foldersShut.erase(folder); else _foldersShut.insert(folder); }); }, std::move(under),
                                         kui::TreeNodeOptions {}.SetLeaf(!any).SetSelected(here).SetIndent(14.f).OnTap([this, folder] { ShowFolder(folder); }).SetTitle(std::move(title)));
                };
                // The project's folders first; under them, everything of one kind, wherever it is.
                std::vector<kui::Widget> side;
                side.push_back(branch({}));
                std::vector<kui::Widget> kinds;
                const std::pair<const char*, Kind> every[] { { "All models", Kind::eModel }, { "All meshes", Kind::eMesh }, { "All textures", Kind::eTexture },
                                                             { "All shaders", Kind::eShader }, { "All scripts", Kind::eScript }, { "All prefabs", Kind::ePrefab } };
                for (const auto& [label, kind] : every) {
                    const bool on = _session->assetKind == static_cast<int>(kind);
                    kinds.push_back(kui::TreeNode(label, false, [](bool) {}, {}, kui::TreeNodeOptions {}.SetLeaf(true).SetSelected(on).SetIndent(14.f).OnTap([this, kind, on] {
                        SetState([&] { _session->assetKind = on ? -1 : static_cast<int>(kind); });
                    })));
                }
                side.push_back(kui::TreeNode("Everything, by kind", !_foldersShut.contains("*kinds"), [this](const bool open) { SetState([&] { if (open) _foldersShut.erase("*kinds"); else _foldersShut.insert("*kinds"); }); },
                                             std::move(kinds), kui::TreeNodeOptions {}.SetIndent(14.f)));

                // ---- over it all: something made, something looked for; under it: what is picked, and how big things are shown
                const std::vector<std::string> made { "Folder...", "Shader...", "Script...", "Import model..." };
                const std::vector<std::string> kindNames { "Every kind", "Models", "Meshes", "Textures", "Shaders", "Scripts", "Scenes", "Prefabs" };
                kui::Widget bar = kui::Row({
                    kui::Tooltip("Makes something in the project: a folder here, a shader, a script; or brings a model's file in.",
                        kui::Dropdown(made, -1, [this](const int index) {
                            SetState([&] {
                                _newId.clear();
                                _newKind = 0;
                                _dialog = index == 0 ? Dialog::eNewFolder : index == 1 ? Dialog::eNewShader : index == 2 ? Dialog::eNewScript : Dialog::eImportModel;
                            });
                        }, kui::DropdownOptions {}.SetPlaceholder("+  Create").SetWidth(120.f))),
                    kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = _session->assetFilter, .placeholder = "Search: part of a name, in every folder",
                        .onChanged = [this](const std::string& typed) { SetState([&] { _session->assetFilter = typed; }); } }).Key("asset filter")),
                    kui::Tooltip("Shows only one kind of thing, wherever it is.",
                        kui::Dropdown(kindNames, _session->assetKind + 1, [this](const int index) { SetState([&] { _session->assetKind = index - 1; }); }, kui::DropdownOptions {}.SetWidth(130.f))),
                }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));

                std::string picked;
                if (const auto it = std::ranges::find(assets, _session->selectedAsset, &Session::Asset::id); it != assets.end() && !_session->selectedAsset.empty())
                    picked = it->kind == Kind::eMesh ? std::format("{}, of {}", it->label, it->file.generic_string()) : it->file.generic_string();
                else if (!_folderPicked.empty()) picked = _folderPicked;
                kui::Widget foot = kui::Row({
                    kui::Expanded(kui::Text(picked, kui::TextStyle {}.SetSize(11.f).SetColor(t.textMuted), kui::TextAlign::eStart, false, 1, true)),
                    kui::Tooltip("How big each thing is shown. At its least, a line each.",
                        kui::SizedBox(110.f, -1.f, kui::Slider(size, [this](const float value) { SetState([&] { _session->assetTileSize = value; }); }, ListSize, 128.f))),
                }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));

                return kui::Column({
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
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
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

            /** Asked before a file or a folder of the project's goes to the trash. */
            kui::Widget TrashDialog()
            {
                const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; }); };
                std::error_code ignored;
                const bool folder = std::filesystem::is_directory(_trash, ignored);
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading(std::format("Move {} to the trash?", _trash.filename().string())),
                    kgui::Line(folder ? "The folder and everything in it goes to the system's trash, and can be had back from there."
                                      : "It goes to the system's trash, and can be had back from there."),
                    kgui::Muted("Whatever of the scene is given it — a pipeline its shader, an entity its picture — says so among the problems until it is given something else."),
                    kui::Row({ kui::Button("Move it to the trash", [this, close] { const auto path = _trash; close(); _session->TrashAsset(path); }),
                               kui::Button("Cancel", close, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) }, kui::FlexOptions {}.SetGap(8.f)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(10.f)).Width(520.f));
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
                    Small("All in one buffer", [changed, all = split([](const std::string&) { return 0; })] { changed(all); }),
                    Small("Positions and UVs · surface · bones", [changed, three = split([&](const std::string& s) { return bone(s) ? 2 : surface(s) ? 1 : s == "POSITION" || s.starts_with("UV") ? 0 : 1; })] {
                        changed(three);
                    }),
                    Small("Positions apart", [changed, apart = split([](const std::string& s) { return s == "POSITION" ? 0 : 1; })] { changed(apart); }),
                };
                if (!choice.leaveOut.empty()) presets.push_back(Small("Read everything", [changed, choice = AttributeChoice(choice)]() mutable { choice.leaveOut.clear(); changed(choice); }));
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
                    kui::Button("Cancel", close, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)),
                }, kui::FlexOptions {}.SetGap(8.f)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::ScrollView(Rows(std::move(rows), 8.f)).Width(620.f));
            }

            kui::Widget AssetInspector(const Session::Asset& asset)
            {
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(asset.label));
                rows.push_back(kgui::Muted(asset.kind == Session::Asset::Kind::eModel ? "Model" : asset.kind == Session::Asset::Kind::eMesh ? "Mesh"
                                           : asset.kind == Session::Asset::Kind::eTexture ? "Texture" : asset.kind == Session::Asset::Kind::eShader ? "Shader"
                                           : asset.kind == Session::Asset::Kind::eScene ? "Scene" : asset.kind == Session::Asset::Kind::ePrefab ? "Prefab" : "Script"));
                rows.push_back(kgui::Labeled("File", kgui::Muted(asset.file.generic_string()), LabelWidth));
                const auto held = _session->AssetHeld(asset.id);
                switch (asset.kind) {
                case Session::Asset::Kind::eTexture:
                    if (asset.pending) rows.push_back(kgui::Muted("Still being read."));
                    else if (held && held->handle) {
                        const auto image = kor::capi::ImageOf(held->handle);
                        const auto extent = image->Extent();
                        rows.push_back(kgui::Labeled("Size", kgui::Line(std::format("{} x {}", extent.x, extent.y)), LabelWidth));
                        rows.push_back(kgui::Labeled("Levels", kgui::Line(std::to_string(image->MipLevels())), LabelWidth));
                        rows.push_back(kui::Image(kor::ResourceRef<const kor::Image>(image), kui::ImageFit::eContain, { -1.f, 220.f }));
                        rows.push_back(Small("Open in the viewer", [this, id = asset.id] { _session->View(id); }));
                    } else rows.push_back(kgui::Muted("Not in the scene: it is read when something is given it."));
                    rows.push_back(kgui::Labeled("Held as", kgui::Line(asset.id), LabelWidth));
                    rows.push_back(kgui::Muted("Drag it from the Assets panel onto a picture field of an entity: an albedoMap, a normalMap."));
                    break;
                case Session::Asset::Kind::eMesh:
                    if (held && held->part) {
                        rows.push_back(kgui::Labeled("Triangles", kgui::Line(std::to_string(held->indexCount / 3)), LabelWidth));
                        rows.push_back(kgui::Labeled("Reaches", kgui::Line(std::format("{:.2f}, {:.2f}, {:.2f}  to  {:.2f}, {:.2f}, {:.2f}", held->bounds[0], held->bounds[1], held->bounds[2],
                                                                                       held->bounds[3], held->bounds[4], held->bounds[5])), LabelWidth));
                    } else rows.push_back(kgui::Muted(asset.pending ? "Still being read." : "A mesh."));
                    if (const auto picture = _session->MeshPicture(asset.id); picture.Get()) rows.push_back(kui::Image(picture, kui::ImageFit::eContain, { -1.f, 160.f }));
                    if (!asset.pending) rows.push_back(Small("Open in the viewer", [this, id = asset.id] { _session->View(id); }));
                    rows.push_back(kgui::Labeled("Held as", kgui::Line(asset.id), LabelWidth));
                    rows.push_back(kgui::Muted("Drag it from the Assets panel onto an entity's Mesh."));
                    break;
                case Session::Asset::Kind::eModel:
                    rows.push_back(kgui::Muted("A model: imported, each thing in it is an entity with its mesh and its material's pictures."));
                    rows.push_back(Small("Import into the scene...", [this, file = asset.file] { AskToImport(file); }));
                    for (auto& row : MeshImportRows(asset)) rows.push_back(std::move(row));
                    break;
                case Session::Asset::Kind::eShader: {
                    const auto entries = _session->EntryPoints(asset.file.stem().string());
                    std::string list;
                    for (const auto& entry : entries) list += (list.empty() ? "" : ", ") + entry;
                    rows.push_back(kgui::Labeled("Module", kgui::Line(asset.file.stem().string()), LabelWidth));
                    rows.push_back(kgui::Labeled("Entry points", kgui::Line(list.empty() ? std::string("none: a module others import") : list), LabelWidth));
                    rows.push_back(Small("Open in Visual Studio Code", [this, file = asset.file] { _session->OpenInCode(file); }));
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
                    rows.push_back(Small("Make an instance", [this, id = asset.id] { _session->PlacePrefab(id); }));
                    break;
                }
                default:
                    rows.push_back(kgui::Muted(asset.file.extension() == ".kt" ? "A Kotlin script: a component of its class's name, once the project runs with its scripts."
                                                                              : "A C# script: a component of its class's name, once the project runs with its scripts."));
                    rows.push_back(Small("Open in Visual Studio Code", [this, file = asset.file] { _session->OpenInCode(file); }));
                    break;
                }
                return Panel(std::move(rows));
            }

            kui::Widget Inspector()
            {
                if (!_session->selectedAsset.empty()) {
                    const auto assets = _session->Assets();
                    if (const auto it = std::ranges::find(assets, _session->selectedAsset, &Session::Asset::id); it != assets.end()) return AssetInspector(*it);
                }
                if (Doc().World().Alive(_session->selectedEntity)) return EntityInspector(_session->selectedEntity);
                if (const kgraph::Node* node = Doc().Graph().Find(_session->selectedNode)) return NodeInspector(*node);
                return Panel({ kgui::Muted("Pick a node or an entity to see what it is made of.") });
            }

            /**
             * An input's control on its node: small, a row high — a number to drag, a box to tick, a colour, a choice, a
             * few numbers side by side. What it is set to is the node's at once.
             */
            kui::Widget NodeControl(const kgraph::Node& node, const kgraph::Port& port, const Value& value)
            {
                const std::string id = node.id, name = port.name;
                const auto set = [this, id, name](Value changed) { Doc().SetInput(id, name, std::move(changed)); };
                const bool whole = (port.builder && (port.builder->kind == kb::Kind::eInt || port.builder->kind == kb::Kind::eUInt))
                    || name == "count" || name == "index" || name == "face" || name == "which";
                const auto drag = [&](const float number, std::function<void(float)> changed, const float width) {
                    return kui::DragValue(number, std::move(changed), kui::DragValueOptions {}.SetSpeed(whole ? 0.1f : 0.01f).SetDecimals(whole ? 0 : 2).SetWidth(width));
                };
                if (!port.choices.empty()) {
                    const std::string* chosen = value.String();
                    const auto at = chosen ? std::ranges::find(port.choices, *chosen) : port.choices.end();
                    // In a box of its own size: what a node's width is worked out from.
                    return kui::SizedBox(118.f, 22.f, kui::Dropdown(port.choices, at == port.choices.end() ? -1 : static_cast<int>(at - port.choices.begin()),
                                                                    [set, choices = port.choices](const int index) { set(Value(choices[static_cast<std::size_t>(index)])); },
                                                                    kui::DropdownOptions {}.SetWidth(118.f)));
                }
                if (port.editor == "color" || (port.type == "vector" && NamesAColor(name))) {
                    const auto* items = value.Items();
                    const bool alpha = !items || items->size() != 3;
                    return kui::SizedBox(40.f, 18.f, ColorOf(value, set, alpha));
                }
                if (const bool* flag = value.Bool(); flag || port.type == "bool")
                    return kui::Checkbox(flag && *flag, [set](const bool on) { set(Value(on)); });
                if (const std::string* text = value.String(); text || port.type == "string")
                    return kui::TextField(kui::TextFieldOptions { .text = text ? *text : std::string(), .onSubmitted = [set](const std::string& typed) { set(Value(typed)); },
                                                                  .width = 112.f }).Key(std::format("node {} {}", id, name));
                if (const auto* items = value.Items(); items && !items->empty() && items->size() <= 4) {
                    std::vector<kui::Widget> numbers;
                    for (std::size_t i = 0; i < items->size(); ++i)
                        numbers.push_back(drag(NumberOf((*items)[i]), [set, list = *items, i](const float changed) {
                            auto now = list;
                            now[i] = Value(changed);
                            set(Value(std::move(now)));
                        }, 44.f));
                    return kui::Row(std::move(numbers), kui::FlexOptions {}.SetGap(2.f));
                }
                // A number, or nothing yet: a number.
                return drag(value.Number() ? static_cast<float>(*value.Number()) : 0.f, [set, whole](const float changed) { set(Value(whole ? std::round(changed) : changed)); }, 64.f);
            }

            /** The control an input is set with: by what kind of thing it is, then by its type. */
            kui::Widget EditorFor(const kgraph::Node& node, const kgraph::Port& port, const Value& value, const std::function<void(Value)>& set, const std::string& key)
            {
                if (!port.choices.empty()) return Choice(port.choices, value, set, port.choices.front());
                if (port.editor == "color") return ColorOf(value, set);
                if (port.editor == "function") {
                    const auto functions = Doc().Functions();
                    return functions.empty() ? TextEdit(value, set, key, "a function's name") : Choice(functions, value, set, "a function");
                }
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
                    // The nodes it can be: for what stands for another pass's node, what the passes before this one
                    // made; for a loop's `of`, the nodes of a loop; for its `first`, what is not of one.
                    std::vector<std::string> ids;
                    if (node.type == "Reference") ids = Doc().Importable(node.pass);
                    else
                        for (const auto& other : Doc().Graph().nodes) {
                            if (other.id == node.id || other.type == "Reference" || !Doc().Runtime().ResourceOf(other.id)) continue;
                            const bool looped = !Doc().Runtime().TurnsOf(other.id).empty();
                            if ((port.name == "of" && !looped) || (port.name == "first" && looped)) continue;
                            ids.push_back(other.id);
                        }
                    return Choice(std::move(ids), value, set, ids.empty() ? "no node it can be" : "a node");
                }
                if (port.name == "format" && node.type == "Image" && port.builder && port.builder->enumeration) {
                    // The formats an image of its use can have: one drawn into as depth, the depth formats; one drawn
                    // into or written by a shader, none that is depth or packed.
                    std::set<std::string> uses;
                    if (const auto usage = node.inputs.find("usage"); usage != node.inputs.end()) {
                        if (const auto* items = usage->second.value.Items()) { for (const auto& item : *items) if (item.String()) uses.insert(*item.String()); }
                        else if (usage->second.value.String()) uses.insert(*usage->second.value.String());
                    }
                    const bool depth = uses.contains("eDepthStencilAttachment"), written = uses.contains("eColorAttachment") || uses.contains("eStorage");
                    std::vector<std::string> names;
                    for (const auto& entry : port.builder->enumeration->values) {
                        const std::string format(entry.name);
                        const bool ofDepth = (format.starts_with("eD") && format.size() > 2 && std::isdigit(static_cast<unsigned char>(format[2]))) || format.starts_with("eS8");
                        const bool packed = format.find("BC") != std::string::npos || format.find("ASTC") != std::string::npos || format.find("ETC") != std::string::npos;
                        if (depth ? !ofDepth : written && (ofDepth || packed)) continue;
                        names.push_back(format);
                    }
                    return Choice(std::move(names), value, set, depth ? "a depth format" : "a format");
                }
                if (port.editor == "components") return Ticks(Doc().ComponentNames(), value, set);
                if (port.editor == "component") return Choice(Doc().ComponentNames(), value, set, "a component");
                if (port.editor == "entity") {
                    // The entities that have the component named beside it: any other could not be the one.
                    const auto beside = node.inputs.find("component");
                    const std::string* component = beside == node.inputs.end() ? nullptr : beside->second.value.String();
                    std::vector<std::string> names;
                    for (const kecs::Entity entity : Doc().Entities())
                        if (const std::string name = Doc().NameOf(entity); std::ranges::find(names, name) == names.end()
                            && (!component || component->empty() || std::ranges::contains(Doc().ComponentsOf(entity), *component))) names.push_back(name);
                    return Choice(std::move(names), value, set, "the first that has it");
                }
                if (port.editor == "parameter") {
                    // The image parameters of the pipeline this pass runs.
                    std::vector<std::string> names;
                    if (const auto pipeline = node.inputs.find("pipeline"); pipeline != node.inputs.end() && pipeline->second.link)
                        for (const auto& each : Doc().Runtime().PortsOf(pipeline->second.link->node))
                            if (each.name.starts_with("param.") && each.type == "Image" && !each.name.ends_with(".sampler")) names.push_back(each.name.substr(6));
                    return Choice(std::move(names), value, set, "an image parameter");
                }
                if (port.editor == "attributes") return Attributes(value, set, key);
                if (port.editor == "fields") return Fields(value, set, key);
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
                if (node.type != "Reference")
                    rows.push_back(kui::Tooltip("What it is called on the graph. Its name stays what wires, files and components know it by.",
                        kgui::Labeled("Title", kui::TextField(kui::TextFieldOptions { .text = node.title, .placeholder = id,
                            .onSubmitted = [this, id](const std::string& typed) { Doc().SetTitle(id, typed); } }).Key("title " + id), LabelWidth)));
                if (const auto& passes = Doc().Passes(); !passes.empty()) {
                    std::vector<std::string> choices { "(in no pass)" };
                    choices.insert(choices.end(), passes.begin(), passes.end());
                    const auto at = std::ranges::find(passes, node.pass);
                    rows.push_back(kui::Tooltip("The pass whose page it is on.", kgui::Labeled("Pass", kui::Dropdown(choices, at == passes.end() ? 0 : static_cast<int>(at - passes.begin()) + 1,
                        [this, id, choices](const int index) {
                            Doc().SetNodePass(id, index <= 0 ? std::string() : choices[static_cast<std::size_t>(index)]);
                            _session->GoTo(id);
                        }), LabelWidth)));
                }
                if (const auto* status = runtime.Status(id); status && !status->ok) rows.push_back(Problem(status->error));
                if (const auto* report = runtime.ReportOf(id)) {
                    rows.push_back(kgui::Muted(std::format("Runs for {} {}.", report->ran + report->culled, report->ran + report->culled == 1 ? "entity" : "entities")));
                    if (report->skipped > 0) rows.push_back(Problem(std::format("{} passed by. {}", report->skipped, report->why)));
                    else if (!report->why.empty()) rows.push_back(Problem(report->why));
                }
                if (const kecs::ComponentId component = runtime.ComponentOf(id); component != kecs::NoComponent)
                    rows.push_back(kgui::Muted(std::format("What its draws take from each entity is the component '{}': a ForEach of it gives its fields.",
                                                           Doc().World().Describe(component).name)));
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
                            if (any) line.push_back(kui::Tooltip("Back to nothing given", SmallIcon("close", remove(parts))));
                            rows.push_back(kui::Tooltip(port.doc.empty() ? port.type : port.doc,
                                                        kgui::Labeled(prefix.substr(0, prefix.size() - 1), kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)), LabelWidth)));
                            at += parts.size() - 1;
                            continue;
                        }
                    }

                    kui::Widget editor;
                    const kgraph::Placement placement = kgraph::PlacementOf(port, node);
                    if (placement == kgraph::Placement::eWire && !(shown.given && input->second.link) && port.kind == kgraph::PortKind::eValue) {
                        // Wired only: from the node that sets it up, made here when it has one, or from what works one out.
                        std::vector<kui::Widget> wire { kui::Expanded(kgui::Muted("wire one in")) };
                        if (!port.companion.empty())
                            wire.push_back(kui::Tooltip(std::format("Make a {} beside this node, wired in: where it is set up", port.companion),
                                Small(std::format("Make a {}", port.companion), [this, id, name] {
                                    std::string error;
                                    const std::string made = Doc().MakeCompanion(id, name, &error);
                                    if (made.empty()) _session->Say(error, true);
                                    else _session->Select(made);
                                })));
                        editor = kui::Row(std::move(wire), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                    } else if (shown.given && input->second.link) {
                        editor = kgui::Line(std::format("from {}{}", input->second.link->node, input->second.link->output == "out" ? "" : "." + input->second.link->output));
                    } else if (port.kind == kgraph::PortKind::eResource) {
                        const std::string wanted = port.type == "Resource" || port.type == "value" ? std::string("wire one in") : std::format("wire one in: {}", port.type);
                        // What it can be wired from, chosen here as well as drawn on the graph: the nodes that give
                        // what it takes, of this page or made before it — one of another pass is brought in.
                        if (const auto from = _session->WireChoices(id, name); !from.empty()) {
                            std::vector<std::string> labels;
                            for (const auto& other : from) {
                                const kgraph::Node* source = Doc().Graph().Find(other);
                                labels.push_back(source && !source->pass.empty() && source->pass != node.pass ? std::format("{}   ·   from {}", other, source->pass) : other);
                            }
                            editor = kui::Dropdown(std::move(labels), -1, [this, id, name, from](const int index) { _session->WireFrom(id, name, from[static_cast<std::size_t>(index)]); },
                                                   kui::DropdownOptions {}.SetPlaceholder(wanted));
                        } else editor = kgui::Muted(wanted + " (nothing gives one yet)");
                        // A buffer a shader declares can be made as the shader lays it out: a node with the struct's
                        // fields as inputs, wired here, for the data to be wired to.
                        if (port.type == "Buffer" && name.starts_with("param."))
                            editor = kui::Row({ kui::Expanded(std::move(editor)),
                                                kui::Tooltip("Make a node laid out as the shader declares this buffer: an input for each field of its struct, wired here",
                                                             Small("Make its buffer", [this, id, name] {
                                                                 std::string error;
                                                                 const std::string made = Doc().MakeBufferFor(id, name, &error);
                                                                 if (made.empty()) _session->Say(error, true);
                                                                 else { _session->Select(made); _session->Say(std::format("Made '{}': wire what each of its fields is.", made)); }
                                                             })) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                    } else if (shown.given && input->second.value.IsNull()) {
                        editor = kgui::Muted("a port: wire a value in");
                    } else {
                        // Not given, it shows what it is all the same: what the node type says it starts as.
                        editor = EditorFor(node, port, shown.given ? input->second.value : port.initial, set(name), id + "/" + name);
                    }
                    std::vector<kui::Widget> line { kui::Expanded(std::move(editor)) };
                    // Where it is set: on the node, with a control in its row, or here. One wired only is neither.
                    if (port.kind == kgraph::PortKind::eValue && placement != kgraph::Placement::eWire && port.editor != "attributes") {
                        const bool onNode = placement == kgraph::Placement::eNode;
                        line.push_back(kui::Tooltip(onNode ? "Set on the node: a control in its row. Press to set it only here" : "Set it on the node as well: a control in its row, and a port to wire",
                            kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(onNode ? "push_pin" : "input", kui::IconStyle::eFilled, onNode ? kui::Theme::Current().primary : kui::Theme::Current().textMuted)),
                                        [this, id, name, onNode] { Doc().SetPlacement(id, name, onNode ? kgraph::Placement::eInspector : kgraph::Placement::eNode); },
                                        kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)))));
                    }
                    // A value a node can work out — a number, a vector, a colour — can be wired instead: a name chosen from a list cannot.
                    // (One set on the node is a port already.)
                    if (placement != kgraph::Placement::eNode && plain(shown) && port.choices.empty() && (port.editor.empty() || port.editor == "color" || port.editor == "numbers"))
                        line.push_back(kui::Tooltip("Make it a port, to wire a value to", SmallIcon("link", [this, id, name] { Doc().SetInput(id, name, Value {}); })));
                    if (shown.given || port.optional)
                        line.push_back(kui::Tooltip(shown.given ? "Back to nothing given" : "Not needed after all", SmallIcon("close", remove({ name }))));
                    // What takes the whole width to set — a vertex's attributes — is under its name, not beside it.
                    if (port.editor == "attributes") {
                        rows.push_back(kui::Row({ kui::Expanded(kgui::Muted(PortLabel(name))), kui::Row(std::vector<kui::Widget>(line.begin() + 1, line.end()), kui::FlexOptions {}.SetGap(4.f)) },
                                                kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                        rows.push_back(std::move(line.front()));
                        continue;
                    }
                    rows.push_back(kui::Tooltip(port.doc.empty() ? port.type : port.doc + " (" + port.type + ")",
                                                kgui::Labeled(PortLabel(name), kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)), LabelWidth)));
                }

                // What else it can be given: asked for here, it joins the inputs above — as a port on the node, when it takes a resource.
                // (One that is given whole or in its parts is offered whole: its parts are not each a line of the list.)
                std::erase_if(more, [&](const kgraph::Port& port) {
                    if (port.name.size() < 3 || port.name[port.name.size() - 2] != '.') return false;
                    const std::string whole = port.name.substr(0, port.name.size() - 2);
                    return std::ranges::any_of(more, [&](const kgraph::Port& other) { return other.name == whole; });
                });
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
                {
                    std::vector<std::string> outputs;
                    bool first = true;
                    for (const auto& output : runtime.OutputsOf(id)) {
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
                                                    SmallIcon("close", [this, id, name = output.name] { _session->Hide(id, "out:" + name); Doc().DisconnectOutput(id, name); })));
                        rows.push_back(kui::Tooltip(output.doc.empty() ? output.type : output.doc + " (" + output.type + ")",
                                                    kgui::Labeled(PortLabel(output.name), kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)), LabelWidth)));
                    }
                    if (!outputs.empty())
                        rows.push_back(kgui::Labeled("Add output", kui::Dropdown(outputs, -1, [this, id, outputs](const int index) {
                            _session->Show(id, "out:" + outputs[static_cast<std::size_t>(index)]);
                        }, kui::DropdownOptions {}.SetPlaceholder(std::format("{} things of it to wire from", outputs.size()))), LabelWidth));
                }
                // What it holds, to look at: an image as a picture, a buffer as numbers. Copied out while it is looked at.
                if (const auto held = runtime.ResourceOf(id); held && held->kind == "Mesh") {
                    rows.push_back(kui::Separator());
                    rows.push_back(Small("Open in the viewer", [this, id] { _session->View(id); }));
                }
                if (const auto held = runtime.ResourceOf(id); held && (runtime.ImageOf(id).Get() || held->kind == "Buffer" || held->kind == "BufferView")) {
                    rows.push_back(kui::Separator());
                    rows.push_back(Small("Open in the viewer: every level, its histogram, a table", [this, id] { _session->View(id); }));
                    if (!_session->IsLooking(id)) {
                        rows.push_back(Small("Look at what it holds", [this, id] { _session->Look(id); }));
                    } else {
                        if (_session->Looking() != id) _session->Look(id);     // (the controls here are this node's)
                        rows.push_back(Small("Stop looking", [this, id] { _session->Unlook(id); }));
                        if (runtime.ImageOf(id).Get()) {
                            rows.push_back(kgui::Labeled("Shown", kui::Dropdown(LookModeNames(), static_cast<int>(_session->lookMode), [this](const int index) {
                                _session->lookMode = static_cast<LookMode>(index);
                                _session->Say("Looking at it another way.");
                            }), LabelWidth));
                            if (const auto depth = runtime.ImageOf(id)->Extent().z; depth > 1)
                                rows.push_back(kgui::Labeled("Slice", kgui::DragFloats({ static_cast<float>(_session->lookSlice) }, [this, depth](const std::vector<float>& changed) {
                                    _session->lookSlice = static_cast<std::uint32_t>(std::clamp(std::round(changed[0]), 0.f, static_cast<float>(depth - 1)));
                                }, 1.f, 0), LabelWidth));
                            rows.push_back(kgui::Muted("It is shown on the node."));
                        }
                        if (!_session->LookSays().empty()) rows.push_back(kgui::Muted(_session->LookSays()));
                        for (const auto& line : _session->LookLines()) rows.push_back(kgui::Muted(line, 11.f));
                    }
                }
                rows.push_back(kui::Separator());
                rows.push_back(Small("Remove the node", [this, id] { Doc().RemoveNodes({ id }); _session->Select(std::string()); }));
                return Panel(std::move(rows));
            }

            /** What a choice of a field that holds something is called: a file's or a mesh's own name, a plain picture as what it is, a node as the graph's. */
            std::string ChoiceLabel(const std::string& id)
            {
                if (id == "$white") return "white (plain)";
                if (id == "$black") return "black (plain)";
                if (id == "$normal") return "flat normal (plain)";
                if (id.starts_with('@')) return _session->AssetLabel(id);
                if (id.starts_with('$')) return std::format("{} (built in)", id.substr(1));
                if (Doc().Graph().Find(id)) return std::format("{}   ·   graph", id);
                return _session->AssetLabel(id);
            }

            kui::Widget EntityInspector(const kecs::Entity entity)
            {
                Document& doc = Doc();
                const kui::Theme& t = kui::Theme::Current();
                const kecs::World& world = doc.World();
                std::vector<kui::Widget> rows;
                const std::string name = doc.NameOf(entity);
                const std::string kind = _session->EntityKind(entity);
                const auto [icon, tint] = EntityIcon(kind);
                const bool can = _session->CanShow(entity), shown = !can || _session->IsShown(entity);

                // What it is, what it is called, and whether it is drawn.
                {
                    std::vector<kui::Widget> head;
                    head.push_back(kui::Tooltip(kind == "empty" ? std::string("An entity with nothing to it yet") : "A " + kind, kui::SizedBox(20.f, 20.f, kui::Icon(icon, kui::IconStyle::eFilled, tint))));
                    head.push_back(kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = name,
                        .onSubmitted = [this, entity](const std::string& typed) { Doc().Rename(entity, typed); } }).Key("name=" + name)));
                    if (can)
                        head.push_back(kui::Tooltip(shown ? "Drawn. Pressed: it and what is under it are not." : "Not drawn. Pressed: it and what is under it are.",
                                                    SmallIcon(shown ? "visibility" : "visibility_off", [this, entity, shown] { _session->SetShown(entity, !shown); })));
                    head.push_back(kui::Tooltip("Frame it in the scene view", SmallIcon("center_focus_strong", [this, entity] { _session->Frame(entity); })));
                    rows.push_back(kui::Row(std::move(head), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                }
                // A pass that could not run for it says why here, where it is mended: a field with nothing in it, a mesh that does not fit.
                for (const auto& [passed, why] : doc.Runtime().PassedBy())
                    if (passed == entity) rows.push_back(Problem(std::format("Not drawn by {}", why)));

                // What it is under: chosen among the entities it can go under — not itself, nor what is under it — or
                // dropped here from the outliner.
                {
                    std::set<kecs::Entity> below { entity };
                    std::vector<kecs::Entity> others;
                    std::vector<std::string> labels { "(nothing: it is at the top)" };
                    std::map<std::string, int> seen;
                    for (const kecs::Entity other : doc.Entities()) {       // parents before their children
                        if (below.contains(other)) continue;
                        if (below.contains(world.ParentOf(other))) { below.insert(other); continue; }
                        std::string label = doc.NameOf(other);
                        if (const int count = ++seen[label]; count > 1) label += std::format("  ({})", count);
                        others.push_back(other);
                        labels.push_back(std::move(label));
                    }
                    const kecs::Entity parent = world.ParentOf(entity);
                    const auto at = std::ranges::find(others, parent);
                    kui::Widget under = kui::Dropdown(labels, parent.IsNull() || at == others.end() ? 0 : static_cast<int>(at - others.begin()) + 1, [this, entity, others](const int index) {
                        Doc().SetParent(entity, index <= 0 ? kecs::NullEntity : others[static_cast<std::size_t>(index - 1)]);
                    });
                    under = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("entity").OnDrop([this, entity](const kui::DragData& dragged, kor::Vec2) {
                        if (const auto* other = dragged.As<kecs::Entity>(); other && *other != entity) _session->MoveEntity(entity, *other, Session::Beside::eUnder);
                    }), std::move(under));
                    rows.push_back(kui::Tooltip("What it is under: it moves, turns and grows with that. Chosen here, or an entity dropped here from the outliner. Where it is in the world stays as it was.",
                                                kgui::Labeled("Under", std::move(under), LabelWidth)));
                    if (const auto count = world.ChildrenOf(entity).size(); count > 0)
                        rows.push_back(kgui::Labeled("Over", kgui::Muted(std::format("{} {}", count, count == 1 ? "entity" : "entities")), LabelWidth));
                }
                // Which prefab it is an instance of — or part of one — and what of it is the instance's own.
                if (const kecs::Entity root = doc.PrefabRootOf(entity); !root.IsNull()) {
                    const auto overrides = doc.OverridesOf(root);
                    const std::string prefab = std::filesystem::path(doc.PrefabOf(root)).stem().string();
                    std::vector<kui::Widget> line;
                    line.push_back(kui::SizedBox(18.f, 18.f, kui::Icon("widgets", kui::IconStyle::eFilled, kui::Color::Hex(0x4FB3BF))));
                    line.push_back(kui::Expanded(kgui::Line(root == entity ? std::format("An instance of {}", prefab)
                                                                           : std::format("Part of {}, an instance of {}", doc.NameOf(root), prefab))));
                    rows.push_back(kui::Row(std::move(line), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                    rows.push_back(kgui::Muted(overrides.empty() ? std::string("As its prefab is: nothing of it is the instance's own.")
                                                                 : std::format("{} {} its own: changes made to it here, kept when the prefab changes.", overrides.size(),
                                                                               overrides.size() == 1 ? "thing is" : "things are")));
                    const auto act = [this, root](bool (Document::*what)(kecs::Entity, std::string*), const char* done) {
                        return [this, root, what, done] {
                            std::string error;
                            if ((Doc().*what)(root, &error)) _session->Say(done); else _session->Say(error, true);
                        };
                    };
                    std::vector<kui::Widget> buttons;
                    buttons.push_back(kui::Tooltip("Writes the instance as it is now to its prefab: every instance of it follows, here and in every scene as it is opened.",
                                                   Small("Apply to the prefab", act(&Document::ApplyPrefab, "Applied to the prefab: every instance of it follows."))));
                    buttons.push_back(kui::Tooltip("Drops what is the instance's own, and what was put under it: it is as its prefab is.",
                                                   Small("Revert", act(&Document::RevertPrefab, "Reverted: it is as its prefab is."))));
                    buttons.push_back(kui::Tooltip("Makes it entities like any other: no prefab's.", Small("Unlink", [this, root] { Doc().Unlink(root); _session->Say("Unlinked: no prefab's now."); })));
                    if (root != entity) buttons.push_back(Small("Pick the instance", [this, root] { _session->Select(root); }));
                    rows.push_back(kui::Row(std::move(buttons), kui::FlexOptions {}.SetGap(6.f)));
                }
                // What draws it: each pass's node, to go to.
                {
                    const auto passes = _session->RunsFor(entity);
                    if (!passes.empty()) {
                        std::vector<kui::Widget> each;
                        for (const auto& pass : passes) each.push_back(kui::Tooltip("Go to the node, on the graph", Small(pass, [this, pass] { _session->GoTo(pass); })));
                        rows.push_back(kgui::Labeled("Run by", kui::Wrap(std::move(each), 4.f, 4.f), LabelWidth));
                    } else if (!can) {
                        rows.push_back(kgui::Labeled("Run by", kgui::Muted("nothing: it has no component of a pipeline's"), LabelWidth));
                    }
                }

                // Where it is, relative to what it is under: every entity has that, and it is no component.
                rows.push_back(kui::Separator());
                rows.push_back(kui::Row({ kui::SizedBox(16.f, 16.f, kui::Icon("open_with", kui::IconStyle::eFilled, t.textMuted)), kui::Expanded(Heading("Transform")),
                                          kui::Tooltip("Back to where what it is under is: no move, no turn, its own size", Small("Reset", [this, entity] {
                                              Doc().SetPosition(entity, kecs::Vec3 { 0.f, 0.f, 0.f });
                                              Doc().SetRotation(entity, kecs::Vec3 { 0.f, 0.f, 0.f });
                                              Doc().SetScale(entity, kecs::Vec3 { 1.f, 1.f, 1.f });
                                          })) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                const auto three = [&](const char* label, const kecs::Vec3 value, const float speed, const char* what, auto set) {
                    rows.push_back(kui::Tooltip(what, kgui::Labeled(label, kgui::DragFloats({ value.x, value.y, value.z }, [this, entity, set](const std::vector<float>& v) {
                        (Doc().*set)(entity, kecs::Vec3 { v[0], v[1], v[2] });
                    }, speed, 2), LabelWidth)));
                };
                three("Position", world.PositionOf(entity), 0.01f, "Where it is, relative to what it is under.", &Document::SetPosition);
                three("Rotation", world.RotationOf(entity), 0.5f, "How it is turned, in degrees: about X, about Y, about Z.", &Document::SetRotation);
                three("Scale", world.ScaleOf(entity), 0.01f, "How big it is, each way.", &Document::SetScale);

                const auto has = doc.ComponentsOf(entity);
                const auto pipelines = _session->PipelineComponents();
                // What kind of thing a component is: said beside its name, and by its picture.
                const auto kindOf = [&](const std::string& component) -> std::tuple<const char*, std::string, kui::Color> {
                    const auto& type = doc.World().Describe(doc.World().Find(component));
                    if (const auto pipeline = pipelines.find(component); pipeline != pipelines.end()) return { "brush", "run by a pipeline", kui::Color::Hex(0xD9703F) };
                    if (component == "Mesh") return { "view_in_ar", "what is drawn", kui::Color::Hex(0x7DBB4B) };
                    if (component == "Camera") return { "videocam", "what the scene is seen through", kui::Color::Hex(0x62C3E8) };
                    if (component.ends_with("Light")) return { "lightbulb", "a light", kui::Color::Hex(0xE8C562) };
                    if (type.fields.empty()) return { "label", "a tag", kui::Color::Hex(0x9AA4B0) };
                    return { "data_object", "data", kui::Color::Hex(0x5B9BD5) };
                };
                for (const auto& component : has) {
                    rows.push_back(kui::Separator());
                    const auto& type = doc.World().Describe(doc.World().Find(component));
                    const auto [componentIcon, what, componentTint] = kindOf(component);
                    const bool shut = _componentsShut.contains(component);
                    const auto pipeline = pipelines.find(component);
                    std::vector<kui::Widget> head;
                    if (!type.fields.empty())
                        head.push_back(kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(shut ? "chevron_right" : "expand_more", kui::IconStyle::eFilled, t.textMuted)),
                                                   [this, component, shut] { SetState([&] { if (shut) _componentsShut.erase(component); else _componentsShut.insert(component); }); },
                                                   kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)));
                    head.push_back(kui::Tooltip("Switched off, it has the component still and nothing runs for it",
                                                kui::Checkbox(doc.IsEnabled(entity, component), [this, entity, component](const bool on) { Doc().SetEnabled(entity, component, on); })));
                    head.push_back(kui::SizedBox(16.f, 16.f, kui::Icon(componentIcon, kui::IconStyle::eFilled, componentTint)));
                    head.push_back(Heading(component));
                    head.push_back(kui::Expanded(kgui::Muted(what, 11.f)));
                    if (pipeline != pipelines.end())
                        head.push_back(kui::Tooltip("Go to the pipeline it is of, on the graph", SmallIcon("hub", [this, node = pipeline->second] { _session->GoTo(node); })));
                    head.push_back(kui::Tooltip("Take the component off the entity", SmallIcon("close", [this, entity, component] { Doc().RemoveComponent(entity, component); })));
                    rows.push_back(kui::ContextMenu({
                        { "Take it off the entity", [this, entity, component] { Doc().RemoveComponent(entity, component); } },
                        { doc.IsEnabled(entity, component) ? "Switch it off" : "Switch it on", [this, entity, component, on = doc.IsEnabled(entity, component)] { Doc().SetEnabled(entity, component, !on); } },
                        { "Go to its pipeline", [this, node = pipeline != pipelines.end() ? pipeline->second : std::string()] { _session->GoTo(node); }, pipeline != pipelines.end() },
                    }, kui::Row(std::move(head), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));
                    if (shut) continue;
                    for (const auto& field : type.fields) {
                        const std::string fieldName = field.name;
                        const Value value = doc.Field(entity, component, fieldName);
                        const auto set = [this, entity, component, fieldName](const Value& changed) {
                            std::string error;
                            if (!Doc().SetField(entity, component, fieldName, changed, &error)) _session->Say(error, true);
                        };
                        kui::Widget editor;
                        if (HoldsResource(field.type)) {
                            // What it can hold, and nothing it cannot: a mesh's field the meshes, a picture's the pictures —
                            // and of the graph only what is made before the pass that would use it.
                            std::vector<std::string> choices { std::string() };
                            int selected = 0;
                            for (const auto& choice : _session->FieldChoices(component, field.type)) {
                                if (value.String() && *value.String() == choice) selected = static_cast<int>(choices.size());
                                choices.push_back(choice);
                            }
                            // What it holds that is none of those is listed all the same: it is what is so.
                            const std::string holds = value.String() ? *value.String() : std::string();
                            if (selected == 0 && !holds.empty()) { selected = static_cast<int>(choices.size()); choices.push_back(holds); }
                            std::vector<std::string> labels;
                            for (const auto& choice : choices) labels.push_back(choice.empty() ? std::string("(nothing)") : ChoiceLabel(choice));
                            editor = kui::Dropdown(std::move(labels), selected, [set, choices](const int index) {
                                set(index <= 0 ? Value {} : Value(choices[static_cast<std::size_t>(index)]));
                            });
                            // What it holds is shown beside its name, small: a picture by itself, a mesh by the picture of it.
                            kui::Widget shownBeside;
                            if (field.type == "Mesh") {
                                if (const auto picture = _session->MeshPicture(holds); picture.Get()) shownBeside = kui::Image(picture, kui::ImageFit::eContain);
                            } else if (!holds.empty()) {
                                if (const auto held = doc.Runtime().ResourceOf(holds); held && held->handle && (held->kind == "Image" || held->kind == "ImageView") && doc.Runtime().ImageOf(holds).Get())
                                    shownBeside = kui::Image(doc.Runtime().ImageOf(holds), kui::ImageFit::eContain);
                                else if (holds.starts_with('@'))
                                    if (const auto file = doc.Runtime().ResourceOf(holds); file && file->handle) shownBeside = kui::Image(kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(file->handle)), kui::ImageFit::eContain);
                            }
                            if (holds.starts_with('@')) editor = kui::Tooltip(holds, std::move(editor));
                            if (shownBeside)
                                editor = kui::Row({ kui::SizedBox(24.f, 24.f, std::move(shownBeside)), kui::Expanded(std::move(editor)) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                            // And what is dropped on it from the Assets panel is what it holds from then on — when it is
                            // the kind of thing the field holds.
                            editor = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this, set, type = field.type, fieldName](const kui::DragData& dragged, kor::Vec2) {
                                const auto* id = dragged.As<std::string>();
                                const auto asset = id ? AssetOf(*id) : std::nullopt;
                                if (!asset) return;
                                const bool mesh = asset->kind == Session::Asset::Kind::eMesh, picture = asset->kind == Session::Asset::Kind::eTexture;
                                if (type == "Mesh" ? mesh : type == "Image" && picture) set(Value(*id));
                                else _session->Say(std::format("'{}' holds a {}, and {} is not one.", fieldName, type == "Mesh" ? "mesh" : type == "Image" ? "picture" : type, asset->label), true);
                            }), std::move(editor));
                        } else if (field.type == "entity") {
                            editor = EntityPicker(doc.World(), value, set);
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

                // What it can be given, each said for what it is: a pipeline's, to be drawn by it; data; a tag.
                rows.push_back(kui::Separator());
                std::vector<std::string> more, moreLabels;
                for (const int order : { 0, 1, 2 })
                    for (const auto& component : doc.ComponentNames()) {
                        if (std::ranges::find(has, component) != has.end()) continue;
                        const bool pipeline = pipelines.contains(component), tag = doc.World().Describe(doc.World().Find(component)).fields.empty();
                        if ((pipeline ? 0 : tag ? 2 : 1) != order) continue;
                        more.push_back(component);
                        moreLabels.push_back(std::format("{}   ·   {}", component, std::get<1>(kindOf(component))));
                    }
                kui::Widget add = kui::Dropdown(moreLabels, -1, [this, entity, more](const int index) {
                    Doc().AddComponent(entity, more[static_cast<std::size_t>(index)]);
                }, kui::DropdownOptions {}.SetPlaceholder(more.empty() ? "it has every component there is" : "a component, or a tag"));
                // A script dropped here from the Assets panel is the component it makes.
                add = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this, entity](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* id = dragged.As<std::string>()) _session->GiveAsset(*id, entity);
                }), std::move(add));
                rows.push_back(kui::Tooltip("Gives it a component. A mesh, a texture or a script dropped here from the Assets panel is given to it too.", kgui::Labeled("Add", std::move(add), LabelWidth)));
                rows.push_back(kgui::Labeled("New tag", kui::TextField(kui::TextFieldOptions { .placeholder = "its name, then Enter",
                    .onSubmitted = [this, entity](const std::string& typed) {
                        if (typed.empty()) return;
                        Doc().AddTag(typed);
                        Doc().AddComponent(entity, typed);
                    } }).Key(std::format("tag{}", has.size())), LabelWidth));
                rows.push_back(kui::Row({ Small("Duplicate", [this, entity] { _session->Duplicate(entity); }),
                                          Small("Destroy the entity", [this, entity] { Doc().DestroyEntity(entity); _session->Select(kecs::NullEntity); }) }, kui::FlexOptions {}.SetGap(6.f)));
                // A script dropped anywhere on the entity's inspector is the component it makes; a mesh or a texture is
                // given to it as Add does. (A field that holds one takes what is dropped on it for itself.)
                return kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this, entity](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* id = dragged.As<std::string>()) _session->GiveAsset(*id, entity);
                }), Panel(std::move(rows)));
            }

            // ---- what is wrong ----------------------------------------------------------------------------------

            kui::Widget Problems()
            {
                const kgraph::Runtime& runtime = Doc().Runtime();
                std::vector<kui::Widget> rows;
                for (const auto& problem : runtime.Problems()) {
                    const std::string node = problem.substr(0, problem.find(':'));
                    rows.push_back(Problem(problem).OnTap([this, node] { _session->GoTo(node); }));
                }
                for (const auto& node : Doc().Graph().nodes)
                    if (const auto* report = runtime.ReportOf(node.id); report && (report->skipped > 0 || (report->ran == 0 && !report->why.empty())))
                        rows.push_back(Problem(report->skipped > 0 ? std::format("{}: {} passed by. {}", node.id, report->skipped, report->why)
                                                                   : std::format("{}: {}", node.id, report->why)).OnTap([this, id = node.id] { _session->GoTo(id); }));
                // Scripts in a language that is not the project's: nothing runs them.
                if (_session->HasProject())
                    for (const auto& file : _session->CurrentProject().ScriptsInAnotherLanguage())
                        rows.push_back(Problem(std::format("scripts/{}: the project's scripts are in {}, and this one is not. Nothing runs it.", file.filename().string(),
                                                           _session->CurrentProject().Language() == 'k' ? "Kotlin" : "C#")));
                // Scripts that are in the project and that nothing is running: pressed, the editor is opened with them.
                if (const auto idle = _session->ScriptsNotRunning(); !idle.empty()) {
                    std::string names;
                    for (const auto& name : idle) names += std::format("{}{}", names.empty() ? "" : ", ", name);
                    rows.push_back(Problem(std::format("Nothing is running the project's scripts ({}): this editor was started without them. Press this, or Project, Open with its scripts.", names))
                                       .OnTap([this] { _session->ReopenWithScripts(); }));
                }
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
            std::string _menuCategory;      // the node menu's: the category shown, empty for all of them
            std::string _menuPreview;       // and the node type previewed: the one last pointed at
            int _newKind = 0;
            // The graph's view: where it looks, shared with the node editor; what a wire let go over nothing waits for;
            // and the page as it was last shown, for putting it in order.
            std::shared_ptr<kui::NodeEditorView> _view = _session->graphView;
            std::optional<LooseWire> _wire;
            // The Assets panel: the folder that is picked in it, the folders shut down its side, what was last pressed
            // and when (twice in a row opens), and what is asked about before it goes to the trash.
            // The outliner: what is looked for in it, the entity whose name is being typed over, and where what is held
            // over it would go. The inspector: the components shut in it.
            std::string _outlinerFilter;
            kecs::Entity _renaming = kecs::NullEntity, _dropOn = kecs::NullEntity;
            Session::Beside _dropWhere = Session::Beside::eUnder;
            std::set<std::string> _componentsShut;
            std::string _folderPicked, _tapped;
            std::set<std::string> _foldersShut;
            std::chrono::steady_clock::time_point _tappedAt {};
            std::filesystem::path _trash;
            std::uint32_t _went = 0, _viewAsked = 0, _played = 0;
            std::uint64_t _followed = 0;     // what the picked entity's inspector was last built for, as it plays
            std::chrono::steady_clock::time_point _followedAt {};
            int _passDrop = -1;     // the pass row a pass held over the list would take the place of
            bool _overview = false; // the page shown is every node at once, of passes: its own layout
            std::string _passHover; // the line of the pass list under the pointer
            bool _addingPass = false;  // the pass list asks for a new pass's name
            std::uint32_t _passFocus = 0;
            std::vector<std::string> _functionOf;   // the nodes a function is being made of
            // A model being imported: its file, what its meshes have (once known), and what of it is to be read.
            std::filesystem::path _importFile;
            std::optional<std::vector<kgraph::Model::Attribute>> _importAttributes;
            AttributeChoice _importChoice;
            bool _importChosen = false;
            int _made = 0;
            std::vector<PlacedNode> _placed;
            std::vector<std::pair<std::string, std::string>> _wires;
        };
    }

    std::map<std::string, std::pair<double, double>> ArrangeNodes(const std::vector<PlacedNode>& nodes, const std::vector<std::pair<std::string, std::string>>& wires,
                                                                  const float aspect)
    {
        std::map<std::string, std::pair<double, double>> where;
        if (nodes.empty()) return where;
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < nodes.size(); ++i) index[nodes[i].id] = i;
        std::vector<std::vector<std::size_t>> before(nodes.size()), after(nodes.size());
        for (const auto& [from, to] : wires) {
            const auto a = index.find(from), b = index.find(to);
            if (a == index.end() || b == index.end() || a->second == b->second) continue;
            before[b->second].push_back(a->second);
            after[a->second].push_back(b->second);
        }
        // The column: one more than the furthest of what it is wired from.
        std::vector<int> column(nodes.size(), -1);
        std::vector<char> visiting(nodes.size(), 0);
        const std::function<int(std::size_t)> depth = [&](const std::size_t i) {
            if (column[i] >= 0) return column[i];
            if (visiting[i]) return 0;      // wired in a ring: it ends somewhere
            visiting[i] = 1;
            int most = 0;
            for (const std::size_t from : before[i]) most = std::max(most, depth(from) + 1);
            visiting[i] = 0;
            return column[i] = most;
        };
        int last = 0;
        for (std::size_t i = 0; i < nodes.size(); ++i) last = std::max(last, depth(i));
        // Then as near what it is wired to as that leaves it: an image is beside the pass that draws into it.
        std::vector<std::size_t> order(nodes.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::stable_sort(order, [&](const std::size_t a, const std::size_t b) { return column[a] > column[b]; });
        for (const std::size_t i : order) {
            if (after[i].empty()) continue;
            int nearest = last + 1;
            for (const std::size_t to : after[i]) nearest = std::min(nearest, column[to]);
            column[i] = std::max(column[i], nearest - 1);
        }
        std::vector<std::vector<std::size_t>> columns(static_cast<std::size_t>(last) + 1);
        std::vector<std::size_t> loose;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (before[i].empty() && after[i].empty()) loose.push_back(i);
            else columns[static_cast<std::size_t>(column[i])].push_back(i);
        }
        std::erase_if(columns, [](const auto& c) { return c.empty(); });
        float left = nodes.front().x, top = nodes.front().y;
        for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }
        constexpr float Across = 90.f, Down = 28.f;
        std::vector<float> middle(nodes.size(), 0.f), tall(columns.size(), 0.f);
        for (std::size_t c = 0; c < columns.size(); ++c) {
            for (const std::size_t i : columns[c]) tall[c] += nodes[i].height + Down;
            tall[c] -= Down;
        }
        const float tallest = columns.empty() ? 0.f : *std::ranges::max_element(tall);
        float x = left;
        for (std::size_t c = 0; c < columns.size(); ++c) {
            auto& here = columns[c];
            // By the middle of what feeds each — where it was, for what nothing here feeds.
            std::vector<float> wants(nodes.size(), 0.f);
            for (const std::size_t i : here) {
                float sum = 0.f;
                int count = 0;
                for (const std::size_t from : before[i])
                    if (where.contains(nodes[from].id)) { sum += middle[from]; ++count; }
                wants[i] = count > 0 ? sum / static_cast<float>(count) : c == 0 ? nodes[i].y : std::numeric_limits<float>::max();
            }
            // What is of one loop stays together: the loop is where the middle of its nodes would be.
            std::map<std::string, std::pair<float, int>> groups;
            for (const std::size_t i : here)
                if (!nodes[i].group.empty() && wants[i] != std::numeric_limits<float>::max()) { groups[nodes[i].group].first += wants[i]; ++groups[nodes[i].group].second; }
            const auto place = [&](const std::size_t i) {
                const auto group = groups.find(nodes[i].group);
                return group == groups.end() || group->second.second == 0 ? wants[i] : group->second.first / static_cast<float>(group->second.second);
            };
            std::ranges::stable_sort(here, [&](const std::size_t a, const std::size_t b) {
                if (const float pa = place(a), pb = place(b); pa != pb) return pa < pb;
                if (nodes[a].group != nodes[b].group) return nodes[a].group < nodes[b].group;
                return wants[a] != wants[b] ? wants[a] < wants[b] : nodes[a].y < nodes[b].y;
            });
            float y = top + (tallest - tall[c]) * 0.5f, width = 0.f;
            for (const std::size_t i : here) {
                where[nodes[i].id] = { std::round(x), std::round(y) };
                middle[i] = y + nodes[i].height * 0.5f;
                y += nodes[i].height + Down;
                width = std::max(width, nodes[i].width);
            }
            x += width + Across;
        }
        float wide = x - left - (columns.empty() ? 0.f : Across), high = tallest;
        // As the view is shaped: a column that runs past a height is wrapped into another beside it, in the same order —
        // the height lowered until the whole is as wide, for how tall it is, as the view.
        if (aspect > 0.f && !columns.empty()) {
            std::vector<float> widest(columns.size(), 0.f);
            float lowest = 0.f;
            for (std::size_t c = 0; c < columns.size(); ++c)
                for (const std::size_t i : columns[c]) { widest[c] = std::max(widest[c], nodes[i].width); lowest = std::max(lowest, nodes[i].height); }
            const auto lay = [&](const float limit, const bool place) {
                float cx = left, most = 0.f;
                for (std::size_t c = 0; c < columns.size(); ++c) {
                    float y = top;
                    for (const std::size_t i : columns[c]) {
                        if (y > top && y - top + nodes[i].height > limit) { cx += widest[c] + Across * 0.5f; y = top; }
                        if (place) where[nodes[i].id] = { std::round(cx), std::round(y) };
                        most = std::max(most, y - top + nodes[i].height);
                        y += nodes[i].height + Down;
                    }
                    cx += widest[c] + Across;
                }
                return std::pair { cx - left - Across, most };
            };
            // What is wired to nothing goes under it, in rows as wide as it: as tall as that makes them.
            const auto underneath = [&](const float across) {
                float lx = 0.f, rows = 0.f, row = 0.f;
                for (const std::size_t i : loose) {
                    if (lx > 0.f && lx + nodes[i].width > across) { lx = 0.f; rows += row + 40.f; row = 0.f; }
                    lx += nodes[i].width + 40.f;
                    row = std::max(row, nodes[i].height);
                }
                return loose.empty() ? 0.f : rows + row + 60.f;
            };
            // Of the heights there are to wrap at, the one whose whole is shaped most nearly as the view.
            float limit = tallest, nearest = std::numeric_limits<float>::max();
            for (float at = tallest; at >= std::max(lowest, 1.f); at *= 0.94f) {
                const auto [w, h] = lay(at, false);
                const float off = std::abs(std::log(w / std::max(h + underneath(std::max(w, 600.f)), 1.f) / aspect));
                if (off < nearest - 1e-4f) { nearest = off; limit = at; }
            }
            std::tie(wide, high) = lay(limit, true);
        }
        // What is wired to nothing: in rows under it all, as wide as the rest (or one row, shaped by nothing).
        float lx = left, ly = top + high + (columns.empty() ? 0.f : 60.f), row = 0.f;
        const float across = aspect > 0.f ? std::max(wide, 600.f) : std::numeric_limits<float>::max();
        for (const std::size_t i : loose) {
            if (lx > left && lx - left + nodes[i].width > across) { lx = left; ly += row + 40.f; row = 0.f; }
            where[nodes[i].id] = { std::round(lx), std::round(ly) };
            lx += nodes[i].width + 40.f;
            row = std::max(row, nodes[i].height);
        }
        return where;
    }

    kui::Widget EditorRoot(std::shared_ptr<Session> session) { return kui::Make<Root>(std::move(session)); }

    // ---- the scene ----------------------------------------------------------------------------------------------

    EditorScene::EditorScene(std::filesystem::path project, const bool handOver) : _projectPath(std::move(project)), _handOver(handOver) {}
    EditorScene::EditorScene(const kor::SceneArgs& arguments)
        : _projectPath(arguments.String("project")), _scriptsHost(arguments.String("scripts")), _handOver(true) {}
    EditorScene::~EditorScene() = default;

    void EditorScene::Initialize()
    {
        // The scene being worked on runs as it would on its own, into an image a panel shows.
        _play = kor::App::Current().OpenOffscreen<PlayScene>({ .title = "Scene", .extent = { 640, 360 } });
        // It does not play until it is told to: what is edited is the scene, not a moment of it.
        _play->SetPlaying(false);
        _session = std::make_shared<Session>(*_play);
        // What it looks like played, seen from its own camera: another view of the same graph, in a panel of its own.
        _game = kor::App::Current().OpenOffscreen<GameScene>({ .title = "Game", .extent = { 640, 360 } }, *_play);
        _session->SetGame(_game);
        _session->LoadObjects();
        // The editor's view, its gizmo and its picking, as the scene updates: in the frame being made.
        _play->EditAs();                    // picked from and drawn over, whatever its graph is
        _play->SetInput(&_game->SceneInput());  // a script that is steered reads the Game panel's keys and mouse
        _play->OnFrame([this] { if (_session) _session->Tick(SceneInput(), static_cast<float>(SceneTime().FrameTime())); });
        Document::Alias(&Graph(), &_play->Graph());     // whoever is handed this scene is after the one it edits
        _session->handOver = _handOver;
        _session->HostScripts(_scriptsHost);
        if (!_projectPath.empty()) _session->OpenProject(_projectPath);
        _ui = std::make_unique<kui::Ui>(EditorRoot(_session));
        Graph().Add<kui::UiPass>(*_ui);
    }

    void EditorScene::Update()
    {
        _session->UpdateHandOver();
        _session->UpdateLook(static_cast<float>(Time::FrameTime()));
        _session->UpdateThumbnails(static_cast<float>(Time::FrameTime()));
        _session->UpdateView(static_cast<float>(Time::FrameTime()));
        _ui->Update();
        // Control+S saves, Control+R runs — unless a field is being typed into.
        const kor::Input& input = SceneInput();
        const bool control = input.IsKeyHeld(kor::Key::eLeftControl) || input.IsKeyHeld(kor::Key::eRightControl);
        // The keys an editor's hands know: taking back, and — with nothing typed into and the view not being flown —
        // the gizmo's tools, framing, and removing what is picked.
        if (!input.InterfaceWantsKeyboard()) {
            Session& session = *_session;
            const bool shift = input.IsKeyHeld(kor::Key::eLeftShift) || input.IsKeyHeld(kor::Key::eRightShift);
            const kecs::Entity picked = session.Doc().World().Alive(session.selectedEntity) ? session.selectedEntity : kecs::NullEntity;
            if (control && input.IsKeyPressed(kor::Key::eZ)) { if (shift) session.Redo(); else session.Undo(); }
            else if (control && input.IsKeyPressed(kor::Key::eY)) session.Redo();
            else if (control && input.IsKeyPressed(kor::Key::eD)) { if (!picked.IsNull()) session.Duplicate(picked); }
            else if (!control && !input.IsMouseButtonHeld(kor::MouseButton::eRight)) {
                if (input.IsKeyPressed(kor::Key::eQ)) session.SetGizmo(false, session.gizmo);
                if (input.IsKeyPressed(kor::Key::eW)) session.SetGizmo(true, kor::GizmoMode::eTranslate);
                if (input.IsKeyPressed(kor::Key::eE)) session.SetGizmo(true, kor::GizmoMode::eRotate);
                if (input.IsKeyPressed(kor::Key::eR)) session.SetGizmo(true, kor::GizmoMode::eScale);
                if (input.IsKeyPressed(kor::Key::eF) && !picked.IsNull()) session.Frame(picked);
                if (input.IsKeyPressed(kor::Key::eDelete)) {
                    if (!picked.IsNull()) session.Delete(picked);
                    else if (!session.selectedNode.empty() && !session.InPlayMode()) { session.Doc().RemoveNodes({ session.selectedNode }); session.Select(std::string()); }
                }
            }
        }
        if (!input.InterfaceWantsKeyboard() && control) {
            if (input.IsKeyPressed(kor::Key::eS)) _session->Save();
            if (input.IsKeyPressed(kor::Key::eSpace)) _session->AskForNodes();     // Control+Space: the nodes there are to add
            if (input.IsKeyPressed(kor::Key::eR)) _session->Run();
        }
    }

    void EditorScene::Shutdown()
    {
        if (_play) { _play->OnFrame({}); _play->SetInput(nullptr); }
        _ui.reset();
        _session.reset();
        Document::Alias(&Graph(), nullptr);
        if (_game) kor::App::Current().Close(*_game);     // it follows the play scene's: it goes first
        _game = nullptr;
        if (_play) kor::App::Current().Close(*_play);
        _play = nullptr;
    }
}
