// What the tests that run a graph share: one headless app for the process, a scene with a graph's runtime in it
// (and a world of entities, when asked), and a pass that copies the screen where a test can read it.

#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <utility>
#include <memory>
#include <string>
#include <vector>

#include <app.h>
#include <buffer.h>
#include <commandBuffer.h>
#include <context.h>
#include <frameGraph.h>
#include <log.h>
#include <scene.h>
#include <scheduler.h>
#include <shader.h>

#include <kecs/kecs.h>
#include <kgraph/runtime.h>
#include <kscript/declarations.h>

#ifdef _WIN32
// The tests set their environment as POSIX does; Windows has its own calls for it.
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

namespace support
{
    constexpr kor::u32 Size = 32;

    /** The app, or null when there is no Vulkan device: then Reason() says why. */
    kor::App* App();
    const std::string& Reason();
    /** One frame, finished. */
    void Frame();
    /**
     * Gives @p entity component @p component — the engine's package's, a script's: one known by name — with the fields
     * @p fields says, each as its numbers. Those not said start as the component's source says.
     */
    void Give(kecs::World& world, kecs::Entity entity, std::string_view component, std::initializer_list<std::pair<std::string_view, std::vector<float>>> fields = {});
    /** One of the engine's functions, as its file in the package has it: packages/koral/functions/<name>.kfunc. */
    std::shared_ptr<const kgraph::Graph> EngineFunction(const std::string& name);
    /** One of an official module's functions: packages/modules/<module>/functions/<name>.kfunc. */
    std::shared_ptr<const kgraph::Graph> ModuleFunction(const std::string& module, const std::string& name);
    /** Where the engine's own modules are: packages/modules. */
    std::filesystem::path OfficialModules();
    /** A field of an entity's component, as its numbers: a float, or a vector's. */
    std::vector<float> FieldOf(const kecs::World& world, kecs::Entity entity, std::string_view component, std::string_view field);

    /** Copies the screen where the test can read it: after whatever wrote it. */
    class ReadScreen final : public kor::RenderPass {
    public:
        explicit ReadScreen(kor::ResourceRef<const kor::Buffer> out) : RenderPass("Read screen"), _out(std::move(out)) {}
        void Setup(kor::PassBuilder& b) override { b.Read(kor::FrameGraph::Screen, kor::Image::Usage::eTransferSrc).SideEffect(); }
        void Initialize(const kor::PassResources& r) override { _screen = r.ImageNamed(kor::FrameGraph::Screen); }
        void Record(kor::CommandBuffer& cb) const override { cb.CopyImageToBuffer(_screen, _out); }
    private:
        kor::ResourceRef<const kor::Buffer> _out;
        kor::ResourceRef<const kor::Image> _screen;
    };

    class GraphScene final : public kor::Scene {
    public:
        /** Whether the next scene opened gives its runtime the world. */
        static inline bool withWorld = false;

        void Initialize() override {
            for (const auto& problem : kscript::DeclarePackage(scripts)) kor::log::Error("{}", problem);    // the scene's Camera, its lights
            readback = kor::Buffer::RawBuilder{}.SetRawSize(Size * Size * 4).SetUsage(kor::Buffer::Usage::eTransferDst)
                .SetType(kor::Buffer::Type::eReadback).Build();
            runtime = std::make_unique<kgraph::Runtime>(Graph(), withWorld ? &world : nullptr);
            Graph().Add<ReadScreen>(kor::ResourceRef<const kor::Buffer>(readback));
        }
        void Update() override {
            const auto start = std::chrono::steady_clock::now();
            runtime->Update();
            updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            // What a test does after the passes are readied and before they record: once, and then forgotten.
            if (afterUpdate) std::exchange(afterUpdate, {})();
        }
        void Shutdown() override { runtime.reset(); }
        [[nodiscard]] kor::U8Vec4 Pixel(const kor::u32 x, const kor::u32 y) const { return readback->Read<kor::U8Vec4>(Size * Size)[y * Size + x]; }
        std::vector<std::string> Schedule() {
            std::vector<std::string> names;
            for (const auto& pass : Graph().Schedule()) names.push_back(pass.name);
            return names;
        }
        kecs::World world;      // before the runtime, which is gone first
        kscript::Scripts scripts { world };
        std::unique_ptr<kgraph::Runtime> runtime;
        kor::Resource<kor::Buffer> readback;
        double updateMs = 0.0;      // what the runtime's Update took, the last frame
        std::function<void()> afterUpdate;     // done once, after the next frame's Update
    };
}
