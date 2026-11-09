// keditor: the editor's session — its project, its scene, its graph.

#include "editor_internal.h"

namespace keditor
{

    // ---- the session ------------------------------------------------------------------------------------------

    void Session::ArrangeEveryPage()
    {
        if (!_toArrange.empty()) return;
        const kgraph::Graph& graph = Doc().Graph();
        for (const auto& pages : { graph.setup, graph.fixed, graph.passes })
            for (const auto& page : pages)
                if (std::ranges::find(_toArrange, page) == _toArrange.end()) _toArrange.push_back(page);
        if (_toArrange.empty()) return;
        _arrangedFrom = _everyPass ? std::string() : _pass;
        ShowPass(_toArrange.front());
    }

    void Session::ArrangedPage()
    {
        if (_toArrange.empty()) return;
        _toArrange.erase(_toArrange.begin());
        if (!_toArrange.empty()) { ShowPass(_toArrange.front()); return; }
        if (_arrangedFrom.empty()) ShowEveryPass(); else ShowPass(_arrangedFrom);
        Say("Every page's nodes are in the order they are wired in. Ctrl+Z puts each back.");
    }

    void Session::Say(std::string status, const bool failed)
    {
        // Said again, it is not logged again: what is pressed twice is one line of the log.
        if (status != _status || failed != _failed) {
            if (failed) kor::log::Error("{}", status);
            else if (!status.empty()) kor::log::Info("{}", status);
        }
        _status = std::move(status);
        _failed = failed;
        ++_changes;
    }

    bool Session::NewProject(const std::filesystem::path& directory, const char language, const bool module, const std::vector<std::filesystem::path>& modules)
    {
        if (_prefab) ClosePrefab(true);
        std::string error;
        Project project;
        // What it imports is in its language: one in another would run nowhere.
        for (const auto& folder : modules) {
            Project imported;
            if (!Project::Open(folder, imported, error)) { Say(error, true); return false; }
            if (!imported.module) { Say(std::format("{} is a project, not a module: only a module is imported.", imported.name), true); return false; }
            if (const char theirs = imported.Language(); theirs != 0 && language != 0 && theirs != language) {
                Say(std::format("{}'s scripts are in {}, and these are to be in {}: what is imported is in the same language.", imported.name,
                                theirs == 'k' ? "Kotlin" : "C#", language == 'k' ? "Kotlin" : "C#"), true);
                return false;
            }
        }
        if (!Project::Create(directory, directory.filename().string(), project, error, language, module, modules)) { Say(error, true); return false; }
        // A new project has nothing in it: not the scene of the project that was open when it was made. Only a scene
        // that was in no project yet — made here, and not saved anywhere — goes into it: this is where it is saved.
        const bool another = _hasProject;
        _project = std::move(project);
        _hasProject = true;
        _scene = _project.start;
        _project.Use();
        WriteStandardObjects(_project.directory / "objects");
        WriteEditorSetup(_project.directory, _project.name);
        if (another) {
            Look({});
            _play.New();
            selectedNode.clear();
            selectedEntity = kecs::NullEntity;
            _pass.clear();
        }
        // Its folder, and what it imports: their scripts' components, objects and assets are its from the start.
        Doc().SetAssetDirectory(_project.directory / "assets");
        Doc().DeclareScripts(_project.directory / "scripts");
        LoadObjects();
        ScanAssets();
        return Save();
    }

    bool Session::ImportModule(const std::filesystem::path& folder)
    {
        if (!_hasProject) { Say("A module is imported into a project: File, New project.", true); return false; }
        Project module;
        std::string error;
        if (!Project::Open(folder, module, error)) { Say(error, true); return false; }
        if (!module.module) { Say(std::format("{} is a project, not a module: only a module is imported.", module.name), true); return false; }
        if (std::filesystem::equivalent(module.directory, _project.directory)) { Say("A module does not import itself.", true); return false; }
        if (const char theirs = module.Language(), ours = _project.Language(); theirs != 0 && ours != 0 && theirs != ours) {
            Say(std::format("{}'s scripts are in {}, and {}'s are in {}: what is imported is in the same language.", module.name,
                            theirs == 'k' ? "Kotlin" : "C#", _project.name, ours == 'k' ? "Kotlin" : "C#"), true);
            return false;
        }
        const std::string reference = _project.Reference(module.directory);
        if (std::ranges::contains(_project.modules, reference)) { Say(std::format("{} imports {} already.", _project.name, module.name)); return true; }
        _project.modules.push_back(reference);
        return ModulesChanged(std::format("{} imports {}: its scripts, shaders, functions, objects and assets are {}'s too.", _project.name, module.name, _project.name));
    }

    bool Session::ForgetModule(const std::string& reference)
    {
        if (!_hasProject || std::erase(_project.modules, reference) == 0) return false;
        return ModulesChanged(std::format("{} no longer imports {}.", _project.name, std::filesystem::path(reference).filename().string()));
    }

    bool Session::ModulesChanged(const std::string& said)
    {
        std::string error;
        if (!_project.Save(error)) { Say(error, true); return false; }
        _project.Use();
        Doc().ReloadModules();
        WriteEditorSetup(_project.directory, _project.name);     // what an editor of code reads: the modules' scripts with its own
        LoadObjects();
        ScanAssets();
        Say(said);
        return true;
    }

    bool Session::OpenProject(const std::filesystem::path& directory)
    {
        if (_prefab) ClosePrefab(true);         // the prefab is the project's that is open: saved, and its scene back, first
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
        if (ScanAssets()) Say(std::format("Opened {}.", _project.name));     // or what was wrong of its assets is what the status says
        return true;
    }

    bool Session::Save()
    {
        if (!_hasProject) { Say("There is no project to save it in yet: File, New project.", true); return false; }
        if (_prefab) return SavePrefab();       // what is open is the prefab: it is what is saved
        if (InPlayMode()) { Say("Stop playing to save: what is saved is the scene, not a moment of it.", true); return false; }
        std::string error;
        if (!Doc().Save(_project.ScenePath(_scene), error)) { Say(error, true); return false; }
        Say(std::format("Saved {}.", _project.ScenePath(_scene).filename().string()));
        return true;
    }

    std::string Session::RunCommand() const
    {
        if (!_hasProject) return {};
        // The player that was built with this editor: beside it. Failing that, whichever is found by name. It runs the
        // project's scripts itself, as this editor does.
#ifdef _WIN32
        const char* name = "koral-play.exe";
        wchar_t self[4096];
        const unsigned long length = GetModuleFileNameW(nullptr, self, static_cast<unsigned long>(std::size(self)));
        const std::filesystem::path here = length > 0 ? std::filesystem::path(std::wstring(self, length)).parent_path() : std::filesystem::path();
#else
        const char* name = "koral-play";
        std::error_code ignored;
        const std::filesystem::path here = std::filesystem::read_symlink("/proc/self/exe", ignored).parent_path();
#endif
        std::error_code ec;
        const std::filesystem::path player = !here.empty() && std::filesystem::exists(here / name, ec) ? here / name : std::filesystem::path(name);
        return std::format("\"{}\" \"{}\"", player.string(), _project.directory.string());
    }

    const std::vector<kscript::ScriptType>& Session::ProjectScriptTypes()
    {
        if (!_hasProject) { _scriptFiles.clear(); _scriptTypes.clear(); _scriptConditions.clear(); return _scriptTypes; }
        // Those of the project's language: one in another runs nowhere, which is a problem of its own.
        const char* ending = ScriptLanguage(_project) == 'k' ? ".kt" : ".cs";
        std::error_code ignored;
        std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> files;
        std::vector<std::filesystem::path> folders { _project.directory / "scripts" };     // its own, and its modules': a class may derive from one of theirs
        for (const auto& module : Doc().Modules()) folders.push_back(module / "scripts");
        for (const auto& folder : folders)
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
            if (entry.path().extension() == ending) files.emplace_back(entry.path(), std::filesystem::last_write_time(entry.path(), ignored));
        std::ranges::sort(files);
        if (files != _scriptFiles) {
            _scriptFiles = std::move(files);
            std::vector<std::filesystem::path> paths;
            for (const auto& file : _scriptFiles) paths.push_back(file.first);
            auto declared = kscript::DeclareFiles(paths);
            _scriptTypes = std::move(declared.types);
            _scriptConditions = std::move(declared.conditions);
        }
        return _scriptTypes;
    }

    const std::vector<kscript::DeclaredConditions>& Session::ProjectConditions()
    {
        (void)ProjectScriptTypes();
        return _scriptConditions;
    }

    std::vector<std::string> Session::ScriptsNotRunning()
    {
        // What is to be run is what the sources declare that is no Data: a file of Data, or of plain classes a script
        // uses, has nothing of its own to run.
        std::vector<std::string> idle;
        for (const auto& type : ProjectScriptTypes())
            if (!type.data && !Doc().Scripts().Registered(type.name)) idle.push_back(type.name);
        std::ranges::sort(idle);
        return idle;
    }

    std::vector<Session::Problem> Session::Problems()
    {
        const kgraph::Runtime& runtime = Doc().Runtime();
        std::vector<Problem> problems;
        for (auto& problem : runtime.Problems()) {
            std::string node = problem.substr(0, problem.find(':'));
            problems.push_back({ std::move(problem), std::move(node) });
        }
        for (const auto& node : Doc().Graph().nodes)
            if (const auto* report = runtime.ReportOf(node.id); report && (report->skipped > 0 || (report->ran == 0 && !report->why.empty())))
                problems.push_back({ report->skipped > 0 ? std::format("{}: {} passed by. {}", node.id, report->skipped, report->why)
                                                         : std::format("{}: {}", node.id, report->why), node.id });
        if (_hasProject) {
            // Scripts in a language that is not the project's: nothing runs them.
            for (const auto& file : _project.ScriptsInAnotherLanguage())
                problems.push_back({ std::format("scripts/{}: the project's scripts are in {}, and this one is not. Nothing runs it.", file.filename().string(),
                                                 _project.Language() == 'k' ? "Kotlin" : "C#") });
            // Scripts that are in the project and that nothing is running: why, as this program has it.
            if (const auto idle = ScriptsNotRunning(); !idle.empty()) {
                std::string names;
                for (const auto& name : idle) names += std::format("{}{}", names.empty() ? "" : ", ", name);
                std::string why;
                problems.push_back({ HostsScripts(&why)
                    ? std::format("Not running: {}. They did not compile, or are being compiled: the log says which.", names)
                    : std::format("Nothing is running the project's scripts ({}): {}.", names, why) });
            }
        }
        for (const auto& note : Doc().Notes()) problems.push_back({ note, {}, true });
        // Two pages that both write something, and nothing says which runs first: a link between them, drawn in the frame.

        for (const auto& tie : Ties()) {
            std::string what;
            for (const auto& id : tie.shared) what += (what.empty() ? "" : ", ") + id;
            problems.push_back({ std::format("{} and {} both write {}, and nothing says which runs first: link them in the frame (or right-click either: Run after...).",
                                             tie.first, tie.second, what), tie.shared.empty() ? std::string() : tie.shared.front(), true });
        }
        return problems;
    }

    bool Session::HostsScripts(std::string* why) const
    {
        if (!_hasProject) return false;
        if (!_scriptsHost.empty() && Same(_scriptsHost, _project.directory)) return true;
        std::string said;
        if (scriptRuntime && scriptRuntime->Runs(_project.directory, said)) return true;
        if (why) *why = scriptRuntime ? said : "this program runs no scripts";
        return false;
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

    kui::Theme Session::SavedTheme()
    {
        std::ifstream in(ConfigDirectory() / "theme");
        std::string design, shade;
        if (!(in >> design >> shade)) return kui::Theme::Dark();
        for (const auto& [name, which] : ThemeDesigns)
            if (design == name) return kui::Theme::Of(which, shade != "light");
        return kui::Theme::Dark();
    }

    bool Session::SetSavedTheme(const kui::ThemeDesign design, const bool dark)
    {
        const std::filesystem::path file = ConfigDirectory() / "theme";
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
        std::ofstream out(file, std::ios::trunc);
        for (const auto& [name, which] : ThemeDesigns)
            if (which == design) out << name << ' ' << (dark ? "dark" : "light") << '\n';
        return static_cast<bool>(out);
    }

    Session::SavedWindow Session::LastWindow()
    {
        // `<name> <numbers>` a line, in ~/.config/koral-engine/window.
        SavedWindow saved;
        if (std::getenv("KORAL_ENGINE_NO_LAYOUT")) return saved;
        std::ifstream in(ConfigDirectory() / "window");
        std::string name;
        while (in >> name) {
            if (name == "extent") {
                std::uint32_t x = 0, y = 0;
                if (in >> x >> y && x >= 320 && y >= 240) saved.extent = { x, y };
            } else if (name == "position") {
                int x = 0, y = 0;
                if (in >> x >> y) saved.position = kor::IVec2 { x, y };
            } else if (name == "maximized") {
                int on = 0;
                if (in >> on) saved.maximized = on != 0;
            } else break;
        }
        return saved;
    }

    void Session::KeepWindow(const kor::Window& window)
    {
        if (!keepsWindow || window.IsOffscreen() || std::getenv("KORAL_ENGINE_NO_LAYOUT")) return;
        // Maximized, the size and place it had before are what it goes back to: kept as they were.
        SavedWindow kept = LastWindow();
        kept.maximized = window.IsMaximized();
        if (!kept.maximized) {
            kept.extent = window.Extent();
            kept.position = kor::Window::CanBePositioned() ? std::optional(window.Position()) : std::nullopt;
        }
        std::error_code ignored;
        std::filesystem::create_directories(ConfigDirectory(), ignored);
        std::ofstream out(ConfigDirectory() / "window", std::ios::trunc);
        out << "extent " << kept.extent.x << ' ' << kept.extent.y << '\n';
        if (kept.position) out << "position " << kept.position->x << ' ' << kept.position->y << '\n';
        out << "maximized " << (kept.maximized ? 1 : 0) << '\n';
    }

    void Session::ChooseTheme(const kui::ThemeDesign design, const bool dark)
    {
        themeChosen = kui::Theme::Of(design, dark);
        // Kept for the next run — but not by an editor that keeps nothing of this user's (a test's).
        if (!std::getenv("KORAL_ENGINE_NO_LAYOUT") && !SetSavedTheme(design, dark)) Say("The theme could not be kept for the next run.", true);
    }

    std::vector<Session::ViewModeChoice> Session::ViewModes() const
    {
        std::vector<ViewModeChoice> modes;
        modes.push_back({ {}, "Lit", "" });
        for (const auto& [channel, name] : MaterialChannels) modes.push_back({ { .kind = ViewMode::Kind::eMaterial, .channel = channel }, name, "Material" });
        if (Overlay* const over = _play.Over())
            for (auto& view : over->Views()) modes.push_back({ { .kind = ViewMode::Kind::eEditor, .file = view.file, .id = view.id }, view.name, "Editor" });
        for (auto& view : _play.Doc().Runtime().DebugViews()) modes.push_back({ { .kind = ViewMode::Kind::eGraph, .id = view.id }, view.name, "Scene graph" });
        return modes;
    }

    void Session::SetViewMode(ViewMode mode)
    {
        if (mode == _viewMode) return;
        _viewMode = std::move(mode);
        ++_changes;
    }

    std::vector<std::string> Session::WithoutMaterialViews() const { return _play.Doc().Runtime().WithoutShaderDebugView(); }

    void Session::SetGrid(const Grid& grid)
    {
        if (grid == _grid) return;
        _grid = grid;
        SaveGrid(_grid);
        ++_changes;
    }

    kor::ResourceRef<const kor::Image> Session::ViewModePicture() const
    {
        return _modeHeld && _modeHeld->handle ? kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(_modeHeld->handle)) : kor::ResourceRef<const kor::Image>();
    }
    kor::ResourceRef<const kor::Image> Session::GridPicture() const
    {
        return _gridHeld && _gridHeld->handle ? kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(_gridHeld->handle)) : kor::ResourceRef<const kor::Image>();
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

    void Session::GoTo(const std::string& id)
    {
        // A node gone to is shown on its page: from the whole frame, gone into.
        if (Doc().Graph().Find(id)) _atFrame = false;
        if (const kgraph::Node* node = Doc().Graph().Find(id); node && !_everyPass && node->pass != _pass) {
            if (node->pass.empty()) _everyPass = true;
            else _pass = node->pass;
        }
        Select(id);
        ++_went;
    }

    void Session::Batch(const std::string& id)
    {
        if (Doc().SetInput(id, "batched", kgraph::Value(true))) ++_changes;
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
        if (const auto* opened = Doc().OpenedOn(page))
            Say(std::format("{} is open: what is done on this page is the function's, written as it is done — every call of it follows. Close it when it is done.", opened->function));
        return true;
    }

    bool Session::OpenFunctionNamed(const std::string& name)
    {
        for (const auto& opened : Doc().OpenedFunctions())
            if (opened.function == name) { ShowPass(opened.pass); return true; }
        for (const auto& node : Doc().Graph().nodes) {
            if (node.type != "Function") continue;
            const auto function = node.inputs.find("function");
            if (function != node.inputs.end() && function->second.value.String() && *function->second.value.String() == name) return OpenFunction(node.id);
        }
        // No call of it in this scene: one for as long as it is open.
        const std::string call = Doc().AddNode(name, 0.0, 0.0);
        if (call.empty()) { Say(std::format("'{}' could not be opened.", name), true); return false; }
        if (!OpenFunction(call)) { Doc().RemoveNodes({ call }); return false; }
        _callsForOpening.insert(call);
        return true;
    }

    bool Session::CloseFunction(const bool keep)
    {
        // The one shown, when a function's page is; else the one last opened.
        const auto* shown = Doc().OpenedOn(_pass);
        const auto opened = shown ? std::optional(*shown) : Doc().Opened();
        if (!opened) return false;
        std::string error;
        if (!Doc().CloseFunctionOn(opened->pass, keep, &error)) { Say(error, true); return false; }
        if (!opened->call.pass.empty()) ShowPass(opened->call.pass);
        if (_callsForOpening.erase(opened->call.id)) Doc().RemoveNodes({ opened->call.id });
        else Select(opened->call.id);
        Say(keep ? std::format("Closed: functions/{}.kfunc is what its page was, and every call follows.", opened->function)
                 : std::format("{} is as it was when it was opened, and every call with it.", opened->function));
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
        if (orthographic) {
            // As much of the scene as the perspective showed at the target, and what is behind where it is seen from too.
            view.orthographic = 2.f * distance * std::tan(fieldOfView * Radians / 2.f);
            view.near = -far;
        }
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

    void Session::LookAlong(const Axis axis, const bool perspective, EditorCamera* view)
    {
        EditorCamera& turned = view ? *view : camera;
        // Just short of straight up or down: a view looks at its target with the world's up as its own.
        switch (axis) {
        case Axis::eTop: turned.yaw = 0.f; turned.pitch = -89.9f; break;
        case Axis::eBottom: turned.yaw = 0.f; turned.pitch = 89.9f; break;
        case Axis::eFront: turned.yaw = 0.f; turned.pitch = 0.f; break;
        case Axis::eBack: turned.yaw = 180.f; turned.pitch = 0.f; break;
        case Axis::eRight: turned.yaw = 90.f; turned.pitch = 0.f; break;
        case Axis::eLeft: turned.yaw = -90.f; turned.pitch = 0.f; break;
        }
        turned.orthographic = !perspective;
        if (!view) lookThroughCamera = false;
        ++_changes;
    }

    void Session::SetOrthographic(const bool orthographic)
    {
        camera.orthographic = orthographic;
        lookThroughCamera = false;
        ++_changes;
    }

    std::vector<kecs::Entity> Session::Selection() const
    {
        const kecs::World& world = _play.Doc().World();
        std::vector<kecs::Entity> all;
        if (world.Alive(selectedEntity)) all.push_back(selectedEntity);
        for (const kecs::Entity entity : alsoSelected)
            if (world.Alive(entity) && !std::ranges::contains(all, entity)) all.push_back(entity);
        return all;
    }

    void Session::SetSplit(const bool split)
    {
        if (split && !splitView) secondCamera = camera;     // from where the first is: then turned away from it
        splitView = split;
        ++_changes;
    }

    void Session::SelectAlso(const kecs::Entity entity)
    {
        if (!Doc().World().Alive(entity)) return;
        if (!Doc().World().Alive(selectedEntity)) { Select(entity); return; }
        if (entity == selectedEntity) {
            // The first let go of: the next is the first.
            selectedEntity = alsoSelected.empty() ? kecs::NullEntity : alsoSelected.front();
            if (!alsoSelected.empty()) alsoSelected.erase(alsoSelected.begin());
        } else if (const auto it = std::ranges::find(alsoSelected, entity); it != alsoSelected.end()) {
            alsoSelected.erase(it);
        } else {
            alsoSelected.push_back(entity);
        }
        selectedNode.clear();
        selectedAsset.clear();
        ++_changes;
    }

    void Session::SelectInBox(const kor::Vec2 from, const kor::Vec2 to, const bool adding)
    {
        Document& doc = Doc();
        kecs::World& world = doc.World();
        Overlay* const over = _play.Over();
        const kor::Mat4 viewProjection = over ? over->ViewProjection() : doc.Runtime().ViewProjection();
        const kor::UVec2 size = viewport->Size();
        const kor::Vec2 low(std::min(from.x, to.x), std::min(from.y, to.y)), high(std::max(from.x, to.x), std::max(from.y, to.y));
        const kecs::ComponentId meshes = world.Find("Mesh");
        if (!adding) { selectedEntity = kecs::NullEntity; alsoSelected.clear(); }
        // What is drawn and seen, whose box's middle is inside the one dragged.
        world.EachEntity([&](const kecs::Entity entity) {
            if (meshes == kecs::NoComponent || !world.Has(entity, meshes) || !world.IsEnabled(entity, meshes) || _viewHidden.contains(entity)) return;
            const kor::Mat4 matrix = kgraph::ToKor(world.WorldMatrix(entity));
            const auto box = MeshBox(doc, entity);
            const kor::Vec3 middle = box ? (box->first + box->second) * 0.5f : kor::Vec3(0.f);
            const auto at = OnScreen(viewProjection, kor::Vec3(matrix * kor::Vec4(middle, 1.f)), size);
            if (!at || at->x < low.x || at->y < low.y || at->x > high.x || at->y > high.y) return;
            if (!world.Alive(selectedEntity)) selectedEntity = entity;
            else if (entity != selectedEntity && !std::ranges::contains(alsoSelected, entity)) alsoSelected.push_back(entity);
        });
        selectedNode.clear();
        selectedAsset.clear();
        const std::size_t picked = Selection().size();
        Say(picked == 0 ? std::string("Nothing drawn is in the box.") : std::format("Picked {} {}.", picked, picked == 1 ? "entity" : "entities"));
        ++_changes;
    }

    void Session::ReadBookmarks()
    {
        const std::string scene = _hasProject ? (_project.directory / _scene).generic_string() : std::string();
        if (scene == _bookmarksOf) return;
        _bookmarksOf = scene;
        _bookmarks = {};
        if (scene.empty() || std::getenv("KORAL_ENGINE_NO_LAYOUT")) return;
        std::ifstream in(BookmarksFile(scene));
        int slot = 0;
        EditorCamera seen;
        int orthographic = 0;
        while (in >> slot >> seen.target.x >> seen.target.y >> seen.target.z >> seen.yaw >> seen.pitch >> seen.distance >> orthographic)
            if (slot >= 0 && slot < 9) { seen.orthographic = orthographic != 0; _bookmarks[slot] = seen; }
    }

    void Session::SaveBookmark(const int slot)
    {
        if (slot < 0 || slot >= 9) return;
        ReadBookmarks();
        _bookmarks[slot] = camera;
        Say(std::format("The view is bookmark {}: {} goes back to it.", slot + 1, slot + 1));
        if (_bookmarksOf.empty() || std::getenv("KORAL_ENGINE_NO_LAYOUT")) return;
        std::error_code ignored;
        std::filesystem::create_directories(BookmarksFile(_bookmarksOf).parent_path(), ignored);
        std::ofstream out(BookmarksFile(_bookmarksOf), std::ios::trunc);
        for (int i = 0; i < 9; ++i)
            if (const auto& mark = _bookmarks[i])
                out << i << ' ' << mark->target.x << ' ' << mark->target.y << ' ' << mark->target.z << ' ' << mark->yaw << ' ' << mark->pitch << ' '
                    << mark->distance << ' ' << (mark->orthographic ? 1 : 0) << '\n';
    }

    bool Session::GoToBookmark(const int slot)
    {
        ReadBookmarks();
        if (slot < 0 || slot >= 9 || !_bookmarks[slot]) return false;
        const EditorCamera& mark = *_bookmarks[slot];
        camera.target = mark.target;
        camera.yaw = mark.yaw;
        camera.pitch = mark.pitch;
        camera.distance = mark.distance;
        camera.orthographic = mark.orthographic;
        lookThroughCamera = false;
        ++_changes;
        return true;
    }

    bool Session::HasBookmark(const int slot) const { return slot >= 0 && slot < 9 && _bookmarks[slot].has_value(); }

    // ---- hidden in the scene view ---------------------------------------------------------------------------------

    void Session::DropAt(const float u, const float v, std::function<void()> then)
    {
        placeAt.reset();
        Overlay* const over = _play.Over();
        if (!over || ThroughSceneCamera() || !over->PickAt(u, v, [this, then](const kgraph::Runtime::PickHit& hit) {
                Probed(hit);
                if (_probe.hit) placeAt = _probe.position;
                then();
            }))
            then();
    }

    void Session::HideSelected()
    {
        const auto picked = Selection();
        if (picked.empty()) return;
        // What is under each goes with it, as the outliner's eye does.
        const kecs::World& world = Doc().World();
        const std::function<void(kecs::Entity)> hide = [&](const kecs::Entity entity) {
            _viewHidden.insert(entity);
            for (const kecs::Entity child : world.ChildrenOf(entity)) hide(child);
        };
        for (const kecs::Entity entity : picked) hide(entity);
        Say(std::format("Hidden in the scene view: {}. Alt+H shows everything again.", picked.size() == 1 ? Doc().NameOf(picked.front()) : std::format("{} entities", picked.size())));
        ++_changes;
    }

    void Session::Isolate()
    {
        const auto picked = Selection();
        if (picked.empty()) return;
        const kecs::World& world = Doc().World();
        std::set<kecs::Entity> kept;
        const std::function<void(kecs::Entity)> keep = [&](const kecs::Entity entity) {
            kept.insert(entity);
            for (const kecs::Entity child : world.ChildrenOf(entity)) keep(child);
        };
        for (const kecs::Entity entity : picked) keep(entity);
        _viewHidden.clear();
        Doc().World().EachEntity([&](const kecs::Entity entity) { if (!kept.contains(entity)) _viewHidden.insert(entity); });
        _isolated = true;
        Say("Shown by itself in the scene view. Alt+H shows everything again.");
        ++_changes;
    }

    void Session::ShowAll()
    {
        if (_viewHidden.empty() && !_isolated) return;
        _viewHidden.clear();
        _isolated = false;
        ++_changes;
    }

    void Session::Steer(EditorCamera& view, kgui::ViewportState& port, std::optional<kor::MouseButton>& drag, const kor::Input& input, const kor::Vec2 delta,
                        const float seconds)
    {
        const kor::UVec2 size = port.Size();
        if (drag && !input.IsMouseButtonHeld(*drag) && !input.IsMouseButtonPressed(*drag)) drag.reset();
        if (!drag && port.Hovered())
            for (const kor::MouseButton button : { kor::MouseButton::eRight, kor::MouseButton::eMiddle })
                if (input.IsMouseButtonPressed(button)) { drag = button; break; }
        if (drag == kor::MouseButton::eRight) {
            // Looked round from where it is, and flown with the keys while the button is held — as a scene's own
            // FlyCamera is: W A S D along and across the way it looks, E and Q up and down, Shift faster.
            view.turnLeft += delta * 0.5f;      // (the mouse's own counts while it is captured: finer than a screen's pixels)
            const auto key = [&](const kor::Key held) { return !input.InterfaceWantsKeyboard() && input.IsKeyHeld(held) ? 1.f : 0.f; };
            const float along = key(kor::Key::eW) - key(kor::Key::eS), across = key(kor::Key::eD) - key(kor::Key::eA), up = key(kor::Key::eE) - key(kor::Key::eQ);
            if (along != 0.f || across != 0.f || up != 0.f) {
                const kor::Vec3 forward = kor::Normalize(view.target - view.Position());
                const kor::Vec3 right = kor::Normalize(kor::Cross(forward, kor::Vec3(0.f, 1.f, 0.f)));
                const float step = std::max(view.distance, 2.f) * 1.2f * seconds * flySpeed * (key(kor::Key::eLeftShift) > 0.f ? 4.f : 1.f);
                // Nothing the panels show is where the view is: moving it is no change for them to be made again for.
                view.target += (forward * along + right * across + kor::Vec3(0.f, up, 0.f)) * step;
            }
        }
        else if (drag == kor::MouseButton::eMiddle) view.Pan(delta.x, delta.y, static_cast<float>(std::max(size.y, 1u)));
        // Looked round from where it is, a little of what the mouse did at a time: over about 15 ms, whatever a frame takes.
        if (view.turnLeft != kor::Vec2(0.f)) {
            const float taken = drag == kor::MouseButton::eRight ? 1.f - std::exp(-seconds / 0.015f) : 1.f;
            const kor::Vec2 turn = view.turnLeft * taken;
            view.turnLeft -= turn;
            if (std::abs(view.turnLeft.x) + std::abs(view.turnLeft.y) < 0.01f) view.turnLeft = kor::Vec2(0.f);
            const kor::Vec3 eye = view.Position();
            view.Orbit(turn.x, turn.y);
            view.target += eye - view.Position();
        }
        // The wheel zooms — or, flying, makes the flight faster or slower.
        if (port.Hovered())
            if (const float wheel = input.MouseScrollDelta().y; wheel != 0.f) {
                if (drag == kor::MouseButton::eRight) {
                    flySpeed = std::clamp(flySpeed * std::pow(1.25f, wheel), 0.05f, 50.f);
                    Say(std::format("Flying at {:.2f}x", flySpeed));
                } else view.Zoom(wheel);
            }
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
            _frameMs = 1000.0 * _secondsCounted / _framesCounted;
            _framesCounted = 0;
            _secondsCounted = 0.f;
        }

        const kecs::Entity selected = world.Alive(selectedEntity) ? selectedEntity : kecs::NullEntity;
        std::erase_if(alsoSelected, [&](const kecs::Entity entity) { return !world.Alive(entity) || entity == selected; });
        runtime.SetSelected(selected);
        // Picking, the line round what is picked and the gizmo are the editor's own passes: no part of the scene's graph.
        Overlay* const over = _play.Over();
        if (over) over->SetSelected(selected);
        // Hidden in this view only — not in the Game panel's, not in the scene saved. While what is picked is dragged
        // onto a surface, the editor's own picking does not see it: the surface under it is what is found.
        std::erase_if(_viewHidden, [&](const kecs::Entity entity) { return !world.Alive(entity); });
        runtime.SetHidden(_viewHidden);
        if (over) {
            std::set<kecs::Entity> unseen = _viewHidden;
            if (surfaceSnap && _gizmoMoved && gizmo == kor::GizmoMode::eTranslate && viewport->Dragging())
                for (const kecs::Entity entity : Selection()) unseen.insert(entity);
            over->SetHidden(std::move(unseen));
        }
        ReadBookmarks();

        // The scene view's keys, while it is what was last clicked and nothing is typed into: a digit goes to a
        // bookmark of the view (Ctrl and one saves it); H hides what is picked, Shift+H shows it by itself, Alt+H all.
        if ((viewport->Focused() || viewport->Hovered()) && !input.InterfaceWantsKeyboard() && !_viewDrag) {
            const bool ctrl = input.IsKeyHeld(kor::Key::eLeftControl) || input.IsKeyHeld(kor::Key::eRightControl);
            const bool shift = input.IsKeyHeld(kor::Key::eLeftShift), alt = input.IsKeyHeld(kor::Key::eLeftAlt);
            for (int slot = 0; slot < 9; ++slot)
                if (input.IsKeyPressed(static_cast<kor::Key>(static_cast<int>(kor::Key::eNum1) + slot))) {
                    if (ctrl) SaveBookmark(slot);
                    else if (!GoToBookmark(slot)) Say(std::format("There is no bookmark {}: Ctrl+{} saves the view as it.", slot + 1, slot + 1));
                }
            if (input.IsKeyPressed(kor::Key::eH)) {
                if (alt) ShowAll();
                else if (shift) Isolate();
                else HideSelected();
            }
        }
        // Its picture is made again when the panel is another size: the interface is told, to show the one there is now.
        // The meshes the Assets panel lists are drawn small, a few frames each, until each has its picture: what shows
        // one is built again by itself (MeshPicturesMade), not the whole interface.
        if (_thumbnails && _thumbnails->Update()) ++_meshPicturesMade;
        // A file read since: what the Assets panel shows of it is another thing now.
        if (const std::size_t reading = Doc().Reading(); reading != _reading) { _reading = reading; ++_changes; }
        // Cooking pictures: the status bar says how far it is, and what could not be cooked once it is done.
        if (const auto& cooking = Doc().Runtime().Cooking(); cooking.total > 0 && cooking.done != _cookedShown) {
            _cookedShown = cooking.done;
            if (cooking.done == cooking.total) {
                if (cooking.failed.empty()) Say(std::format("{} {} compressed", cooking.total, cooking.total == 1 ? "texture" : "textures"));
                else Say(std::format("{} of {} textures could not be compressed: {}", cooking.failed.size(), cooking.total, cooking.failed.front()), true);
            }
            ++_changes;
        }
        // The one it shows meanwhile is kept until it has stopped showing it, a few frames on.
        std::erase_if(_overBefore, [](auto& kept) { return --kept.second <= 0; });
        const auto hold = [this](std::shared_ptr<kgraph::Resource>& showing, std::shared_ptr<kgraph::Resource> held) {
            if (held == showing) return;
            if (showing) _overBefore.emplace_back(std::move(showing), 6);
            showing = std::move(held);
            ++_changes;
        };
        hold(_overHeld, over ? over->PictureHeld() : nullptr);

        // How the Scene panel shows the scene, told to the scene's runtime — the Game panel's is another, and lit — and
        // to the editor's graphs. A DebugView no longer there (the graph was changed) is the scene lit again.
        runtime.SetEditorView(true);
        if (!std::exchange(_gridRead, true)) _grid = ReadGrid();
        if (_viewMode.kind == ViewMode::Kind::eGraph || _viewMode.kind == ViewMode::Kind::eEditor) {
            const auto modes = ViewModes();
            if (std::ranges::find(modes, _viewMode, &ViewModeChoice::mode) == modes.end()) { _viewMode = {}; ++_changes; }
        }
        const bool graphView = _viewMode.kind == ViewMode::Kind::eGraph, editorView = _viewMode.kind == ViewMode::Kind::eEditor;
        runtime.ShowDebugView(graphView ? _viewMode.id : std::string());
        runtime.SetShaderDebugView(_viewMode.kind == ViewMode::Kind::eMaterial ? static_cast<int>(_viewMode.channel) : 0);
        if (over) {
            over->ShowView(editorView ? _viewMode.file : std::filesystem::path(), editorView ? _viewMode.id : std::string());
            over->SetGrid(_grid);
            over->SetProjectViews(_hasProject ? _project.directory / "editor" : std::filesystem::path());
        }
        hold(_modeHeld, graphView ? runtime.ResourceOf(_viewMode.id) : editorView && over ? over->ViewPictureHeld() : nullptr);
        hold(_gridHeld, over ? over->GridHeld() : nullptr);

        // The view, moved by the mouse over the scene's panel: how far it went since the last frame, from where it was.
        // Looking round, the mouse is captured (Aiming), which moves where it says it is: not taken for movement.
        const kor::Vec2 mouse = input.MousePosition();
        if (const bool aiming = _viewDrag == kor::MouseButton::eRight; aiming != _aiming) { _aiming = aiming; _lookSettling = 2; }
        const kor::Vec2 delta = _lastMouse && _lookSettling == 0 ? mouse - *_lastMouse : kor::Vec2(0.f);
        if (_lookSettling > 0) --_lookSettling;
        _lastMouse = mouse;
        const auto pointer = viewport->PointerPosition();
        const kor::UVec2 size = viewport->Size();
        // The second view, split beside the first: steered the same way, and drawn as the first is.
        if (GameScene* second = _second) {
            if (splitView) Steer(secondCamera, *secondViewport, _secondDrag, input, delta, seconds);
            else _secondDrag.reset();
            second->SetShown(splitView);
            second->SetView(splitView ? std::optional(secondCamera.View()) : std::nullopt, _viewHidden);
        }
        if (ThroughSceneCamera()) {
            _viewDrag.reset();
            runtime.SetView(std::nullopt);
            if (over) over->SetView(std::nullopt);
        } else {
            Steer(camera, *viewport, _viewDrag, input, delta, seconds);
            runtime.SetView(camera.View());
            if (over) over->SetView(camera.View());

            // What the editor's view shows of what is not drawn: each scene camera as the pyramid it sees through,
            // and the picked light's reach.
            const float aspect = over ? over->Aspect() : runtime.Aspect();
            // Which they are: a camera is of the Camera kind; a light goes by what its component holds, as a View node
            // goes — a range and a cone a spot; a range alone a point light; an intensity and no range, light from far away.
            const kecs::ComponentId cameras = world.Find("Camera");
            const auto isCamera = [&](const kecs::Entity entity) {
                return cameras != kecs::NoComponent && world.Has(entity, cameras) && world.IsEnabled(entity, cameras);
            };
            const auto number = [&](const kecs::Entity entity, const kecs::ComponentId id, const char* name) -> std::optional<float> {
                const kecs::FieldDescription* field = world.Describe(id).FindField(name);
                const auto* bytes = static_cast<const std::byte*>(world.Get(entity, id));
                if (!field || !bytes || field->type != "f32") return std::nullopt;
                float v = 0.f;
                std::memcpy(&v, bytes + field->offset, 4);
                return v;
            };
            world.EachEntity([&](const kecs::Entity entity) {
                if (!isCamera(entity)) return;
                const kgraph::View lens = kgraph::ViewOf(world, entity);
                debug.Camera(lens.view, lens.Projection(aspect, false), 0.5f);
            });
            // An icon where each light, camera and entity that draws nothing is, the same size whatever its distance:
            // a click on one picks it (what draws nothing the picking pass cannot find).
            _icons.clear();
            if (iconsShown) {
                const kecs::ComponentId meshes = world.Find("Mesh");
                const kgraph::View seen = camera.View();
                const kor::Vec3 eye = camera.Position(), forward = kor::Normalize(camera.target - eye);
                world.EachEntity([&](const kecs::Entity entity) {
                    if (_viewHidden.contains(entity)) return;
                    if (meshes != kecs::NoComponent && world.Has(entity, meshes) && world.IsEnabled(entity, meshes)) return;
                    enum class Kind { eLight, eCamera, eEmpty } kind = isCamera(entity) ? Kind::eCamera : Kind::eEmpty;
                    for (const kecs::ComponentId id : world.ComponentsOf(entity)) {
                        if (kind == Kind::eCamera || !world.IsEnabled(entity, id)) continue;
                        if (number(entity, id, "intensity")) kind = Kind::eLight;
                    }
                    // An entity of nothing but a place is shown only when it has nothing under it to show where it is.
                    if (kind == Kind::eEmpty && !world.ChildrenOf(entity).empty()) return;
                    const kecs::Mat4 matrix = world.WorldMatrix(entity);
                    const kor::Vec3 at(matrix.At(3, 0), matrix.At(3, 1), matrix.At(3, 2));
                    const float far = seen.orthographic > 0.f ? seen.orthographic : std::max(kor::Dot(at - eye, forward), 0.05f) * 2.f * std::tan(seen.fieldOfView / 2.f);
                    const float size = far * 0.022f;
                    const kor::Vec3 facing = seen.orthographic > 0.f ? -forward : kor::Normalize(eye - at);
                    const bool picked = std::ranges::contains(Selection(), entity);
                    kor::DebugDraw::Style style;
                    style.onTop = true;
                    style.color = picked ? kor::Vec4(1.f, 0.55f, 0.15f, 1.f) : kind == Kind::eLight ? kor::Vec4(1.f, 0.85f, 0.35f, 1.f)
                                : kind == Kind::eCamera ? kor::Vec4(0.55f, 0.8f, 1.f, 1.f) : kor::Vec4(0.75f, 0.75f, 0.8f, 1.f);
                    style.fill = kor::Vec4(style.color.x, style.color.y, style.color.z, 0.35f);
                    const kor::Vec3 right = kor::Normalize(kor::Cross(facing, std::abs(facing.y) > 0.99f ? kor::Vec3(0.f, 0.f, 1.f) : kor::Vec3(0.f, 1.f, 0.f)));
                    const kor::Vec3 up = kor::Cross(right, facing);
                    if (kind == Kind::eLight) {
                        // A sun: a disc and its rays.
                        debug.Circle(at, facing, size * 0.45f, style, 16);
                        kor::DebugDraw::Style ray = style;
                        ray.fill = kor::Vec4(0.f);
                        for (int i = 0; i < 8; ++i) {
                            const float a = static_cast<float>(i) * 0.785398f;
                            const kor::Vec3 way = right * std::cos(a) + up * std::sin(a);
                            debug.Line(at + way * (size * 0.65f), at + way * size, ray);
                        }
                    } else if (kind == Kind::eCamera) {
                        // A box and its lens.
                        debug.Quad(at - right * size * 0.6f - up * size * 0.4f, at + right * size * 0.3f - up * size * 0.4f,
                                   at + right * size * 0.3f + up * size * 0.4f, at - right * size * 0.6f + up * size * 0.4f, style);
                        debug.Triangle(at + right * size * 0.3f, at + right * size * 0.75f - up * size * 0.35f, at + right * size * 0.75f + up * size * 0.35f, style);
                    } else {
                        // A diamond: a place, and nothing more.
                        debug.Quad(at + up * size * 0.5f, at + right * size * 0.5f, at - up * size * 0.5f, at - right * size * 0.5f, style);
                    }
                    _icons.emplace_back(entity, at);
                });
            }
            if (!selected.IsNull()) {
                kor::Vec3 position, scale;
                kor::Quat rotation;
                if (kor::Decompose(kgraph::ToKor(world.WorldMatrix(selected)), position, rotation, scale)) {
                    const kor::Vec3 forward = rotation * kor::Vec3(0.f, 0.f, -1.f);
                    for (const kecs::ComponentId id : world.ComponentsOf(selected)) {
                        const auto range = number(selected, id, "range"), outer = number(selected, id, "outerAngle"), inner = number(selected, id, "innerAngle");
                        if (range && outer) debug.SpotLight(position, forward, *range, *outer * Radians, inner.value_or(*outer) * Radians);
                        else if (range && number(selected, id, "intensity")) debug.PointLight(position, *range);
                        else if (number(selected, id, "intensity") && !isCamera(selected)) debug.DirectionalLight(position, forward);
                    }
                }
            }
        }

        // The gizmo on what is picked, and what dragging it does to its transform.
        kor::GizmoPointer hand = viewport->GizmoPointer();
        if (!selected.IsNull() && gizmoEnabled && size.x > 0 && size.y > 0 && !_boxFrom) {
            const kor::Mat4 before = kgraph::ToKor(world.WorldMatrix(selected));
            kor::Mat4 transform = before;
            kor::GizmoOptions options;
            options.space = gizmoWorld ? kor::GizmoSpace::eWorld : kor::GizmoSpace::eLocal;
            // Snapped as it is set, or the other way while Ctrl is held.
            const bool ctrl = input.IsKeyHeld(kor::Key::eLeftControl) || input.IsKeyHeld(kor::Key::eRightControl);
            if (snap.on != ctrl) options.snap = gizmo == kor::GizmoMode::eTranslate ? snap.move : gizmo == kor::GizmoMode::eRotate ? snap.turn : snap.scale;
            const char* what = gizmo == kor::GizmoMode::eTranslate ? "position" : gizmo == kor::GizmoMode::eRotate ? "rotation" : "scale";
            if (debug.Gizmo(gizmo, transform, over ? over->ViewProjection() : runtime.ViewProjection(), hand, options)) {
                _gizmoMoved = true;
                // Put on the surface under the pointer, its box's foot on it, rather than where the axis took it.
                if (surfaceSnap && gizmo == kor::GizmoMode::eTranslate && _probe.hit) {
                    const auto box = MeshBox(doc, selected);
                    const kor::Vec3 up = kor::Normalize(_probe.normal);
                    float lift = 0.f;
                    if (box) {
                        // How far below its origin its box reaches, along the surface's up, as it is turned and scaled now.
                        lift = std::numeric_limits<float>::max();
                        for (int corner = 0; corner < 8; ++corner) {
                            const kor::Vec3 local((corner & 1) ? box->second.x : box->first.x, (corner & 2) ? box->second.y : box->first.y, (corner & 4) ? box->second.z : box->first.z);
                            const kor::Vec3 offset = kor::Vec3(before * kor::Vec4(local, 1.f)) - kor::Vec3(before[3]);
                            lift = std::min(lift, kor::Dot(offset, up));
                        }
                        lift = -lift;
                    }
                    transform = before;
                    const kor::Vec3 to = _probe.position + up * lift;
                    transform[3] = kor::Vec4(to, 1.f);
                }
                // Only what this gizmo moves: taking the rest back out of the matrix would round it off a little each frame.
                world.SetWorldMatrix(selected, kgraph::ToKecs(transform), gizmo == kor::GizmoMode::eTranslate, gizmo == kor::GizmoMode::eRotate,
                                     gizmo == kor::GizmoMode::eScale);
                // Not the interface built again for it (each frame of the drag): what shows where it is follows it (LiveValue).
                doc.Moved(selected, what);
                // The rest of what is picked, moved as the first was: turned and scaled round it.
                const kor::Mat4 change = transform * kor::Inverse(before);
                for (const kecs::Entity other : Selection()) {
                    if (other == selected) continue;
                    const kor::Mat4 moved = change * kgraph::ToKor(world.WorldMatrix(other));
                    world.SetWorldMatrix(other, kgraph::ToKecs(moved), true, gizmo != kor::GizmoMode::eTranslate, gizmo == kor::GizmoMode::eScale);
                    doc.Moved(other, what);
                }
            }
        }
        // What else is picked shows by a box round it: the line round the first is the picking pass's own.
        for (const kecs::Entity other : Selection()) {
            if (other == selected) continue;
            kor::DebugDraw::Style style;
            style.color = kor::Vec4(1.f, 0.55f, 0.15f, 0.9f);
            const kor::Mat4 matrix = kgraph::ToKor(world.WorldMatrix(other));
            if (const auto box = MeshBox(doc, other)) {
                const kor::Vec3 middle = (box->first + box->second) * 0.5f, half = (box->second - box->first) * 0.5f;
                debug.Box(matrix * kor::Translate(kor::Mat4(1.f), middle) * kor::Scale(kor::Mat4(1.f), half * 2.f), style);
            } else {
                debug.Point(kor::Vec3(matrix[3]), 0.1f, style);
            }
        }

        // What is under the pointer, asked for every other frame while it is wanted: what the probe says, the ruler's
        // ends, a surface to put what is dragged on. The answer comes a few frames on.
        const bool probing = measure != Measure::eOff || (surfaceSnap && gizmo == kor::GizmoMode::eTranslate && viewport->Dragging());
        if (probing && over && pointer && size.x > 0 && size.y > 0 && ++_probing >= 2) {
            _probing = 0;
            over->PickAt(pointer->x / static_cast<float>(size.x), pointer->y / static_cast<float>(size.y), [this](const kgraph::Runtime::PickHit& hit) { Probed(hit); });
        }
        if (!probing && _probe.hit) { _probe = {}; ++_changes; }

        // A click on the picture — the button let go where it went down, the gizmo not dragged — picks what is under it:
        // an icon, nearest first, before what is drawn. With Ctrl or Shift, it is picked as well as what is; measuring,
        // it is a ruler's end. Dragged from where nothing moves, it is a box round what is to be picked.
        const bool down = hand.down;
        const bool adding = input.IsKeyHeld(kor::Key::eLeftControl) || input.IsKeyHeld(kor::Key::eRightControl) || input.IsKeyHeld(kor::Key::eLeftShift);
        if (down && !_wasDown) { _pressAt = pointer; _gizmoMoved = false; }
        if (down && _pressAt && pointer && !_gizmoMoved && !_boxFrom && measure == Measure::eOff && kor::Length(*pointer - *_pressAt) >= 6.f) _boxFrom = _pressAt;
        if (_boxFrom && pointer) {
            // Drawn on the near plane, where it is in the picture.
            const kor::Mat4 inverse = kor::Inverse(over ? over->ViewProjection() : runtime.ViewProjection());
            const kor::Vec2 a = *_boxFrom, b = *pointer;
            const float depth = camera.orthographic ? 0.5f : 0.001f;
            const kor::Vec3 p0 = InScene(inverse, a.x, a.y, size, depth), p1 = InScene(inverse, b.x, a.y, size, depth),
                            p2 = InScene(inverse, b.x, b.y, size, depth), p3 = InScene(inverse, a.x, b.y, size, depth);
            kor::DebugDraw::Style style;
            style.onTop = true;
            style.color = kor::Vec4(1.f, 0.55f, 0.15f, 1.f);
            style.fill = kor::Vec4(1.f, 0.55f, 0.15f, 0.12f);
            debug.Quad(p0, p1, p2, p3, style);
        }
        if (!down && _wasDown) {
            if (_boxFrom && pointer) {
                SelectInBox(*_boxFrom, *pointer, adding);
            } else if (_pressAt && pointer && !_gizmoMoved && kor::Length(*pointer - *_pressAt) < 4.f && size.x > 0 && size.y > 0) {
                const float u = pointer->x / static_cast<float>(size.x), v = pointer->y / static_cast<float>(size.y);
                const kor::Mat4 viewProjection = over ? over->ViewProjection() : runtime.ViewProjection();
                // An icon within a few pixels of the click, the nearest.
                std::optional<std::pair<float, kecs::Entity>> icon;
                for (const auto& [entity, at] : _icons)
                    if (const auto there = OnScreen(viewProjection, at, size))
                        if (const float off = kor::Length(*there - *pointer); off < 12.f && (!icon || off < icon->first)) icon = std::pair { off, entity };
                if (measure == Measure::eRuler) {
                    // A ruler's end where the surface is: its second end, then a new ruler.
                    if (over) over->PickAt(u, v, [this](const kgraph::Runtime::PickHit& hit) {
                        Probed(hit);
                        if (!_probe.hit) { Say("Nothing is there to measure from: click on a surface.", true); return; }
                        if (ruler.size() >= 2) ruler.clear();
                        ruler.push_back(_probe.position);
                        if (ruler.size() == 2) Say(std::format("{:.3f} m", kor::Length(ruler[1] - ruler[0])));
                        ++_changes;
                    });
                } else if (icon) {
                    if (adding) SelectAlso(icon->second);
                    else Select(icon->second);
                } else {
                    // The answer comes a few frames on: by then something else may have been picked, and that, being later, stands.
                    const kecs::Entity before = selectedEntity;
                    const auto picked = [this, before, adding](const kecs::Entity entity) {
                        if (selectedEntity != before) return;
                        if (adding) { if (!entity.IsNull()) SelectAlso(entity); }
                        else Select(entity);
                    };
                    if (over) over->Pick(u, v, picked);
                    else runtime.Pick(u, v, picked);
                }
            }
            _pressAt.reset();
            _boxFrom.reset();
        }
        _wasDown = down;

        // The ruler, and where the probe is.
        kor::DebugDraw::Style measured;
        measured.onTop = true;
        measured.color = kor::Vec4(0.35f, 0.9f, 1.f, 1.f);
        if (measure == Measure::eRuler) {
            for (const kor::Vec3& end : ruler) debug.Point(end, 0.06f, measured);
            if (ruler.size() == 2) debug.Line(ruler[0], ruler[1], measured);
            else if (ruler.size() == 1 && _probe.hit) debug.Line(ruler[0], _probe.position, measured);
        }
        if (measure != Measure::eOff && _probe.hit) {
            const float reach = std::max(_probe.distance * 0.05f, 0.02f);
            debug.Circle(_probe.position, _probe.normal, reach * 0.4f, measured, 20);
            debug.Arrow(_probe.position, _probe.position + kor::Normalize(_probe.normal) * reach, measured);
        }
    }

    void Session::Probed(const kgraph::Runtime::PickHit& hit)
    {
        Probe probe;
        probe.entity = hit.entity;
        // A depth of the far plane (1, or 0 reversed) is nothing there.
        const float depth = hit.depth[4];
        if (hit.deep && hit.extent.x > 0 && depth > 0.f && depth < 1.f) {
            const kor::Mat4 inverse = kor::Inverse(hit.viewProjection);
            const auto at = [&](const int dx, const int dy) {
                return InScene(inverse, static_cast<float>(hit.pixel.x + dx), static_cast<float>(hit.pixel.y + dy), hit.extent, hit.depth[(dy + 1) * 3 + (dx + 1)]);
            };
            probe.hit = true;
            probe.position = at(0, 0);
            // Which way it faces: across the pixels beside it, each way the nearer side (an edge is not a slope).
            const kor::Vec3 left = at(-1, 0), right = at(1, 0), above = at(0, -1), below = at(0, 1);
            const kor::Vec3 across = kor::Length(right - probe.position) < kor::Length(probe.position - left) ? right - probe.position : probe.position - left;
            const kor::Vec3 down = kor::Length(below - probe.position) < kor::Length(probe.position - above) ? below - probe.position : probe.position - above;
            kor::Vec3 normal = kor::Cross(down, across);
            const kor::Vec3 eye = camera.Position();
            if (kor::Length(normal) > 1e-12f) {
                normal = kor::Normalize(normal);
                if (kor::Dot(normal, eye - probe.position) < 0.f) normal = -normal;    // facing the view
                probe.normal = normal;
            }
            probe.distance = kor::Length(eye - probe.position);
        }
        // (What shows it follows it, a live line: not the interface built again each time the pointer moves.)
        _probe = probe;
    }

    // ---- a prefab, opened ---------------------------------------------------------------------------------------

    bool Session::NewPrefab(const std::string& name)
    {
        if (!_hasProject) { Say("A prefab goes in a project's prefabs folder: File, New project.", true); return false; }
        std::string clean = name;
        while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.back()))) clean.pop_back();
        if (clean.empty() || std::ranges::any_of(clean, [](const unsigned char c) { return !std::isalnum(c) && c != ' ' && c != '_' && c != '-'; })) {
            Say(std::format("'{}' is not a name a prefab can have: letters, digits, spaces, '-' and '_'.", name), true);
            return false;
        }
        const auto file = _project.directory / "prefabs" / (clean + ".prefab");
        std::error_code ignored;
        if (std::filesystem::exists(file, ignored)) { Say(std::format("There is a prefab called {} already.", clean), true); return false; }
        // Of one empty entity, written from a world of its own: what is opened is then that.
        if (_prefab && !ClosePrefab(true)) return false;
        if (InPlayMode()) Stop();
        const std::string scene = Doc().WorldJson();
        const bool dirty = Doc().Dirty();
        for (const kecs::Entity root : std::vector(Doc().World().Roots())) Doc().DestroyEntity(root);
        const kecs::Entity made = Doc().CreateEntity(clean);
        std::string error;
        std::filesystem::create_directories(file.parent_path(), ignored);
        const bool written = Doc().SaveAsPrefab(made, file, &error);
        std::string restored;
        Doc().RestoreWorld(scene, restored);
        Doc().SetDirty(dirty);
        if (!written) { Say(error, true); return false; }
        return OpenPrefab("prefab:" + clean + ".prefab");
    }

    bool Session::OpenPrefab(const std::string& id)
    {
        if (!_hasProject || !id.starts_with("prefab:")) { Say("Only a project's prefab is opened by itself.", true); return false; }
        const std::string name = "prefabs/" + id.substr(7);
        if (_prefab && _prefab->name == name) return true;
        if (_prefab && !ClosePrefab(true)) return false;
        if (InPlayMode()) Stop();
        Commit();
        Document& doc = Doc();
        OpenPrefabState open;
        open.name = name;
        open.file = _project.directory / name;
        open.scene = doc.WorldJson();
        open.sceneDirty = doc.Dirty();
        open.undo = std::move(_undo);
        open.redo = std::move(_redo);
        open.now = _now;
        // The prefab alone: an instance of it, made entities like any — what is under it of another prefab stays an instance of that.
        for (const kecs::Entity root : std::vector(doc.World().Roots())) doc.DestroyEntity(root);
        std::string error;
        const kecs::Entity root = doc.Instantiate(open.file, kecs::NullEntity, &error);
        if (root.IsNull()) {
            std::string restored;
            doc.RestoreWorld(open.scene, restored);
            doc.SetDirty(open.sceneDirty);
            _undo = std::move(open.undo);
            _redo = std::move(open.redo);
            Say(error.empty() ? std::format("{} could not be opened.", name) : error, true);
            return false;
        }
        doc.Unlink(root);
        _prefab = std::move(open);
        _undo.clear();
        _redo.clear();
        _now = { doc.Graph().ToJson(), doc.WorldJson() };
        _undoRevision = doc.Revision();
        _undoPending = false;
        _prefab->saved = doc.Revision();
        _folded.clear();
        Select(root);           // its top entity in the inspector, as the outliner has it
        assetFolder = "prefabs";
        Say(std::format("Editing {}: the scene is put aside. Save writes the prefab; Back to the scene, and every instance of it follows.", name));
        ++_changes;
        return true;
    }

    bool Session::PrefabChanged() { return _prefab && Doc().Revision() != _prefab->saved; }

    bool Session::SavePrefab()
    {
        if (!_prefab) return false;
        Document& doc = Doc();
        const auto& roots = doc.World().Roots();
        if (roots.empty()) { Say("A prefab is at least one entity: there is none to save.", true); return false; }
        const kecs::Entity root = roots.front();
        std::string error;
        if (!doc.SaveAsPrefab(root, _prefab->file, &error)) { Say(error, true); return false; }
        doc.Unlink(root);       // saved, it is what is edited still, not an instance of itself
        _prefab->saved = doc.Revision();
        Say(roots.size() > 1 ? std::format("Saved {} — {} and what is under it: what is beside it at the top is no part of it.", _prefab->name, doc.NameOf(root))
                             : std::format("Saved {}.", _prefab->name), roots.size() > 1);
        ++_changes;
        return true;
    }

    bool Session::ClosePrefab(const bool save)
    {
        if (!_prefab) return false;
        if (save && PrefabChanged() && !SavePrefab()) return false;
        Document& doc = Doc();
        OpenPrefabState open = std::move(*_prefab);
        _prefab.reset();
        std::string error;
        if (!doc.RestoreWorld(open.scene, error)) Say(error, true);
        // Its instances as it is now — and the scene, changed by that only where an instance was.
        doc.UpdatePrefabs(open.file);
        const bool followed = doc.WorldJson() != open.scene;
        doc.SetDirty(open.sceneDirty || followed);
        _undo = std::move(open.undo);
        _redo = std::move(open.redo);
        _now = { doc.Graph().ToJson(), doc.WorldJson() };
        _undoRevision = doc.Revision();
        _undoPending = false;
        _folded.clear();
        selectedEntity = kecs::NullEntity;
        Say(followed ? std::format("Back to the scene: its instances of {} follow it.", open.name) : std::string("Back to the scene."));
        ++_changes;
        return true;
    }

    // ---- playing ------------------------------------------------------------------------------------------------

    void Session::StartPlaying()
    {
        if (_prefab) { Say("A prefab is open: back to the scene to play it.", true); return; }
        if (_snapshot.empty()) {
            _play.Restart();       // from the beginning: what setup made starts from the world as it is
            _snapshot = Doc().WorldJson();
            // Scripts nothing is running: said, since a scene that plays with them idle looks like a script that does nothing.
            if (const auto idle = ScriptsNotRunning(); !idle.empty())
                Say(std::format("Playing, but {}{} {} not running: the Problems say why.", idle.front(),
                                idle.size() > 1 ? std::format(" and {} more", idle.size() - 1) : std::string(), idle.size() > 1 ? "are" : "is"), true);
            else if (const kecs::ComponentId camera = Doc().World().Find("Camera");
                     camera == kecs::NoComponent || std::ranges::none_of(Doc().World().All(), [&](const kecs::Entity e) { return Doc().World().Has(e, camera); }))
                Say("There is no Camera in the scene: it plays seen from where its main View's defaults are.", true);
            else Say("Playing.");
            ++_playStarted;
        }
        _play.SetPlaying(true);
        ++_changes;
    }

    void Session::Step()
    {
        if (!InPlayMode()) StartPlaying();      // from the beginning, as Play is
        _play.SetPlaying(false);
        _play.StepOnce();
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
        _play.Restart();           // put back: setup made again from the world as it was
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
        // Its modules', the farthest first, then its own: one of a name read later is what that name is.
        std::vector<std::filesystem::path> folders;
        if (_hasProject) {
            for (const auto& module : Doc().Modules() | std::views::reverse) folders.push_back(module / "objects");
            folders.push_back(_project.directory / "objects");
        }
        for (const auto& folder : folders) {
            std::vector<std::filesystem::path> in;
            for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
                if (entry.is_regular_file() && entry.path().extension() == ".json") in.push_back(entry.path());
            std::ranges::sort(in);
            files.insert(files.end(), in.begin(), in.end());
        }
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
        // The Assets panel shows it, picked: from there it is dragged into the scene for more of it.
        assetFolder = "prefabs";
        assetFilter.clear();
        selectedAsset = "prefab:" + file + ".prefab";
        ++_changes;
        return true;
    }

    kecs::Entity Session::PlacePrefab(const std::string& id, const kecs::Entity parent)
    {
        if (!_hasProject || !id.starts_with("prefab:")) return kecs::NullEntity;
        std::string error;
        const kecs::Entity made = Doc().Instantiate(_project.directory / "prefabs" / id.substr(7), parent, &error);
        if (made.IsNull()) { Say(error, true); return made; }
        if (parent.IsNull())
            if (const auto at = std::exchange(placeAt, std::nullopt)) Doc().SetPosition(made, { at->x, at->y, at->z });
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

    const std::vector<std::pair<std::string, std::string>>& Session::ShaderKinds()
    {
        static const std::vector<std::pair<std::string, std::string>> kinds {
            { "Vertex", "Vertex and fragment shaders: each entity's mesh drawn, or one triangle over the whole picture. For a GraphicsPipeline and a Draw." },
            { "MeshShading", "Mesh shaders and a fragment shader: triangles made with no vertex buffer, a group at a time. For a GraphicsPipeline drawn with eMesh." },
            { "Compute", "Works on an image, a group of pixels at a time: for a ComputePipeline and a Dispatch." },
            { "RayTracing", "A ray generation shader, a miss shader and a hit group: a ray traced for each pixel. For a RayTracingPipeline and a TraceRays." },
        };
        return kinds;
    }

    std::string Session::ShaderMissing(const std::string_view kind, const std::string_view option)
    {
        // What the device was made with: mesh shaders and ray tracing where the GPU has them, tessellation and geometry
        // shaders as the graph's runtime asks for them.
        if (kind == "MeshShading" && !kor::Context::Supports(kor::Feature::eMeshShader)) return "This GPU has no mesh shaders.";
        if (kind == "RayTracing" && !kor::Context::SupportsRayTracing()) return "This GPU does not trace rays.";
        if (kind == "Vertex" && option == "tessellation" && !kor::Context::Supports(kor::Feature::eTessellationShader)) return "This GPU has no tessellation shaders.";
        if (kind == "Vertex" && option == "geometry" && !kor::Context::Supports(kor::Feature::eGeometryShader)) return "This GPU has no geometry shaders.";
        return {};
    }

    std::vector<std::pair<std::string, std::string>> Session::ScriptKindsHere() const
    {
        const char language = _hasProject ? ScriptLanguage(_project) : 0;
        std::vector<std::pair<std::string, std::string>> kinds;
        for (const auto& kind : ScriptKinds()) {
            if (language == 0) kinds.emplace_back(kind.first, kind.second + " The project's scripts are all in the language its first is in.");
            else if (kind.first.starts_with("Kotlin") == (language == 'k')) kinds.push_back(kind);
        }
        return kinds;
    }

    const std::vector<std::pair<std::string, std::string>>& Session::ScriptKinds()
    {
        static const std::vector<std::pair<std::string, std::string>> kinds {
            { "C#", "Compiled when the project opens, and again whenever it is saved: it runs in the editor, and in the game." },
            { "Kotlin", "Compiled when the project opens, and again whenever it is saved: it runs in the editor, and in the game." },
            { "C# ResourceScript", "Makes images, buffers, views and samplers, and fills them on the CPU as they are made: a Script node of it gives each." },
            { "Kotlin ResourceScript", "Makes images, buffers, views and samplers, and fills them on the CPU as they are made: a Script node of it gives each." },
        };
        return kinds;
    }

    bool Session::NewShader(const std::string& name, const std::string& kind, const ShaderChoice& choice)
    {
        if (!_hasProject) { Say("A shader goes in a project's shaders folder: File, New project.", true); return false; }
        if (!IsIdentifier(name)) { Say(std::format("'{}' is not a name a shader module can have: letters, digits and underscores, not starting with a digit.", name), true); return false; }
        if (!std::ranges::contains(ShaderKinds(), kind, &std::pair<std::string, std::string>::first)) { Say(std::format("'{}' is no kind of shader.", kind), true); return false; }
        // What it has that this device cannot run is said, and nothing made.
        for (const auto& [option, asked] : { std::pair { "", true }, { "tessellation", choice.tessellation }, { "geometry", choice.geometry } })
            if (asked)
                if (const std::string why = ShaderMissing(kind, option); !why.empty()) { Say(why, true); return false; }
        const std::string source = kind == "Vertex" ? VertexShader(choice) : kind == "MeshShading" ? MeshShading(choice) : kind == "RayTracing" ? RayTracing(choice) : std::string(ComputeShader);
        const auto path = _project.ShaderDirectory() / (name + ".slang");
        std::error_code ignored;
        if (std::filesystem::exists(path, ignored)) { Say(std::format("There is a shader called {} already.", name), true); return false; }
        std::filesystem::create_directories(path.parent_path(), ignored);
        // One that draws each entity's mesh reads the frame engineCommon declares, the lit graph's: the project's from here on.
        if (kind == "Vertex" && !choice.wholePicture)
            WriteShaders(_project.ShaderDirectory(), { *std::ranges::find(LitShaders(), std::string("engineCommon.slang"), &std::pair<std::string, std::string>::first) });
        { std::ofstream out(path, std::ios::binary); out << Named(source, name); if (!out) { Say(std::format("{} could not be written.", path.string()), true); return false; } }
        Say(std::format("Made shaders/{}.slang: a pipeline node names it as its module, '{}'.", name, name));
        return true;
    }

    bool Session::NewScript(const std::string& name, const std::string& kind)
    {
        if (!_hasProject) { Say("A script goes in a project's scripts folder: File, New project.", true); return false; }
        if (!IsIdentifier(name)) { Say(std::format("'{}' is not a name a script's class can have: letters, digits and underscores, not starting with a digit.", name), true); return false; }
        // Its language, and what it is: a script of entities, or one that makes resources ("Kotlin ResourceScript").
        const std::string language = kind.substr(0, kind.find(' '));
        const std::string what = kind.find(' ') == std::string::npos ? std::string("Script") : kind.substr(kind.find(' ') + 1);
        if (what != "Script" && what != "ResourceScript") { Say(std::format("'{}' is no kind of script.", what), true); return false; }
        const bool kotlin = language == "Kotlin";
        if (!kotlin && language != "C#") { Say(std::format("'{}' is not a language scripts are written in.", language), true); return false; }
        // A project's scripts are all in one language: the first one's, kept in project.json.
        if (const char said = ScriptLanguage(_project); said != 0 && said != (kotlin ? 'k' : 'c')) {
            Say(std::format("This project's scripts are in {}: a project has its scripts in one language.", said == 'k' ? "Kotlin" : "C#"), true);
            return false;
        }
        if (Doc().World().Find(name) != kecs::NoComponent) { Say(std::format("There is a component called {} already: a script is one, by its class's name.", name), true); return false; }
        if (std::ranges::contains(Doc().ResourceScripts(), name, &kgraph::ResourceScript::name)) { Say(std::format("There is a script called {} already.", name), true); return false; }
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
        const char* source = what == "ResourceScript" ? (kotlin ? KotlinResourceScript : CSharpResourceScript) : (kotlin ? KotlinScript : CSharpScript);
        { std::ofstream out(path, std::ios::binary); out << Named(source, name); if (!out) { Say(std::format("{} could not be written.", path.string()), true); return false; } }
        WriteEditorSetup(_project.directory, _project.name);
        // Its component is what its source declares, from now: to give entities, and as a node of the graph.
        Doc().DeclareScripts(path);
        if (what != "Script")
            Say(std::format("Made scripts/{}.{}: a Script node of it — '{}', among the nodes to add — gives what it makes.", name, kotlin ? "kt" : "cs", name));
        else
            Say(std::format("Made scripts/{}.{}, a component: it is compiled now, and runs once it is.", name, kotlin ? "kt" : "cs"));
        return true;
    }

    bool Session::NewStandardScene() { return NewScene(false); }
    bool Session::NewLitScene() { return NewScene(true); }

    bool Session::NewScene(const bool lit)
    {
        if (_prefab) ClosePrefab(true);         // the scene is what changes: the prefab saved, and the scene back, first
        if (!_hasProject) { Say("A new scene's shaders go in a project: File, New project.", true); return false; }
        if (InPlayMode()) Stop();
        if (lit) WriteLitShaders(_project.ShaderDirectory());
        else WriteStandardShaders(_project.ShaderDirectory());
        WriteStandardObjects(_project.directory / "objects");
        _play.New();
        Doc().SetAssetDirectory(_project.directory / "assets");     // the project's: its files, and its prefabs, are named from it
        Doc().DeclareScripts(_project.directory / "scripts");       // and its scripts' components, as a scene loaded from a file has
        selectedNode.clear();
        selectedEntity = kecs::NullEntity;
        _folded.clear();
        std::string error;
        if (!(lit ? AddLitGraph(Doc(), error) : AddStandardGraph(Doc(), error))) { Say(error, true); return false; }
        Select(lit ? AddLitScene(Doc()) : AddStandardScene(Doc()));
        LoadObjects();
        Say("A new scene: saving writes it to the project.");
        return true;
    }

    bool Session::AddScene(const std::string& name, const bool lit)
    {
        if (!_hasProject) { Say("A scene of its own goes in a project: File, New project.", true); return false; }
        if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos) { Say(std::format("'{}' is no scene's name: a file's name, without / \\ : * ? \" < > |", name), true); return false; }
        std::error_code ec;
        if (std::ranges::contains(_project.scenes, name) || std::filesystem::exists(_project.ScenePath(name), ec)) { Say(std::format("The project has a scene called {} already.", name), true); return false; }
        // What was done to the one open is kept: saved before it is let go of.
        if (Doc().Dirty() && !Save()) return false;
        const std::string was = _scene;
        _scene = name;
        if (!NewScene(lit) || !Save()) {
            std::string again;
            _scene = was;
            (void)_play.Open(_project.ScenePath(was), again);
            return false;
        }
        _project.scenes.push_back(name);
        std::string error;
        if (!_project.Save(error)) Say(std::format("{} is made, and project.json could not list it: {}", name, error), true);
        else Say(std::format("Made {}, a scene of the project's.", name));
        return true;
    }

    bool Session::Revert()
    {
        if (_prefab) ClosePrefab(true);         // the scene is what changes: the prefab saved, and the scene back, first
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

    bool Session::IsStage(const std::string_view stage, const std::string_view slotStage)
    {
        const auto same = [](const std::string_view s) { return s == "pixel" ? std::string_view("fragment") : s == "amplification" ? std::string_view("task") : s; };
        return same(stage) == same(slotStage);
    }

    std::vector<Session::ShaderPick> Session::ShadersOf(const std::string_view stage) const
    {
        std::vector<ShaderPick> choices;
        for (const auto& module : ShaderModules())
            for (const auto& entry : ShaderEntries(module))
                if (IsStage(entry.stage, stage)) choices.push_back({ module, entry.name });
        return choices;
    }

    void Session::SetShader(const std::string& node, const std::string& slot, const std::string& module, const std::string& entry)
    {
        Doc().SetInput(node, SlotInput(slot, "module"), kgraph::Value(module));
        Doc().SetInput(node, SlotInput(slot, "entry"), kgraph::Value(entry));
        ++_changes;
    }

    bool Session::DropShader(const std::string& node, const std::string& slot, const std::string_view stage, const std::string& asset)
    {
        if (!asset.starts_with("shader:")) { Say("Only a shader goes here.", true); return false; }
        const std::string named = asset.substr(7);
        const auto hash = named.find('#');
        const std::string module = named.substr(0, hash);
        const auto entries = ShaderEntries(module);
        const auto fits = [&](const ShaderEntry& entry) { return IsStage(entry.stage, stage) && (hash == std::string::npos || entry.name == named.substr(hash + 1)); };
        const auto found = std::ranges::find_if(entries, fits);
        if (found == entries.end()) {
            Say(hash == std::string::npos ? std::format("{} has no {} shader.", module, stage) : std::format("{} is no {} shader.", named.substr(hash + 1), stage), true);
            return false;
        }
        SetShader(node, slot, module, found->name);
        return true;
    }

    void Session::ClearShader(const std::string& node, const std::string& slot)
    {
        Doc().ClearInput(node, SlotInput(slot, "module"));
        Doc().ClearInput(node, SlotInput(slot, "entry"));
        ++_changes;
    }

    int Session::ShaderListSize(const std::string& node, const std::string_view list) const
    {
        int size = 0;
        const auto noted = [&](const std::string_view name) {
            if (name.size() <= list.size() + 2 || !name.starts_with(list) || name[list.size()] != '[') return;
            size = std::max(size, std::atoi(name.data() + list.size() + 1) + 1);
        };
        if (const kgraph::Node* found = _play.Doc().Graph().Find(node)) {
            for (const auto& name : found->inputs | std::views::keys) noted(name);
            for (const auto& name : found->shown) noted(name);
        }
        return size;
    }

    void Session::Show(const std::string& node, const std::string& input)
    {
        if (Doc().SetShown(node, input, true)) ++_changes;
    }

    void Session::Hide(const std::string& node, const std::string& input)
    {
        if (Doc().SetShown(node, input, false)) ++_changes;
    }

    bool Session::Asked(const std::string& node, const std::string& input) const
    {
        const kgraph::Node* found = _play.Doc().Graph().Find(node);
        return found && std::ranges::contains(found->shown, input);
    }

    void Session::AddToShaderList(const std::string& node, const std::string_view list)
    {
        Show(node, std::format("{}[{}]", list, ShaderListSize(node, list)));
    }

    void Session::RemoveFromShaderList(const std::string& node, const std::string_view list, const int index)
    {
        const kgraph::Node* found = Doc().Graph().Find(node);
        if (!found) return;
        const int size = ShaderListSize(node, list);
        const std::string opening = std::format("{}[", list);
        // What each input of the list is: read first, as taking them away moves the node's inputs.
        std::vector<std::tuple<int, std::string, kgraph::Value>> items;
        for (const auto& [name, input] : found->inputs)
            if (name.starts_with(opening) && !input.link) items.emplace_back(std::atoi(name.c_str() + opening.size()), name.substr(name.find(']') + 1), input.value);
        for (const auto& [at, rest, value] : items) Doc().ClearInput(node, std::format("{}{}]{}", opening, at, rest));
        for (const auto& [at, rest, value] : items)
            if (at != index) Doc().SetInput(node, std::format("{}{}]{}", opening, at > index ? at - 1 : at, rest), value);
        for (int at = 0; at < size; ++at) Hide(node, std::format("{}{}]", opening, at));
        for (int at = 0; at + 1 < size; ++at) Show(node, std::format("{}{}]", opening, at));
    }
}
