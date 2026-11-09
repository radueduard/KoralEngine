// Pipelines run for each entity: the component generated for what a node leaves unwired, a descriptor set an
// entity, passes ordered by the resources entities hold, tags and switched-off components, and a shader edited
// under all of it. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <kasset/artifact.h>
#include <kasset/cache.h>

#include <kmath/transform.h>

#include <capiInterop.h>

#include <kgraph/scene.h>

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <functional>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include <image.h>
#include <mesh.h>
#include <vertexLayout.h>

#include <kecs/json.h>
#include <kecs/serialize.h>
#include <kgraph/entities.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using kgraph::Parameters;

    struct Lit {};
    struct Hidden {};

    // Many entities, each with an image of its own. One compute pass fills each entity's image with the entity's
    // colour, over a backdrop its pipeline wires for all of them; one graphics pass draws each entity's image where the
    // entity says. Each runs once an element of a ForEach — of its pipeline's component, the values its draws take — and
    // is wired what each element gives. The passes are listed last first.
    std::string SceneGraph(const std::string& stampModule = "stamp")
    {
        return std::format(R"json({{
      "kgraph": 1,
      "nodes": [
        {{ "id": "blit", "type": "Blit", "inputs": {{ "source": {{ "$link": ["canvas", "out"] }}, "destination": {{ "$link": ["screen", "out"] }} }} }},
        {{ "id": "draw", "type": "Draw", "inputs": {{ "pipeline": {{ "$link": ["show", "out"] }}, "mesh": {{ "$link": ["shown", "Mesh.mesh"] }},
            "param.source": {{ "$link": ["shown", "field.source"] }}, "push.offset": {{ "$link": ["shown", "field.offset"] }} }} }},
        {{ "id": "shown", "type": "ForEach", "inputs": {{ "component": "show", "with": ["Mesh"], "without": ["Hidden"] }} }},
        {{ "id": "run", "type": "Dispatch", "inputs": {{
            "pipeline": {{ "$link": ["stamp", "out"] }}, "cover": "target", "groupSize.x": 8, "groupSize.y": 8,
            "param.target": {{ "$link": ["lit", "field.target"] }}, "push.tint": {{ "$link": ["lit", "field.tint"] }}, "push.gain": {{ "$link": ["lit", "field.gain"] }} }} }},
        {{ "id": "lit", "type": "ForEach", "inputs": {{ "component": "stamp", "with": ["Lit"] }} }},
        {{ "id": "tint backdrop", "type": "Clear", "inputs": {{ "image": {{ "$link": ["backdrop", "out"] }}, "color": [0, 0, 0.25, 0] }} }},
        {{ "id": "screen", "type": "Screen", "inputs": {{}} }},

        {{ "id": "backdrop", "type": "Image", "inputs": {{
            "format": "eRGBA8_UNORM", "extent.x": 4, "extent.y": 4, "extent.z": 1, "usage": ["eSampled", "eTransferDst"] }} }},
        {{ "id": "stamp shader", "type": "Shader", "inputs": {{ "entryPoint.module": "{}", "entryPoint.entry": "computeMain" }} }},
        {{ "id": "stamp", "type": "ComputePipeline", "inputs": {{
            "computeShader": {{ "$link": ["stamp shader", "out"] }}, "param.backdrop": {{ "$link": ["backdrop", "out"] }} }} }},

        {{ "id": "canvas", "type": "Image", "inputs": {{
            "format": "eRGBA8_UNORM", "extent.x": 32, "extent.y": 32, "extent.z": 1, "usage": ["eColorAttachment", "eTransferSrc"] }} }},
        {{ "id": "canvas view", "type": "ImageView", "inputs": {{ "image": {{ "$link": ["canvas", "out"] }} }} }},
        {{ "id": "target", "type": "Framebuffer", "inputs": {{ "color[0].name": "color", "color[0].view": {{ "$link": ["canvas view", "out"] }} }} }},
        {{ "id": "layout", "type": "VertexLayout", "inputs": {{ "attributes": [ {{ "semantic": "POSITION", "type": "eFloat", "count": 3 }} ] }} }},
        {{ "id": "vertex", "type": "Shader", "inputs": {{ "entryPoint.module": "show", "entryPoint.entry": "vertexMain" }} }},
        {{ "id": "fragment", "type": "Shader", "inputs": {{ "entryPoint.module": "show", "entryPoint.entry": "fragmentMain" }} }},
        {{ "id": "show", "type": "GraphicsPipeline", "inputs": {{
            "vertexShader.shader": {{ "$link": ["vertex", "out"] }}, "vertexShader.layout": {{ "$link": ["layout", "out"] }},
            "fragmentShader": {{ "$link": ["fragment", "out"] }}, "framebuffer": {{ "$link": ["target", "out"] }} }} }}
      ]
    }})json", stampModule);
    }

    // SceneGraph, batched: one dispatch stamps every entity's image, along z, and one draw shows all of them. Their
    // pipelines' components are named as SceneGraph's are and have the same fields, so the same entities serve both.
    // The images are of two sizes: as many groups as the largest needs, and each shader looks at its own image's size.
    std::string BatchedSceneGraph()
    {
        return R"json({
      "kgraph": 1,
      "nodes": [
        { "id": "blit", "type": "Blit", "inputs": { "source": { "$link": ["canvas", "out"] }, "destination": { "$link": ["screen", "out"] } } },
        { "id": "draw", "type": "Draw", "inputs": { "pipeline": { "$link": ["show", "out"] }, "mesh": { "$link": ["shown", "Mesh.mesh"] }, "batched": true } },
        { "id": "shown", "type": "Filter", "inputs": { "entities": { "$link": ["all", "out"] }, "with": ["show", "Mesh"], "without": ["Hidden"] } },
        { "id": "run", "type": "Dispatch", "inputs": { "pipeline": { "$link": ["stamp", "out"] }, "entities": { "$link": ["lit", "out"] }, "groups.x": 2, "groups.y": 2 } },
        { "id": "lit", "type": "Filter", "inputs": { "entities": { "$link": ["all", "out"] }, "with": ["stamp", "Lit"] } },
        { "id": "all", "type": "Entities", "inputs": {} },
        { "id": "tint backdrop", "type": "Clear", "inputs": { "image": { "$link": ["backdrop", "out"] }, "color": [0, 0, 0.25, 0] } },
        { "id": "screen", "type": "Screen", "inputs": {} },

        { "id": "backdrop", "type": "Image", "inputs": {
            "format": "eRGBA8_UNORM", "extent.x": 4, "extent.y": 4, "extent.z": 1, "usage": ["eSampled", "eTransferDst"] } },
        { "id": "stamp shader", "type": "Shader", "inputs": { "entryPoint.module": "stampBatched", "entryPoint.entry": "computeMain" } },
        { "id": "stamp", "type": "ComputePipeline", "inputs": {
            "computeShader": { "$link": ["stamp shader", "out"] }, "param.backdrop": { "$link": ["backdrop", "out"] } } },

        { "id": "canvas", "type": "Image", "inputs": {
            "format": "eRGBA8_UNORM", "extent.x": 32, "extent.y": 32, "extent.z": 1, "usage": ["eColorAttachment", "eTransferSrc"] } },
        { "id": "canvas view", "type": "ImageView", "inputs": { "image": { "$link": ["canvas", "out"] } } },
        { "id": "target", "type": "Framebuffer", "inputs": { "color[0].name": "color", "color[0].view": { "$link": ["canvas view", "out"] } } },
        { "id": "layout", "type": "VertexLayout", "inputs": { "attributes": [ { "semantic": "POSITION", "type": "eFloat", "count": 3 } ] } },
        { "id": "vertex", "type": "Shader", "inputs": { "entryPoint.module": "showBatched", "entryPoint.entry": "vertexMain" } },
        { "id": "fragment", "type": "Shader", "inputs": { "entryPoint.module": "showBatched", "entryPoint.entry": "fragmentMain" } },
        { "id": "show", "type": "GraphicsPipeline", "inputs": {
            "vertexShader.shader": { "$link": ["vertex", "out"] }, "vertexShader.layout": { "$link": ["layout", "out"] },
            "fragmentShader": { "$link": ["fragment", "out"] }, "framebuffer": { "$link": ["target", "out"] } } }
      ]
    })json";
    }

    /** Another view of a runtime's graph, over its world, in a scene of its own: Set again whenever the first is. */
    class FollowScene final : public kor::Scene {
    public:
        static inline kgraph::Runtime* source = nullptr;
        static inline kecs::World* world = nullptr;
        void Initialize() override { runtime = std::make_unique<kgraph::Runtime>(Graph(), world, *source); }
        void Update() override
        {
            if (source->Revision() != _seen) { _seen = source->Revision(); runtime->Set(source->Current()); }
            runtime->Update();
        }
        void Shutdown() override { runtime.reset(); }
        std::unique_ptr<kgraph::Runtime> runtime;
    private:
        std::uint64_t _seen = 0;
    };

    class EntityTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "entities", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }
        void Load(const std::string& json) {
            kgraph::Graph graph;
            std::string error;
            ASSERT_TRUE(kgraph::Graph::FromJson(json, graph, error)) << error;
            scene->runtime->Set(std::move(graph));
            for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
            stamp = scene->runtime->ComponentOf("stamp");
            show = scene->runtime->ComponentOf("show");
        }

        static std::shared_ptr<kgraph::Resource> Image(const kor::u32 size) {
            return kgraph::MakeResource(kor::Image::Builder{}
                .SetType(kor::Image::Type::e2D).SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2 { size, size })
                .SetUsage(kor::Image::Usage::eStorage | kor::Image::Usage::eSampled).Build());
        }
        /** A unit square: positions alone, or — to be one a pipeline of positions cannot draw — with something before them. */
        static std::shared_ptr<kgraph::Resource> Square(const bool fits = true) {
            const std::array<kor::u32, 6> indices { 0, 1, 2, 2, 1, 3 };
            if (fits) {
                const std::array<kor::Vec3, 4> corners { kor::Vec3 { 0, 0, 0 }, kor::Vec3 { 1, 0, 0 }, kor::Vec3 { 0, 1, 0 }, kor::Vec3 { 1, 1, 0 } };
                return kgraph::MakeResource(kor::Mesh::Builder()
                    .SetVertexBuffer(0, kor::Mesh::MakeBuffer(corners, kor::Buffer::Usage::eVertex))
                    .SetIndexBuffer(kor::Mesh::MakeBuffer(indices, kor::Buffer::Usage::eIndex))
                    .SetVertexLayout(kor::VertexLayout {
                        .bindings = { kor::VertexInputBindingDescription(0, sizeof(kor::Vec3)) },
                        .attributes = { kor::VertexLayout::Attribute("POSITION", "", 0, 0, kor::ChannelType::eFloat, 3) } })
                    .Build());
            }
            // One that does not fit has no POSITION at all: what the vertex shader reads. (Laid out otherwise is no
            // matter: a pipeline is made again for any layout that has what its shader reads.)
            const std::array<kor::Vec4, 4> colors {};
            return kgraph::MakeResource(kor::Mesh::Builder()
                .SetVertexBuffer(0, kor::Mesh::MakeBuffer(colors, kor::Buffer::Usage::eVertex))
                .SetIndexBuffer(kor::Mesh::MakeBuffer(indices, kor::Buffer::Usage::eIndex))
                .SetVertexLayout(kor::VertexLayout {
                    .bindings = { kor::VertexInputBindingDescription(0, sizeof(kor::Vec4)) },
                    .attributes = { kor::VertexLayout::Attribute("COLOR", "", 0, 0, kor::ChannelType::eFloat, 4) } })
                .Build());
        }

        /** An entity with an image of its own, stamped @p tint and drawn in quarter @p quarter of the picture. */
        kecs::Entity Make(const int quarter, const kor::Vec4 tint, const float gain = 1.f, const kor::u32 imageSize = 8) {
            auto& world = scene->world;
            const kecs::Entity entity = world.Create();
            const auto image = Image(imageSize);
            world.Add(entity, stamp);
            Parameters(world, entity, stamp).Set("target", image).Set("tint", tint).Set("gain", gain);
            world.Add(entity, show);
            Parameters(world, entity, show).Set("source", image).Set("offset", kor::Vec2 { quarter % 2 ? 0.f : -1.f, quarter / 2 ? 0.f : -1.f });
            world.Add<kgraph::Mesh>(entity, { square });
            world.Add<Lit>(entity);
            return entity;
        }
        /** The middle of quarter @p quarter of the picture. */
        [[nodiscard]] kor::U8Vec4 Quarter(const int quarter) const { return scene->Pixel(quarter % 2 ? 24 : 8, quarter / 2 ? 24 : 8); }
        static void ExpectColor(const kor::U8Vec4 pixel, const int r, const int g, const int b, const char* what) {
            EXPECT_NEAR(pixel.x, r, 1) << what;
            EXPECT_NEAR(pixel.y, g, 1) << what;
            EXPECT_NEAR(pixel.z, b, 1) << what;
        }
        void MakeFour() {
            square = Square();
            entities = {
                Make(0, { 1, 0, 0, 1 }),
                Make(1, { 0, 1, 0, 1 }, 1.f, 16),     // a larger image: it takes more groups to cover
                Make(2, { 1, 1, 0, 1 }, 0.5f),
                Make(3, { 0, 1, 1, 1 }),
            };
        }

        GraphScene* scene = nullptr;
        kecs::ComponentId stamp = kecs::NoComponent, show = kecs::NoComponent;
        std::shared_ptr<kgraph::Resource> square;
        std::vector<kecs::Entity> entities;
    };
}

// A tag is asked for by name in a node: these are theirs.
template <> struct kecs::Name<Lit> { static constexpr std::string_view value = "Lit"; };
template <> struct kecs::Name<Hidden> { static constexpr std::string_view value = "Hidden"; };

// E7's gate, for compute: one Dispatch of a Filter's entities, along z, stamps every entity's own image as a Dispatch
// each does — each image written by its bindless storage index — and one batched Draw shows them: the same picture.
TEST_F(EntityTest, OneDispatchOfEveryEntityStampsWhatADispatchEachDoes) {
    Load(SceneGraph());
    MakeFour();
    for (int i = 0; i < 3; ++i) Frame();
    const auto picture = [&] {
        std::vector<kor::U8Vec4> pixels;
        for (kor::u32 y = 0; y < support::Size; ++y)
            for (kor::u32 x = 0; x < support::Size; ++x) pixels.push_back(scene->Pixel(x, y));
        return pixels;
    };
    const auto each = picture();
    ExpectColor(Quarter(1), 0, 255, 64, "a dispatch each: green");

    Load(BatchedSceneGraph());
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(scene->runtime->Ok());
    // Run once for all of them, and ran: no report, as for any pass that runs once.
    for (const char* id : { "run", "draw" })
        if (const auto* report = scene->runtime->ReportOf(id)) ADD_FAILURE() << id << ": ran " << report->ran << ", skipped " << report->skipped << ": " << report->why;
    EXPECT_EQ(scene->runtime->ValueOf("draw/commands", "drawCount"), kgraph::Value(1)) << "one mesh";
    // What the images are is known to the frame graph by what the instances name: written, then read.
    const auto schedule = scene->Schedule();
    EXPECT_LT(std::ranges::find(schedule, "run") - schedule.begin(), std::ranges::find(schedule, "draw") - schedule.begin());
    const auto batched = picture();
    int differing = 0;
    for (std::size_t i = 0; i < each.size(); ++i)
        if (std::abs(each[i].x - batched[i].x) > 1 || std::abs(each[i].y - batched[i].y) > 1 || std::abs(each[i].z - batched[i].z) > 1) ++differing;
    EXPECT_EQ(differing, 0) << "the same picture";

    // An entity's colour changed: stamped again, and shown.
    Parameters(scene->world, entities[0], stamp).Set("tint", kor::Vec4 { 0, 0, 1, 1 });
    for (int i = 0; i < 3; ++i) Frame();
    ExpectColor(Quarter(0), 0, 0, 255, "blue now, over the backdrop's blue");
    ExpectColor(Quarter(3), 0, 255, 255, "the others as they were");
}

// One Dispatch of every entity counts its groups along z as one dispatch does: three entities of 20,000 groups each are
// 60,000, run, every element's own; a fourth is 80,000, past the 65,535 a dispatch counts — and a dispatch from a base
// counts no further — so it is not run, and its report says why. Nothing the device would not have.
TEST_F(EntityTest, ADispatchOfEveryEntityRunsAsManyGroupsAsOneDispatchCounts) {
    Load(R"json({
      "kgraph": 1,
      "nodes": [
        { "id": "all", "type": "Entities", "inputs": {} },
        { "id": "counted", "type": "Filter", "inputs": { "entities": { "$link": ["all", "out"] }, "with": ["counter"] } },
        { "id": "counts", "type": "Buffer", "inputs": { "instanceCount": 16, "usage": ["eStorage", "eTransferSrc", "eTransferDst"] } },
        { "id": "zero", "type": "ClearBuffer", "inputs": { "buffer": { "$link": ["counts", "out"] } } },
        { "id": "counter shader", "type": "Shader", "inputs": { "entryPoint.module": "countBatched", "entryPoint.entry": "computeMain" } },
        { "id": "counter", "type": "ComputePipeline", "inputs": {
            "computeShader": { "$link": ["counter shader", "out"] }, "param.counts": { "$link": ["counts", "out"] } } },
        { "id": "run", "type": "Dispatch", "inputs": { "pipeline": { "$link": ["counter", "out"] }, "entities": { "$link": ["counted", "out"] }, "groups.x": 1, "groups.y": 1, "groups.z": 20000 } }
      ]
    })json");
    auto& runtime = *scene->runtime;
    const kecs::ComponentId counter = runtime.ComponentOf("counter");
    ASSERT_NE(counter, kecs::NoComponent);
    const auto make = [&](const int which) {
        const kecs::Entity entity = scene->world.Create();
        scene->world.Add(entity, counter);
        Parameters(scene->world, entity, counter).Set("which", static_cast<float>(which));
    };
    const std::uint64_t before = kor::log::LastSequence();
    for (int i = 0; i < 3; ++i) make(i);
    for (int i = 0; i < 3; ++i) Frame();        // the entities found, and run
    runtime.Watch("counts");
    for (int i = 0; i < 4; ++i) Frame();
    const std::optional<kgraph::Peek> peek = runtime.Watched("counts");
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    if (const auto* report = runtime.ReportOf("run")) EXPECT_EQ(report->skipped, 0u) << report->why;
    ASSERT_TRUE(peek && peek->bytes.size() >= 4 * sizeof(std::uint32_t)) << "nothing to look at";
    std::uint32_t counts[4] {};
    std::memcpy(counts, peek->bytes.data(), sizeof(counts));
    for (int i = 0; i < 3; ++i) EXPECT_EQ(counts[i], 20000u) << "element " << i << ": cleared each frame, then all 20,000 of its groups";

    make(3);
    for (int i = 0; i < 4; ++i) Frame();
    const auto* report = runtime.ReportOf("run");
    ASSERT_NE(report, nullptr) << "one past what a dispatch counts is said";
    EXPECT_EQ(report->ran, 0u);
    EXPECT_NE(report->why.find("65535"), std::string::npos) << report->why;
    for (const auto& record : kor::log::HistorySince(before))
        if (record.message.find("VUID") != std::string::npos) ADD_FAILURE() << record.message;
}

// An entity destroyed after the passes are readied and before they record — what stopping a scene that plays does to
// what its scripts made — takes its mesh with it, if it alone held it. Readied with it, it is drawn with it this frame
// all the same: what is drawn is held until the passes are readied again, never recorded gone.
TEST_F(EntityTest, AnEntityDestroyedBeforeItsDrawRecordsIsDrawnWithWhatItHad) {
    Load(SceneGraph());
    MakeFour();
    // Its own mesh, which nothing else holds: not the first element's, which a ForEach's outputs hold besides.
    scene->world.Mutable<kgraph::Mesh>(entities[2])->mesh = Square();
    std::weak_ptr<kgraph::Resource> mesh = scene->world.Get<kgraph::Mesh>(entities[2])->mesh;
    for (int i = 0; i < 3; ++i) Frame();
    ExpectColor(Quarter(2), 128, 128, 64, "drawn, in its own mesh");

    const std::uint64_t before = kor::log::LastSequence();
    scene->afterUpdate = [&] { scene->world.Destroy(entities[2]); };
    Frame();
    EXPECT_FALSE(mesh.expired()) << "held for the frame it was readied in";
    for (const auto& record : kor::log::HistorySince(before)) ADD_FAILURE() << record.message;
    Frame();
    for (const auto& record : kor::log::HistorySince(before)) ADD_FAILURE() << record.message;
    EXPECT_TRUE(mesh.expired()) << "and let go of once it is not drawn";
    ExpectColor(Quarter(2), 0, 0, 0, "gone: nothing drawn there");
    ExpectColor(Quarter(0), 255, 0, 64, "and the others as they were");
}

TEST_F(EntityTest, APipelinesUnwiredParametersAreAComponent) {
    Load(SceneGraph());
    auto& world = scene->world;
    ASSERT_NE(stamp, kecs::NoComponent);
    ASSERT_NE(show, kecs::NoComponent);
    EXPECT_EQ(world.Find("stamp"), stamp) << "named after the pipeline's node";

    // What the node left unwired, and the shader's push constants. What it wired is the pipeline's own.
    const auto& type = world.Describe(stamp);
    ASSERT_NE(type.FindField("target"), nullptr);
    EXPECT_EQ(type.FindField("target")->type, "Image");
    ASSERT_NE(type.FindField("tint"), nullptr);
    EXPECT_EQ(type.FindField("tint")->type, "vec4");
    EXPECT_EQ(type.FindField("gain")->type, "f32");
    EXPECT_EQ(type.FindField("backdrop"), nullptr);
    EXPECT_EQ(type.fields.size(), 3u);
    EXPECT_EQ(scene->runtime->PerEntityParameters("stamp"), std::vector<std::string> { "target" });
    EXPECT_EQ(world.Describe(show).FindField("offset")->type, "vec2");

    const kecs::Entity entity = world.Create();
    world.Add(entity, stamp);
    Parameters values(world, entity, stamp);
    EXPECT_EQ(values.ResourceAt("target"), nullptr);
    EXPECT_EQ(values.Get<float>("gain"), 0.f);
    const auto image = Image(8);
    values.Set("target", image).Set("tint", kor::Vec4 { 1, 2, 3, 4 });
    EXPECT_EQ(values.ResourceAt("target"), image);
    EXPECT_EQ(values.Get<kor::Vec4>("tint"), (kor::Vec4 { 1, 2, 3, 4 }));
    EXPECT_THROW(values.Set("tint", 1.f), std::invalid_argument) << "a vec4 is not a float";
    EXPECT_THROW(values.Set("nothing", 1.f), std::invalid_argument);
    EXPECT_THROW(values.Set("gain", image), std::invalid_argument);

    // Held by the component, and let go with it.
    std::weak_ptr<kgraph::Resource> held = image;
    const kecs::Entity copy = world.Create();
    world.Add(copy, stamp, std::as_const(world).Get(entity, stamp));
    world.Destroy(entity);
    EXPECT_EQ(Parameters(world, copy, stamp).ResourceAt("target"), image) << "a copy holds it too";
    world.Destroy(copy);
    EXPECT_EQ(held.use_count(), 1);
}

TEST_F(EntityTest, EachEntitysImageIsComputedThenDrawn) {
    Load(SceneGraph());
    MakeFour();
    Frame();
    Frame();
    for (const char* id : { "run", "draw" }) {
        const auto* report = scene->runtime->ReportOf(id);
        ASSERT_NE(report, nullptr);
        EXPECT_EQ(report->ran, 4u) << id;
        EXPECT_EQ(report->skipped, 0u) << id << ": " << report->why;
    }
    // The node list has them last first. What they use puts them right: the backdrop is cleared before it is read,
    // and the images entities hold are written before they are drawn — though no wire says the two passes share them.
    EXPECT_EQ(scene->Schedule(), (std::vector<std::string> { "tint backdrop", "run", "draw", "blit", "Read screen" }));

    ExpectColor(Quarter(0), 255, 0, 64, "red, over the backdrop's blue");
    ExpectColor(Quarter(1), 0, 255, 64, "green: an image four times the size, covered whole");
    ExpectColor(Quarter(2), 128, 128, 64, "yellow at half gain");
    ExpectColor(Quarter(3), 0, 255, 255, "cyan");
    ExpectColor(scene->Pixel(15, 15), 255, 0, 64, "each fills its quarter");
    ExpectColor(scene->Pixel(16, 16), 0, 255, 255, "and no more");

    // An entity's values are read each frame.
    Parameters(scene->world, entities[0], stamp).Set("tint", kor::Vec4 { 0, 0, 1, 1 });
    Parameters(scene->world, entities[3], show).Set("offset", kor::Vec2 { -0.5f, -0.5f });
    Frame();
    Frame();
    ExpectColor(scene->Pixel(4, 4), 0, 0, 255, "its new colour");
    ExpectColor(scene->Pixel(16, 16), 0, 255, 255, "moved to the middle, drawn last");
    ExpectColor(scene->Pixel(28, 28), 0, 0, 0, "and gone from where it was");

    // Given another image to draw — one another entity fills — it draws that.
    Parameters(scene->world, entities[2], show).Set("source", Parameters(scene->world, entities[1], stamp).ResourceAt("target"));
    Frame();
    Frame();
    ExpectColor(scene->Pixel(4, 28), 0, 255, 64, "the green one's image, where the yellow one is drawn");
}

TEST_F(EntityTest, TagsAndSwitchedOffComponentsNarrowWhichEntitiesRun) {
    Load(SceneGraph());
    MakeFour();
    auto& world = scene->world;
    Frame();
    Frame();

    // Hidden: the draw passes it by. The compute pass does not ask.
    world.Add<Hidden>(entities[3]);
    // Its drawing switched off: it has the component still, and is not drawn.
    world.SetEnabled(entities[1], show, false);
    // No longer Lit: its image is left as it was, whatever its colour is now.
    world.Remove<Lit>(entities[0]);
    Parameters(world, entities[0], stamp).Set("tint", kor::Vec4 { 0, 0, 1, 1 });
    Frame();
    Frame();
    EXPECT_EQ(scene->runtime->ReportOf("draw")->ran, 2u);
    EXPECT_EQ(scene->runtime->ReportOf("run")->ran, 3u);
    ExpectColor(Quarter(3), 0, 0, 0, "hidden");
    ExpectColor(Quarter(1), 0, 0, 0, "switched off");
    ExpectColor(Quarter(0), 255, 0, 64, "drawn, with the image it was last given");
    ExpectColor(Quarter(2), 128, 128, 64, "untouched by any of it");

    world.Remove<Hidden>(entities[3]);
    world.SetEnabled(entities[1], show, true);
    world.Add<Lit>(entities[0]);
    world.Destroy(entities[2]);
    Frame();
    Frame();
    ExpectColor(Quarter(3), 0, 255, 255, "shown again");
    ExpectColor(Quarter(1), 0, 255, 64, "switched back on");
    ExpectColor(Quarter(0), 0, 0, 255, "lit again, in its new colour");
    ExpectColor(Quarter(2), 0, 0, 0, "destroyed");
    EXPECT_EQ(scene->runtime->ReportOf("draw")->ran, 3u);
}

TEST_F(EntityTest, AnEntityThatCannotBeRunIsReportedAndPassedBy) {
    Load(SceneGraph());
    MakeFour();
    auto& world = scene->world;
    // One with a mesh that has not what the vertex shader reads, one with no image to draw, one with no mesh at all.
    world.Add<kgraph::Mesh>(entities[0], { Square(false) });
    Parameters(world, entities[1], show).Set("source", nullptr);
    world.Add<kgraph::Mesh>(entities[2], {});
    Frame();
    Frame();
    const auto* report = scene->runtime->ReportOf("draw");
    EXPECT_EQ(report->ran, 1u);
    EXPECT_EQ(report->skipped, 3u);
    EXPECT_NE(report->why.find(std::format("entity {}", entities[0].index)), std::string::npos) << report->why;
    EXPECT_NE(report->why.find("mesh"), std::string::npos) << "the first of them, and why: " << report->why;
    EXPECT_EQ(scene->runtime->ReportOf("run")->ran, 4u);
    ExpectColor(Quarter(0), 0, 0, 0, "not drawn");
    ExpectColor(Quarter(3), 0, 255, 255, "the one that can be");
    EXPECT_TRUE(scene->runtime->Ok()) << "the node is fine: it is the entities that are not";

    // A parameter holding the wrong kind of thing.
    Parameters(world, entities[3], show).Set("source", square);
    Frame();
    EXPECT_EQ(scene->runtime->ReportOf("draw")->ran, 0u);

    // A tag nothing has, because nothing has made it yet: nobody.
    kgraph::Graph graph = scene->runtime->Current();
    graph.Find("lit")->Set("with", kgraph::Value::List { "NeverHeardOf" });
    scene->runtime->Set(graph);
    Frame();
    EXPECT_EQ(scene->runtime->ReportOf("run")->ran, 0u);

    // And mistakes in the nodes, which are the nodes'.
    graph.Find("lit")->Set("with", "Lit");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("lit")->error.find("list"), std::string::npos) << scene->runtime->Status("lit")->error;
    graph.Find("lit")->Set("with", kgraph::Value::List { "Lit" });
    graph.Find("run")->Set("with", kgraph::Value::List { "Lit" });
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("run")->error.find("a ForEach's now"), std::string::npos) << scene->runtime->Status("run")->error;
    graph.Find("run")->inputs.erase("with");
    graph.Find("run")->Set("cover", "backdrops");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("run")->error.find("'backdrops' is not one"), std::string::npos) << scene->runtime->Status("run")->error;
    graph.Find("draw")->Set("command", "eDrawSideways");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("draw")->error.find("eDrawIndexed"), std::string::npos) << scene->runtime->Status("draw")->error;
}

// A draw's parameter wired from elsewhere than its pipeline's component is no field of it: the component loses it, and
// the entities keep the rest.
TEST_F(EntityTest, WiringADrawsParameterFromElsewhereTakesItOffTheComponent) {
    Load(SceneGraph());
    MakeFour();
    Frame();
    Frame();

    // The Draw now gives every entity the same image to draw: the field goes, and what the entities keep, they keep.
    kgraph::Graph graph = scene->runtime->Current();
    graph.Add("shared", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 4).Set("extent.y", 4).Set("extent.z", 1)
        .Set("usage", kgraph::Value::List { "eSampled", "eTransferDst" });
    graph.Add("white", "Clear").Wire("image", "shared").Set("color", kgraph::Value::List { 1, 1, 1, 1 });
    graph.Find("draw")->Wire("param.source", "shared");
    scene->runtime->Set(graph);
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;

    const auto* migration = scene->runtime->MigrationOf("show");
    ASSERT_NE(migration, nullptr);
    EXPECT_EQ(migration->removed, std::vector<std::string> { "source" });
    EXPECT_EQ(migration->kept, std::vector<std::string> { "offset" });
    EXPECT_EQ(migration->entities, 4u);
    EXPECT_EQ(scene->runtime->ComponentOf("show"), show) << "the same component, in a new shape";
    EXPECT_FALSE(Parameters(scene->world, entities[0], show).Has("source"));
    EXPECT_EQ(Parameters(scene->world, entities[3], show).Get<kor::Vec2>("offset"), (kor::Vec2 { 0, 0 }));
    Frame();
    Frame();
    for (int quarter = 0; quarter < 4; ++quarter) ExpectColor(Quarter(quarter), 255, 255, 255, "every entity, where it was, with the image the Draw is wired");

    // Wired on the pipeline instead, it is the same for every draw, and the Draw is given it no more.
    graph.Find("draw")->inputs.erase("param.source");
    graph.Find("show")->Wire("param.source", "shared");
    scene->runtime->Set(graph);
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    const auto ports = scene->runtime->PortsOf("draw");
    EXPECT_EQ(std::ranges::find(ports, "param.source", &kgraph::Port::name), ports.end()) << "the pipeline has it: no draw's to give";
    Frame();
    Frame();
    ExpectColor(Quarter(2), 255, 255, 255, "drawn with it all the same");
}

TEST_F(EntityTest, AShaderEditedKeepsWhatKeepsItsNameAndType) {
    // A copy of the shader that can be written to, under a name of its own each run (the framework keeps a shader
    // it compiled, by name).
    static int run = 0;
    const std::string module = std::format("stampEdited{}", run++);
    const auto directory = std::filesystem::temp_directory_path() / "kgraph-entities";
    std::filesystem::create_directories(directory);
    std::ifstream in(std::filesystem::path(KGRAPH_TEST_SHADERS) / "stamp.slang");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    { std::ofstream(directory / (module + ".slang")) << text; }
    kor::Shader::AddSearchPath(directory, true);

    Load(SceneGraph(module));
    MakeFour();
    Frame();
    Frame();
    ExpectColor(Quarter(0), 255, 0, 64, "as written");

    // Edited: the gain is called strength now, and the backdrop base. The tint and the target are as they were.
    const auto replace = [&](const std::string& from, const std::string& to) {
        std::size_t count = 0;
        for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) { text.replace(at, from.size(), to); ++count; }
        ASSERT_GT(count, 0u) << from;
    };
    replace("gain", "strength");
    replace("backdrop", "base");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::ofstream(directory / (module + ".slang")) << text; }
    bool reloaded = false;
    for (int i = 0; i < 200 && !reloaded; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        Frame();
        reloaded = scene->world.Describe(stamp).FindField("strength") != nullptr;
    }
    ASSERT_TRUE(reloaded) << "the edit never reached the component";

    // On the node: the wire to a parameter the shader no longer has is taken off, and said to have been.
    EXPECT_EQ(scene->runtime->TakeReset(), (std::vector<std::string> { "stamp.param.backdrop", "run.push.gain" })) << "and the Dispatch's to what it no longer has";
    EXPECT_FALSE(scene->runtime->Current().Find("stamp")->inputs.contains("param.backdrop"));
    EXPECT_TRUE(scene->runtime->TakeReset().empty());

    // On the entities: the same component, moved to its new shape.
    EXPECT_EQ(scene->runtime->ComponentOf("stamp"), stamp);
    const auto* migration = scene->runtime->MigrationOf("stamp");
    ASSERT_NE(migration, nullptr);
    const auto has = [](const std::vector<std::string>& list, const char* name) { return std::ranges::find(list, name) != list.end(); };
    EXPECT_TRUE(has(migration->kept, "target") && has(migration->kept, "tint")) << "name and type the same";
    EXPECT_TRUE(has(migration->removed, "gain"));
    EXPECT_TRUE(has(migration->reset, "strength") && has(migration->reset, "base")) << "new: empty, for each entity to be given";
    EXPECT_EQ(migration->entities, 4u);
    Parameters values(scene->world, entities[2], stamp);
    EXPECT_EQ(values.Get<kor::Vec4>("tint"), (kor::Vec4 { 1, 1, 0, 1 }));
    EXPECT_NE(values.ResourceAt("target"), nullptr);
    EXPECT_EQ(values.Get<float>("strength"), 0.f);
    EXPECT_EQ(values.ResourceAt("base"), nullptr);

    // Nothing runs until the new parameter is wired: the Dispatch says so.
    EXPECT_FALSE(scene->runtime->Status("run")->ok);
    EXPECT_NE(scene->runtime->Status("run")->error.find("'param.base' is given nothing"), std::string::npos) << scene->runtime->Status("run")->error;
    kgraph::Graph graph = scene->runtime->Current();
    graph.Find("run")->Wire("param.base", "lit", "field.base").Wire("push.strength", "lit", "field.strength");
    scene->runtime->Set(graph);
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    const auto base = scene->runtime->ResourceOf("backdrop");
    for (const kecs::Entity entity : entities) Parameters(scene->world, entity, stamp).Set("base", base).Set("strength", 1.f);
    Parameters(scene->world, entities[0], stamp).Set("tint", kor::Vec4 { 0, 1, 0, 1 });
    Frame();
    Frame();
    EXPECT_EQ(scene->runtime->ReportOf("run")->ran, 4u) << scene->runtime->ReportOf("run")->why;
    ExpectColor(Quarter(0), 0, 255, 64, "running again, with the edited shader");
    ExpectColor(Quarter(3), 0, 255, 255, "and the colours they kept");
    std::filesystem::remove_all(directory);
}

TEST_F(EntityTest, ManyEntitiesEachWithAnImageOfItsOwn) {
    Load(SceneGraph());
    square = Square();
    constexpr int Count = 500;
    for (int i = 0; i < Count; ++i) entities.push_back(Make(i % 4, i % 4 == 0 ? kor::Vec4 { 1, 0, 0, 1 } : kor::Vec4 { 0, 1, 0, 1 }, 1.f, 4));
    Frame();
    Frame();
    EXPECT_EQ(scene->runtime->ReportOf("run")->ran, static_cast<std::size_t>(Count));
    EXPECT_EQ(scene->runtime->ReportOf("draw")->ran, static_cast<std::size_t>(Count));
    EXPECT_EQ(scene->Schedule(), (std::vector<std::string> { "tint backdrop", "run", "draw", "blit", "Read screen" }));
    ExpectColor(Quarter(0), 255, 0, 64, "every fourth is red, and they are all drawn there");
    ExpectColor(Quarter(1), 0, 255, 64, "the rest green");

    // A frame in which nothing changed makes nothing: it looks at what each entity holds, and that is all.
    const auto began = std::chrono::steady_clock::now();
    constexpr int Frames = 20;
    for (int i = 0; i < Frames; ++i) scene->runtime->Update();
    const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / Frames;
    RecordProperty("update_ms_500_entities", std::to_string(each));
    std::printf("            readying %d entities for two passes: %.3f ms a frame\n", Count, each);
    EXPECT_LT(each, 5.0) << "a frame's readying should not be remaking what it made";

    // Half of them gone: what was made for them goes too.
    for (int i = 0; i < Count; i += 2) scene->world.Destroy(entities[static_cast<std::size_t>(i)]);
    Frame();
    EXPECT_EQ(scene->runtime->ReportOf("draw")->ran, static_cast<std::size_t>(Count / 2));
}

// An entity is drawn where it is: its own transform, under those of everything it is under. And with a default
// mesh, which no node makes.
TEST_F(EntityTest, AnEntityIsDrawnWhereItsTransformPutsIt) {
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("shape", "Primitive");   // only for its layout: the meshes drawn are the default ones
    graph.Add("placed", "GraphicsPipeline").Set("vertex.module", "placed").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "placed").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    // Every entity with the pipeline's component and a Mesh: its mesh, where it is, and its colour, each wired.
    graph.Add("drawn", "ForEach").Set("component", "placed").Set("with", Value::List { "Mesh" });
    graph.Add("draw", "Draw").Wire("pipeline", "placed").Wire("mesh", "drawn", "Mesh.mesh").Wire("push.model", "drawn", "matrix")
        .Wire("push.color", "drawn", "field.color");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    scene->runtime->Set(graph);
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(scene->runtime->Ok());

    const kecs::ComponentId placed = scene->runtime->ComponentOf("placed");
    ASSERT_NE(placed, kecs::NoComponent);
    const auto& fields = scene->world.Describe(placed).fields;
    EXPECT_EQ(fields.size(), 1u) << "the colour is the entity's; its model matrix is wired from where it is, and no field";
    EXPECT_EQ(fields.front().name, "color");

    const auto cube = scene->runtime->ResourceOf("$cube");
    ASSERT_NE(cube, nullptr) << "a default mesh, with no node for it";
    EXPECT_EQ(cube->kind, "Mesh");
    EXPECT_EQ(scene->runtime->ResourceOf("$cube"), cube) << "made once";
    EXPECT_EQ(scene->runtime->ResourceOf("$blob"), nullptr);

    // A parent off to the right and half the size; under it, a cube one of its units to the left: the middle of the picture.
    kecs::World& world = scene->world;
    const kecs::Entity parent = world.Create(), child = world.Create();
    world.SetParent(child, parent);
    world.SetPosition(parent, { 0.5f, 0, 0 });
    world.SetScale(parent, { 0.5f, 0.5f, 0.5f });
    world.SetPosition(child, { -1, 0, 0 });
    world.Add<kgraph::Mesh>(child, { cube });
    world.Add(child, placed);
    kgraph::Parameters(world, child, placed).Set("color", kor::Vec4 { 1, 0, 0, 1 });
    Frame();
    Frame();
    ASSERT_EQ(scene->runtime->ReportOf("draw")->ran, 1u) << scene->runtime->ReportOf("draw")->why;
    // The cube is half a unit across in the world: a quarter of the picture's width, about its middle.
    EXPECT_EQ(scene->Pixel(16, 16).x, 255) << "in the middle";
    EXPECT_EQ(scene->Pixel(13, 19).x, 255);
    EXPECT_EQ(scene->Pixel(10, 16).x, 0) << "and no wider than it is";
    EXPECT_EQ(scene->Pixel(22, 16).x, 0);

    // The parent moved: the child with it, with nothing of the child's changed.
    world.SetPosition(parent, { 0, 0.5f, 0 });
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(16, 16).x, 0);
    EXPECT_EQ(scene->Pixel(8, 24).x, 255) << "half a unit left of its parent, which is half way down";

    // Saved and read back, it is where it was and what it was.
    const auto codecs = [&] { kecs::Codecs c = kecs::Codecs::Default(); kgraph::RegisterCodecs(c, scene->runtime.get()); return c; }();
    const std::string saved = kecs::Save(world, codecs).json;
    EXPECT_NE(saved.find("\"$cube\""), std::string::npos) << saved;
    world.Destroy(parent);
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(8, 24).x, 0) << "gone, with its parent";
    ASSERT_TRUE(kecs::Load(world, saved, codecs).ok);
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(8, 24).x, 255);
}

// A Filter of one entity alone, by its id: the picked one (a Selected node's `entityId`), drawn by itself, as an editor
// draws what it outlines; none when nothing is picked.
TEST_F(EntityTest, AFilterKeepsOneEntityByItsId) {
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("shape", "Primitive");
    graph.Add("placed", "GraphicsPipeline").Set("vertex.module", "placed").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "placed").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("picked", "Selected");
    graph.Add("all", "Entities");
    graph.Add("chosen", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Wire("entityId", "picked", "entityId");
    graph.Add("draw", "Draw").Wire("pipeline", "placed").Wire("mesh", "chosen", "Mesh.mesh").Wire("push.model", "chosen", "matrix")
        .Set("push.color", Value::List { 0, 1, 0, 1 });
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;

    kecs::World& world = scene->world;
    const auto cube = runtime.ResourceOf("$cube");
    const auto make = [&](const float x) {
        const kecs::Entity made = world.Create();
        world.SetPosition(made, { x, 0, 0 });
        world.SetScale(made, { 0.5f, 0.5f, 0.5f });
        world.Add<kgraph::Mesh>(made, { cube });
        return made;
    };
    make(-0.5f);
    const kecs::Entity right = make(0.5f);
    Frame();
    Frame();
    EXPECT_EQ(runtime.ReportOf("draw")->ran, 0u) << "nothing picked: none kept";
    runtime.SetSelected(right);
    Frame();
    Frame();
    EXPECT_EQ(runtime.ReportOf("draw")->ran, 1u) << "the picked one alone";
    EXPECT_EQ(scene->Pixel(24, 16).y, 255) << "drawn";
    EXPECT_EQ(scene->Pixel(8, 16).y, 0) << "and not the other";
}

// A draw is made of what it is wired: a ForEach of Mesh gives each entity's mesh and where it is, and anything worked
// out of an element is that element's. With everything wired from elsewhere, the pipeline has no component at all.
TEST_F(EntityTest, ADrawIsMadeOfWhateverEachElementIsWired) {
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("shape", "Primitive");
    graph.Add("placed", "GraphicsPipeline").Set("vertex.module", "placed").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "placed").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("meshes", "ForEach").Set("component", "Mesh").Set("with", Value::List { "Lit" });
    // The first red, the rest green: worked out of each element's index.
    graph.Add("first", "Less").Wire("a", "meshes", "index").Set("b", 1);
    graph.Add("tint", "Select").Wire("condition", "first").Set("a", Value::List { 1, 0, 0, 1 }).Set("b", Value::List { 0, 1, 0, 1 });
    graph.Add("draw", "Draw").Wire("pipeline", "placed").Wire("mesh", "meshes", "field.mesh").Wire("push.model", "meshes", "matrix").Wire("push.color", "tint");
    graph.Add("picked", "Selected");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ComponentOf("placed"), kecs::NoComponent) << "its draws take everything from elsewhere: nothing is left for a component of its own";
    const auto drawPorts = runtime.PortsOf("draw");
    for (const char* name : { "push.model", "push.color" }) EXPECT_NE(std::ranges::find(drawPorts, name, &kgraph::Port::name), drawPorts.end()) << name;
    const auto pipelinePorts = runtime.PortsOf("placed");
    EXPECT_EQ(std::ranges::find(pipelinePorts, "push.model", &kgraph::Port::name), pipelinePorts.end()) << "a push constant is a draw's, not the pipeline's";

    kecs::World& world = scene->world;
    const auto cube = runtime.ResourceOf("$cube");
    const auto make = [&](const float x) {
        const kecs::Entity made = world.Create();
        world.SetPosition(made, { x, 0, 0 });
        world.SetScale(made, { 0.5f, 0.5f, 0.5f });
        world.Add<kgraph::Mesh>(made, { cube });
        world.Add<Lit>(made);
        return made;
    };
    const kecs::Entity left = make(-0.5f), right = make(0.5f);
    world.Add<kgraph::Mesh>(world.Create(), { cube });     // not Lit: not drawn
    Frame();
    Frame();
    ASSERT_EQ(runtime.ReportOf("draw")->ran, 2u) << runtime.ReportOf("draw")->why;
    EXPECT_EQ(scene->Pixel(8, 16).x, 255) << "the first, red";
    EXPECT_EQ(scene->Pixel(8, 16).y, 0);
    EXPECT_EQ(scene->Pixel(24, 16).y, 255) << "the second, green";
    EXPECT_EQ(scene->Pixel(24, 16).x, 0);
    EXPECT_EQ(scene->Pixel(16, 16).y, 0) << "nothing between them";

    // What a ForEach gives of the first element: its mesh as a resource, and its normal matrix.
    EXPECT_EQ(runtime.ResourceOf("meshes", "field.mesh"), cube);
    const Value normal = runtime.ValueOf("meshes", "normalMatrix");
    ASSERT_TRUE(normal.Items() && normal.Items()->size() == 16u) << normal.ToJson();
    EXPECT_NEAR(*normal.Items()->at(0).Number(), 2.0, 1e-5) << "a half scale's inverse, transposed, is twice";

    // The one picked, as a number any draw can be given.
    runtime.SetSelected(right);
    Frame();
    EXPECT_EQ(runtime.ValueOf("picked", "entityId"), Value(right.index + 1.0));
    runtime.SetSelected(kecs::NullEntity);
    Frame();
    EXPECT_EQ(runtime.ValueOf("picked", "entityId"), Value(0));

    // Moved: drawn where it is now, with nothing built again.
    const auto builds = runtime.Status("draw")->builds;
    world.SetPosition(left, { 0.5f, 0.5f, 0 });
    world.Destroy(right);
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(8, 16).x, 0) << "gone from where it was";
    EXPECT_EQ(scene->Pixel(24, 24).x, 255) << "the first, still red, where it is now";
    EXPECT_EQ(runtime.Status("draw")->builds, builds);
}

// Every entity, then those a Filter keeps — with a Mesh and the pipeline's component, and of those, none Hidden — and
// a draw of each: its mesh, where it is, and its colour, a field of the component the Filter keeps.
TEST_F(EntityTest, EveryEntityIsFilteredThenDrawn) {
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("shape", "Primitive");
    graph.Add("placed", "GraphicsPipeline").Set("vertex.module", "placed").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "placed").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh", "placed" });
    graph.Add("shown", "Filter").Wire("entities", "meshes").Set("without", Value::List { "Hidden" }).Set("with", Value::List { "Mesh", "placed" });
    graph.Add("draw", "Draw").Wire("pipeline", "placed").Wire("mesh", "shown", "Mesh.mesh").Wire("push.model", "shown", "matrix")
        .Wire("push.color", "shown", "placed.color");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const kecs::ComponentId placed = runtime.ComponentOf("placed");
    ASSERT_NE(placed, kecs::NoComponent);
    ASSERT_EQ(scene->world.Describe(placed).fields.size(), 1u) << "its colour, wired from the Filter of it; where it is, from elsewhere";

    kecs::World& world = scene->world;
    const auto cube = runtime.ResourceOf("$cube");
    const auto make = [&](const float x, const kor::Vec4 color) {
        const kecs::Entity made = world.Create();
        world.SetPosition(made, { x, 0, 0 });
        world.SetScale(made, { 0.5f, 0.5f, 0.5f });
        world.Add<kgraph::Mesh>(made, { cube });
        world.Add(made, placed);
        kgraph::Parameters(world, made, placed).Set("color", color);
        return made;
    };
    make(-0.5f, { 1, 0, 0, 1 });
    const kecs::Entity right = make(0.5f, { 0, 1, 0, 1 });
    world.Create();                                         // nothing: not kept
    world.Add<kgraph::Mesh>(world.Create(), { cube });      // a mesh and no colour: not kept
    Frame();
    Frame();
    EXPECT_EQ(runtime.ValueOf("all", "count"), Value(static_cast<double>(world.All().size()))) << "every entity";
    EXPECT_EQ(runtime.ValueOf("meshes", "count"), Value(2));
    ASSERT_EQ(runtime.ReportOf("draw")->ran, 2u) << runtime.ReportOf("draw")->why;
    EXPECT_EQ(scene->Pixel(8, 16).x, 255) << "red, on the left";
    EXPECT_EQ(scene->Pixel(24, 16).y, 255) << "green, on the right";

    world.Add<Hidden>(right);
    Frame();
    Frame();
    EXPECT_EQ(runtime.ReportOf("draw")->ran, 1u) << "the second Filter keeps it no more";
    EXPECT_EQ(scene->Pixel(24, 16).y, 0);
    EXPECT_EQ(scene->Pixel(8, 16).x, 255);
}

// What a scene's components hold is there as values, which follow the world every frame: a ForEach of the
// engine's package's components — the Camera, a light — worked on by other nodes, and packed into a buffer as a
// shader's block lays it out. Nothing is built again for it, and nothing in the graph knows a camera from a light.
TEST_F(EntityTest, ComponentsAreValuesThatFollowTheWorldIntoABuffer) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    const kecs::Entity eye = world.Create(), lamp = world.Create();
    world.Rename(eye, "Eye");
    support::Give(world, eye, "PerspectiveCamera", { { "fieldOfView", { 90.f } }, { "near", { 0.5f } }, { "far", { 50.f } } });
    world.SetPosition(eye, { 1, 2, 3 });
    world.Rename(lamp, "Lamp");
    support::Give(world, lamp, "PointLight", { { "color", { 1, 1, 1 } }, { "range", { 7.f } } });
    world.SetPosition(lamp, { 0, 4, 0 });

    kgraph::Graph graph;
    graph.passes = { "frame", "later" };
    graph.Add("eye", "Entity").Set("component", "PerspectiveCamera");
    graph.Add("looking at", "Add").Wire("a", "eye", "worldPosition").Wire("b", "eye", "forward");
    graph.Add("camera", "View").Wire("position", "eye", "worldPosition").Wire("target", "looking at").Wire("up", "eye", "up").Set("main", true);
    graph.Add("lens", "Perspective").Set("main", true).Wire("fieldOfView", "eye", "field.fieldOfView").Wire("near", "eye", "field.near")
        .Wire("far", "eye", "field.far").Set("aspect", 2);
    graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "camera");
    graph.Add("unseen", "MatrixInverse").Wire("a", "seen");
    graph.Add("sky", "Entity").Set("component", "Sky");
    graph.Add("zenith", "ColorConvert").Wire("color", "sky", "field.zenith").Set("from", "eGamma22").Set("to", "eLinear");
    graph.Add("sun", "Entity").Set("component", "DirectionalLight");
    graph.Add("lamps", "ForEach").Set("component", "PointLight");
    graph.Add("place and reach", "Append").Wire("a", "lamps", "worldPosition").Wire("b", "lamps", "field.range");
    graph.Add("lights", "Collect").Wire("value", "place and reach");
    graph.Add("far", "Entity").Set("component", "Camera");     // the kind: what every camera has, of whichever it is
    graph.Add("twice as far", "Multiply").Wire("a", "far", "field.far").Set("b", 2);
    graph.Add("where", "Entity").Set("entity", "Lamp");     // of no component: the one so named, wherever it is
    graph.Add("placed", "Append").Wire("a", "camera", "position").Wire("b", "far", "field.far");
    graph.Add("back", "MatrixMultiply").Wire("a", "seen").Wire("b", "unseen");
    // A block as a shader declares it: a matrix, a float3 with a float after it in its fourth place, a uint4.
    const auto layout = [](const char* third) {
        Value::List fields;
        for (const auto& [name, type] : { std::pair { "seen", "eMat4" }, { "eye", "eVec3" }, { "reach", third }, { "count", "eUVec4" } })
            fields.emplace_back(Value::Members { { "name", name }, { "type", type } });
        return Value(std::move(fields));
    };
    graph.Add("block", "BufferBuilder").Set("usage", "eUniform").Set("layout", layout("eFloat")).Wire("field.seen", "seen").Wire("field.eye", "camera", "position")
        .Wire("field.reach", "twice as far").Wire("field.count", "lights", "count");
    graph.Add("taken", "Reference").Set("node", "lens");
    graph.Find("taken")->pass = "later";
    graph.Add("from afar", "Add").Wire("a", "taken", "far").Set("b", 1);
    scene->runtime->Set(graph);
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(scene->runtime->Ok());
    auto& runtime = *scene->runtime;

    const auto near = [](const Value& value, const std::vector<double>& numbers) {
        const auto* items = value.Items();
        if (!items || items->size() != numbers.size()) return false;
        for (std::size_t i = 0; i < numbers.size(); ++i)
            if (std::abs(*(*items)[i].Number() - numbers[i]) > 1e-4) return false;
        return true;
    };
    EXPECT_TRUE(near(runtime.ValueOf("camera", "position"), { 1, 2, 3 })) << runtime.ValueOf("camera", "position").ToJson();
    EXPECT_NEAR(*runtime.ValueOf("lens", "fieldOfView").Number(), 90, 1e-4);
    EXPECT_EQ(runtime.ValueOf("lens", "far"), Value(50));
    EXPECT_EQ(runtime.ValueOf("far", "field.far"), Value(50));
    EXPECT_EQ(runtime.ValueOf("twice as far"), Value(100));
    EXPECT_TRUE(near(runtime.ValueOf("placed"), { 1, 2, 3, 50 }));
    EXPECT_EQ(runtime.ValueOf("where", "worldPosition"), Value(Value::List { 0, 4, 0 }));
    EXPECT_EQ(runtime.ValueOf("lights"), Value(Value::List { 0, 4, 0, 7 })) << "the lamp's place and its reach, collected";
    EXPECT_EQ(runtime.ValueOf("lights", "count"), Value(1));
    EXPECT_EQ(runtime.ValueOf("sun", "present"), Value(0)) << "there is no directional light";
    EXPECT_EQ(runtime.ValueOf("sky", "present"), Value(0));
    EXPECT_TRUE(near(runtime.ValueOf("zenith"), { std::pow(0.22, 2.2), std::pow(0.42, 2.2), std::pow(0.78, 2.2) }))
        << "with no Sky, a Sky as the package's source starts it, in linear terms: " << runtime.ValueOf("zenith").ToJson();
    EXPECT_NEAR(*runtime.ValueOf("from afar").Number(), 51, 1e-4) << "through what another pass brought in";
    const auto identity = *runtime.ValueOf("back").Items();
    for (std::size_t i = 0; i < 16; ++i) EXPECT_NEAR(*identity[i].Number(), i % 5 == 0 ? 1.0 : 0.0, 1e-3) << i;
    EXPECT_EQ(runtime.ValueOf("block", "bytes"), Value(64 + 16 + 16)) << "a matrix; a float3 and the float in its fourth place; a uint4";
    EXPECT_EQ(runtime.Aspect(), 2.f) << "the main view's picture is twice as wide as it is tall";
    EXPECT_EQ(runtime.CurrentView().view, kgraph::ViewOf(world, eye).view) << "the main view is the scene's";
    EXPECT_NEAR(runtime.CurrentView().far, 50.f, 1e-4) << "and its lens is the one wired from it";

    const auto bytes = [&](const char* node) { return kor::capi::BufferOf(runtime.ResourceOf(node)->handle)->Read<float>(); };
    Frame();
    {
        const auto block = bytes("block");
        ASSERT_GE(block.size(), 24u);
        EXPECT_NEAR(block[16], 1.f, 1e-5) << "the position, after the matrix";
        EXPECT_NEAR(block[18], 3.f, 1e-5);
        EXPECT_EQ(block[19], 100.f) << "and the number, in the vector's fourth place";
        kor::u32 count = 0;
        std::memcpy(&count, &block[20], 4);
        EXPECT_EQ(count, 1u) << "a whole number, as the shader reads it";
    }

    // The world changes: the values follow, and so does what is in the buffer. No node is built again.
    const auto builds = [&](const char* node) { return runtime.Status(node)->builds; };
    const std::uint64_t before[] { builds("lens"), builds("block"), builds("twice as far"), builds("from afar"), builds("lights") };
    world.SetPosition(eye, { 5, 6, 7 });
    support::Give(world, eye, "PerspectiveCamera", { { "fieldOfView", { 90.f } }, { "near", { 0.5f } }, { "far", { 80.f } } });
    world.Destroy(lamp);
    Frame();
    Frame();
    EXPECT_TRUE(near(runtime.ValueOf("camera", "position"), { 5, 6, 7 }));
    EXPECT_EQ(runtime.ValueOf("twice as far"), Value(160));
    EXPECT_NEAR(*runtime.ValueOf("from afar").Number(), 81, 1e-4);
    EXPECT_EQ(runtime.ValueOf("lights", "count"), Value(0));
    EXPECT_EQ(runtime.ValueOf("lights"), Value(Value::List {}));
    EXPECT_EQ(runtime.ValueOf("where", "present"), Value(0)) << "the lamp is gone";
    {
        const auto block = bytes("block");
        EXPECT_NEAR(block[16], 5.f, 1e-5);
        EXPECT_EQ(block[19], 160.f);
    }
    const std::uint64_t after[] { builds("lens"), builds("block"), builds("twice as far"), builds("from afar"), builds("lights") };
    for (int i = 0; i < 5; ++i) EXPECT_EQ(after[i], before[i]) << "a value changed where it was: nothing was built again";

    // Whoever runs the graph can look from somewhere of its own: the main view is then that, and no entity's, and the
    // Perspective wired from it has its lens.
    kgraph::View view;
    view.view = kor::LookAt(kor::Vec3(0, 0, 9), kor::Vec3(0));
    view.fieldOfView = 0.5f;
    view.far = 30.f;
    runtime.SetView(view);
    Frame();
    EXPECT_TRUE(near(runtime.ValueOf("camera", "position"), { 0, 0, 9 }));
    EXPECT_NEAR(*runtime.ValueOf("lens", "fieldOfView").Number(), 0.5 * 180.0 / 3.14159265358979, 1e-3);
    EXPECT_EQ(runtime.ValueOf("lens", "far"), Value(30));

    // Another runtime that follows this one is another view of the same graph and the same world: seen from where it
    // is told — the scene's camera, told nothing — while this one is seen from where it was told. What is in files and
    // the default meshes are the first one's; what each node makes is each one's own.
    FollowScene::source = &runtime;
    FollowScene::world = &world;
    auto* other = support::App()->OpenOffscreen<FollowScene>({ .title = "follows", .extent = { support::Size, support::Size } });
    ASSERT_NE(other, nullptr);
    Frame();
    Frame();
    kgraph::Runtime& follows = *other->runtime;
    for (const auto& problem : follows.Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(near(follows.ValueOf("camera", "position"), { 5, 6, 7 })) << "the scene's camera: " << follows.ValueOf("camera", "position").ToJson();
    EXPECT_EQ(follows.ValueOf("lens", "far"), Value(80));
    EXPECT_TRUE(near(runtime.ValueOf("camera", "position"), { 0, 0, 9 })) << "and the first is still seen from where it was told";
    EXPECT_EQ(follows.ResourceOf("$cube"), runtime.ResourceOf("$cube")) << "a default mesh is made once, by the first";
    EXPECT_EQ(follows.ResourceOf("$white"), runtime.ResourceOf("$white"));
    ASSERT_NE(follows.ResourceOf("block"), nullptr);
    EXPECT_NE(follows.ResourceOf("block"), runtime.ResourceOf("block")) << "what a node makes is its own";
    // The graph edited: the other follows it.
    graph.Add("thrice as far", "Multiply").Wire("a", "far", "field.far").Set("b", 3);
    graph.Add("numbers", "Buffer").Set("instanceCount", 8).Set("usage", Value::List { "eStorage" });
    runtime.Set(graph);
    Frame();
    Frame();
    EXPECT_EQ(follows.ValueOf("thrice as far"), Value(240));
    // A buffer something outside the graph writes into is shared, when it is said to be: the first's own, not another.
    ASSERT_NE(follows.ResourceOf("numbers"), nullptr);
    EXPECT_NE(follows.ResourceOf("numbers"), runtime.ResourceOf("numbers"));
    follows.Share({ "numbers" });
    Frame();
    EXPECT_EQ(follows.ResourceOf("numbers"), runtime.ResourceOf("numbers")) << "the first's buffer";
    graph.Find("numbers")->Set("instanceCount", 16);
    runtime.Set(graph);
    Frame();
    Frame();
    EXPECT_EQ(follows.ResourceOf("numbers"), runtime.ResourceOf("numbers")) << "and the one it made in its place, once it made another";
    follows.Share({});
    Frame();
    EXPECT_NE(follows.ResourceOf("numbers"), runtime.ResourceOf("numbers")) << "not shared, its own again";
    std::erase_if(graph.nodes, [](const kgraph::Node& node) { return node.id == "numbers"; });
    support::App()->Close(*other);
    Frame();
    std::erase_if(graph.nodes, [](const kgraph::Node& node) { return node.id == "thrice as far"; });
    runtime.Set(graph);
    runtime.SetView(std::nullopt);

    // One by name, the first, and one there is not.
    const kecs::Entity second = world.Create();
    world.Rename(second, "Second");
    support::Give(world, second, "PerspectiveCamera", { { "far", { 7.f } } });
    graph.Find("eye")->Set("entity", "Second");
    runtime.Set(graph);
    Frame();
    EXPECT_EQ(runtime.ValueOf("lens", "far"), Value(7));
    EXPECT_EQ(runtime.EachOf("lens"), "") << "what is wired from one entity is no loop";
    graph.Find("eye")->inputs.erase("entity");
    runtime.Set(graph);
    Frame();
    EXPECT_EQ(runtime.ValueOf("lens", "far"), Value(80)) << "the first of them, in the hierarchy's order";
    graph.Find("eye")->Set("entity", "Nobody");
    runtime.Set(graph);
    Frame();
    EXPECT_EQ(runtime.ValueOf("eye", "present"), Value(0));

    // What a field is not, and what a block cannot be, is said.
    graph.Add("written", "WriteComponent").Set("component", "Camera").Set("field", "nearness").Set("value", 1);
    graph.Find("block")->Set("layout", layout("eBlob"));
    graph.Add("all lamps", "ForEach").Set("component", "PointLight").Set("select", "eFirst");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("written")->error.find("'Camera' has no field 'nearness'"), std::string::npos) << runtime.Status("written")->error;
    EXPECT_FALSE(runtime.Status("block")->ok);
    EXPECT_NE(runtime.Status("all lamps")->error.find("an Entity node is one"), std::string::npos) << runtime.Status("all lamps")->error;
}

// Every component of the world is offered as a ForEach of it; its buffer is one ComponentBuffer node, told which.
TEST_F(EntityTest, EachComponentIsOfferedAsANode) {
    const auto presets = scene->runtime->Presets();
    const auto find = [&](const std::string_view name) { return std::ranges::find(presets, name, &kgraph::Preset::name); };
    for (const char* name : { "Camera", "PerspectiveCamera", "OrthographicCamera", "Light", "PointLight", "SpotLight", "DirectionalLight", "Sky" }) {
        const auto each = find(name);
        ASSERT_NE(each, presets.end()) << name;
        EXPECT_EQ(each->type, "ForEach");
        EXPECT_EQ(find(std::string(name) + " Buffer"), presets.end()) << name << ": no buffer of each component";
    }
    // A component made since — a script's — is offered as well.
    kscript::ScriptType glow;
    glow.name = "Glow";
    glow.size = 4;
    glow.alignment = 4;
    glow.fields = { { "strength", "f32", 0, 4 } };
    ASSERT_NE(scene->scripts.Declare(glow), kecs::NoComponent);
    const auto now = scene->runtime->Presets();
    EXPECT_NE(std::ranges::find(now, "Glow", &kgraph::Preset::name), now.end());
    EXPECT_NE(kgraph::FindType("ComponentBuffer"), nullptr);
}

// A graph is in passes: each node on the page of one, the passes in the order they are drawn. Saved and read back,
// it is as it was; and where what is used does not say which of two passes goes first, their order does.
TEST_F(EntityTest, PassesAreKeptAndDecideTheOrder) {
    using kgraph::Value;
    kgraph::Graph graph;
    graph.passes = { "second", "first" };
    graph.Add("picture", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { 8, 8 }).Set("usage", Value::List { "eTransferSrc", "eTransferDst" });
    graph.Add("screen", "Screen");
    // Two clears of one picture: nothing they use says which is last.
    graph.Add("red", "Clear").Wire("image", "picture").Set("color", Value::List { 1, 0, 0, 1 });
    graph.Add("green", "Clear").Wire("image", "picture").Set("color", Value::List { 0, 1, 0, 1 });
    graph.Add("show", "Blit").Wire("source", "picture").Wire("destination", "screen");
    graph.Find("red")->pass = "second";
    graph.Find("green")->pass = "first";

    kgraph::Graph read;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(graph.ToJson(), read, error)) << error;
    EXPECT_EQ(read, graph);
    EXPECT_EQ(read.passes, (std::vector<std::string> { "second", "first" }));
    EXPECT_EQ(read.Find("red")->pass, "second");
    EXPECT_TRUE(read.Find("picture")->pass.empty());

    scene->runtime->Set(graph);
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(4, 4).y, 255) << "'first' is the later of the passes: its green is what is left";
    std::swap(graph.passes[0], graph.passes[1]);
    scene->runtime->Set(graph);
    Frame();
    Frame();
    EXPECT_EQ(scene->Pixel(4, 4).x, 255) << "the other way round, the red";
    EXPECT_EQ(scene->runtime->Status("picture")->builds, 1u) << "a pass moved builds nothing again";
}

TEST_F(EntityTest, AWorldIsSavedAndLoadedWithItsPipelinesComponents) {
    Load(SceneGraph());
    auto& world = scene->world;
    kecs::Codecs codecs;
    kgraph::RegisterCodecs(codecs, scene->runtime.get());

    const kecs::Entity entity = world.Create();
    world.Add(entity, stamp);
    world.Add(entity, show);
    // One resource a node makes, and one made in code.
    Parameters(world, entity, stamp).Set("target", scene->runtime->ResourceOf("backdrop")).Set("tint", kor::Vec4 { 0.25f, 0.5f, 0.75f, 1 }).Set("gain", 2.f);
    Parameters(world, entity, show).Set("source", Image(8)).Set("offset", kor::Vec2 { -1, 0.5f });

    const auto saved = kecs::Save(world, codecs);
    EXPECT_NE(saved.json.find("\"tint\""), std::string::npos) << saved.json;
    world.Destroy(entity);
    const auto loaded = kecs::Load(world, saved.json, codecs);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    ASSERT_EQ(loaded.entities.size(), 1u);

    const kecs::Entity again = loaded.entities.front();
    Parameters values(world, again, stamp);
    ASSERT_TRUE(values.Valid());
    EXPECT_EQ(values.Get<kor::Vec4>("tint"), (kor::Vec4 { 0.25f, 0.5f, 0.75f, 1 }));
    EXPECT_EQ(values.Get<float>("gain"), 2.f);
    EXPECT_EQ(values.ResourceAt("target"), scene->runtime->ResourceOf("backdrop")) << "found again by the node that makes it";
    EXPECT_EQ(Parameters(world, again, show).Get<kor::Vec2>("offset"), (kor::Vec2 { -1, 0.5f }));
    EXPECT_EQ(Parameters(world, again, show).ResourceAt("source"), nullptr) << "one made in code has no name to be found by";
}

namespace {
    /** A square a unit across, about @p centre: two triangles, with what is asked of its vertices. */
    kgraph::VertexData SquareData(const float centre, const bool normals, const std::vector<std::vector<std::string>>& buffers)
    {
        using kgraph::AttributeType;
        kgraph::VertexData data;
        data.vertexCount = 4;
        const float corners[4][2] { { -0.25f, -0.25f }, { 0.25f, -0.25f }, { 0.25f, 0.25f }, { -0.25f, 0.25f } };
        kgraph::VertexAttribute position { "POSITION", AttributeType::eFloat, 3, {} }, normal { "NORMAL", AttributeType::eFloat, 3, {} },
            uv { "UV", AttributeType::eFloat, 2, {} }, wind { "WIND", AttributeType::eFloat, 1, {} };
        for (int v = 0; v < 4; ++v) {
            for (const float f : { corners[v][0] + centre, corners[v][1], 0.f }) position.words.push_back(std::bit_cast<std::uint32_t>(f));
            for (const float f : { 0.f, 0.f, 1.f }) normal.words.push_back(std::bit_cast<std::uint32_t>(f));
            for (const float f : { 0.f, 0.f }) uv.words.push_back(std::bit_cast<std::uint32_t>(f));
            wind.words.push_back(std::bit_cast<std::uint32_t>(static_cast<float>(v) * 0.5f));
        }
        data.attributes = { std::move(wind), std::move(position), std::move(uv) };
        if (normals) data.attributes.push_back(std::move(normal));
        data.indices = { 0, 1, 2, 0, 2, 3 };
        data.buffers = buffers;
        return data;
    }
}

// A pipeline draws every mesh that has what its vertex shader reads, however the mesh lays its vertices out: in other
// buffers, with more besides. The pipeline is made again for each other layout. One that lacks what is read is passed
// by, and says what it lacks. A script's mesh, of attributes of its own, is read back as it was made.
TEST_F(EntityTest, AnyMeshThatHasWhatTheShaderReadsIsDrawn) {
    using kgraph::Value;
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("shape", "Primitive");   // the layout it is made with: POSITION, NORMAL and UV, in one buffer
    // Its vertex shader reads POSITION and NORMAL, by their semantics: what else a mesh has, and where, is no matter.
    graph.Add("placed", "GraphicsPipeline").Set("vertex.module", "placedNormal").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "shape", "layout")
        .Set("fragment.module", "placedNormal").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("drawn", "ForEach").Set("component", "placed").Set("with", Value::List { "Mesh" });
    graph.Add("draw", "Draw").Wire("pipeline", "placed").Wire("mesh", "drawn", "Mesh.mesh").Wire("push.model", "drawn", "matrix")
        .Wire("push.color", "drawn", "field.color");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const kecs::ComponentId placed = runtime.ComponentOf("placed");
    ASSERT_NE(placed, kecs::NoComponent);

    // Left: a square laid out as the pipeline is. Right: one in two buffers, POSITION in the second, with WIND besides.
    std::string error;
    ASSERT_TRUE(runtime.MakeMesh("$script/plain", SquareData(-0.5f, true, {}), &error)) << error;
    ASSERT_TRUE(runtime.MakeMesh("$script/windy", SquareData(0.5f, true, { { "WIND", "NORMAL", "UV" }, { "POSITION" } }), &error)) << error;
    ASSERT_TRUE(runtime.MakeMesh("$script/flat", SquareData(0.f, false, {}), &error)) << error;
    const auto windy = runtime.ResourceOf("$script/windy");
    ASSERT_NE(windy, nullptr);
    EXPECT_EQ(kor::capi::MeshOf(windy->handle)->Layout().bindings.size(), 2u) << "a vertex buffer for each group";

    kecs::World& world = scene->world;
    const auto entity = [&](const std::string& mesh, const kor::Vec4 color) {
        const kecs::Entity made = world.Create();
        world.Add<kgraph::Mesh>(made, { runtime.ResourceOf(mesh) });
        world.Add(made, placed);
        kgraph::Parameters(world, made, placed).Set("color", color);
        return made;
    };
    entity("$script/plain", { 1, 0, 0, 1 });
    entity("$script/windy", { 0, 1, 0, 1 });
    entity("$script/flat", { 0, 0, 1, 1 });
    Frame();
    Frame();
    const auto* report = runtime.ReportOf("draw");
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(report->ran, 2u) << report->why;
    EXPECT_EQ(report->skipped, 1u) << "the one with no NORMAL";
    EXPECT_NE(report->why.find("has not what the vertex shader reads"), std::string::npos) << report->why;
    EXPECT_EQ(scene->Pixel(8, 16).x, 255) << "the plain square, on the left";
    EXPECT_EQ(scene->Pixel(24, 16).y, 255) << "the windy one, on the right, drawn with the pipeline made for its layout";
    EXPECT_EQ(scene->Pixel(16, 16).z, 0) << "the flat one, passed by";

    // Read back as it was made: every attribute, in the buffers it was put in.
    const auto read = runtime.ReadMesh("$script/windy", &error);
    ASSERT_TRUE(read) << error;
    EXPECT_EQ(read->vertexCount, 4u);
    const auto* wind = read->Find("WIND");
    ASSERT_NE(wind, nullptr);
    EXPECT_EQ(std::bit_cast<float>(wind->words[3]), 1.5f);
    EXPECT_EQ(read->buffers, (std::vector<std::vector<std::string>> { { "WIND", "NORMAL", "UV" }, { "POSITION" } }));
    EXPECT_EQ(read->indices, (std::vector<std::uint32_t> { 0, 1, 2, 0, 2, 3 }));
    const auto cube = runtime.ReadMesh("$cube", &error);
    ASSERT_TRUE(cube) << error;
    EXPECT_NE(cube->Find("UV"), nullptr);

    // Made again with NORMAL: what held the flat one holds the new one, and it is drawn.
    ASSERT_TRUE(runtime.MakeMesh("$script/flat", SquareData(0.f, true, {}), &error)) << error;
    Frame();
    Frame();
    EXPECT_EQ(runtime.ReportOf("draw")->ran, 3u) << runtime.ReportOf("draw")->why;
    EXPECT_EQ(scene->Pixel(16, 16).z, 255);

    kgraph::VertexData wrong = SquareData(0.f, true, {});
    wrong.attributes[1].words.pop_back();
    EXPECT_FALSE(runtime.MakeMesh("$script/wrong", wrong, &error));
    EXPECT_NE(error.find("POSITION"), std::string::npos) << error;
}

// A model's meshes are read with every attribute they have; the scene can leave some out — they are then not held at
// all — and put the rest in buffers of its choosing. Changed, the file is read again, and what held its meshes follows.
TEST_F(EntityTest, AModelsAttributesAreReadAsTheSceneSays) {
    const auto file = std::filesystem::temp_directory_path() / "kgraph-import.obj";
    std::ofstream(file) << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";
    auto& runtime = *scene->runtime;
    runtime.Set(kgraph::Graph {});
    const std::string name = file.generic_string(), id = "@" + name + "#0";
    const auto wait = [&](const std::function<bool()>& done) { for (int i = 0; i < 400 && !done(); ++i) Frame(); return done(); };
    const auto held = runtime.ResourceOf(id);
    ASSERT_NE(held, nullptr);
    ASSERT_TRUE(wait([&] { const auto now = runtime.ResourceOf(id); return now && !now->pending; })) << "read";
    std::string error;
    auto read = runtime.ReadMesh(id, &error);
    ASSERT_TRUE(read) << error;
    for (const char* semantic : { "POSITION", "NORMAL", "UV", "TANGENT", "BITANGENT" }) EXPECT_NE(read->Find(semantic), nullptr) << semantic << ": a model with UVs has its tangents worked out";
    EXPECT_TRUE(read->buffers.empty()) << "all in one buffer, by default";
    const auto before = runtime.ResourceOf(id);

    // UV left out, POSITION in a buffer of its own: read again so.
    kgraph::Graph graph;
    graph.meshImports[name] = { .leaveOut = { "UV" }, .buffers = { { "POSITION" } } };
    runtime.Set(graph);
    ASSERT_TRUE(wait([&] { return before->replacedBy != nullptr; })) << "read again";
    read = runtime.ReadMesh(id, &error);
    ASSERT_TRUE(read) << error;
    EXPECT_EQ(read->Find("UV"), nullptr) << "not held at all";
    EXPECT_NE(read->Find("NORMAL"), nullptr);
    ASSERT_EQ(read->buffers.size(), 2u);
    EXPECT_EQ(read->buffers[0], std::vector<std::string> { "POSITION" });
    const auto now = runtime.ResourceOf(id);
    const auto layout = kor::capi::MeshOf(now->source->handle)->Layout();
    EXPECT_EQ(layout.bindings.size(), 2u);
    EXPECT_TRUE(std::ranges::none_of(layout.attributes, [](const auto& a) { return a.semantic == "UV"; }));

    // Saved with the graph, and read back.
    kgraph::Graph again;
    ASSERT_TRUE(kgraph::Graph::FromJson(graph.ToJson(), again, error)) << error;
    EXPECT_EQ(again.meshImports, graph.meshImports);
    std::filesystem::remove(file);
}

// A generator's mesh is kept in the project's cache when it is made, and is there when it is asked for again with
// nothing made of it: a scene opened without its scripts still has what they made.
TEST_F(EntityTest, AGeneratorsMeshIsKeptInTheCache) {
    const auto root = std::filesystem::temp_directory_path() / "kgraph-generated";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "assets");
    auto& runtime = *scene->runtime;
    runtime.Set(kgraph::Graph {});
    runtime.SetAssetDirectory(root / "assets");
    std::string error;
    ASSERT_TRUE(runtime.MakeMesh("$script/gen/Square/aa", SquareData(0.f, true, {}), &error)) << error;
    const kasset::Cache cache(kasset::CacheDirectoryOf(root));
    EXPECT_TRUE(std::filesystem::exists(cache.PathOf("meshes", kasset::Key("generated@1").Add("$script/gen/Square/aa"), ".kmesh"))) << "kept when it is made";
    ASSERT_TRUE(cache.Write("meshes", kasset::Key("generated@1").Add("$script/gen/Square/bb"), ".kmesh", kasset::Serialize(SquareData(0.5f, false, {}))));
    const auto held = runtime.ResourceOf("$script/gen/Square/bb");
    ASSERT_NE(held, nullptr) << "what nothing made this run is had from the cache";
    EXPECT_EQ(held->kind, "Mesh");
    const auto read = runtime.ReadMesh("$script/gen/Square/bb", &error);
    ASSERT_TRUE(read) << error;
    EXPECT_EQ(read->vertexCount, SquareData(0.5f, false, {}).vertexCount);
    EXPECT_EQ(runtime.ResourceOf("$script/gen/Square/cc"), nullptr) << "and what was never made is nothing";
    // A picture a generator made, the same.
    std::vector<std::byte> texels(4 * 4 * 4, std::byte { 200 });
    ASSERT_TRUE(runtime.MakeImage("$script/gen/Paint/aa/picture", 4, 4, texels, &error)) << error;
    EXPECT_TRUE(std::filesystem::exists(cache.PathOf("images", kasset::Key("generated@1").Add("$script/gen/Paint/aa/picture"), ".kimage")));
    EXPECT_FALSE(runtime.MakeImage("$script/gen/Paint/bad", 4, 4, std::span(texels).first(10), &error)) << "so many texels are so many bytes";
    const auto picture = runtime.ResourceOf("$script/gen/Paint/aa/picture");
    ASSERT_TRUE(picture);
    EXPECT_EQ(picture->kind, "Image");
    runtime.SetAssetDirectory({});
    std::filesystem::remove_all(root);
}

// A folder of the assets may be a link to one kept elsewhere: a model in it is found by its name in the assets, and its
// meshes arrive under the names they were asked for, not as stand-ins forever.
TEST_F(EntityTest, AModelInAFolderLinkedIntoTheAssetsArrives) {
    const auto root = std::filesystem::temp_directory_path() / "kgraph-linked-assets";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "elsewhere");
    std::filesystem::create_directories(root / "assets");
    std::ofstream(root / "elsewhere" / "quad.obj") << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n";
    std::filesystem::create_directory_symlink(root / "elsewhere", root / "assets" / "linked");
    auto& runtime = *scene->runtime;
    runtime.Set(kgraph::Graph {});
    runtime.SetAssetDirectory(root / "assets");
    const std::string id = "@linked/quad.obj#0";
    EXPECT_EQ(runtime.AssetId(root / "assets" / "linked" / "quad.obj", 0), id);
    const auto held = runtime.ResourceOf(id);
    ASSERT_NE(held, nullptr);
    for (int i = 0; i < 400 && !held->replacedBy; ++i) Frame();
    EXPECT_NE(held->replacedBy, nullptr) << "the stand-in is led to the mesh read";
    const auto now = runtime.ResourceOf(id);
    ASSERT_NE(now, nullptr);
    EXPECT_FALSE(now->pending);
    runtime.SetAssetDirectory({});
    std::filesystem::remove_all(root);
}

// Many entities of a few meshes drawn by one indirect draw: a Filter groups them by their mesh, a MeshPack packs the
// meshes as one, and an IndirectDraws writes a command for each mesh — its part of the pack, its group's instances.
TEST_F(EntityTest, EntitiesGroupedByMeshAreDrawnByOneIndirectDraw) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube"), sphere = runtime.ResourceOf("$sphere");
    ASSERT_TRUE(cube && sphere);
    // Left to right: a red cube, a green sphere, a blue cube, a white sphere.
    const std::array<std::pair<std::shared_ptr<kgraph::Resource>, std::vector<float>>, 4> made { {
        { cube, { 1, 0, 0 } }, { sphere, { 0, 1, 0 } }, { cube, { 0, 0, 1 } }, { sphere, { 1, 1, 1 } } } };
    std::vector<kecs::Entity> entities;
    for (std::size_t i = 0; i < made.size(); ++i) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { -0.75f + 0.5f * static_cast<float>(i), 0, 0 });
        world.Add<kgraph::Mesh>(entity, { made[i].first });
        support::Give(world, entity, "PointLight", { { "color", made[i].second } });
        entities.push_back(entity);
    }

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh", "PointLight" }).Set("groupBy", "Mesh");
    graph.Add("place", "Append").Wire("a", "meshes", "worldPosition").Set("b", 0.4);
    graph.Add("tint", "Append").Wire("a", "meshes", "PointLight.color").Set("b", 1);
    graph.Add("instances", "BufferBuilder")
        .Set("layout", Value::List { Value::Members { { "name", "place" }, { "type", "eVec4" } }, Value::Members { { "name", "color" }, { "type", "eVec4" } } })
        .Wire("field.place", "place").Wire("field.color", "tint");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("drawer", "GraphicsPipeline").Set("vertex.module", "indirect").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "indirect").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "drawer").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount").Wire("param.instances", "instances");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    // Grouped by mesh: the cubes, then the spheres, each group where its first one was.
    EXPECT_EQ(runtime.ValueOf("meshes", "groupCount"), Value(2));
    EXPECT_EQ(runtime.ValueOf("meshes", "groups"), Value(Value::List { Value::Members { { "first", 0 }, { "count", 2 } }, Value::Members { { "first", 2 }, { "count", 2 } } }));
    EXPECT_EQ(runtime.ValueOf("pack", "count"), Value(2)) << "two meshes, packed once each";
    const Value rangesValue = runtime.ValueOf("pack", "ranges");
    const auto* ranges = rangesValue.Items();
    ASSERT_TRUE(ranges && ranges->size() == 2) << rangesValue.ToJson();
    const auto number = [&](const std::size_t mesh, const char* name) {
        const Value* found = (*ranges)[mesh].Find(name);
        return found && found->Number() ? *found->Number() : -1.0;
    };
    EXPECT_EQ(number(0, "firstIndex"), 0.0) << rangesValue.ToJson();
    EXPECT_GT(number(1, "firstIndex"), 0.0) << "the sphere's indices after the cube's";
    EXPECT_GT(number(1, "vertexOffset"), 0.0);
    EXPECT_EQ(runtime.ValueOf("commands", "drawCount"), Value(2));
    {
        const auto words = kor::capi::BufferOf(runtime.ResourceOf("commands", "commands")->handle)->Read<std::uint32_t>();
        ASSERT_GE(words.size(), 10u);
        EXPECT_EQ(words[1], 2u) << "two cubes";
        EXPECT_EQ(words[4], 0u) << "from the first instance";
        EXPECT_EQ(words[6], 2u) << "two spheres";
        EXPECT_EQ(words[9], 2u) << "after the cubes";
        EXPECT_EQ(words[7], static_cast<std::uint32_t>(number(1, "firstIndex")));
    }
    // All four drawn by the one draw, each where it is and in its own colour.
    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(4, { 255, 0, 0 })) << "the red cube";
    EXPECT_TRUE(is(12, { 0, 255, 0 })) << "the green sphere";
    EXPECT_TRUE(is(20, { 0, 0, 255 })) << "the blue cube";
    EXPECT_TRUE(is(28, { 255, 255, 255 })) << "the white sphere";
    EXPECT_TRUE(is(8, { 0, 0, 0 })) << "and nothing between them";

    // One given another mesh: grouped again, and the meshes packed again.
    world.Get<kgraph::Mesh>(entities[1]) ? (void)(world.Mutable<kgraph::Mesh>(entities[1])->mesh = cube) : (void)0;
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(runtime.ValueOf("meshes", "groups"), Value(Value::List { Value::Members { { "first", 0 }, { "count", 3 } }, Value::Members { { "first", 3 }, { "count", 1 } } }));
    EXPECT_TRUE(is(12, { 0, 255, 0 })) << "a green cube now, where it was";
    EXPECT_TRUE(is(28, { 255, 255, 255 }));

    // What it holds, looked at: a command a mesh, by field — so that what is batched can be seen.
    runtime.Watch("commands");
    std::optional<kgraph::Peek> peek;
    for (int i = 0; i < 8 && !(peek = runtime.Watched("commands")); ++i) Frame();
    ASSERT_TRUE(peek) << runtime.WatchProblem("commands");
    EXPECT_FALSE(peek->image);
    EXPECT_EQ(peek->stride, 20u);
    EXPECT_EQ(peek->count, 2u) << "one command a mesh";
    std::vector<std::string> named;
    for (const auto& field : peek->fields) named.push_back(field.name);
    EXPECT_EQ(named, (std::vector<std::string> { "indexCount", "instanceCount", "firstIndex", "vertexOffset", "firstInstance" }));
    const auto word = [&](const std::size_t at) { std::uint32_t v = 0; std::memcpy(&v, peek->bytes.data() + at, 4); return v; };
    ASSERT_GE(peek->bytes.size(), 40u);
    EXPECT_EQ(word(4), 3u) << "three cubes, in one command";
    EXPECT_EQ(word(24), 1u) << "the sphere left, in another";
    EXPECT_EQ(word(36), 3u) << "after the cubes";
    runtime.Unwatch("commands");

    // For a compute pass to count what it draws: every command starts at none, and nothing is drawn of itself.
    graph.Find("commands")->Set("mode", "eGPU");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    ASSERT_TRUE(runtime.Status("commands")->ok) << runtime.Status("commands")->error;
    EXPECT_NE(runtime.ResourceOf("commands", "visible"), nullptr) << "and room to say which instances it draws";
    EXPECT_EQ(kor::capi::BufferOf(runtime.ResourceOf("commands", "commands")->handle)->Read<std::uint32_t>()[1], 0u);
    EXPECT_TRUE(is(4, { 0, 0, 0 })) << "no cull ran: nothing counted, nothing drawn";

    // A pass that counts them: every frame from none, however many frames there are — not what the frames before
    // counted into the copy of the buffer it is given, added up.
    kgraph::Graph counting = graph;
    counting.Add("count", "ComputePipeline").Set("compute.module", "countAll").Set("compute.entry", "countAll")
        .Wire("param.commands", "commands", "commands").Wire("param.visible", "commands", "visible");
    counting.Add("run count", "Dispatch").Wire("pipeline", "count").Set("groups.x", 1).Set("groups.y", 1).Set("groups.z", 1)
        .Set("push.instances", 4).Wire("push.commandCount", "commands", "drawCount");
    runtime.Set(counting);
    for (int i = 0; i < 12; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(is(12, { 0, 255, 0 })) << "counted, drawn";
    runtime.Watch("commands");
    std::optional<kgraph::Peek> counted;
    for (int i = 0; i < 8 && !(counted = runtime.Watched("commands")); ++i) Frame();
    ASSERT_TRUE(counted) << runtime.WatchProblem("commands");
    const auto countedWord = [&](const std::size_t at) { std::uint32_t v = 0; std::memcpy(&v, counted->bytes.data() + at, 4); return v; };
    EXPECT_EQ(countedWord(4), 3u) << "the three cubes, counted this frame and no other";
    EXPECT_EQ(countedWord(24), 1u);
    runtime.Unwatch("commands");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();

    // Meshes whose vertices are not laid out alike are not one mesh, and it says so.
    kgraph::VertexData flat;
    flat.vertexCount = 3;
    flat.attributes.push_back({ "POSITION", kgraph::AttributeType::eFloat, 3, { 0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0 } });
    flat.indices = { 0, 1, 2 };
    ASSERT_TRUE(runtime.MakeMesh("$script/flat", flat));
    world.Mutable<kgraph::Mesh>(entities[3])->mesh = runtime.ResourceOf("$script/flat");
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NE(runtime.Status("pack")->error.find("not laid out alike"), std::string::npos) << runtime.Status("pack")->error;

    // Told which of them to pack, it packs those of each, whatever else each has: zeros where one has not.
    graph.Find("pack")->Set("attributes", Value::List { "POSITION", "NORMAL", "UV" });
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    ASSERT_TRUE(runtime.Status("pack")->ok) << runtime.Status("pack")->error;
    const Value layout = runtime.ValueOf("pack", "layout");
    const auto* bindings = layout.Find("bindings") ? layout.Find("bindings")->Items() : nullptr;
    ASSERT_NE(bindings, nullptr) << layout.ToJson();
    EXPECT_EQ(bindings->size(), 3u) << "a buffer each: " << layout.ToJson();
    const auto* attributes = layout.Find("attributes") ? layout.Find("attributes")->Items() : nullptr;
    ASSERT_NE(attributes, nullptr);
    ASSERT_EQ(attributes->size(), 3u);
    EXPECT_EQ(*(*attributes)[2].Find("semantic")->String(), "UV");
    EXPECT_EQ(*(*attributes)[2].Find("channelCount")->Number(), 2.0);
    graph.Find("pack")->Set("attributes", Value::List { "COLOR7" });
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_NE(runtime.Status("pack")->error.find("COLOR7"), std::string::npos) << "what no mesh has: " << runtime.Status("pack")->error;
}

// Nothing yet to draw — the scene's meshes made by a script once it plays — is no problem: the pack is an empty mesh,
// the draw clears and draws nothing, and what comes later is drawn.
TEST_F(EntityTest, AnIndirectDrawOfNothingYetClearsAndDrawsWhatComesLater) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh", "PointLight" }).Set("groupBy", "Mesh");
    graph.Add("place", "Append").Wire("a", "meshes", "worldPosition").Set("b", 0.4);
    graph.Add("tint", "Append").Wire("a", "meshes", "PointLight.color").Set("b", 1);
    graph.Add("instances", "BufferBuilder")
        .Set("layout", Value::List { Value::Members { { "name", "place" }, { "type", "eVec4" } }, Value::Members { { "name", "color" }, { "type", "eVec4" } } })
        .Wire("field.place", "place").Wire("field.color", "tint");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("drawer", "GraphicsPipeline").Set("vertex.module", "indirect").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "indirect").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "drawer").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Set("clearColor", Value::List { 0, 0, 1, 1 })
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount").Wire("param.instances", "instances");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok()) << "nothing to pack is nothing to draw, not a problem";
    EXPECT_EQ(runtime.ValueOf("pack", "count"), Value(0));
    EXPECT_EQ(runtime.ValueOf("commands", "drawCount"), Value(0));
    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(16, { 0, 0, 255 })) << "cleared, and nothing drawn";

    const kecs::Entity entity = world.Create();
    world.SetPosition(entity, { -0.75f, 0, 0 });
    world.Add<kgraph::Mesh>(entity, { runtime.ResourceOf("$cube") });
    support::Give(world, entity, "PointLight", { { "color", { 1.f, 0.f, 0.f } } });
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ValueOf("commands", "drawCount"), Value(1));
    EXPECT_TRUE(is(4, { 255, 0, 0 })) << "the cube, once there is one";
    EXPECT_TRUE(is(16, { 0, 0, 255 }));
}

// A buffer its shader marks [entity("PER_INSTANCE")] is the Draw's to make and fill: an element for each entity of the
// Filter its MeshPack packs, where each is and which, and its own fields as a field of the pipeline's component.
TEST_F(EntityTest, AShaderSaysWhatEachInstanceIsAndTheDrawFillsIt) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    std::vector<kecs::Entity> entities;
    for (const float x : { -0.5f, 0.5f }) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, 0, 0.5f });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { cube });
        entities.push_back(entity);
    }

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("tinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    // Nothing given for its instances: the shader says what they are.
    graph.Add("draw", "Draw").Wire("pipeline", "tinted").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
    // Another Draw of the same, over the first: it would write the same into its buffer, so it has the first's.
    graph.Add("again", "Draw").Wire("pipeline", "tinted").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount").Set("load", true);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    ASSERT_NE(runtime.ResourceOf("draw", "instances"), nullptr) << "the Draw made its buffer of instances";
    EXPECT_EQ(runtime.ResourceOf("again", "instances"), runtime.ResourceOf("draw", "instances")) << "one buffer for both";
    {   // The same shader, another pipeline: its colour is another component's, so its buffer is its own.
        kgraph::Graph other = graph;
        other.Add("retinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
            .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
        other.Add("elsewhere", "Draw").Wire("pipeline", "retinted").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
            .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount").Set("load", true);
        runtime.Set(other);
        for (int i = 0; i < 3; ++i) Frame();
        for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
        ASSERT_NE(runtime.ResourceOf("elsewhere", "instances"), nullptr);
        EXPECT_NE(runtime.ResourceOf("elsewhere", "instances"), runtime.ResourceOf("draw", "instances"));
        EXPECT_EQ(runtime.ResourceOf("again", "instances"), runtime.ResourceOf("draw", "instances"));
        runtime.Set(graph);
        for (int i = 0; i < 3; ++i) Frame();
    }

    // What is not the entity's place nor which it is, is its own: the pipeline's component has it, and nothing else of the buffer.
    const kecs::ComponentId tinted = world.Find("tinted");
    ASSERT_NE(tinted, kecs::NoComponent) << "the pipeline's component";
    std::vector<std::string> fields;
    for (const auto& field : world.Describe(tinted).fields) fields.push_back(field.name);
    EXPECT_EQ(fields, (std::vector<std::string> { "color" })) << "not the matrix, not the id, not the buffer";

    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(8, { 0, 0, 0 })) << "an entity without the component is drawn with nothing of its own: black";
    support::Give(world, entities[0], "tinted", { { "color", { 1, 0, 0, 1 } } });
    support::Give(world, entities[1], "tinted", { { "color", { 0, 1, 0, 1 } } });
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(is(8, { 255, 0, 0 })) << "each where its matrix says, in its own colour";
    EXPECT_TRUE(is(24, { 0, 255, 0 }));
    EXPECT_TRUE(is(16, { 0, 0, 0 })) << "and nothing between them";

    // Moved, it is drawn where it is now: the buffer follows the world each frame.
    world.SetPosition(entities[0], { 0, 0, 0.5f });
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(16, { 255, 0, 0 }));
    EXPECT_TRUE(is(8, { 0, 0, 0 }));

    // More than there is room for: another buffer, and the two still drawn as they are.
    for (int i = 0; i < 80; ++i) {
        const kecs::Entity far = world.Create();
        world.SetPosition(far, { 5, 5, 0.5f });
        world.Add<kgraph::Mesh>(far, { cube });
    }
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(is(16, { 255, 0, 0 }));
    EXPECT_TRUE(is(24, { 0, 255, 0 }));
    ASSERT_NE(runtime.ResourceOf("draw", "instances"), nullptr);
    EXPECT_EQ(runtime.ResourceOf("again", "instances"), runtime.ResourceOf("draw", "instances")) << "outgrown, both have the one made in its place";
}

// A loop's `instances`, wired to a Draw's buffer of each instance's, say whose entities fill it: a Filter's, for one
// indirect draw of them all, or a ForEach's, for a draw an element that reads its own. It is filled as best what they
// have fills it — a field the shader does not mark is the same-named field of one of the loop's components — and what
// none of them has is said on the Draw, which draws all the same.
TEST_F(EntityTest, ALoopsInstancesFillADrawsBufferFromWhatItsEntitiesHave) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    const std::array<std::array<float, 3>, 2> colors { { { 1, 0, 0 }, { 0, 1, 0 } } };
    for (int i = 0; i < 2; ++i) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { i == 0 ? -0.5f : 0.5f, 0, 0.5f });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { cube });
        support::Give(world, entity, "PointLight", { { "color", { colors[i][0], colors[i][1], colors[i][2] } } });
    }

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh", "PointLight" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("tinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "tinted").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount").Wire("param.instances", "meshes", "instances");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    ASSERT_NE(runtime.ResourceOf("draw", "instances"), nullptr) << "the Draw made its buffer, of the Filter's entities";
    ASSERT_NE(runtime.Status("draw"), nullptr);
    EXPECT_EQ(runtime.Status("draw")->note, "") << "every field found: the colour is each one's PointLight's";
    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(8, { 255, 0, 0 })) << "each in its own PointLight's colour, with nothing of the pipeline's component";
    EXPECT_TRUE(is(24, { 0, 255, 0 }));
    const auto outputs = runtime.OutputsOf("meshes");
    const auto instances = std::ranges::find(outputs, std::string("instances"), &kgraph::Port::name);
    ASSERT_NE(instances, outputs.end());
    EXPECT_EQ(instances->type, "Instances") << "a wire of its own";
    EXPECT_FALSE(instances->optional) << "on the node";

    {   // Of a loop whose entities have nothing of that name: said on the Draw, which draws all the same.
        kgraph::Graph bare = graph;
        bare.Find("meshes")->Set("with", Value::List { "Mesh" });
        runtime.Set(bare);
        for (int i = 0; i < 3; ++i) Frame();
        for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
        ASSERT_NE(runtime.Status("draw"), nullptr);
        EXPECT_TRUE(runtime.Status("draw")->ok);
        EXPECT_NE(runtime.Status("draw")->note.find("color"), std::string::npos) << runtime.Status("draw")->note;
        EXPECT_TRUE(is(8, { 0, 0, 0 })) << "no colour of its own: black";
    }

    {   // A ForEach's, a draw an element: each reads its own element of the one buffer.
        kgraph::Graph each = graph;
        std::erase_if(each.nodes, [](const kgraph::Node& node) { return node.id == "draw"; });
        each.Add("lamps", "ForEach").Set("component", "Mesh").Set("with", Value::List { "PointLight" });
        each.Add("draw", "Draw").Wire("pipeline", "tinted").Wire("mesh", "lamps", "field.mesh").Wire("param.instances", "lamps", "instances");
        runtime.Set(each);
        for (int i = 0; i < 3; ++i) Frame();
        for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
        ASSERT_TRUE(runtime.Ok());
        EXPECT_TRUE(is(8, { 255, 0, 0 })) << "the first element's draw reads the first instance";
        EXPECT_TRUE(is(24, { 0, 255, 0 })) << "and the second, the second";

        // Its Mesh, taken apart, gives its buffers and layout too.
        each.Add("mesh parts", "Unbundle").Wire("of", "lamps", "Mesh");
        runtime.Set(each);
        Frame();
        const auto parts = runtime.OutputsOf("mesh parts");
        const auto part = [&](const char* name) { return std::ranges::find(parts, std::string(name), &kgraph::Port::name); };
        for (const char* shown : { "mesh", "vertices", "indices", "layout" }) {
            ASSERT_NE(part(shown), parts.end()) << shown;
            EXPECT_FALSE(part(shown)->optional) << shown << " is on the node";
        }
        ASSERT_NE(part("vertexCount"), parts.end());
        EXPECT_TRUE(part("vertexCount")->optional) << "there to be asked for";
        EXPECT_EQ(part("vertices")->type, "Buffer");
    }
}

// A buffer of instances is written again an element at a time, only where something changed: an entity moved — by
// itself or by what it is under — given, rid of or written its own fields, gone, or another in its place. Each is seen
// in every copy of the buffer the frames in flight read, and what did not change stays as it was written.
TEST_F(EntityTest, AnInstanceIsWrittenAgainWhenWhatItIsMadeOfChanges) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    const auto make = [&](const float x) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, 0, 0.5f });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { cube });
        return entity;
    };
    const kecs::Entity left = make(-0.5f);
    // The right one is under a parent of no mesh, which places it.
    const kecs::Entity holder = world.Create();
    const kecs::Entity right = make(0.f);
    world.SetParent(right, holder, -1, false);
    world.SetPosition(holder, { 0.5f, 0, 0 });

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("tinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "tinted").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const kecs::ComponentId tinted = world.Find("tinted");
    ASSERT_NE(tinted, kecs::NoComponent);

    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    // Enough frames for every copy of the buffer to have been read: what one copy missed, another would show.
    const auto frames = [&] { for (int i = 0; i < 4; ++i) Frame(); for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem; };
    support::Give(world, left, "tinted", { { "color", { 1, 0, 0, 1 } } });
    support::Give(world, right, "tinted", { { "color", { 0, 1, 0, 1 } } });
    frames();
    EXPECT_TRUE(is(8, { 255, 0, 0 })) << "given their own colours";
    EXPECT_TRUE(is(24, { 0, 255, 0 })) << "the right one where its parent places it";

    // Its own field written in place: marked changed, and the next frames show it.
    static_cast<float*>(world.Mutable(left, tinted))[2] = 1.f;
    frames();
    EXPECT_TRUE(is(8, { 255, 0, 255 })) << "written in place";
    EXPECT_TRUE(is(24, { 0, 255, 0 })) << "the other as it was";

    // The parent moved, and the child with it: only the parent's place changed, and the child is drawn where it is now.
    world.SetPosition(holder, { 0.25f, 0, 0 });
    frames();
    EXPECT_TRUE(is(20, { 0, 255, 0 })) << "moved with what it is under";
    EXPECT_TRUE(is(28, { 0, 0, 0 }));
    world.SetPosition(holder, { 0.5f, 0, 0 });
    world.SetPosition(holder, { 0.5f, 0, 0 });     // the same again: nothing more to do
    frames();
    EXPECT_TRUE(is(24, { 0, 255, 0 }));

    // Rid of its own fields: nothing of its own, which the shader draws as black; given them back: as they were given.
    world.Remove(left, tinted);
    frames();
    EXPECT_TRUE(is(8, { 0, 0, 0 })) << "no component: nothing of its own";
    support::Give(world, left, "tinted", { { "color", { 0, 0, 1, 1 } } });
    frames();
    EXPECT_TRUE(is(8, { 0, 0, 255 }));

    // Gone, and another made in its place — in its element, maybe with its index: drawn as the new one is.
    world.Destroy(left);
    const kecs::Entity other = make(-0.5f);
    support::Give(world, other, "tinted", { { "color", { 1, 1, 0, 1 } } });
    frames();
    EXPECT_TRUE(is(8, { 255, 255, 0 })) << "the one in its place";
    EXPECT_TRUE(is(24, { 0, 255, 0 }));

    // Nothing changed: what is drawn stays as it is, frame after frame, every copy read in turn.
    for (int i = 0; i < 6; ++i) {
        Frame();
        EXPECT_TRUE(is(8, { 255, 255, 0 })) << "frame " << i;
        EXPECT_TRUE(is(24, { 0, 255, 0 })) << "frame " << i;
    }
}

// A Draw that idles draws nothing, and its buffer of instances is kept as the entities are all the same: another pass
// reads it — the meshlets read the forward Draw's while the forward Draw rests.
TEST_F(EntityTest, AnIdleDrawsBufferOfInstancesFollowsTheEntitiesForWhatReadsIt) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    const kecs::Entity entity = world.Create();
    world.SetPosition(entity, { 0, 0, 0.5f });
    world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
    world.Add<kgraph::Mesh>(entity, { cube });

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("tinted", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    // Resting, always — told so by a value, as a branch not chosen is: its buffer is what the other reads.
    graph.Add("rest", "Double").Set("value", 1);
    graph.Add("resting", "Draw").Wire("pipeline", "tinted").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount").Wire("idle", "rest");
    graph.Add("reader", "GraphicsPipeline").Set("vertex.module", "instanced").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "instanced").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target")
        .Wire("param.instances", "resting", "instances");
    graph.Add("draw", "Draw").Wire("pipeline", "reader").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    const auto is = [&](const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(16, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    support::Give(world, entity, "tinted", { { "color", { 1, 0, 0, 1 } } });
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is({ 255, 0, 0 })) << "given its colour while the Draw whose buffer it is rests";
    support::Give(world, entity, "tinted", { { "color", { 0, 0, 1, 1 } } });
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is({ 0, 0, 255 })) << "and changed";
}

// A field of each instance's marked [entity("TEXTURE")] is a picture the entity holds, read by its bindless index: one
// indirect draw of entities that each have pictures of their own. What the draw reads by index is still read: the
// frame graph is told, so a picture last written as a copy's destination is ready to be sampled.
TEST_F(EntityTest, EachInstanceReadsItsOwnPictureByIndex) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    std::vector<kecs::Entity> entities;
    for (const float x : { -0.5f, 0.5f }) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, 0, 0.5f });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { cube });
        entities.push_back(entity);
    }
    // A picture of one colour: cleared, and left as a copy's destination.
    const auto solid = [](const kor::Vec4 color) {
        auto image = kor::Image::Builder {}.SetType(kor::Image::Type::e2D).SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2 { 4, 4 })
            .SetUsage(kor::Image::Usage::eSampled | kor::Image::Usage::eTransferDst).Build();
        kor::CommandBuffer::SingleTimeCommand([&](kor::CommandBuffer& cb) { cb.ClearColorImage(image, color); }, kor::CommandBuffer::Usage::eGraphics).Wait();
        return kgraph::MakeResource(std::move(image));
    };

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("pictured", "GraphicsPipeline").Set("vertex.module", "pictured").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "pictured").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "pictured").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    // The picture is the component's: a field that holds an image, as a parameter's would be.
    const kecs::ComponentId pictured = world.Find("pictured");
    ASSERT_NE(pictured, kecs::NoComponent);
    const kecs::FieldDescription* field = world.Describe(pictured).FindField("picture");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->type, "Image");
    EXPECT_EQ(world.Describe(pictured).fields.size(), 1u) << "not the matrix, not the buffer";

    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(8, { 255, 0, 255 })) << "without the component it holds no picture: the shader is told so (~0)";
    world.Add(entities[0], pictured);
    world.Add(entities[1], pictured);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(8, { 255, 0, 255 })) << "with it, and no picture given: none, as well";
    kgraph::Parameters(world, entities[0], pictured).Set("picture", solid({ 1, 0, 0, 1 }));
    kgraph::Parameters(world, entities[1], pictured).Set("picture", solid({ 0, 1, 0, 1 }));
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(is(8, { 255, 0, 0 })) << "each in its own picture's colour, in one draw";
    EXPECT_TRUE(is(24, { 0, 255, 0 }));
    EXPECT_EQ(runtime.ValueOf("commands", "drawCount"), Value(1)) << "one mesh: one command";

    // Another picture given: the next frame reads it.
    kgraph::Parameters(world, entities[1], pictured).Set("picture", solid({ 0, 0, 1, 1 }));
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(24, { 0, 0, 255 }));

    // Every element the buffer has room for is written, not only the list's: a GPU cull reads it to its end. Past the
    // list, and an entity's that is gone, is nothing — nowhere, and no picture — whatever the memory held: an index read
    // from what was there before is a picture that is not there, and the GPU that samples it hangs. A buffer is written
    // whole, into each of its copies, the first time; after that only the runtime writes it, an element when it changes.
    const auto buffer = kor::capi::BufferOf(runtime.ResourceOf("draw", "instances")->handle);
    const std::size_t words = buffer->size() / 4, stride = words / 64;   // room for 64: what a buffer of a few is made with
    ASSERT_EQ(words % 64, 0u);
    world.Destroy(entities[1]);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto read = buffer->Read<std::uint32_t>();
    ASSERT_EQ(read.size(), words);
    EXPECT_EQ(read[15], std::bit_cast<std::uint32_t>(1.0f)) << "the one left is where it is";
    EXPECT_NE(read[16], ~0u) << "and holds its picture";
    for (std::size_t row = 1; row < 64; ++row) {
        EXPECT_EQ(read[row * stride + 15], 0u) << "row " << row << ": nowhere";
        EXPECT_EQ(read[row * stride + 16], ~0u) << "row " << row << ": no picture";
    }
    EXPECT_TRUE(is(8, { 255, 0, 0 }));
    EXPECT_TRUE(is(24, { 0, 0, 0 })) << "the one gone is not drawn";
}

// A file written again while the scene has it — a picture painted over, a model exported again — is read again, and
// what held it holds what it is now.
TEST_F(EntityTest, AFileWrittenAgainIsReadAgain) {
    kgraph::Runtime& runtime = *scene->runtime;
    const auto root = std::filesystem::temp_directory_path() / "kgraph-written-again";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "assets");
    const auto write = [&](const unsigned char red) {
        std::ofstream file(root / "assets" / "paint.ppm", std::ios::binary | std::ios::trunc);
        file << "P6\n8 8\n255\n";
        for (int i = 0; i < 64; ++i) file.put(static_cast<char>(red)).put(0).put(0);
    };
    const auto writeModel = [&](const float height) {
        std::ofstream(root / "assets" / "tri.obj", std::ios::trunc) << std::format("v 0 0 0\nv 1 0 0\nv 0 {} 0\nf 1 2 3\n", height);
    };
    write(10);
    writeModel(1.f);
    runtime.Set(kgraph::Graph {});
    runtime.SetAssetDirectory(root / "assets");
    const auto picture = runtime.ResourceOf("@paint.ppm");
    const auto mesh = runtime.ResourceOf("@tri.obj#0");
    ASSERT_TRUE(picture && mesh);
    for (int i = 0; i < 400 && (runtime.Reading() > 0 || mesh->pending && !mesh->replacedBy); ++i) Frame();
    for (int i = 0; i < 3; ++i) Frame();
    const auto read = [&](const std::string& id) { auto held = runtime.ResourceOf(id); while (held && held->replacedBy) held = held->replacedBy; return held; };
    const auto pictureBefore = read("@paint.ppm"), meshBefore = read("@tri.obj#0");
    ASSERT_TRUE(meshBefore && !meshBefore->pending);

    // Written again, a moment later: the time a file system keeps may be no finer than a second.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write(250);
    writeModel(3.f);
    for (int i = 0; i < 400 && (read("@paint.ppm") == pictureBefore || read("@tri.obj#0") == meshBefore); ++i) {
        Frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_NE(read("@paint.ppm"), pictureBefore) << "the picture is read again";
    EXPECT_NE(read("@tri.obj#0"), meshBefore) << "and the model";
    std::string error;
    const auto data = runtime.ReadMesh("@tri.obj#0", &error);
    ASSERT_TRUE(data) << error;
    EXPECT_FLOAT_EQ(std::bit_cast<float>(data->Find("POSITION")->words[7]), 3.f) << "as it is now";
    runtime.SetAssetDirectory({});
    std::filesystem::remove_all(root);
}

// A picture file kept block-compressed is cooked into the project's cache and read from there, in its place: the scene
// still names the file. A pack is one picture of others' channels; each file it is made of reads, to whoever holds it,
// as the pack's channel it went into, in red, green and blue — before it is cooked (put together as it is read) and
// after. BC4 reads its one channel in red, green and blue; BC5 its two, with 1 for the z it has not.
TEST_F(EntityTest, PicturesKeptBlockCompressedAreCookedAndReadFromTheCache) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto root = std::filesystem::temp_directory_path() / "kgraph-cooked-pictures";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "assets");
    // Pictures of one colour each, 8 texels a side: a colour, and three numbers a material's ORM is made of.
    const auto write = [&](const std::string& name, const std::vector<unsigned char>& texel) {
        std::ofstream file(root / "assets" / name, std::ios::binary);
        file << (texel.size() == 3 ? "P6" : "P5") << "\n8 8\n255\n";
        for (int i = 0; i < 64; ++i) file.write(reinterpret_cast<const char*>(texel.data()), static_cast<std::streamsize>(texel.size()));
    };
    write("red.ppm", { 200, 40, 10 });
    write("m_AO.pgm", { 30 });
    write("m_Roughness.pgm", { 100 });
    write("m_Metalness.pgm", { 220 });
    runtime.SetAssetDirectory(root / "assets");

    const auto cube = runtime.ResourceOf("$cube");
    ASSERT_TRUE(cube);
    std::vector<kecs::Entity> entities;
    for (const float x : { -0.5f, 0.5f }) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { x, 0, 0.5f });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { cube });
        entities.push_back(entity);
    }
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh");
    graph.Add("commands", "IndirectDraws").Wire("ranges", "pack", "ranges").Wire("groups", "meshes", "groups");
    graph.Add("pictured", "GraphicsPipeline").Set("vertex.module", "pictured").Set("vertex.entry", "vertexMain").Wire("vertex.layout", "pack", "layout")
        .Set("fragment.module", "pictured").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("draw", "Draw").Wire("pipeline", "pictured").Wire("mesh", "pack", "mesh").Set("command", "eDrawIndexedIndirect")
        .Wire("indirect", "commands", "commands").Wire("drawCount", "commands", "drawCount");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    graph.textureImports["@red.ppm"] = { kgraph::TextureFormat::eBC7 };
    kgraph::TexturePack orm;
    orm.channels = { kgraph::TexturePack::Channel { "@m_AO.pgm" }, { "@m_Roughness.pgm" }, { "@m_Metalness.pgm" }, { {}, 0, 1.f } };
    graph.texturePacks["@m_ORM.pack"] = orm;
    runtime.Set(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    const kecs::ComponentId pictured = world.Find("pictured");
    ASSERT_NE(pictured, kecs::NoComponent);
    world.Add(entities[0], pictured);
    world.Add(entities[1], pictured);
    kgraph::Parameters(world, entities[0], pictured).Set("picture", runtime.ResourceOf("@red.ppm"));
    kgraph::Parameters(world, entities[1], pictured).Set("picture", runtime.ResourceOf("@m_Roughness.pgm"));
    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 6 && std::abs(p.y - rgb[1]) < 6 && std::abs(p.z - rgb[2]) < 6;
    };
    const auto shown = [&](const kor::u32 x) { const auto p = scene->Pixel(x, 16); return std::format("({}, {}, {})", p.x, p.y, p.z); };
    for (int i = 0; i < 200 && (runtime.Reading() > 0 || !is(24, { 100, 100, 100 })); ++i) Frame();
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(8, { 200, 40, 10 })) << "not cooked yet: the file, as it is read — " << shown(8);
    EXPECT_TRUE(is(24, { 100, 100, 100 })) << "a file of a pack not cooked yet: its channel of the pack put together as it is read — " << shown(24);
    EXPECT_FALSE(runtime.Cooked("@red.ppm"));
    const auto pictures = runtime.Pictures();
    EXPECT_NE(std::ranges::find(pictures, "@m_ORM.pack"), pictures.end()) << "the pack is a picture it reads";

    const auto cookAll = [&] {
        runtime.Cook(runtime.Pictures());
        for (int i = 0; i < 3000 && runtime.Cooking().done < runtime.Cooking().total; ++i) Frame();
        for (const auto& why : runtime.Cooking().failed) ADD_FAILURE() << why;
        for (int i = 0; i < 200 && runtime.Reading() > 0; ++i) Frame();
        for (int i = 0; i < 3; ++i) Frame();
    };
    cookAll();
    EXPECT_EQ(runtime.Cooking().total, 2u) << "the file, and the pack: the files the pack is made of are not cooked by themselves";
    EXPECT_TRUE(runtime.Cooked("@red.ppm"));
    EXPECT_TRUE(runtime.Cooked("@m_ORM.pack"));
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(runtime.TextureCache())) files += entry.path().extension() == ".ktx2";
    EXPECT_EQ(files, 2u) << "in the project's cache, beside its assets";
    EXPECT_EQ(runtime.TextureCache(), root / ".koral" / "cache" / "textures");
    const auto red = runtime.ResourceOf("@red.ppm");
    ASSERT_TRUE(red && red->kind == "Image");
    EXPECT_EQ(kor::capi::ImageOf(red->handle)->PixelFormat(), kor::Image::Format::eBC7_UNORM) << "read from what was cooked";
    const auto rough = runtime.ResourceOf("@m_Roughness.pgm");
    ASSERT_TRUE(rough && rough->kind == "ImageView" && rough->source);
    EXPECT_EQ(kor::capi::ImageOf(rough->source->handle)->PixelFormat(), kor::Image::Format::eBC7_UNORM) << "a view of the cooked pack";
    EXPECT_TRUE(is(8, { 200, 40, 10 })) << "cooked, it reads as the file did — " << shown(8);
    EXPECT_TRUE(is(24, { 100, 100, 100 })) << "and the file of the pack, as its channel of the cooked one — " << shown(24);

    // Kept otherwise: cooked again, as that, and what it reads follows.
    graph.textureImports["@red.ppm"] = { kgraph::TextureFormat::eBC4 };
    runtime.Set(graph);
    EXPECT_FALSE(runtime.Cooked("@red.ppm")) << "what was cooked was of other settings";
    cookAll();
    EXPECT_TRUE(is(8, { 200, 200, 200 })) << "BC4: its red, in red, green and blue — " << shown(8);
    graph.textureImports["@red.ppm"] = { kgraph::TextureFormat::eBC5 };
    runtime.Set(graph);
    cookAll();
    EXPECT_TRUE(is(8, { 200, 40, 255 })) << "BC5: its red and green, and 1 — " << shown(8);
    files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(runtime.TextureCache())) files += entry.path().extension() == ".ktx2";
    EXPECT_EQ(files, 2u) << "what was cooked of it before is let go of";

    // Kept as it is read again: the file, and nothing of the cache.
    graph.textureImports.clear();
    runtime.Set(graph);
    for (int i = 0; i < 200 && runtime.Reading() > 0; ++i) Frame();
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(8, { 200, 40, 10 })) << shown(8);
    EXPECT_NE(kor::capi::ImageOf(runtime.ResourceOf("@red.ppm")->handle)->PixelFormat(), kor::Image::Format::eBC5_UNORM);
    runtime.SetAssetDirectory({});
    std::filesystem::remove_all(root);
}

// A function is nodes as one: each call written out into them, given what the call is given and giving what they give.
TEST_F(EntityTest, AFunctionsCallsAreItsNodesWrittenOut) {
    using kgraph::Value;
    kgraph::Runtime& runtime = *scene->runtime;
    // Times three, plus what it is told to add: 1 when it is told nothing.
    auto tripled = std::make_shared<kgraph::Graph>();
    tripled->Add("x", "FunctionInput").Set("name", "x").Set("kind", "eValue");
    tripled->Find("x")->y = 0;
    tripled->Add("plus", "FunctionInput").Set("name", "plus").Set("default", 1);
    tripled->Find("plus")->y = 100;
    tripled->Add("times", "Multiply").Wire("a", "x").Set("b", 3);
    tripled->Add("sum", "Add").Wire("a", "times").Wire("b", "plus");
    tripled->Add("result", "FunctionOutput").Set("name", "result").Wire("value", "sum");
    // One that calls the other: the entities a Filter keeps, counted, then tripled.
    auto counted = std::make_shared<kgraph::Graph>();
    counted->Add("of", "FunctionInput").Set("name", "of").Set("kind", "eEntities");
    counted->Add("lit", "Filter").Wire("entities", "of").Set("with", Value::List { "PointLight" });
    counted->Add("inner", "Function").Set("function", "tripled").Wire("x", "lit", "count");
    counted->Add("count", "FunctionOutput").Set("name", "count").Wire("value", "inner", "result");

    for (int i = 0; i < 2; ++i) support::Give(scene->world, scene->world.Create(), "PointLight", { { "range", { 3.f } } });
    kgraph::Graph graph;
    graph.functions = { { "tripled", tripled }, { "counted", counted } };
    graph.Add("two", "Double").Set("value", 2);
    graph.Add("a", "Function").Set("function", "tripled").Wire("x", "two");
    graph.Add("b", "Function").Set("function", "tripled").Set("x", 10).Set("plus", 0);
    graph.Add("both", "Add").Wire("a", "a", "result").Wire("b", "b", "result");
    graph.Add("all", "Entities");
    graph.Add("c", "Function").Set("function", "counted").Wire("of", "all");
    graph.Add("nobody", "Function").Set("function", "missing");
    graph.Add("self", "Function").Set("function", "itself");
    auto itself = std::make_shared<kgraph::Graph>();
    itself->Add("again", "Function").Set("function", "itself");
    graph.functions["itself"] = itself;
    runtime.Set(graph);
    Frame();

    EXPECT_EQ(runtime.ValueOf("a", "result"), Value(7)) << "2 * 3 + 1, its default";
    EXPECT_EQ(runtime.ValueOf("b", "result"), Value(30)) << "10 * 3 + 0, as it was told";
    EXPECT_EQ(runtime.ValueOf("both"), Value(37)) << "wired from like any node";
    EXPECT_EQ(runtime.ValueOf("a/times"), Value(6)) << "its nodes are there, by the call's name";
    EXPECT_TRUE(runtime.Status("a")->ok);
    EXPECT_EQ(runtime.ValueOf("c", "count"), Value(7)) << "two lights, tripled, plus one: a call in a call, of entities from outside";
    ASSERT_NE(runtime.Status("nobody"), nullptr);
    EXPECT_NE(runtime.Status("nobody")->error.find("no function 'missing'"), std::string::npos) << runtime.Status("nobody")->error;
    EXPECT_NE(runtime.Status("self")->error.find("calls itself"), std::string::npos) << runtime.Status("self")->error;

    // Its ports are its function's edges, top to bottom; its output is what the node inside gives.
    std::vector<std::string> ports;
    for (const auto& port : runtime.PortsOf("a")) ports.push_back(port.name);
    EXPECT_EQ(ports, (std::vector<std::string> { "function", "x", "plus" }));
    const auto outputs = runtime.OutputsOf("a");
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].name, "result");
    ASSERT_FALSE(runtime.OutputsOf("a/sum").empty());
    EXPECT_EQ(outputs[0].type, runtime.OutputsOf("a/sum").front().type) << "as the Add inside gives it";
    EXPECT_EQ(runtime.PortsOf("c")[1].type, "entities");

    // A node inside that fails says so on the call, and which.
    tripled->Find("times")->Set("b", "three");
    graph.functions["tripled"] = std::make_shared<kgraph::Graph>(*tripled);
    runtime.Set(graph);
    Frame();
    ASSERT_FALSE(runtime.Status("a")->ok);
    EXPECT_TRUE(runtime.Status("a")->error.starts_with("times: ")) << runtime.Status("a")->error;

    // A function opened in place: its edges in the graph itself stand for what they are given.
    kgraph::Graph open;
    open.Add("five", "Double").Set("value", 5);
    open.Add("edge", "FunctionInput").Set("name", "x").Wire("value", "five");
    open.Add("times", "Multiply").Wire("a", "edge").Set("b", 3);
    open.Add("out", "FunctionOutput").Set("name", "result").Wire("value", "times");
    open.Add("after", "Add").Wire("a", "out").Set("b", 1);
    runtime.Set(open);
    Frame();
    EXPECT_EQ(runtime.ValueOf("after"), Value(16));
    EXPECT_TRUE(runtime.Status("edge")->ok);
    EXPECT_EQ(runtime.ValueOf("edge"), Value(5));
}

// A buffer sized by a count of entities is made again when the count changes: what is wired from the world follows it,
// and a resource made of it is made of it again.
TEST_F(EntityTest, ABufferSizedByACountOfEntitiesIsMadeAgainWhenTheCountChanges) {
    auto& world = scene->world;
    for (int i = 0; i < 3; ++i) world.Add<Lit>(world.Create());
    kgraph::Graph graph;
    graph.Add("lit", "ForEach").Set("component", "Lit");
    graph.Add("bodies", "Buffer").Set("usage", kgraph::Value::List { "eStorage" }).Wire("instanceCount", "lit", "count");
    scene->runtime->Set(graph);
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
    Frame();
    EXPECT_EQ(scene->runtime->ValueOf("bodies", "instanceCount"), kgraph::Value(3));
    const auto builds = scene->runtime->Status("bodies")->builds;
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(scene->runtime->Status("bodies")->builds, builds) << "nothing changed: not made again";

    world.Add<Lit>(world.Create());
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(scene->runtime->ValueOf("bodies", "instanceCount"), kgraph::Value(4)) << "made again for four";
    EXPECT_GT(scene->runtime->Status("bodies")->builds, builds);
}

// The same entities drawn by meshlets: the pack cut into runs of a few dozen vertices, a task shader's group for each
// instance sending a mesh shader's group for each meshlet of its mesh. Every triangle is in one meshlet, and each is
// drawn where its entity is, in its colour.
TEST_F(EntityTest, EntitiesAreDrawnByMeshletsOfTheirPack) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube"), sphere = runtime.ResourceOf("$sphere");
    ASSERT_TRUE(cube && sphere);
    const std::array<std::pair<std::shared_ptr<kgraph::Resource>, std::vector<float>>, 4> made { {
        { cube, { 1, 0, 0 } }, { sphere, { 0, 1, 0 } }, { cube, { 0, 0, 1 } }, { sphere, { 1, 1, 1 } } } };
    for (std::size_t i = 0; i < made.size(); ++i) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { -0.75f + 0.5f * static_cast<float>(i), 0, 0 });
        world.Add<kgraph::Mesh>(entity, { made[i].first });
        support::Give(world, entity, "PointLight", { { "color", made[i].second } });
    }
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh", "PointLight" }).Set("groupBy", "Mesh");
    graph.Add("place", "Append").Wire("a", "meshes", "worldPosition").Set("b", 0.4);
    graph.Add("tint", "Append").Wire("a", "meshes", "PointLight.color").Wire("b", "meshes", "group");
    graph.Add("instances", "BufferBuilder")
        .Set("layout", Value::List { Value::Members { { "name", "place" }, { "type", "eVec4" } }, Value::Members { { "name", "color" }, { "type", "eVec4" } } })
        .Wire("field.place", "place").Wire("field.color", "tint");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh").Set("attributes", Value::List { "POSITION", "NORMAL", "UV" });
    graph.Add("cut", "Meshlets").Wire("mesh", "pack", "mesh").Wire("ranges", "pack", "ranges");
    graph.Add("buffers", "MeshBuffers").Wire("mesh", "pack", "mesh");
    graph.Add("drawer", "GraphicsPipeline").Set("stages", "eMesh").Set("task.module", "meshlets").Set("task.entry", "taskMain")
        .Set("mesh.module", "meshlets").Set("mesh.entry", "meshMain").Set("fragment.module", "meshlets").Set("fragment.entry", "fragmentMain")
        .Wire("framebuffer", "target").Wire("param.instances", "instances").Wire("param.meshlets", "cut", "meshlets")
        .Wire("param.meshletVertices", "cut", "vertices").Wire("param.meshletTriangles", "cut", "triangles").Wire("param.meshRanges", "cut", "meshRanges")
        .Wire("param.positions", "buffers", "vertices");
    graph.Add("draw", "Draw").Wire("pipeline", "drawer").Set("command", "eDrawMeshTasks").Wire("tasks.x", "meshes", "count").Set("tasks.y", 1).Set("tasks.z", 1)
        .Wire("push.stride", "buffers", "stride").Wire("push.offset", "buffers", "position");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    if (const auto* status = runtime.Status("drawer"); status && !status->ok && status->error.find("mesh shader") != std::string::npos) GTEST_SKIP() << status->error;
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    // Every triangle of both meshes is in one meshlet, none has more than it may, and its corners are its own.
    const auto read = [&](const char* output) { return kor::capi::BufferOf(runtime.ResourceOf("cut", output)->handle)->Read<std::uint32_t>(); };
    const auto meshlets = read("meshlets"), triangles = read("triangles"), ranges = read("meshRanges");
    const auto count = static_cast<std::size_t>(*runtime.ValueOf("cut", "count").Number());
    ASSERT_GE(meshlets.size(), count * 12);
    std::size_t covered = 0;
    for (std::size_t m = 0; m < count; ++m) {
        const std::uint32_t vertexCount = meshlets[m * 12 + 1], triangleOffset = meshlets[m * 12 + 2], triangleCount = meshlets[m * 12 + 3];
        EXPECT_LE(vertexCount, 64u);
        EXPECT_LE(triangleCount, 124u);
        for (std::uint32_t t = 0; t < triangleCount; ++t)
            for (int c = 0; c < 3; ++c) EXPECT_LT((triangles[triangleOffset + t] >> (8 * c)) & 255u, vertexCount);
        covered += triangleCount;
    }
    const Value packedRanges = runtime.ValueOf("pack", "ranges");
    std::size_t expected = 0;
    for (const auto& range : *packedRanges.Items()) expected += static_cast<std::size_t>(*range.Find("indexCount")->Number()) / 3;
    EXPECT_EQ(covered, expected) << "every triangle, once";
    EXPECT_EQ(ranges[0], 0u);
    EXPECT_EQ(ranges[4], ranges[1]) << "the second mesh's meshlets after the first's";
    EXPECT_EQ(ranges[1] + ranges[5], count);

    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(4, { 255, 0, 0 })) << "the red cube";
    EXPECT_TRUE(is(12, { 0, 255, 0 })) << "the green sphere";
    EXPECT_TRUE(is(20, { 0, 0, 255 })) << "the blue cube";
    EXPECT_TRUE(is(28, { 255, 255, 255 })) << "the white sphere";
    EXPECT_TRUE(is(8, { 0, 0, 0 })) << "and nothing between them";
}

// The same entities as a scene rays are traced through: a structure for each mesh of their pack, an instance of it
// where each entity is, InstanceID() its row. A ray a pixel sees each in its colour; one moved, the scene is made again
// and the rays see it where it went — read by its bindless index, so nothing is built again for it.
TEST_F(EntityTest, EntitiesAreASceneRaysAreTracedThrough) {
    using kgraph::Value;
    if (!kor::Context::SupportsRayTracing()) GTEST_SKIP() << "no ray tracing here";
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube"), sphere = runtime.ResourceOf("$sphere");
    ASSERT_TRUE(cube && sphere);
    const std::array<std::pair<std::shared_ptr<kgraph::Resource>, std::vector<float>>, 4> made { {
        { cube, { 1, 0, 0 } }, { sphere, { 0, 1, 0 } }, { cube, { 0, 0, 1 } }, { sphere, { 1, 1, 1 } } } };
    std::vector<kecs::Entity> entities;
    for (std::size_t i = 0; i < made.size(); ++i) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { -0.75f + 0.5f * static_cast<float>(i), 0, 0 });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { made[i].first });
        support::Give(world, entity, "PointLight", { { "color", made[i].second } });
        entities.push_back(entity);
    }
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eStorage", "eTransferSrc" });
    graph.Add("all", "Entities");
    graph.Add("meshes", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh", "PointLight" }).Set("groupBy", "Mesh");
    graph.Add("tint", "Append").Wire("a", "meshes", "PointLight.color").Wire("b", "meshes", "group");
    graph.Add("instances", "BufferBuilder")
        .Set("layout", Value::List { Value::Members { { "name", "place" }, { "type", "eVec4" } }, Value::Members { { "name", "color" }, { "type", "eVec4" } } })
        .Wire("field.color", "tint");
    graph.Add("pack", "MeshPack").Wire("meshes", "meshes", "Mesh.mesh").Set("attributes", Value::List { "POSITION", "NORMAL", "UV" });
    graph.Add("rays", "RayScene").Wire("meshes", "meshes", "Mesh.mesh").Wire("mesh", "pack", "mesh").Wire("ranges", "pack", "ranges");
    graph.Add("tracer", "RayTracingPipeline").Set("raygen.module", "rayScene").Set("raygen.entry", "raygenMain")
        .Set("miss[0].module", "rayScene").Set("miss[0].entry", "missMain")
        .Set("hitGroup[0].closestHit.module", "rayScene").Set("hitGroup[0].closestHit.entry", "hitMain")
        .Wire("param.instances", "instances").Wire("param.scene", "rays", "scene").Wire("param.target", "canvas");
    graph.Add("trace", "TraceRays").Wire("pipeline", "tracer").Wire("size", "canvas", "extent");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    EXPECT_EQ(runtime.ValueOf("rays", "count"), Value(4));

    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(4, { 255, 0, 0 })) << "the red cube";
    EXPECT_TRUE(is(12, { 0, 255, 0 })) << "the green sphere";
    EXPECT_TRUE(is(20, { 0, 0, 255 })) << "the blue cube";
    EXPECT_TRUE(is(28, { 255, 255, 255 })) << "the white sphere";
    EXPECT_TRUE(is(8, { 0, 0, 0 })) << "and nothing between them";

    // The red cube moved down, out of the row looked at: the rays find it gone, and the pipeline was not made again.
    const std::uint64_t built = runtime.Status("tracer")->builds;
    world.SetPosition(entities[0], { -0.75f, 0.8f, 0 });
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_TRUE(is(4, { 0, 0, 0 })) << "where it was, nothing";
    const auto p = scene->Pixel(4, 29);
    EXPECT_TRUE(p.x > 250 && p.y < 4 && p.z < 4) << "where it went: " << int(p.x) << " " << int(p.y) << " " << int(p.z);
    EXPECT_EQ(runtime.Status("tracer")->builds, built) << "read by its index, nothing of it is built again";

    // Every one of them gone: the rays find nothing, not the scene as it was.
    for (const kecs::Entity entity : entities) world.Destroy(entity);
    for (int i = 0; i < 4; ++i) Frame();
    for (const kor::u32 x : { 12u, 20u, 28u }) EXPECT_TRUE(is(x, { 0, 0, 0 })) << "nothing at " << x;
    EXPECT_EQ(runtime.ValueOf("rays", "count"), Value(0));
}

// A ray tracing pipeline has a material too: the buffer its shader marks [entity("PER_INSTANCE")] is filled by the
// TraceRays, an element for each entity of the Filter wired to its `entities` — the same rows a RayScene of that Filter
// numbers its instances by — from the pipeline's component, which the entities hold as a Draw's hold theirs.
TEST_F(EntityTest, RaysAreShadedByTheirPipelinesOwnMaterial) {
    using kgraph::Value;
    if (!kor::Context::SupportsRayTracing()) GTEST_SKIP() << "no ray tracing here";
    kecs::World& world = scene->world;
    kgraph::Runtime& runtime = *scene->runtime;
    const auto cube = runtime.ResourceOf("$cube"), sphere = runtime.ResourceOf("$sphere");
    ASSERT_TRUE(cube && sphere);
    const std::array<std::shared_ptr<kgraph::Resource>, 4> meshes { cube, sphere, cube, sphere };
    std::vector<kecs::Entity> entities;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const kecs::Entity entity = world.Create();
        world.SetPosition(entity, { -0.75f + 0.5f * static_cast<float>(i), 0, 0 });
        world.SetScale(entity, { 0.4f, 0.4f, 0.4f });
        world.Add<kgraph::Mesh>(entity, { meshes[i] });
        entities.push_back(entity);
    }
    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eStorage", "eTransferSrc" });
    graph.Add("all", "Entities");
    graph.Add("solids", "Filter").Wire("entities", "all").Set("with", Value::List { "Mesh" }).Set("groupBy", "Mesh");
    graph.Add("pack", "MeshPack").Wire("meshes", "solids", "Mesh.mesh").Set("attributes", Value::List { "POSITION", "NORMAL", "UV" });
    graph.Add("rays", "RayScene").Wire("meshes", "solids", "Mesh.mesh").Wire("mesh", "pack", "mesh").Wire("ranges", "pack", "ranges");
    graph.Add("tracer", "RayTracingPipeline").Set("raygen.module", "rayMaterial").Set("raygen.entry", "raygenMain")
        .Set("miss[0].module", "rayMaterial").Set("miss[0].entry", "missMain")
        .Set("hitGroup[0].closestHit.module", "rayMaterial").Set("hitGroup[0].closestHit.entry", "hitMain")
        .Wire("param.scene", "rays", "scene").Wire("param.target", "canvas");
    graph.Add("trace", "TraceRays").Wire("pipeline", "tracer").Wire("size", "canvas", "extent").Wire("entities", "solids");
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    Frame();
    const kecs::ComponentId material = runtime.ComponentOf("tracer");
    ASSERT_NE(material, kecs::NoComponent) << "the pipeline has a component: its material";
    EXPECT_TRUE(world.Describe(material).FindField("tint"));
    EXPECT_FALSE(world.Describe(material).FindField("id")) << "which entity it is is the world's, not the material's";
    const std::array<kor::Vec4, 4> tints { kor::Vec4 { 1, 0, 0, 1 }, kor::Vec4 { 0, 1, 0, 1 }, kor::Vec4 { 0, 0, 1, 1 }, kor::Vec4 { 1, 1, 1, 1 } };
    for (std::size_t i = 0; i < entities.size(); ++i) {
        world.Add(entities[i], material);
        kgraph::Parameters(world, entities[i], material).Set("tint", tints[i]);
    }
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    const auto is = [&](const kor::u32 x, const std::array<int, 3> rgb) {
        const auto p = scene->Pixel(x, 16);
        return std::abs(p.x - rgb[0]) < 4 && std::abs(p.y - rgb[1]) < 4 && std::abs(p.z - rgb[2]) < 4;
    };
    EXPECT_TRUE(is(4, { 255, 0, 0 })) << "the red cube";
    EXPECT_TRUE(is(12, { 0, 255, 0 })) << "the green sphere";
    EXPECT_TRUE(is(20, { 0, 0, 255 })) << "the blue cube";
    EXPECT_TRUE(is(28, { 255, 255, 255 })) << "the white sphere";
    EXPECT_NE(runtime.ResourceOf("trace", "instances"), nullptr) << "its buffer of materials, for a pass to read as a Draw's is";

    // Its material changed: the rays see it so the next frame, and nothing is built again.
    const std::uint64_t built = runtime.Status("tracer")->builds;
    kgraph::Parameters(world, entities[0], material).Set("tint", kor::Vec4 { 1, 1, 0, 1 });
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(is(4, { 255, 255, 0 })) << "the cube made yellow";
    EXPECT_EQ(runtime.Status("tracer")->builds, built);
}

// Where an entity was the frame before is an output of each: what a shader works out motion from. The box round some of
// them is a SceneBounds'; a WriteComponent writes a value into one, and only when it is not what it holds.
TEST_F(EntityTest, WhereEachWasTheBoxRoundThemAndAValueWrittenIntoOne) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    const kecs::Entity lamp = world.Create(), other = world.Create();
    world.Rename(lamp, "Lamp");
    support::Give(world, lamp, "PointLight", { { "range", { 3.f } } });
    support::Give(world, other, "PointLight", { { "range", { 3.f } } });
    world.SetPosition(lamp, { 1, 0, 0 });
    world.SetPosition(other, { -1, 2, 0 });
    kgraph::Graph graph;
    graph.Add("lamp", "Entity").Set("entity", "Lamp");
    graph.Add("all", "Entities");
    graph.Add("lamps", "Filter").Wire("entities", "all").Set("with", Value::List { "PointLight" });
    graph.Add("box", "SceneBounds").Wire("entities", "lamps");
    graph.Add("reach", "Multiply").Set("a", 4).Set("b", 2);
    graph.Add("write", "WriteComponent").Set("component", "PointLight").Set("field", "range").Set("entity", "Lamp").Wire("value", "reach");
    graph.Add("shapes", "DebugShapes").Set("kind", "eBox").Wire("min", "box", "min").Wire("max", "box", "max");
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    Frame();
    EXPECT_EQ(runtime.ValueOf("box", "min"), Value(Value::List { -1, 0, 0 }));
    EXPECT_EQ(runtime.ValueOf("box", "max"), Value(Value::List { 1, 2, 0 }));
    EXPECT_EQ(runtime.ValueOf("box", "count"), Value(2));
    EXPECT_EQ(runtime.ValueOf("write", "written"), Value(true));
    const kecs::ComponentId lights = world.Find("PointLight");
    const kecs::FieldDescription* range = world.Describe(lights).FindField("range");
    float written = 0.f;
    std::memcpy(&written, static_cast<const std::byte*>(world.Get(lamp, lights)) + range->offset, 4);
    EXPECT_EQ(written, 8.f) << "4 times 2, into the lamp's range";
    const auto changes = world.ChangesOf(lights);
    Frame();
    EXPECT_EQ(world.ChangesOf(lights), changes) << "what it holds already is not written again";

    // Moved: where it was the frame before, and where it is now.
    world.SetPosition(lamp, { 5, 0, 0 });
    Frame();
    const auto at = [&](const char* output) { const Value m = runtime.ValueOf("lamp", output); return *(*m.Items())[12].Number(); };
    EXPECT_EQ(at("matrix"), 5.0);
    EXPECT_EQ(at("previousMatrix"), 1.0) << "where it was the frame before";
    Frame();
    EXPECT_EQ(at("previousMatrix"), 5.0) << "it moved no more";
}

namespace {
    /** One of the engine's functions, as its file has it: packages/koral/functions/<name>.kfunc. */
    std::shared_ptr<const kgraph::Graph> PackageFunction(const std::string& name)
    {
        const std::filesystem::path file = std::filesystem::path(KGRAPH_PACKAGE_SHADERS).parent_path() / "functions" / (name + ".kfunc");
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        kecs::json::Value parsed;
        std::string error;
        auto graph = std::make_shared<kgraph::Graph>();
        if (!kecs::json::Parse(text, parsed, error) || !parsed.Find("graph") || !kgraph::Graph::FromJson(parsed.Find("graph")->raw, *graph, error))
            ADD_FAILURE() << name << ": " << error;
        return graph;
    }
}

// The engine's Lights: a buffer of each kind as koral_lights.slang has them — KoralDirectionalLight, KoralPointLight,
// KoralSpotLight — each colour in linear terms times its intensity, and how many of each.
TEST_F(EntityTest, TheEnginesLightsAreABufferOfEachKind) {
    using kgraph::Value;
    kecs::World& world = scene->world;
    const kecs::Entity sun = world.Create(), lamp = world.Create(), other = world.Create(), spot = world.Create();
    support::Give(world, sun, "DirectionalLight", { { "color", { 1, 1, 1 } }, { "intensity", { 3.f } } });
    support::Give(world, lamp, "PointLight", { { "color", { 1, 0, 0 } }, { "intensity", { 2.f } }, { "range", { 5.f } } });
    support::Give(world, other, "PointLight", { { "color", { 0, 1, 0 } }, { "intensity", { 1.f } }, { "range", { 6.f } } });
    support::Give(world, spot, "SpotLight", { { "color", { 1, 1, 1 } }, { "intensity", { 1.f } }, { "range", { 9.f } }, { "innerAngle", { 0.f } }, { "outerAngle", { 60.f } } });
    world.SetPosition(lamp, { 1, 2, 3 });
    kgraph::Graph graph;
    graph.functions["Lights"] = PackageFunction("Lights");
    graph.Add("lights", "Function").Set("function", "Lights");
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    Frame();
    EXPECT_EQ(runtime.ValueOf("lights", "directionalCount"), Value(1));
    EXPECT_EQ(runtime.ValueOf("lights", "pointCount"), Value(2));
    EXPECT_EQ(runtime.ValueOf("lights", "spotCount"), Value(1));
    EXPECT_EQ(runtime.ValueOf("lights", "counts"), Value(Value::List { 1, 2, 1, 0 })) << "as a draw's push constants take them";
    const auto floatsOf = [&](const char* output) {
        const auto held = runtime.ResourceOf("lights", output);
        return held ? kor::capi::BufferOf(held->handle)->Read<float>() : std::vector<float> {};
    };
    const auto suns = floatsOf("directional"), lamps = floatsOf("points"), spots = floatsOf("spots");
    ASSERT_GE(suns.size(), 8u) << "eight numbers: a KoralDirectionalLight is 32 bytes";
    EXPECT_EQ(suns[4], 3.f) << "white times three";
    ASSERT_GE(lamps.size(), 16u) << "two of eight numbers: a KoralPointLight is 32 bytes";
    EXPECT_EQ(lamps[0], 1.f) << "where the first lamp is";
    EXPECT_EQ(lamps[2], 3.f);
    EXPECT_EQ(lamps[3], 5.f) << "and how far it reaches";
    EXPECT_EQ(lamps[4], 2.f) << "red, twice";
    EXPECT_EQ(lamps[8 + 3], 6.f) << "the second, as far as it reaches";
    ASSERT_GE(spots.size(), 12u) << "twelve numbers: a KoralSpotLight is 48 bytes";
    EXPECT_EQ(spots[3], 9.f);
    EXPECT_NEAR(spots[7], 1.f, 1e-5) << "the cosine of none";
    EXPECT_NEAR(spots[11], 0.5f, 1e-5) << "of sixty degrees";
}
