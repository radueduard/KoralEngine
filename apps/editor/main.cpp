// The Koral Engine editor: `koral-editor [project folder] [--frames N] [--no-vsync]`.

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include <app.h>

#include <keditor/editor.h>

int main(const int argc, char** argv)
{
    std::filesystem::path project;
    long frames = 0;    // 0: until the window is closed
    bool vsync = true;  // frames wait for the display; --no-vsync draws them as fast as they are made
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--no-vsync") == 0) vsync = false;
        else project = argv[i];
    }

    kor::App app;
    app.Open("Editor", std::make_unique<keditor::EditorScene>(project), { .title = "Koral Engine", .extent = { 1600, 950 }, .vsync = vsync });
    if (frames <= 0) return app.Run();
    for (long i = 0; i < frames; ++i) app.Frame();
    return 0;
}
