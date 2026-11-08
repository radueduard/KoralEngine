// Runs a Koral Engine project on its own: `koral-play <project folder | scene file> [--frames N] [--no-vsync] [--validate] [--timings]
// [--capture <node>[:<output>] <file>] [--debug-view <node>]`: an image captured as a .ppm, a buffer (or any .bin) as its bytes.
// The same scene, run by the same code, as the editor shows in its Scene panel — its project's scripts, in C# or in
// Kotlin, run inside this program (khost), as they do in the editor.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <filesystem>
#include <memory>

#include <app.h>

#include <keditor/editor.h>
#include <keditor/play.h>
#include <keditor/project.h>
#include <khost/scripts.h>

#include <frames.h>

int main(const int argc, char** argv)
{
    std::filesystem::path given;
    long frames = 0;    // 0: until the window is closed
    bool vsync = true;  // frames wait for the display; --no-vsync draws them as fast as they are made
    // --validate: Vulkan's validation layer checks every call, as a debug build of the framework asks it to when the
    // layer is installed. It costs a fifth of a frame and more in a scene of many draws, so it is for whoever asks.
    bool validate = false;
    bool timings = false;   // --timings, with --frames: what the frames took, and what each pass of them did
    std::string captured, captureFile;  // --capture <node> <file.ppm>, with --frames: the node's picture after the last frame
    std::string debugView;              // --debug-view <node>: a DebugView shown, as an editor's scene view would show it
    kor::WindowPlatform platform = kor::WindowPlatform::eAuto;     // --platform x11 | wayland
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--no-vsync") == 0) vsync = false;
        else if (std::strcmp(argv[i], "--validate") == 0) validate = true;
        else if (std::strcmp(argv[i], "--timings") == 0) timings = true;
        else if (std::strcmp(argv[i], "--capture") == 0 && i + 2 < argc) { captured = argv[++i]; captureFile = argv[++i]; }
        else if (std::strcmp(argv[i], "--debug-view") == 0 && i + 1 < argc) debugView = argv[++i];
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
    khost::Scripts scripts;
    scripts.WarmUp(project.directory);      // a Kotlin project's compiler, warmed while the window and the scene are made
    kor::App app(kor::AppSettings { .platform = platform });
    if (!project.directory.empty()) project.Use();
    auto play = std::make_unique<keditor::PlayScene>(scene);
    keditor::PlayScene* running = play.get();
    app.Open("Play", std::move(play), { .title = title, .vsync = vsync });
    if (!running->Error().empty()) { std::fprintf(stderr, "koral-play: %s\n", running->Error().c_str()); return 1; }
    if (!debugView.empty()) running->Doc().Runtime().ShowDebugView(debugView);
    const auto document = [&]() -> keditor::Document* { return &running->Doc(); };
    if (frames <= 0) { apps::RunFrames(app, scripts, document); return 0; }
    const auto began = std::chrono::steady_clock::now();
    // The last few hundred are timed apart: by then whatever the scene had to read from files is read.
    const long settled = std::max(frames - 300, 0L);
    auto settledAt = began;
    apps::RunFrames(app, scripts, document, frames, [&](const long i) { if (i == settled) settledAt = std::chrono::steady_clock::now(); });
    if (timings) {
        const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        const double last = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - settledAt).count() / static_cast<double>(frames - settled);
        std::printf("the last %ld frames took %.2f ms each: %.0f a second\n", frames - settled, last, 1000.0 / last);
        const auto graph = running->Graph().Timing();
        std::printf("%ld frames, %.2f ms each: the graph's passes take the GPU %.2f ms, readying them %.2f ms, recording them %.2f ms\n",
                    frames, total / static_cast<double>(frames), graph.gpuMs, graph.prepareMs, graph.recordWallMs);
        for (const auto& pass : running->Graph().PassTimings())
            std::printf("  %-28s GPU %7.3f ms   record %6.3f ms   prepare %6.3f ms\n", pass.name.c_str(), pass.gpuMs, pass.recordMs, pass.prepareMs);
        // The order the frame graph ran them in, and what it left out.
        if (std::getenv("KORAL_PLAY_ORDER")) {
            const auto frame = running->Doc().Runtime().Frame();
            for (const auto& pass : frame.passes) {
                std::printf("  order %-28s level %u:", pass.node.c_str(), pass.level);
                for (const auto& [what, how] : pass.uses) std::printf(" %s(%c)", what.c_str(), how);
                std::printf("\n");
            }
            for (const auto& [node, why] : frame.leftOut) std::printf("  left out %s: %s\n", node.c_str(), why.c_str());
        }
        // What a pass could not run for, and why.
        for (const auto& pass : running->Graph().PassTimings())
            if (const auto* report = running->Doc().Runtime().ReportOf(pass.name); report && (report->skipped > 0 || std::getenv("KORAL_PLAY_ORDER")))
                std::printf("  %s ran %zu, skipped %zu, culled %zu%s: %s\n", pass.name.c_str(), report->ran, report->skipped, report->culled,
                            report->unchanged ? ", unchanged" : "", report->why.c_str());
    }
    if (!captured.empty()) {
        // Watched some frames before it is read: the copy arrives a few frames late, and what is read before then is
        // whatever the memory held.
        auto& runtime = running->Doc().Runtime();
        // `node:output` watches another of its outputs than the one it shows.
        if (const auto colon = captured.rfind(':'); colon != std::string::npos) {
            runtime.WatchOutput(captured.substr(0, colon), captured.substr(colon + 1));
            captured = captured.substr(0, colon);
        } else runtime.Watch(captured);
        for (int i = 0; i < 16; ++i) app.Frame();
        const auto peek = runtime.Watched(captured);
        const auto layout = peek ? kgraph::TexelLayoutOf(peek->format) : kgraph::TexelLayout {};
        if (peek && (!peek->image || layout.channels < 3 || captureFile.ends_with(".bin"))) {
            // A buffer, or an image of fewer than three channels (a depth, an id): its bytes as they are.
            std::printf("%s: %s %u x %u\n", captured.c_str(), peek->image ? peek->format.c_str() : "buffer", peek->extent.x, peek->extent.y);
            std::ofstream(captureFile, std::ios::binary).write(reinterpret_cast<const char*>(peek->bytes.data()), static_cast<std::streamsize>(peek->bytes.size()));
        } else if (!peek || (layout.bits != 8 && !(layout.bits == 16 && layout.kind == 'f'))) {
            std::fprintf(stderr, "koral-play: '%s' cannot be captured: %s\n", captured.c_str(), peek ? peek->format.c_str() : runtime.WatchProblem(captured).c_str());
        } else {
            // 8 bits a channel as it is; half floats clamped to 0..1 and put on the display's curve.
            std::ofstream out(captureFile, std::ios::binary);
            out << "P6\n" << peek->extent.x << " " << peek->extent.y << "\n255\n";
            const std::size_t texel = static_cast<std::size_t>(layout.channels) * layout.bits / 8;
            const bool bgr = peek->format.find("BGR") != std::string::npos;
            for (std::size_t i = 0; i + texel <= peek->bytes.size(); i += texel) {
                unsigned char rgb[3];
                for (int c = 0; c < 3; ++c) {
                    const int from = bgr ? 2 - c : c;
                    if (layout.bits == 8) rgb[c] = static_cast<unsigned char>(peek->bytes[i + from]);
                    else {
                        std::uint16_t h;
                        std::memcpy(&h, &peek->bytes[i + 2 * from], 2);
                        const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
                        float f = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) : exponent == 31 ? 1.f : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
                        if (sign) f = 0.f;
                        rgb[c] = static_cast<unsigned char>(std::clamp(std::pow(std::min(f, 1.f), 1.f / 2.2f), 0.f, 1.f) * 255.f + 0.5f);
                    }
                }
                out.write(reinterpret_cast<const char*>(rgb), 3);
            }
        }
    }
    for (const auto& problem : running->Doc().Runtime().Problems()) std::fprintf(stderr, "koral-play: %s\n", problem.c_str());
    return running->Doc().Runtime().Ok() ? 0 : 1;
}
