// A buffer a shader declares, filled from the world: a ShaderBuffer node has an input for each field of the shader's
// struct, and an Entities node gives each field of a component for every entity that has it. Wired together they
// are an element for each entity, where the shader reads it. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

#include <capiInterop.h>

#include <kgraph/scene.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using kgraph::Value;

    struct Glow { float strength = 3.f; };

    class BufferTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "buffers", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { support::App()->Close(*scene); Frame(); }
        }
        kecs::Entity Lamp(const kor::Vec3 color, const float intensity, const float range, const kor::Vec3 at = {}) {
            const kecs::Entity lamp = scene->world.Create();
            support::Give(scene->world, lamp, "PointLight", { { "color", { color.x, color.y, color.z } }, { "intensity", { intensity } }, { "range", { range } } });
            scene->world.SetPosition(lamp, { at.x, at.y, at.z });
            return lamp;
        }
        /** The lights of lights.slang: the point lights, then the spots, in one buffer; and the view they are counted in. */
        static kgraph::Graph Lights() {
            kgraph::Graph graph;
            graph.Add("lamps", "ForEach").Set("component", "PointLight");
            graph.Add("spots", "ForEach").Set("component", "SpotLight");
            graph.Add("tinted", "Multiply").Wire("a", "lamps", "field.color").Wire("b", "lamps", "field.intensity");
            const auto buffer = [&](const char* id, const char* parameter) -> kgraph::Node& {
                return graph.Add(id, "ShaderBuffer").Set("module", "lights").Set("entry", "computeMain").Set("parameter", parameter);
            };
            buffer("lamp data", "lights").Wire("field.position", "lamps", "worldPosition").Wire("field.range", "lamps", "field.range")
                .Wire("field.color", "tinted").Set("field.kind", 1);
            buffer("light data", "lights").Wire("after", "lamp data").Wire("field.color", "spots", "field.color").Set("field.kind", 2)
                .Set("field.cone", Value::List { 0.5, 0.25 });
            graph.Add("eye", "Entity").Set("component", "Camera");
            graph.Add("camera", "View").Wire("position", "eye", "worldPosition").Set("main", true);
            graph.Add("lens", "Perspective").Set("main", true).Wire("fieldOfView", "eye", "field.fieldOfView");
            graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "camera");
            buffer("view data", "view").Wire("field.viewProjection", "seen").Wire("field.eye", "camera", "position")
                .Wire("field.lightCount", "light data", "count");
            graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 32).Set("extent.y", 32)
                .Set("usage", Value::List { "eStorage", "eTransferSrc" });
            graph.Add("sum", "ComputePipeline").Set("compute.module", "lights").Set("compute.entry", "computeMain")
                .Wire("param.target", "target").Wire("param.lights", "light data").Wire("param.view", "view data");
            graph.Add("run", "Dispatch").Wire("pipeline", "sum").Set("groups.x", 4).Set("groups.y", 4);
            graph.Add("screen", "Screen");
            graph.Add("show", "Blit").Wire("source", "target").Wire("destination", "screen");
            return graph;
        }
        [[nodiscard]] std::vector<float> Floats(const char* node) const { return kor::capi::BufferOf(scene->runtime->ResourceOf(node)->handle)->Read<float>(); }
        [[nodiscard]] static bool Has(const std::vector<kgraph::Port>& ports, const std::string_view name) { return std::ranges::find(ports, name, &kgraph::Port::name) != ports.end(); }

        GraphScene* scene = nullptr;
    };
}

template <> struct kecs::Name<Glow> { static constexpr std::string_view value = "Glow"; };
KECS_FIELDS(Glow, strength)

// A Buffer given no `type` is kept where it is best kept: on the device while only passes reach it; once the CPU is handed
// it — a script writes it every frame — made again where the CPU writes straight into it. One given a type keeps it.
TEST_F(BufferTest, ABufferGivenNoTypeIsKeptWhereWhatReachesItIsFastest) {
    using kgraph::Value;
    kgraph::Runtime& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("passes alone", "Buffer").Set("instanceCount", 64).Set("usage", Value::List { "eStorage", "eTransferDst", "eTransferSrc" });
    graph.Add("written by a script", "Buffer").Set("instanceCount", 64).Set("usage", Value::List { "eStorage", "eTransferDst", "eTransferSrc" });
    graph.Add("said", "Buffer").Set("instanceCount", 64).Set("usage", Value::List { "eStorage", "eTransferDst", "eTransferSrc" }).Set("type", "eDynamic");
    runtime.Set(graph);
    Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    const auto type = [&](const char* node) { return kor::capi::BufferOf(runtime.ResourceOf(node)->handle)->MemoryType(); };
    EXPECT_EQ(type("passes alone"), kor::Buffer::Type::eDeviceLocal) << "what only passes reach: on the device";
    EXPECT_EQ(type("written by a script"), kor::Buffer::Type::eDeviceLocal);
    EXPECT_EQ(type("said"), kor::Buffer::Type::eDynamic) << "a type given is the one it is made with";

    // Handed to the CPU (what kengine_node_resource does for a script): made again where the CPU writes into it.
    runtime.ReachedFromTheCpu("written by a script");
    for (int i = 0; i < 2; ++i) Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(type("written by a script"), kor::Buffer::Type::eDynamic);
    EXPECT_EQ(type("passes alone"), kor::Buffer::Type::eDeviceLocal) << "and nothing else";
    // Written by the CPU, and read back as written.
    const std::vector<float> numbers(16, 2.5f);
    const_cast<kor::Buffer&>(*kor::capi::BufferOf(runtime.ResourceOf("written by a script")->handle)).Write(numbers);
    EXPECT_EQ(Floats("written by a script"), numbers);
    // Built again for another reason, it stays where the CPU reaches it.
    graph.Find("written by a script")->Set("instanceCount", 128);
    runtime.Set(graph);
    Frame();
    EXPECT_EQ(type("written by a script"), kor::Buffer::Type::eDynamic);
}

// The struct is the shader's: its fields are the node's inputs, and each entity's values go where the shader has them.
TEST_F(BufferTest, EachEntityIsAnElementLaidOutAsTheShaderHasIt) {
    kecs::World& world = scene->world;
    const kecs::Entity red = Lamp({ 1, 0, 0 }, 0.5f, 1.f, { 0, 4, 0 });
    Lamp({ 0, 1, 0 }, 0.25f, 2.f, { 7, 0, 0 });
    Lamp({ 0, 0, 1 }, 1.f, 0.25f);
    const kecs::Entity spot = world.Create();
    support::Give(world, spot, "SpotLight", { { "color", { 1, 1, 1 } } });

    auto& runtime = *scene->runtime;
    runtime.Set(Lights());
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());

    // What the two nodes have, they have from the shader and from the component.
    const auto fields = runtime.PortsOf("lamp data");
    for (const char* name : { "field.position", "field.range", "field.color", "field.kind", "field.cone" }) EXPECT_TRUE(Has(fields, name)) << name;
    EXPECT_TRUE(Has(runtime.PortsOf("view data"), "field.viewProjection"));
    const auto gives = runtime.OutputsOf("lamps");
    for (const char* name : { "field.color", "field.intensity", "field.range", "worldPosition", "count" }) EXPECT_TRUE(Has(gives, name)) << name;
    EXPECT_FALSE(Has(runtime.OutputsOf("spots"), "field.nothing"));
    EXPECT_TRUE(Has(runtime.OutputsOf("spots"), "field.outerAngle"));

    EXPECT_EQ(runtime.ValueOf("lamps", "count"), Value(3));
    EXPECT_EQ(runtime.ValueOf("lamp data", "count"), Value(3));
    EXPECT_EQ(runtime.ValueOf("light data", "count"), Value(4)) << "the lamps, and the spot after them";
    EXPECT_EQ(runtime.ValueOf("lamp data", "stride"), Value(48)) << "a float3 and a float, a float4, a uint, and a float2 on a boundary of eight";
    EXPECT_EQ(runtime.ValueOf("view data", "count"), Value(1));
    EXPECT_EQ(runtime.ValueOf("tinted"), Value(Value::List { 0.5, 0, 0 })) << "wired anywhere else, an entity's values are the first one's";

    Frame();
    Frame();
    {
        const auto lights = Floats("light data");
        ASSERT_GE(lights.size(), 4u * 12u);
        EXPECT_EQ(lights[1], 4.f) << "the first lamp's height";
        EXPECT_EQ(lights[3], 1.f) << "its range, in the float3's fourth place";
        EXPECT_EQ(lights[4], 0.5f) << "its colour, times its intensity";
        EXPECT_EQ(lights[12], 7.f) << "the second lamp, an element on";
        EXPECT_EQ(lights[12 + 5], 0.25f);
        kor::u32 kind = 0;
        std::memcpy(&kind, &lights[8], 4);
        EXPECT_EQ(kind, 1u) << "a whole number, as the shader reads it";
        std::memcpy(&kind, &lights[3 * 12 + 8], 4);
        EXPECT_EQ(kind, 2u) << "the spot, after the three lamps";
        EXPECT_EQ(lights[3 * 12 + 10], 0.5f) << "its cone";
        const auto view = Floats("view data");
        ASSERT_GE(view.size(), 20u);
        kor::u32 count = 0;
        std::memcpy(&count, &view[19], 4);
        EXPECT_EQ(count, 4u);
    }
    // And the shader reads them there: each point light's colour times its range, added up.
    const auto pixel = scene->Pixel(16, 16);
    EXPECT_NEAR(pixel.x, 128, 1);
    EXPECT_NEAR(pixel.y, 128, 1);
    EXPECT_NEAR(pixel.z, 64, 1);

    // The world changes and the buffer follows, with nothing built again while there is room.
    const auto builds = [&](const char* node) { return runtime.Status(node)->builds; };
    const std::uint64_t before[] { builds("lamps"), builds("lamp data"), builds("light data"), builds("sum") };
    world.Destroy(red);
    support::Give(world, spot, "SpotLight", { { "color", { 0.25f, 0.5f, 1.f } } });
    Frame();
    Frame();
    EXPECT_EQ(runtime.ValueOf("light data", "count"), Value(3));
    EXPECT_EQ(Floats("light data")[2 * 12 + 5], 0.5f) << "the spot is the third now, with its new colour";
    EXPECT_NEAR(scene->Pixel(16, 16).x, 0, 1) << "the red lamp is gone";
    EXPECT_NEAR(scene->Pixel(16, 16).y, 128, 1);
    const std::uint64_t after[] { builds("lamps"), builds("lamp data"), builds("light data"), builds("sum") };
    for (int i = 0; i < 4; ++i) EXPECT_EQ(after[i], before[i]) << "an element changed where it was: nothing was built again";

    // More entities than there is room for: the buffer is made again, larger, and so is what reads it.
    for (int i = 0; i < 6; ++i) Lamp({ 0, 0, 0.125f }, 1.f, 1.f);
    Frame();
    Frame();
    Frame();
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(runtime.ValueOf("light data", "count"), Value(9));
    EXPECT_GT(builds("light data"), after[2]);
    EXPECT_GE(Floats("light data").size(), 9u * 12u);
    EXPECT_NEAR(scene->Pixel(16, 16).z, 64 + 6 * 32, 2) << "the blue lamp, and six more of an eighth each";
}

// What a shader does not declare, and what an element cannot be, is said on the node.
TEST_F(BufferTest, WhatTheShaderDoesNotHaveIsSaid) {
    Lamp({ 1, 1, 1 }, 1.f, 1.f);
    auto& runtime = *scene->runtime;
    kgraph::Graph graph = Lights();
    graph.Find("lamp data")->Set("field.brightness", 2);
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("lamp data")->error.find("Light has no field 'brightness'. It has: position, range, color, kind, cone"), std::string::npos) << runtime.Status("lamp data")->error;
    EXPECT_TRUE(Has(runtime.PortsOf("lamp data"), "field.color")) << "its fields are still there to be given";
    graph = Lights();
    graph.Find("view data")->Set("parameter", "camera");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("view data")->error.find("uses no buffer 'camera'"), std::string::npos) << runtime.Status("view data")->error;
    EXPECT_NE(runtime.Status("view data")->error.find("lights"), std::string::npos) << "those it does use are listed";

    // An element is one entity's: fields of two lists of them are no element.
    graph = Lights();
    graph.Find("lamp data")->Wire("field.cone", "spots", "field.innerAngle");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("lamp data")->error.find("more than one ForEach"), std::string::npos) << runtime.Status("lamp data")->error;

    // A component there is none of yet has nobody; made, its fields are there.
    graph = Lights();
    graph.Find("lamps")->Set("component", "Glow");
    graph.Find("tinted")->inputs.clear();
    graph.Find("tinted")->Set("a", 1).Set("b", 1);
    graph.Find("lamp data")->inputs.erase("field.range");
    runtime.Set(graph);
    EXPECT_TRUE(runtime.Status("lamps")->ok);
    EXPECT_EQ(runtime.ValueOf("lamps", "count"), Value(0));
    scene->world.Add<Glow>(scene->world.Create(), {});
    Frame();
    Frame();
    EXPECT_EQ(runtime.ValueOf("lamps", "count"), Value(1));
    EXPECT_TRUE(Has(runtime.OutputsOf("lamps"), "field.strength"));
    EXPECT_EQ(runtime.ValueOf("lamps", "field.strength"), Value(3));
}

// A Bundle fills a buffer's fields by name: a ForEach's element is each entity's fields and where it is, a Bundle node
// what it is given. A field wired by itself takes its own wire, and what matches nothing is said on the node.
TEST_F(BufferTest, ABundleFillsTheFieldsOfItsNames) {
    Lamp({ 1, 0, 0 }, 0.5f, 1.f, { 0, 4, 0 });
    Lamp({ 0, 1, 0 }, 0.25f, 2.f, { 7, 0, 0 });
    Lamp({ 0, 0, 1 }, 1.f, 0.25f);
    support::Give(scene->world, scene->world.Create(), "SpotLight", { { "color", { 1, 1, 1 } } });

    kgraph::Graph graph = Lights();
    kgraph::Node& lamps = *graph.Find("lamp data");
    lamps.inputs.erase("field.position");
    lamps.inputs.erase("field.range");
    lamps.Wire("fields", "lamps", "element");     // its position and its range, by name; its colour is wired by itself
    // The view's block, of a Bundle node made of the camera's matrix and where it is.
    graph.Add("camera bundle", "Bundle").Set("names", Value::List { "viewProjection", "eye" }).Wire("field.viewProjection", "seen").Wire("field.eye", "camera", "position");
    kgraph::Node& view = *graph.Find("view data");
    view.inputs.erase("field.viewProjection");
    view.inputs.erase("field.eye");
    view.Wire("fields", "camera bundle");
    graph.Add("parts", "Unbundle").Wire("of", "camera bundle");
    // A layout with a name the element has not.
    graph.Add("misspelled", "BufferBuilder").Set("layout", Value::List { Value::Members { { "name", "rnage" }, { "type", "eFloat" } },
                                                                         Value::Members { { "name", "color" }, { "type", "eVec3" } } })
        .Wire("fields", "lamps", "element");

    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(runtime.Ok());
    Frame();
    Frame();
    const auto lights = Floats("light data");
    ASSERT_GE(lights.size(), 4u * 12u);
    EXPECT_EQ(lights[1], 4.f) << "the first lamp's place, from its element";
    EXPECT_EQ(lights[3], 1.f) << "its range, from its element";
    EXPECT_EQ(lights[4], 0.5f) << "its colour wired by itself, times its intensity: not the element's own";
    EXPECT_EQ(lights[12], 7.f);
    const auto pixel = scene->Pixel(16, 16);
    EXPECT_NEAR(pixel.x, 128, 1) << "drawn as the same buffers wired a field at a time draw";
    EXPECT_NEAR(pixel.z, 64, 1);

    EXPECT_NE(runtime.Status("lamp data")->note.find("'intensity'"), std::string::npos) << "what the struct has no field for: " << runtime.Status("lamp data")->note;
    EXPECT_NE(runtime.Status("misspelled")->note.find("'rnage'"), std::string::npos) << runtime.Status("misspelled")->note;
    EXPECT_EQ(runtime.BundleOf("parts", "of"), (std::vector<std::string> { "viewProjection", "eye" }));
    EXPECT_TRUE(Has(runtime.OutputsOf("parts"), "eye"));
    EXPECT_TRUE(Has(runtime.PortsOf("camera bundle"), "field.viewProjection")) << "an input for each of its names";

    // Wired from what is no Bundle, it says so.
    graph.Find("view data")->Wire("fields", "seen");
    runtime.Set(graph);
    ASSERT_FALSE(runtime.Status("view data")->ok);
    EXPECT_NE(runtime.Status("view data")->error.find("takes a Bundle"), std::string::npos) << runtime.Status("view data")->error;
}
