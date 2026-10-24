// The Koral Engine editor: `koral-editor [project folder] [--frames N] [--no-vsync] [--validate]`. With no project it opens the one
// this user said to start with (Project, Open at startup), or else the engine's own: Forward+, made with nodes (forward-plus, in the projects folder).
// A project with scripts is opened by their runner instead, which runs them: before this opens any window, in a
// process forked for it — and when one is opened from the File menu, this closes its window and does the same.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <app.h>

#include <keditor/editor.h>

namespace {
    /**
     * The editor on @p folder through its scripts' runner, in a process of its own, waited for: what it exited with.
     * Nothing for a project with no scripts — or when the runner stopped with an error before it had run a minute (no
     * JDK, no runner built): then this program opens the project itself, without them, and says why.
     */
    std::optional<int> ThroughScripts(const std::filesystem::path& folder)
    {
        keditor::Project project;
        std::string error;
        if (!keditor::Project::Open(folder, project, error)) return std::nullopt;
        const auto started = std::chrono::steady_clock::now();
        const auto exited = keditor::RunThroughScripts(project);
        if (!exited || *exited == 0 || std::chrono::steady_clock::now() - started > std::chrono::minutes(1)) return exited;
        std::fprintf(stderr, "koral-editor: the runner of %s's scripts stopped (%d) before it got going: the editor opens it without them.\n",
                     project.name.c_str(), *exited);
        return std::nullopt;
    }
}

int main(const int argc, char** argv)
{
    std::filesystem::path project;
    std::vector<std::string> flags;     // what is said besides the project: kept when this runs itself on another
    long frames = 0;    // 0: until the window is closed
    bool vsync = true;  // frames wait for the display; --no-vsync draws them as fast as they are made
    // --validate: Vulkan's validation layer checks every call, as a debug build of the framework asks it to when the
    // layer is installed. It costs a fifth of a frame and more in a scene of many draws, so it is for whoever asks.
    bool validate = false;
    kor::WindowPlatform platform = kor::WindowPlatform::eAuto;     // --platform x11 | wayland
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--no-vsync") == 0) { vsync = false; flags.emplace_back(argv[i]); }
        else if (std::strcmp(argv[i], "--validate") == 0) { validate = true; flags.emplace_back(argv[i]); }
        else if (std::strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            flags.emplace_back(argv[i]);
            flags.emplace_back(argv[i + 1]);
            const std::string_view which = argv[++i];
            platform = which == "x11" ? kor::WindowPlatform::eX11 : which == "wayland" ? kor::WindowPlatform::eWayland : kor::WindowPlatform::eAuto;
        }
        else project = argv[i];
    }

    // Given no project, the one this user starts with; failing that, the one the engine comes with — when it is where the build left it.
    if (project.empty()) project = keditor::Session::StartupProject();
    if (project.empty() && std::filesystem::exists(std::filesystem::path(KORAL_ENGINE_DEFAULT_PROJECT) / "project.json")) project = KORAL_ENGINE_DEFAULT_PROJECT;

    // A project with scripts is the editor run by their runner, which runs them: known before any window is opened,
    // and started in a process of its own, this one waiting for it. (Not when that runner is what started this.)
    if (!project.empty() && frames <= 0)
        if (const auto exited = ThroughScripts(project)) return *exited;

#ifdef _WIN32
    if (!validate && !std::getenv("VK_LOADER_LAYERS_DISABLE")) _putenv_s("VK_LOADER_LAYERS_DISABLE", "*validation*");
#else
    if (!validate) setenv("VK_LOADER_LAYERS_DISABLE", "*validation*", 0);   // (not over what the environment already says)
#endif
#ifndef _WIN32
    // Another project with scripts opened from the File menu: the window closes, and this goes to its runner as above.
    keditor::Session::handOverOnExit = frames <= 0;
#endif
    int code = 0;
    {
        kor::App app(kor::AppSettings { .platform = platform });
        app.Open("Editor", std::make_unique<keditor::EditorScene>(project, true), { .title = "Koral Engine", .extent = { 1600, 950 }, .vsync = vsync });
        if (frames > 0) {
            for (long i = 0; i < frames; ++i) app.Frame();
            return 0;
        }
        code = app.Run();
    }
#ifndef _WIN32
    if (const auto next = std::exchange(keditor::Session::handOverPending, std::nullopt)) {
        // This program again, on that project: which goes to its runner, in this terminal, before any window.
        std::vector<std::string> words { argv[0] };
        words.insert(words.end(), flags.begin(), flags.end());
        words.push_back(next->string());
        std::vector<char*> pointers;
        for (auto& word : words) pointers.push_back(word.data());
        pointers.push_back(nullptr);
        execv("/proc/self/exe", pointers.data());
        std::perror("koral-editor: it could not be started again");
        return 1;
    }
#endif
    return code;
}
