// Pipelines run for each entity: the component generated for what a node leaves unwired, a descriptor set an
// entity, passes ordered by the resources entities hold, tags and switched-off components, and a shader edited
// under all of it. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include <image.h>
#include <mesh.h>
#include <vertexLayout.h>

#include <kecs/serialize.h>
#include <kgraph/entities.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using kgraph::Parameters;

    struct Lit {};
    struct Hidden {};

    // Many entities, each with an image of its own. One compute node fills each entity's image with the entity's
    // colour, over a backdrop the node wires for all of them; one graphics node draws each entity's image where the
    // entity says. The passes are listed last first.
    std::string SceneGraph(const std::string& stampModule = "stamp")
    {
        return std::format(R"json({{
      "kgraph": 1,
      "nodes": [
        {{ "id": "blit", "type": "Blit", "inputs": {{ "source": {{ "$link": ["canvas", "out"] }}, "destination": {{ "$link": ["screen", "out"] }} }} }},
        {{ "id": "draw", "type": "Draw", "inputs": {{ "pipeline": {{ "$link": ["show", "out"] }}, "without": ["Hidden"] }} }},
        {{ "id": "run", "type": "Dispatch", "inputs": {{
            "pipeline": {{ "$link": ["stamp", "out"] }}, "cover": "target", "groupSize.x": 8, "groupSize.y": 8, "with": ["Lit"] }} }},
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
            struct Vertex { kor::Vec4 color; kor::Vec3 position; };
            const std::array<Vertex, 4> corners {};
            return kgraph::MakeResource(kor::Mesh::Builder()
                .SetVertexBuffer(0, kor::Mesh::MakeBuffer(corners, kor::Buffer::Usage::eVertex))
                .SetIndexBuffer(kor::Mesh::MakeBuffer(indices, kor::Buffer::Usage::eIndex))
                .SetVertexLayout(kor::VertexLayout {
                    .bindings = { kor::VertexInputBindingDescription(0, sizeof(Vertex)) },
                    .attributes = { kor::VertexLayout::Attribute("POSITION", "", 0, offsetof(Vertex, position), kor::ChannelType::eFloat, 3) } })
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
    // One with a mesh the pipeline's layout cannot read, one with no image to draw, one with no mesh at all.
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

    // A tag nothing has, because nothing has made it yet: nobody, and it says so.
    kgraph::Graph graph = scene->runtime->Current();
    graph.Find("run")->Set("with", kgraph::Value::List { "NeverHeardOf" });
    scene->runtime->Set(graph);
    Frame();
    EXPECT_EQ(scene->runtime->ReportOf("run")->ran, 0u);
    EXPECT_NE(scene->runtime->ReportOf("run")->why.find("NeverHeardOf"), std::string::npos);

    // And mistakes in the node, which are the node's.
    graph.Find("run")->Set("with", "Lit");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("run")->error.find("list"), std::string::npos) << scene->runtime->Status("run")->error;
    graph.Find("run")->Set("with", kgraph::Value::List { "Lit" }).Set("cover", "backdrops");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("run")->error.find("'backdrops' is not one"), std::string::npos) << scene->runtime->Status("run")->error;
    graph.Find("draw")->Set("command", "eDrawSideways");
    scene->runtime->Set(graph);
    EXPECT_NE(scene->runtime->Status("draw")->error.find("eDrawIndexed"), std::string::npos) << scene->runtime->Status("draw")->error;
}

TEST_F(EntityTest, WiringAParameterTakesItOffEveryEntity) {
    Load(SceneGraph());
    MakeFour();
    Frame();
    Frame();

    // The node now gives every entity the same image to draw: the field goes, and what the entities keep, they keep.
    kgraph::Graph graph = scene->runtime->Current();
    graph.Add("shared", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 4).Set("extent.y", 4).Set("extent.z", 1)
        .Set("usage", kgraph::Value::List { "eSampled", "eTransferDst" });
    graph.Add("white", "Clear").Wire("image", "shared").Set("color", kgraph::Value::List { 1, 1, 1, 1 });
    graph.Find("show")->Wire("param.source", "shared");
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
    for (int quarter = 0; quarter < 4; ++quarter) ExpectColor(Quarter(quarter), 255, 255, 255, "every entity, where it was, with the image the node wires");
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
    for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;

    // On the node: the wire to a parameter the shader no longer has is taken off, and said to have been.
    EXPECT_EQ(scene->runtime->TakeReset(), std::vector<std::string> { "stamp.param.backdrop" });
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

    // Nothing runs until they are: each entity is passed by, and why is said.
    EXPECT_EQ(scene->runtime->ReportOf("run")->ran, 0u);
    EXPECT_NE(scene->runtime->ReportOf("run")->why.find("'base' has nothing in it"), std::string::npos) << scene->runtime->ReportOf("run")->why;
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
