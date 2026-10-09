// Runs a Koral Engine project on its own: `koral-play <project folder | scene file> [--frames N] [--no-vsync]`.
// The same scene, run by the same code, as the editor shows in its Scene panel.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include <app.h>

#include <keditor/play.h>
#include <keditor/project.h>

int main(const int argc, char** argv)
{
    std::filesystem::path given;
    long frames = 0;    // 0: until the window is closed
    bool vsync = true;  // frames wait for the display; --no-vsync draws them as fast as they are made
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--no-vsync") == 0) vsync = false;
        else given = argv[i];
    }
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

    kor::App app;
    if (!project.directory.empty()) project.Use();
    auto play = std::make_unique<keditor::PlayScene>(scene);
    keditor::PlayScene* running = play.get();
    app.Open("Play", std::move(play), { .title = title, .vsync = vsync });
    if (!running->Error().empty()) { std::fprintf(stderr, "koral-play: %s\n", running->Error().c_str()); return 1; }
    if (frames <= 0) return app.Run();
    for (long i = 0; i < frames; ++i) app.Frame();
    for (const auto& problem : running->Doc().Runtime().Problems()) std::fprintf(stderr, "koral-play: %s\n", problem.c_str());
    return running->Doc().Runtime().Ok() ? 0 : 1;
}
