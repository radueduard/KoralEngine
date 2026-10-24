// Runs a Koral Engine project on its own: `koral-play <project folder | scene file> [--frames N] [--no-vsync] [--validate] [--timings]`.
// The same scene, run by the same code, as the editor shows in its Scene panel.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <filesystem>
#include <memory>

#include <app.h>

#include <keditor/editor.h>
#include <keditor/play.h>
#include <keditor/project.h>

int main(const int argc, char** argv)
{
    std::filesystem::path given;
    long frames = 0;    // 0: until the window is closed
    bool vsync = true;  // frames wait for the display; --no-vsync draws them as fast as they are made
    // --validate: Vulkan's validation layer checks every call, as a debug build of the framework asks it to when the
    // layer is installed. It costs a fifth of a frame and more in a scene of many draws, so it is for whoever asks.
    bool validate = false;
    bool timings = false;   // --timings, with --frames: what the frames took, and what each pass of them did
    kor::WindowPlatform platform = kor::WindowPlatform::eAuto;     // --platform x11 | wayland
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--no-vsync") == 0) vsync = false;
        else if (std::strcmp(argv[i], "--validate") == 0) validate = true;
        else if (std::strcmp(argv[i], "--timings") == 0) timings = true;
        else if (std::strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            const std::string_view which = argv[++i];
            platform = which == "x11" ? kor::WindowPlatform::eX11 : which == "wayland" ? kor::WindowPlatform::eWayland : kor::WindowPlatform::eAuto;
        }
        else given = argv[i];
    }
    // Given no project, the one this user starts with (the editor's Project, Open at startup); failing that, the one
    // the engine comes with — when it is where the build left it.
    if (given.empty()) given = keditor::Session::StartupProject();
    if (given.empty() && std::filesystem::exists(std::filesystem::path(KORAL_ENGINE_DEFAULT_PROJECT) / "project.json")) given = KORAL_ENGINE_DEFAULT_PROJECT;
    if (given.empty()) {
        std::fprintf(stderr, "koral-play: which project? Give its folder, or a scene file.\n");
        return 2;
    }

    // A scene file by itself runs with the shaders beside its `scenes` folder, as in a project.
    std::filesystem::path scene = given;
    std::string title = given.stem().string();
    keditor::Project project;
    std::string error;
    const bool isScene = std::filesystem::is_regular_file(given) && given.filename() != "project.json";
    if (!keditor::Project::Open(isScene ? given.parent_path().parent_path() : given, project, error)) {
        if (!isScene) { std::fprintf(stderr, "koral-play: %s\n", error.c_str()); return 1; }
    } else {
        if (!isScene) scene = project.ScenePath(project.start);
        title = project.name;
    }

#ifdef _WIN32
    if (!validate && !std::getenv("VK_LOADER_LAYERS_DISABLE")) _putenv_s("VK_LOADER_LAYERS_DISABLE", "*validation*");
#else
    if (!validate) setenv("VK_LOADER_LAYERS_DISABLE", "*validation*", 0);   // (not over what the environment already says)
#endif
    kor::App app(kor::AppSettings { .platform = platform });
    if (!project.directory.empty()) project.Use();
    auto play = std::make_unique<keditor::PlayScene>(scene);
    keditor::PlayScene* running = play.get();
    app.Open("Play", std::move(play), { .title = title, .vsync = vsync });
    if (!running->Error().empty()) { std::fprintf(stderr, "koral-play: %s\n", running->Error().c_str()); return 1; }
    if (frames <= 0) return app.Run();
    const auto began = std::chrono::steady_clock::now();
    // The last few hundred are timed apart: by then whatever the scene had to read from files is read.
    const long settled = std::max(frames - 300, 0L);
    auto settledAt = began;
    for (long i = 0; i < frames; ++i) {
        if (i == settled) settledAt = std::chrono::steady_clock::now();
        app.Frame();
    }
    if (timings) {
        const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        const double last = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - settledAt).count() / static_cast<double>(frames - settled);
        std::printf("the last %ld frames took %.2f ms each: %.0f a second\n", frames - settled, last, 1000.0 / last);
        const auto graph = running->Graph().Timing();
        std::printf("%ld frames, %.2f ms each: the graph's passes take the GPU %.2f ms, readying them %.2f ms, recording them %.2f ms\n",
                    frames, total / static_cast<double>(frames), graph.gpuMs, graph.prepareMs, graph.recordWallMs);
        for (const auto& pass : running->Graph().PassTimings())
            std::printf("  %-28s GPU %7.3f ms   record %6.3f ms   prepare %6.3f ms\n", pass.name.c_str(), pass.gpuMs, pass.recordMs, pass.prepareMs);
    }
    for (const auto& problem : running->Doc().Runtime().Problems()) std::fprintf(stderr, "koral-play: %s\n", problem.c_str());
    return running->Doc().Runtime().Ok() ? 0 : 1;
}
