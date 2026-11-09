// Many of one mesh, and what the CPU spends on them a frame. A hundred thousand entities, drawn by one indirect draw a
// pass as forward-plus draws its scene — a depth pass of where each is, and a forward pass of where each is, which way it
// faces, its own colour and which it is — with nothing moving, with some moving and with all of them moving. And an
// Instancer's tens of millions, made, culled and drawn on the GPU, whose frame costs the CPU the same whatever the count.
// Offscreen.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

#include <kecs/json.h>
#include <kgraph/entities.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;

    /** How many: KGRAPH_SCALE_ENTITIES, or a hundred thousand (ten thousand in a debug build). */
    int Count()
    {
        if (const char* said = std::getenv("KGRAPH_SCALE_ENTITIES"); said && *said) return std::max(1, std::atoi(said));
#ifdef NDEBUG
        return 100'000;
#else
        return 10'000;
#endif
    }

    class ScaleTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "scale", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }
        /** What the runtime's Update took over @p frames frames, the median, with @p between done before each. */
        template <typename Between>
        double Median(const int frames, Between&& between) {
            std::vector<double> times;
            for (int i = 0; i < frames; ++i) {
                between(i);
                Frame();
                times.push_back(scene->updateMs);
            }
            std::ranges::sort(times);
            return times[times.size() / 2];
        }
        GraphScene* scene = nullptr;
    };
}

TEST_F(ScaleTest, AHundredThousandOfOneMeshAreReadiedInLittleTime) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("depth", "GraphicsPipeline").Set("vertex.module", "scale").Set("vertex.entry", "depthVertex").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "scale").Set("fragment.entry", "depthFragment").Wire("framebuffer", "target");
    graph.Add("forward", "GraphicsPipeline").Set("vertex.module", "scale").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "scale").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("prepass", "Draw").Wire("pipeline", "depth").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
    graph.Add("draw", "Draw").Wire("pipeline", "forward").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount").Set("load", true);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const kecs::ComponentId forward = world.Find("forward");
    ASSERT_NE(forward, kecs::NoComponent) << "the forward pipeline's component, of its colour";

    // A grid of them over the picture, each its own colour by where it is: the right half red, the left green.
    const int n = Count();
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const float cell = 2.f / static_cast<float>(side);
    std::vector<kecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(n));
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        const float x = -1.f + cell * (static_cast<float>(i % side) + 0.5f);
        const float y = -1.f + cell * (static_cast<float>(i / side) + 0.5f);
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, y, 0.5f });
        world.SetScale(entity, { 0.1f, 0.1f, 0.01f });      // many pixels each, overlapping: every pixel is covered
        world.Add<kgraph::Mesh>(entity, { cube });
        support::Give(world, entity, "forward", { { "color", x > 0.f ? std::vector { 1.f, 0.f, 0.f, 1.f } : std::vector { 0.f, 1.f, 0.f, 1.f } } });
        entities.push_back(entity);
    }
    const double madeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    start = std::chrono::steady_clock::now();
    Frame();
    const double firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (int i = 0; i < 4; ++i) Frame();    // the buffers grown to fit, and every copy of them written
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ValueOf("commands", "drawCount"), Value(1));
    const auto colour = [&](const kor::u32 x, const kor::u32 y) { const auto p = scene->Pixel(x, y); return std::array<int, 3> { p.x, p.y, p.z }; };
    // Lit by how much they face the viewer: half to all of their colour.
    EXPECT_GT(colour(24, 16)[0], 100) << "the right half red";
    EXPECT_LT(colour(24, 16)[1], 20);
    EXPECT_GT(colour(8, 16)[1], 100) << "the left half green";
    EXPECT_LT(colour(8, 16)[0], 20);

    const double stillMs = Median(30, [](int) {});
    const int few = std::max(1, n / 100);
    const double someMs = Median(30, [&](const int frame) {
        for (int i = 0; i < few; ++i) {
            const kecs::Entity entity = entities[static_cast<std::size_t>((i * 97 + frame * 13) % n)];
            const kecs::Vec3 at = world.PositionOf(entity);
            world.SetPosition(entity, { at.x, at.y, frame % 2 ? 0.5f : 0.55f });
        }
    });
    const double allMs = Median(30, [&](const int frame) {
        for (const kecs::Entity entity : entities) {
            const kecs::Vec3 at = world.PositionOf(entity);
            world.SetPosition(entity, { at.x, at.y, frame % 2 ? 0.5f : 0.55f });
        }
    });
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_GT(colour(24, 16)[0], 100) << "still drawn, after all that moving";

    const auto timings = runtime.Timings();
    const auto prepare = [&](const char* id) { const auto found = timings.find(id); return found == timings.end() ? 0.0 : found->second.prepareMs; };
    const auto gpu = [&](const char* id) { const auto found = timings.find(id); return found == timings.end() ? 0.0 : found->second.gpuMs; };
    const std::string report = std::format(
        "{} entities: made in {:.1f} ms, first frame {:.1f} ms\n"
        "  runtime Update, median a frame: nothing moving {:.2f} ms, {} moving {:.2f} ms, all moving {:.2f} ms\n"
        "  readied (last frames, averaged): prepass {:.2f} ms, draw {:.2f} ms; GPU: prepass {:.2f} ms, draw {:.2f} ms",
        n, madeMs, firstMs, stillMs, few, someMs, allMs, prepare("prepass"), prepare("draw"), gpu("prepass"), gpu("draw"));
    std::printf("%s\n", report.c_str());
    RecordProperty("still_ms", std::format("{:.3f}", stillMs));
    RecordProperty("some_ms", std::format("{:.3f}", someMs));
    RecordProperty("all_ms", std::format("{:.3f}", allMs));
}

namespace {
    /** How many instances: KGRAPH_INSTANCES, or ten million (a million in a debug build). */
    int InstanceCount()
    {
        if (const char* said = std::getenv("KGRAPH_INSTANCES"); said && *said) return std::max(1, std::atoi(said));
#ifdef NDEBUG
        return 10'000'000;
#else
        return 1'000'000;
#endif
    }

    /** Sets an int field of an entity's component, by name: support::Give sets floats. */
    void GiveInt(kecs::World& world, const kecs::Entity entity, const std::string_view component, const std::string_view field, const std::int32_t value)
    {
        const kecs::ComponentId id = world.Find(component);
        const kecs::FieldDescription* found = world.Describe(id).FindField(field);
        ASSERT_NE(found, nullptr) << component << "." << field;
        std::memcpy(static_cast<std::byte*>(world.Mutable(entity, id)) + found->offset, &value, 4);
    }
}

// An Instancer's instances — tens of millions of one mesh on one entity — made, culled and drawn on the GPU: near, each
// is its mesh; further, a point; a cluster smaller than a pixel, one point. The CPU's frame is the same whatever the count.
TEST_F(ScaleTest, AnInstancersMillionsCostTheCpuNothing) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    const int count = InstanceCount();
    // About a unit apart: the square grows with the count.
    const double side = std::sqrt(static_cast<double>(count));

    const kecs::Entity field = world.Create();
    world.Rename(field, "field");
    world.Add<kgraph::Mesh>(field, { cube });
    support::Give(world, field, "Instancer", { { "extent", { static_cast<float>(side), static_cast<float>(side) } }, { "scale", { 0.4f, 0.8f } },
                                                { "pointDistance", { 40.f } }, { "color", { 0.f, 1.f, 0.f } } });
    GiveInt(world, field, "Instancer", "count", count);
    GiveInt(world, field, "Instancer", "seed", 7);

    constexpr int Picture = 512;
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { Picture, Picture })
        .Set("usage", Value::List { "eColorAttachment", "eStorage", "eTransferSrc" });
    graph.Add("depth", "Image").Set("format", "eD32_SFLOAT").Set("extent", Value::List { Picture, Picture })
        .Set("usage", Value::List { "eDepthStencilAttachment", "eSampled" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("depth view", "ImageView").Wire("image", "depth");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view")
        .Wire("depthStencil.view", "depth view").Set("depthStencil.which", "Depth").Set("depthStencil.name", "depth").Set("depthStencil.depth", 1);

    // Seen from above one corner, across the square: the near ones as cubes, then points to the far side.
    graph.Add("instancer", "Entity").Set("component", "Instancer").Set("with", Value::List { "Mesh" });
    graph.Add("lens", "Perspective").Set("fieldOfView", 60).Set("aspect", 1).Set("near", 0.5).Set("far", 4.0 * side + 100.0);
    graph.Add("eye", "View").Set("position", Value::List { -side * 0.5, 12.0, -side * 0.5 }).Set("target", Value::List { side * 0.1, 0.0, side * 0.1 });
    graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "eye");
    graph.Add("to clip", "MatrixMultiply").Wire("a", "seen").Wire("b", "instancer", "matrix");

    graph.Add("make", "Function").Set("function", "Instances").Wire("count", "instancer", "field.count").Wire("seed", "instancer", "field.seed")
        .Wire("extent", "instancer", "field.extent").Wire("scale", "instancer", "field.scale").Wire("jitter", "instancer", "field.jitter");
    graph.Add("parts", "MeshBuffers").Wire("mesh", "instancer", "Mesh.mesh");
    graph.Add("cull", "Function").Set("function", "CullInstances").Wire("instances", "make", "instances").Wire("count", "instancer", "field.count")
        .Wire("toClip", "to clip").Set("height", Picture).Set("meshRadius", 0.9).Wire("pointDistance", "instancer", "field.pointDistance")
        .Wire("drawDistance", "instancer", "field.drawDistance").Wire("indexCount", "parts", "indexCount").Wire("firstIndex", "parts", "firstIndex")
        .Wire("vertexOffset", "parts", "vertexOffset");
    graph.Add("shape", "Primitive").Set("shape", "eCube");
    graph.Add("near", "GraphicsPipeline").Set("vertex.module", "instancedField").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "instancedField").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target")
        .Set("depthStencilState", Value::Members { { "depthTestEnable", true }, { "depthWriteEnable", true }, { "depthCompareOp", "eLess" } });
    graph.Add("draw", "Draw").Wire("pipeline", "near").Wire("mesh", "instancer", "Mesh.mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "cull", "commands").Set("drawCount", 1).Wire("param.instances", "make", "instances").Wire("param.visible", "cull", "visible")
        .Wire("push.toClip", "to clip").Set("clearColor", Value::List { 0, 0, 1, 1 });
    graph.Add("far", "Function").Set("function", "SplatPoints").Wire("instances", "make", "instances").Wire("counters", "cull", "counters")
        .Wire("points", "cull", "points").Wire("groups", "cull", "groups").Wire("target", "canvas").Wire("depth", "depth")
        .Wire("count", "instancer", "field.count").Wire("toClip", "to clip").Wire("color", "instancer", "field.color")
        .Set("width", Picture).Set("height", Picture).Set("meshRadius", 0.9);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    for (const char* name : { "Instances", "CullInstances", "SplatPoints" }) graph.functions[name] = support::EngineFunction(name);

    const auto start = std::chrono::steady_clock::now();
    runtime.Set(graph);
    for (int i = 0; i < 4; ++i) Frame();
    const double firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    // Sky above, cubes near (red), points far (green).
    const auto colour = [&](const kor::u32 x, const kor::u32 y) { const auto p = scene->Pixel(x, y); return std::array<int, 3> { p.x, p.y, p.z }; };
    std::string picture;
    for (kor::u32 y = 0; y < support::Size; y += 2) {
        for (kor::u32 x = 0; x < support::Size; ++x) {
            const auto c = colour(x, y);
            picture += c[0] > 80 && c[1] < 60 ? 'R' : c[1] > 80 && c[0] < 60 ? 'G' : c[2] > 200 && c[0] < 40 && c[1] < 40 ? '.' : '?';
        }
        picture += '\n';
    }
    std::printf("%s", picture.c_str());
    EXPECT_NE(picture.find('R'), std::string::npos) << "near: cubes";
    EXPECT_NE(picture.find('G'), std::string::npos) << "far: points";
    EXPECT_NE(picture.find('.'), std::string::npos) << "and the sky";
    // The picture as it is drawn, to be looked at: KGRAPH_SCALE_PICTURE names a .ppm file for it.
    if (const char* file = std::getenv("KGRAPH_SCALE_PICTURE"); file && *file) {
        runtime.Watch("canvas");
        for (int i = 0; i < 4; ++i) Frame();
        if (const auto peek = runtime.Watched("canvas"); peek && peek->bytes.size() >= std::size_t { Picture } * Picture * 4) {
            std::ofstream out(file, std::ios::binary);
            out << "P6\n" << Picture << " " << Picture << "\n255\n";
            for (std::size_t i = 0; i < std::size_t { Picture } * Picture; ++i) out.write(reinterpret_cast<const char*>(peek->bytes.data() + i * 4), 3);
        }
        runtime.Unwatch("canvas");
    }

    const double stillMs = Median(30, [](int) {});
    // Moving the whole field moves every instance with it: no instance is touched on the CPU.
    const double movedMs = Median(30, [&](const int frame) { world.SetPosition(field, { 0.f, frame % 2 ? 0.f : 0.5f, 0.f }); });
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    std::string timings;
    double gpu = 0.0;
    for (const auto& [id, timing] : runtime.Timings()) {
        gpu += timing.gpuMs;
        if (timing.gpuMs >= 0.01) timings += std::format("    {:<32} {:7.3f} ms\n", id, timing.gpuMs);
    }
    std::printf("%d instances (%.1f MB): set up and first frames %.0f ms; runtime Update, median a frame: still %.3f ms, field moved %.3f ms; GPU %.2f ms a frame:\n%s",
                count, (32.0 + 32.0 * std::ceil(count / 256.0) + 20.0 * count) / 1e6, firstMs, stillMs, movedMs, gpu, timings.c_str());
    EXPECT_LT(stillMs, 1.0) << "nothing a frame on the CPU is a number of instances";
}

// Ten thousand entities drawn by a Draw run once an element of a ForEach: a draw each, its matrix and colour as push
// constants. What that costs the CPU a frame: readying the draws, and recording them.
TEST_F(ScaleTest, TenThousandEntitiesDrawnOneByOne) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("shape", "Primitive").Set("shape", "eCube");
    graph.Add("placed", "GraphicsPipeline").Set("vertex.module", "placed").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "placed").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("drawn", "ForEach").Set("component", "placed").Set("with", Value::List { "Mesh" });
    graph.Add("draw", "Draw").Wire("pipeline", "placed").Wire("mesh", "drawn", "Mesh.mesh").Wire("push.model", "drawn", "matrix")
        .Wire("push.color", "drawn", "field.color");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const kecs::ComponentId placed = world.Find("placed");
    ASSERT_NE(placed, kecs::NoComponent);

    const int n = std::getenv("KGRAPH_SCALE_ENTITIES") ? Count() : 10'000;
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const float cell = 2.f / static_cast<float>(side);
    std::vector<kecs::Entity> entities;
    for (int i = 0; i < n; ++i) {
        const float x = -1.f + cell * (static_cast<float>(i % side) + 0.5f), y = -1.f + cell * (static_cast<float>(i / side) + 0.5f);
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, y, 0.f });
        world.SetScale(entity, { 0.1f, 0.1f, 0.1f });
        world.Add<kgraph::Mesh>(entity, { cube });
        support::Give(world, entity, "placed", { { "color", x > 0.f ? std::vector { 1.f, 0.f, 0.f, 1.f } : std::vector { 0.f, 1.f, 0.f, 1.f } } });
        entities.push_back(entity);
    }
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto colour = [&](const kor::u32 x, const kor::u32 y) { const auto p = scene->Pixel(x, y); return std::array<int, 3> { p.x, p.y, p.z }; };
    EXPECT_GT(colour(24, 16)[0], 200) << "the right half red";
    EXPECT_GT(colour(8, 16)[1], 200) << "the left half green";

    const double stillMs = Median(30, [](int) {});
    const double allMs = Median(30, [&](const int frame) {
        for (const kecs::Entity entity : entities) {
            const kecs::Vec3 at = world.PositionOf(entity);
            world.SetPosition(entity, { at.x, at.y, frame % 2 ? 0.f : 0.01f });
        }
    });
    const auto timing = runtime.Timings().at("draw");
    std::printf("%d entities drawn one by one: runtime Update, median a frame: nothing moving %.2f ms, all moving %.2f ms; "
                "the Draw's recording %.2f ms, readying %.2f ms; GPU %.2f ms\n", n, stillMs, allMs, timing.recordMs, timing.prepareMs, timing.gpuMs);
}

// E7's gate: a Draw `batched` draws the same picture as a Draw each, and ten thousand entities cost the CPU far less. The
// same ForEach, the same meshes; what each draw had as push constants, each instance has in its buffer. And one that is
// wired a value of each element is drawn one at a time, and says why.
TEST_F(ScaleTest, ABatchedDrawDrawsTheSamePictureForLess) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    const auto sphere = runtime.ResourceOf("$sphere");
    ASSERT_TRUE(cube && sphere);

    // A draw each, with placed.slang's push constants; batched, with placedInstanced.slang's buffer of instances; or
    // asked to be batched with placed.slang's, wired each element's matrix and colour.
    const auto make = [&](const bool batched, const bool wiredEach) {
        kgraph::Graph graph;
        graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
            .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
        graph.Add("canvas view", "ImageView").Wire("image", "canvas");
        graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
        graph.Add("shape", "Primitive").Set("shape", "eCube");
        const char* module = batched && !wiredEach ? "placedInstanced" : "placed";
        graph.Add("pipeline", "GraphicsPipeline").Set("vertex.module", module).Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
            .Set("fragment.module", module).Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
        graph.Add("drawn", "ForEach").Set("component", "pipeline").Set("with", Value::List { "Mesh" });
        kgraph::Node& draw = graph.Add("draw", "Draw").Wire("pipeline", "pipeline").Wire("mesh", "drawn", "Mesh.mesh");
        if (!batched || wiredEach) draw.Wire("push.model", "drawn", "matrix").Wire("push.color", "drawn", "field.color");
        if (batched) draw.Set("batched", true);
        graph.Add("screen", "Screen");
        graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
        return graph;
    };
    const auto picture = [&] {
        std::vector<std::array<int, 3>> pixels;
        for (kor::u32 y = 0; y < support::Size; ++y)
            for (kor::u32 x = 0; x < support::Size; ++x) { const auto p = scene->Pixel(x, y); pixels.push_back({ p.x, p.y, p.z }); }
        return pixels;
    };

    runtime.Set(make(false, false));
    Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const kecs::ComponentId own = world.Find("pipeline");
    ASSERT_NE(own, kecs::NoComponent);

    // Two meshes, every colour of a grid of them: what a batch groups by mesh, and draws in another order.
    const int n = std::getenv("KGRAPH_SCALE_ENTITIES") ? Count() : 10'000;
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const float cell = 2.f / static_cast<float>(side);
    std::vector<kecs::Entity> entities;
    for (int i = 0; i < n; ++i) {
        const float x = -1.f + cell * (static_cast<float>(i % side) + 0.5f), y = -1.f + cell * (static_cast<float>(i / side) + 0.5f);
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, y, 0.f });
        world.SetScale(entity, { cell * 0.8f, cell * 0.8f, cell * 0.8f });
        world.Add<kgraph::Mesh>(entity, { i % 3 == 0 ? sphere : cube });
        support::Give(world, entity, "pipeline", { { "color", { (x + 1.f) * 0.5f, (y + 1.f) * 0.5f, i % 3 == 0 ? 1.f : 0.f, 1.f } } });
        entities.push_back(entity);
    }
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto each = picture();
    const double eachMs = Median(30, [](int) {});
    const double eachRecordMs = runtime.Timings().at("draw").recordMs;

    // Batched: the same pipeline's component, now each instance's — "pipeline" names both.
    runtime.Set(make(true, false));
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    ASSERT_NE(runtime.Status("draw"), nullptr);
    EXPECT_TRUE(runtime.Status("draw")->note.empty()) << runtime.Status("draw")->note;
    EXPECT_EQ(runtime.ValueOf("draw/commands", "drawCount"), Value(2)) << "a command for each mesh";
    const auto batch = picture();
    int differing = 0;
    for (std::size_t i = 0; i < each.size(); ++i)
        if (std::abs(each[i][0] - batch[i][0]) > 1 || std::abs(each[i][1] - batch[i][1]) > 1 || std::abs(each[i][2] - batch[i][2]) > 1) ++differing;
    EXPECT_EQ(differing, 0) << "the same picture, pixel for pixel";
    const double batchMs = Median(30, [](int) {});
    const double batchRecordMs = runtime.Timings().at("draw").recordMs;
    // Moved, a batch draws them where they are now.
    world.SetPosition(entities[0], { 0.f, 0.f, 0.f });
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    world.SetPosition(entities[0], { -1.f + cell * 0.5f, -1.f + cell * 0.5f, 0.f });

    // Wired each element's matrix: one at a time, and it says so.
    runtime.Set(make(true, true));
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_NE(runtime.Status("draw"), nullptr);
    EXPECT_NE(runtime.Status("draw")->note.find("one at a time"), std::string::npos) << runtime.Status("draw")->note;
    EXPECT_NE(runtime.Status("draw")->note.find("'push.model'"), std::string::npos) << runtime.Status("draw")->note;
    const auto oneAtATime = picture();
    differing = 0;
    for (std::size_t i = 0; i < each.size(); ++i)
        if (std::abs(each[i][0] - oneAtATime[i][0]) > 1 || std::abs(each[i][1] - oneAtATime[i][1]) > 1 || std::abs(each[i][2] - oneAtATime[i][2]) > 1) ++differing;
    EXPECT_EQ(differing, 0) << "drawn one at a time, as it is wired";

    std::printf("%d entities: a draw each, runtime Update %.2f ms and recording %.2f ms a frame; batched, %.3f ms and %.3f ms\n",
                n, eachMs, eachRecordMs, batchMs, batchRecordMs);
    EXPECT_LT(batchMs + batchRecordMs, (eachMs + eachRecordMs) / 4) << "measurably faster";
}

// The same for compute: ten thousand entities, an image each, stamped by a Dispatch each and then by one Dispatch of
// all of them along z, each image written by its bindless storage index. What each costs the CPU a frame.
TEST_F(ScaleTest, OneDispatchOfTenThousandEntitiesCostsLess) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto make = [&](const bool batched) {
        kgraph::Graph graph;
        graph.Add("backdrop", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 4, 4 }).Set("usage", Value::List { "eSampled", "eTransferDst" });
        graph.Add("stamp shader", "Shader").Set("entryPoint.module", batched ? "stampBatched" : "stamp").Set("entryPoint.entry", "computeMain");
        graph.Add("stamp", "ComputePipeline").Wire("computeShader", "stamp shader").Wire("param.backdrop", "backdrop");
        if (batched) {
            graph.Add("all", "Entities");
            graph.Add("stamped", "Filter").Wire("entities", "all").Set("with", Value::List { "stamp" });
            graph.Add("run", "Dispatch").Wire("pipeline", "stamp").Wire("entities", "stamped").Set("groups.x", 1).Set("groups.y", 1);
        } else {
            graph.Add("stamped", "ForEach").Set("component", "stamp");
            graph.Add("run", "Dispatch").Wire("pipeline", "stamp").Set("cover", "target").Wire("param.target", "stamped", "field.target")
                .Wire("push.tint", "stamped", "field.tint").Wire("push.gain", "stamped", "field.gain");
        }
        return graph;
    };
    runtime.Set(make(false));
    Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const kecs::ComponentId stamp = world.Find("stamp");
    ASSERT_NE(stamp, kecs::NoComponent);
    const int n = std::getenv("KGRAPH_SCALE_ENTITIES") ? Count() : 10'000;
    for (int i = 0; i < n; ++i) {
        const kecs::Entity entity = world.Create();
        world.Add(entity, stamp);
        kgraph::Parameters(world, entity, stamp).Set("target", kgraph::MakeResource(kor::Image::Builder {}.SetType(kor::Image::Type::e2D)
                .SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2 { 8, 8 }).SetUsage(kor::Image::Usage::eStorage | kor::Image::Usage::eSampled).Build()))
            .Set("tint", kor::Vec4 { 1, 0, 0, 1 }).Set("gain", 1.f);
    }
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const double eachMs = Median(30, [](int) {});
    const double eachRecordMs = runtime.Timings().at("run").recordMs;

    runtime.Set(make(true));
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.ReportOf("run"), nullptr) << "run once, for all of them";
    const double batchMs = Median(30, [](int) {});
    const double batchRecordMs = runtime.Timings().at("run").recordMs;
    std::printf("%d entities: a Dispatch each, runtime Update %.2f ms and recording %.2f ms a frame; one Dispatch of all, %.3f ms and %.3f ms\n",
                n, eachMs, eachRecordMs, batchMs, batchRecordMs);
    EXPECT_LT(batchMs + batchRecordMs, (eachMs + eachRecordMs) / 4) << "measurably faster";
}

// The engine's CullInSight: of an indirect draw of entities, only what is in sight is drawn, worked out on the GPU. Half
// the cubes are in the picture and half beside it; the cull counts the first half into their mesh's command, and the
// draw draws them, through `visible`. Moved into sight, one more is drawn.
TEST_F(ScaleTest, CullInSightDrawsOnlyWhatIsSeen) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups").Set("mode", "eGPU");
    graph.Add("culled", "GraphicsPipeline").Set("vertex.module", "instancedCulled").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instancedCulled").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    // The matrices place them in the picture as they are: what they are seen with is the identity.
    const Value::List identity { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    graph.Add("cull", "Function").Set("function", "CullInSight").Wire("instances", "draw", "instances").Set("stride", 80).Set("matrixOffset", 0)
        .Wire("ranges", "pack", "rangeBuffer").Wire("commands", "commands", "commands")
        .Wire("drawCount", "commands", "drawCount").Wire("visible", "commands", "visible").Set("viewProjection", identity);
    graph.Add("draw", "Draw").Wire("pipeline", "culled").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "cull", "commands").Wire("drawCount", "commands", "drawCount").Wire("param.visible", "cull", "visible");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    graph.functions["CullInSight"] = support::EngineFunction("CullInSight");

    // Eight in the picture, along its middle; eight beside it, to the right.
    std::vector<kecs::Entity> entities;
    for (int i = 0; i < 16; ++i) {
        const kecs::Entity entity = world.Create();
        const float x = i < 8 ? -0.875f + 0.25f * static_cast<float>(i) : 2.f + 0.25f * static_cast<float>(i - 8);
        world.SetPosition(entity, { x, 0.f, 0.5f });
        world.SetScale(entity, { 0.2f, 0.2f, 0.2f });
        world.Add<kgraph::Mesh>(entity, { cube });
        entities.push_back(entity);
    }
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    for (const kecs::Entity entity : entities) support::Give(world, entity, "culled", { { "color", { 1, 0, 0, 1 } } });

    // How many the command says it draws: what the cull counted, read back.
    runtime.Watch("commands");
    const auto drawn = [&] {
        for (int i = 0; i < 6; ++i) Frame();
        for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
        const auto peek = runtime.Watched("commands");
        if (!peek || peek->bytes.size() < 20) return -1;
        std::uint32_t command[5];
        std::memcpy(command, peek->bytes.data(), 20);
        return static_cast<int>(command[1]);
    };
    EXPECT_EQ(drawn(), 8) << "those in sight, and none beside it";
    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(2, { 255, 0, 0 })) << "the first, drawn";
    EXPECT_TRUE(is(29, { 255, 0, 0 })) << "the last in sight, drawn";

    world.SetPosition(entities[8], { 0.f, 0.5f, 0.5f });
    EXPECT_EQ(drawn(), 9) << "moved into sight, it is drawn";
    world.SetPosition(entities[0], { -3.f, 0.f, 0.5f });
    EXPECT_EQ(drawn(), 8) << "and one moved out of it is not";
    EXPECT_TRUE(is(2, { 0, 0, 0 })) << "where it was, nothing";
}

// A batched Draw given `cull` is culled on the GPU by itself: CullInSight's passes are written out with it, and what
// its shader reads is compacted — the elements in sight one after another — so that a shader of [entity("PER_INSTANCE")]
// that reads its instance at first + index, and knows nothing of culling, draws only what is in sight, as it was.
TEST_F(ScaleTest, ABatchedDrawGivenCullIsCulledOnTheGpu) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    std::vector<kecs::Entity> entities;
    for (int i = 0; i < 16; ++i) {
        const kecs::Entity entity = world.Create();
        const float x = i < 8 ? -0.875f + 0.25f * static_cast<float>(i) : 2.f + 0.25f * static_cast<float>(i - 8);
        world.SetPosition(entity, { x, 0.f, 0.5f });
        world.SetScale(entity, { 0.2f, 0.2f, 0.2f });
        world.Add<kgraph::Mesh>(entity, { cube });
        entities.push_back(entity);
    }
    const auto make = [&](const bool culled) {
        kgraph::Graph graph;
        graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
            .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
        graph.Add("canvas view", "ImageView").Wire("image", "canvas");
        graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
        graph.Add("shape", "Primitive").Set("shape", "eCube");
        graph.Add("tinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
            .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
        graph.Add("drawn", "ForEach").Set("component", "Mesh");
        // The matrices place them in the picture as they are: what they are seen with is the identity.
        graph.Add("sight", "MatrixTranslate").Set("offset", Value::List { 0, 0, 0 });
        kgraph::Node& draw = graph.Add("draw", "Draw").Wire("pipeline", "tinted").Wire("mesh", "drawn", "field.mesh").Set("batched", true);
        if (culled) draw.Wire("cull", "sight");
        graph.Add("screen", "Screen");
        graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
        return graph;
    };
    const auto picture = [&] {
        std::vector<kor::U8Vec4> pixels;
        for (kor::u32 y = 0; y < support::Size; ++y)
            for (kor::u32 x = 0; x < support::Size; ++x) pixels.push_back(scene->Pixel(x, y));
        return pixels;
    };
    const auto frames = [&] { for (int i = 0; i < 6; ++i) Frame(); for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem; };

    runtime.Set(make(false));
    frames();
    ASSERT_TRUE(runtime.Ok());
    for (const kecs::Entity entity : entities) support::Give(world, entity, "tinted", { { "color", { 1, 0, 0, 1 } } });
    frames();
    const auto whole = picture();

    runtime.Set(make(true));
    frames();
    ASSERT_TRUE(runtime.Ok());
    ASSERT_NE(runtime.Status("draw"), nullptr);
    EXPECT_TRUE(runtime.Status("draw")->note.empty()) << runtime.Status("draw")->note;
    ASSERT_NE(runtime.Status("draw/cull"), nullptr) << "the cull, written out with it";
    EXPECT_TRUE(runtime.Status("draw/cull")->ok) << runtime.Status("draw/cull")->error;
    // How many its command draws: what the cull counted, read back.
    runtime.Watch("draw/commands");
    const auto drawn = [&] {
        frames();
        const auto peek = runtime.Watched("draw/commands");
        if (!peek || peek->bytes.size() < 20) return -1;
        std::uint32_t command[5];
        std::memcpy(command, peek->bytes.data(), 20);
        return static_cast<int>(command[1]);
    };
    EXPECT_EQ(drawn(), 8) << "those in sight, and none beside it";
    const auto culled = picture();
    int differing = 0;
    for (std::size_t i = 0; i < whole.size(); ++i)
        if (std::abs(whole[i].x - culled[i].x) > 1 || std::abs(whole[i].y - culled[i].y) > 1 || std::abs(whole[i].z - culled[i].z) > 1) ++differing;
    EXPECT_EQ(differing, 0) << "the same picture: what was left out was not in it";

    // One moved into sight is drawn, where it is, with what it holds.
    world.SetPosition(entities[8], { 0.f, 0.5f, 0.5f });
    EXPECT_EQ(drawn(), 9);
    const auto p = scene->Pixel(16, 24);     // y down the picture, as Vulkan has it
    EXPECT_GT(p.x, 200) << "red, drawn from its own element";
    EXPECT_LT(p.z, 50) << "not the magenta of an element no entity is in";
}

// What gathers every element of a ForEach into one — a Collect, a buffer of them, their meshes packed — is of them all,
// not each element's: a batched Draw wired from one (a lamp's shadow, idle by a PointShadows of a Collect of the lights;
// a pipeline laid out as a MeshPack of the same entities) is still one draw.
TEST_F(ScaleTest, ADrawWiredFromAllTheElementsAtOnceIsStillBatched) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    for (int i = 0; i < 4; ++i) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { -0.75f + 0.5f * static_cast<float>(i), 0.f, 0.5f });
        world.SetScale(entity, { 0.2f, 0.2f, 0.2f });
        world.Add<kgraph::Mesh>(entity, { cube });
    }
    const kecs::Entity lamp = world.Create();
    support::Give(world, lamp, "PointLight", { { "range", { 5 } } });
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas");
    graph.Add("drawn", "ForEach").Set("component", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "drawn", "field.mesh");
    graph.Add("tinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("lamps", "ForEach").Set("component", "PointLight");
    graph.Add("reaches", "Collect").Wire("value", "lamps", "field.range");
    graph.Add("first reach", "Slice").Wire("of", "reaches").Set("first", 0).Set("count", 1);
    graph.Add("never", "Multiply").Wire("a", "first reach").Set("b", 0);
    graph.Add("draw", "Draw").Wire("pipeline", "tinted").Wire("mesh", "drawn", "field.mesh").Set("batched", true).Wire("idle", "never");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_NE(runtime.Status("draw"), nullptr);
    EXPECT_TRUE(runtime.Status("draw")->note.empty()) << runtime.Status("draw")->note;
    EXPECT_NE(runtime.Status("draw/pack"), nullptr) << "written out as one draw: its meshes packed";
}

// A shadow's cascades, as a batched Draw has them: a turn each of a Repeat, `cull` several matrices one after another and
// `cullIndex` the turn's number. Each turn is culled on the GPU by its own — the `cullIndex`th — and all of them share the
// batch and the pack. Here the first sees the cubes in the picture, the second those beside it. The entities have none of
// the pipeline's component: what each instance is, is where its entity is, from the first frame on.
TEST_F(ScaleTest, CascadesOfABatchedDrawAreEachCulledByTheirOwnMatrix) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    for (int i = 0; i < 16; ++i) {
        const kecs::Entity entity = world.Create();
        const float x = i < 8 ? -0.875f + 0.25f * static_cast<float>(i) : 2.f + 0.25f * static_cast<float>(i - 8);
        world.SetPosition(entity, { x, 0.f, 0.5f });
        world.SetScale(entity, { 0.2f, 0.2f, 0.2f });
        world.Add<kgraph::Mesh>(entity, { cube });
    }
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("shape", "Primitive").Set("shape", "eCube");
    graph.Add("tinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("drawn", "ForEach").Set("component", "Mesh");
    // Two cascades' matrices, one after the other: as the picture is, and moved left by 2.875 — what is beside it, in it.
    graph.Add("as it is", "MatrixTranslate").Set("offset", Value::List { 0, 0, 0 });
    graph.Add("beside", "MatrixTranslate").Set("offset", Value::List { -2.875, 0, 0 });
    graph.Add("cascades", "Append").Wire("a", "as it is").Wire("b", "beside");
    graph.Add("cascade", "Repeat").Set("count", 2);
    graph.Add("draw", "Draw").Wire("pipeline", "tinted").Wire("mesh", "drawn", "field.mesh").Set("batched", true)
        .Wire("cull", "cascades").Wire("cullIndex", "cascade", "index").Set("load", true);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 6; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    for (const char* turn : { "draw [0]", "draw [1]" }) {
        ASSERT_NE(runtime.Status(turn), nullptr) << turn;
        EXPECT_TRUE(runtime.Status(turn)->note.empty()) << turn << ": " << runtime.Status(turn)->note;
        ASSERT_NE(runtime.Status(std::string(turn) + "/cull"), nullptr) << turn << ": culled on the GPU";
    }
    EXPECT_NE(runtime.Status("draw [0]/pack"), nullptr);
    EXPECT_EQ(runtime.Status("draw [1]/pack"), nullptr) << "one pack for both";
    // Each turn's command read back, a turn at a time.
    for (const char* turn : { "draw [0]", "draw [1]" }) {
        runtime.Watch(std::string(turn) + "/commands");
        for (int i = 0; i < 6; ++i) Frame();
        const auto peek = runtime.Watched(std::string(turn) + "/commands");
        ASSERT_TRUE(peek && peek->bytes.size() >= 20) << turn;
        std::uint32_t command[5];
        std::memcpy(command, peek->bytes.data(), 20);
        EXPECT_EQ(command[1], 8u) << turn << ": its own cascade's eight";
        runtime.Unwatch(std::string(turn) + "/commands");
    }
}

// The engine's InstanceCull: two draws of the same instances, the first of what was seen the frame before, the second of
// what the first's depth does not hide and was not seen. A wall is in front of four cubes and beside two: once it has
// settled, the first draw draws the wall and the two beside it, and the second nothing — the four behind are never drawn.
// The wall taken away, the four are seen again.
TEST_F(ScaleTest, InstanceCullDrawsNothingBehindWhatItDrewFirst) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);

    kgraph::Graph graph;
    graph.functions["InstanceCull"] = support::EngineFunction("InstanceCull");
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("depth", "Image").Set("format", "eD32_SFLOAT").Set("extent", Value::List { support::Size, support::Size });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("depth view", "ImageView").Wire("image", "depth");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view")
        .Wire("depthStencil.view", "depth view").Set("depthStencil.which", "Depth").Set("depthStencil.name", "depth").Set("depthStencil.depth", 1);
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    for (const char* phase : { "early", "late" })
        graph.Add(phase, "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups").Set("mode", "eGPU");
    graph.Add("culled", "GraphicsPipeline").Set("vertex.module", "instancedCulled").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instancedCulled").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    const Value::List identity { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    graph.Add("cull", "Function").Set("function", "InstanceCull").Wire("instances", "draw early", "instances").Set("stride", 80).Set("matrixOffset", 0)
        .Wire("ranges", "pack", "rangeBuffer").Wire("drawCount", "early", "drawCount").Set("viewProjection", identity)
        .Wire("early", "early", "commands").Wire("earlyVisible", "early", "visible").Wire("late", "late", "commands").Wire("lateVisible", "late", "visible")
        .Wire("depth", "depth").Set("count", 64);
    graph.Add("draw early", "Draw").Wire("pipeline", "culled").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "cull", "early").Wire("drawCount", "early", "drawCount").Wire("param.visible", "cull", "earlyVisible");
    graph.Add("draw late", "Draw").Wire("pipeline", "culled").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect").Set("load", true)
        .Wire("indirect", "cull", "late").Wire("drawCount", "late", "drawCount").Wire("param.visible", "cull", "lateVisible");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");

    // The matrices place them in the picture as they are: a wall across the middle, near; four small cubes behind it, far;
    // two beside it, as far.
    const auto place = [&](const kor::Vec3 at, const kor::Vec3 scale) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { at.x, at.y, at.z });
        world.SetScale(entity, { scale.x, scale.y, scale.z });
        world.Add<kgraph::Mesh>(entity, { cube });
        return entity;
    };
    const kecs::Entity wall = place({ 0.f, 0.f, 0.2f }, { 1.f, 1.f, 0.05f });
    std::vector<kecs::Entity> entities { wall };
    for (const float x : { -0.3f, -0.1f, 0.1f, 0.3f, -0.85f, 0.85f }) entities.push_back(place({ x, 0.f, 0.8f }, { 0.1f, 0.1f, 0.1f }));
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    for (const kecs::Entity entity : entities) support::Give(world, entity, "culled", { { "color", { 1, 0, 0, 1 } } });

    runtime.Watch("early");
    runtime.Watch("late");
    const auto drawn = [&](const char* phase) {
        const auto peek = runtime.Watched(phase);
        if (!peek || peek->bytes.size() < 20) return -1;
        std::uint32_t command[5];
        std::memcpy(command, peek->bytes.data(), 20);
        return static_cast<int>(command[1]);
    };
    for (int i = 0; i < 12; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(drawn("early"), 3) << "the wall and the two beside it: seen the frame before";
    EXPECT_EQ(drawn("late"), 0) << "and nothing more: the four behind the wall are behind what was drawn first";

    world.SetPosition(wall, { 5.f, 0.f, 0.2f });
    for (int i = 0; i < 12; ++i) Frame();
    EXPECT_EQ(drawn("early"), 6) << "the wall gone, the four are seen, and drawn first from the frame after";
    EXPECT_EQ(drawn("late"), 0);
}

// A buffer of every point light, through what is worked out of each (its colour times two) and where each is: a frame in
// which none of them changed works none of them out again, and costs a fraction of one in which all did. One changed is
// in the buffer the frame after, and what is not worked out again is what would have been.
TEST_F(ScaleTest, ABufferOfThousandsWorksOutOnlyWhatChanged) {
    using kgraph::Value;
    auto& runtime = *scene->runtime;
    auto& world = scene->world;
    const int count = Count();
    std::vector<kecs::Entity> lamps;
    for (int i = 0; i < count; ++i) {
        const kecs::Entity lamp = world.Create();
        support::Give(world, lamp, "PointLight", { { "color", { 0.25f, 0.5f, 1.f } }, { "range", { static_cast<float>(i % 100) } } });
        world.SetPosition(lamp, { static_cast<float>(i), 0.f, 0.f });
        lamps.push_back(lamp);
    }
    kgraph::Graph graph;
    graph.Add("lamps", "ForEach").Set("component", "PointLight");
    graph.Add("brighter", "Multiply").Wire("a", "lamps", "field.color").Set("b", 2);
    graph.Add("lights", "ShaderBuffer").Set("module", "lights").Set("entry", "computeMain").Set("parameter", "lights")
        .Wire("field.color", "brighter").Wire("field.range", "lamps", "field.range").Wire("field.position", "lamps", "worldPosition");
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    for (int i = 0; i < 3; ++i) Frame();

    const kecs::ComponentId light = world.Find("PointLight");
    ASSERT_NE(light, kecs::NoComponent);
    const auto& description = world.Describe(light);
    const auto range = std::ranges::find(description.fields, std::string("range"), &kecs::FieldDescription::name);
    ASSERT_NE(range, description.fields.end());
    const auto setRange = [&](const kecs::Entity lamp, const float to) {
        auto* bytes = static_cast<std::byte*>(world.Mutable(lamp, light));
        std::memcpy(bytes + range->offset, &to, sizeof(float));
        world.MarkChanged(lamp, light);
    };

    const double still = Median(9, [](int) {});
    const double all = Median(9, [&](const int frame) { for (int i = 0; i < count; ++i) setRange(lamps[i], static_cast<float>(frame + i % 7)); });
    RecordProperty("still_ms", std::to_string(still));
    RecordProperty("all_changed_ms", std::to_string(all));
    std::printf("            a buffer of %d lights: %.3f ms a frame with none changed, %.3f ms with all\n", count, still, all);
    EXPECT_LT(still, all / 4) << "a frame of nothing changed works nothing out";

    // One changed: in the buffer, at its own element, and the rest as they were.
    setRange(lamps[count / 2], 1234.f);
    runtime.Watch("lights");
    for (int i = 0; i < 4; ++i) Frame();
    const auto peek = runtime.Watched("lights");
    ASSERT_TRUE(peek);
    constexpr std::size_t stride = 48, rangeAt = 12, colorAt = 16;
    ASSERT_GE(peek->bytes.size(), static_cast<std::size_t>(count) * stride);
    const auto number = [&](const std::size_t element, const std::size_t at) {
        float value = 0;
        std::memcpy(&value, peek->bytes.data() + element * stride + at, sizeof(float));
        return value;
    };
    EXPECT_EQ(number(count / 2, rangeAt), 1234.f) << "the one changed";
    EXPECT_EQ(number(count / 2 + 1, rangeAt), static_cast<float>(8 + (count / 2 + 1) % 7)) << "the next as the last of the frames that changed all left it";
    EXPECT_EQ(number(3, colorAt), 0.5f) << "its colour twice what it holds: worked out, and kept";
    EXPECT_EQ(number(3, 0), 3.f) << "where it is";

    // Kept is what would have been worked out: the same bytes as a buffer made anew.
    const std::vector<std::byte> kept = peek->bytes;
    runtime.Set(graph);
    for (int i = 0; i < 4; ++i) Frame();
    const auto fresh = runtime.Watched("lights");
    ASSERT_TRUE(fresh);
    EXPECT_TRUE(fresh->bytes == kept) << "what was kept is what is worked out";
}

// A model of many vertices, read: laid out as the device holds it off the frame, so that the frame it arrives in makes its
// buffers and no more. What the frames cost while it is read and as it arrives, the dearest of them.
TEST_F(ScaleTest, AModelOfManyVerticesArrivesInAFrameOfLittleWork) {
    constexpr int side = 200;      // 40,000 vertices, twice as many triangles less a row
    const auto file = std::filesystem::temp_directory_path() / std::format("kgraph-scale-grid-{}.obj", side);
    {
        std::ofstream out(file);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) out << "v " << x << ' ' << y << " 0\nvn 0 0 1\nvt " << x / float(side) << ' ' << y / float(side) << '\n';
        for (int y = 0; y + 1 < side; ++y)
            for (int x = 0; x + 1 < side; ++x) {
                const int a = y * side + x + 1, b = a + 1, c = a + side, d = c + 1;
                out << std::format("f {0}/{0}/{0} {1}/{1}/{1} {2}/{2}/{2}\nf {1}/{1}/{1} {3}/{3}/{3} {2}/{2}/{2}\n", a, b, c, d);
            }
    }
    auto& runtime = *scene->runtime;
    runtime.Set(kgraph::Graph {});
    const std::string id = "@" + file.generic_string() + "#0";
    ASSERT_NE(runtime.ResourceOf(id), nullptr);
    double dearest = 0.0;
    bool arrived = false;
    // Read on another thread, as long as that takes: a Debug build reads it in seconds.
    const auto began = std::chrono::steady_clock::now();
    while (!arrived && std::chrono::steady_clock::now() - began < std::chrono::seconds(60)) {
        Frame();
        dearest = std::max(dearest, scene->updateMs);
        const auto now = runtime.ResourceOf(id);
        arrived = now && !now->pending;
    }
    ASSERT_TRUE(arrived) << "read";
    std::string error;
    const auto read = runtime.ReadMesh(id, &error);
    ASSERT_TRUE(read) << error;
    EXPECT_EQ(read->vertexCount, static_cast<std::uint32_t>(side * side));
    RecordProperty("dearest_frame_ms", std::to_string(dearest));
    std::printf("            a model of %d vertices: its dearest frame %.3f ms\n", side * side, dearest);
    // Laid out on the frame, as it was, the frame it arrived in took about 100 ms (Debug); laid out off it, about 30.
    EXPECT_LT(dearest, 60.0) << "the frame it arrives in makes its buffers, and lays nothing out";
    std::filesystem::remove(file);
}
