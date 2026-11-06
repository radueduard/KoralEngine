// The Koral Engine editor: `koral-editor [project folder] [--frames N] [--no-vsync] [--validate] [--arrange]`. With no project it opens the one
// this user said to start with (Project, Open at startup), or else the engine's own: Forward+, made with nodes (forward-plus, in the projects folder).
// A project's scripts — in C# or in Kotlin, as the project says — are run inside this program: .NET or a JVM is started
// in it the first time a project needs one, and the scripts are compiled and run on the scene that is open (khost).
// --arrange puts every page of the project's scene in order, as the Graph menu's Arrange every page does, saves the
// scene and exits: a graph a script wrote, laid out as the editor would.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <filesystem>
#include <memory>

#include <app.h>

#include <keditor/editor.h>
#include <khost/scripts.h>

#include <frames.h>

int main(const int argc, char** argv)
{
    std::filesystem::path project;
    long frames = 0;    // 0: until the window is closed
    bool vsync = true;  // frames wait for the display; --no-vsync draws them as fast as they are made
    // --validate: Vulkan's validation layer checks every call, as a debug build of the framework asks it to when the
    // layer is installed. It costs a fifth of a frame and more in a scene of many draws, so it is for whoever asks.
    bool validate = false;
    bool arrange = false;
    kor::WindowPlatform platform = kor::WindowPlatform::eAuto;     // --platform x11 | wayland
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--no-vsync") == 0) vsync = false;
        else if (std::strcmp(argv[i], "--validate") == 0) validate = true;
        else if (std::strcmp(argv[i], "--arrange") == 0) arrange = true;
        else if (std::strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            const std::string_view which = argv[++i];
            platform = which == "x11" ? kor::WindowPlatform::eX11 : which == "wayland" ? kor::WindowPlatform::eWayland : kor::WindowPlatform::eAuto;
        }
        else project = argv[i];
    }

    // Given no project, the one this user starts with; failing that, the one the engine comes with — when it is where the build left it.
    if (project.empty()) project = keditor::Session::StartupProject();
    if (project.empty() && std::filesystem::exists(std::filesystem::path(KORAL_ENGINE_DEFAULT_PROJECT) / "project.json")) project = KORAL_ENGINE_DEFAULT_PROJECT;

#ifdef _WIN32
    if (!validate && !std::getenv("VK_LOADER_LAYERS_DISABLE")) _putenv_s("VK_LOADER_LAYERS_DISABLE", "*validation*");
#else
    if (!validate) setenv("VK_LOADER_LAYERS_DISABLE", "*validation*", 0);   // (not over what the environment already says)
#endif
    khost::Scripts scripts;     // before the editor: what its Problems ask whether the scripts run
    kor::App app(kor::AppSettings { .platform = platform });
    auto scene = std::make_unique<keditor::EditorScene>(project);
    keditor::EditorScene* editor = scene.get();
    // As this user last left it — not for a run of so many frames, or one that arranges and saves.
    const bool kept = keditor::Session::keepsWindow = frames <= 0 && !arrange;
    const keditor::Session::SavedWindow last = kept ? keditor::Session::LastWindow() : keditor::Session::SavedWindow {};
    kor::Scene* opened = app.Open("Editor", std::move(scene), { .title = "Koral Engine", .extent = last.extent, .vsync = vsync, .position = last.position });
    if (last.maximized) editor->SceneWindow().ToggleMaximize();
    if (arrange) {
        // Drawn a few times first, so that each node is as big as it comes out; then each page in turn, then saved.
        for (int i = 0; i < 30; ++i) app.Frame();
        keditor::Session& session = editor->CurrentSession();
        session.ArrangeEveryPage();
        for (int i = 0; i < 100000 && !session.PagesToArrange().empty(); ++i) app.Frame();
        for (int i = 0; i < 5; ++i) app.Frame();
        if (!session.Save()) { std::fprintf(stderr, "koral-editor: %s\n", session.Status().c_str()); return 1; }
        std::printf("koral-editor: every page of %s is in order, and saved\n", project.string().c_str());
        return 0;
    }
    // The scripts follow the scene being worked on: another project opened from the File menu has its own run, here.
    apps::RunFrames(app, scripts, [&]() -> keditor::Document* { return app.IsOpen(opened) ? &editor->CurrentSession().Doc() : nullptr; }, frames);
    return 0;
}
