// A buffer laid out on its node: a BufferBuilder has the fields it is given, packed as a shader's struct of the same
// fields is, and a ForEach is every entity of a component an element at a time — as a struct, and as its fields. A
// light's element worked out into what it sees (a View) is a buffer of cameras the same way. Offscreen, with no
// windowing system.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <capiInterop.h>

#include <kgraph/scene.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using kgraph::Value;

    Value Field(const char* name, const char* type) { return Value::Members { { "name", name }, { "type", type } }; }

    class BuilderTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "builder", .extent = { support::Size, support::Size } });
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
        kecs::Entity Spot(const kor::Vec3 at, const float range, const float outer) {
            const kecs::Entity spot = scene->world.Create();
            support::Give(scene->world, spot, "SpotLight", { { "range", { range } }, { "innerAngle", { outer / 2.f } }, { "outerAngle", { outer } } });
            scene->world.SetPosition(spot, { at.x, at.y, at.z });
            return spot;
        }
        [[nodiscard]] std::vector<float> Floats(const char* node) const { return kor::capi::BufferOf(scene->runtime->ResourceOf(node)->handle)->Read<float>(); }
        [[nodiscard]] static bool Has(const std::vector<kgraph::Port>& ports, const std::string_view name) { return std::ranges::find(ports, name, &kgraph::Port::name) != ports.end(); }
        /** Where a column-major matrix of sixteen, at @p m, takes the point @p p: x, y and w of what it gives. */
        static std::array<float, 3> Clip(const float* m, const kor::Vec3 p) {
            float out[4];
            for (int row = 0; row < 4; ++row) out[row] = m[row] * p.x + m[4 + row] * p.y + m[8 + row] * p.z + m[12 + row];
            return { out[0], out[1], out[3] };
        }
        void Ok() const {
            for (const auto& problem : scene->runtime->Problems()) ADD_FAILURE() << problem;
            ASSERT_TRUE(scene->runtime->Ok());
        }

        GraphScene* scene = nullptr;
    };
}

// The fields of lights.slang's two structs, set out on two nodes: the shader reads them where the nodes put them, a
// StructuredBuffer by std430 and a ConstantBuffer by std140.
TEST_F(BuilderTest, AnElementLaidOutOnTheNodeIsWhereTheShaderReadsIt) {
    Lamp({ 1, 0, 0 }, 0.5f, 1.f, { 0, 4, 0 });
    Lamp({ 0, 1, 0 }, 0.25f, 2.f, { 7, 0, 0 });
    Lamp({ 0, 0, 1 }, 1.f, 0.25f);

    kgraph::Graph graph;
    graph.Add("lamps", "ForEach").Set("component", "PointLight");
    graph.Add("tinted", "Multiply").Wire("a", "lamps", "field.color").Wire("b", "lamps", "field.intensity");
    graph.Add("light data", "BufferBuilder")
        .Set("layout", Value::List { Field("position", "eVec3"), Field("range", "eFloat"), Field("color", "eVec4"), Field("kind", "eUInt"), Field("cone", "eVec2") })
        .Wire("field.position", "lamps", "worldPosition").Wire("field.range", "lamps", "field.range").Wire("field.color", "tinted").Set("field.kind", 1);
    graph.Add("view data", "BufferBuilder").Set("usage", "eUniform")
        .Set("layout", Value::List { Field("viewProjection", "eMat4"), Field("eye", "eVec3"), Field("lightCount", "eUInt") })
        .Wire("field.lightCount", "light data", "count");
    graph.Add("target", "Image").Set("format", "eRGBA8_UNORM").Set("extent.x", 32).Set("extent.y", 32).Set("usage", Value::List { "eStorage", "eTransferSrc" });
    graph.Add("sum", "ComputePipeline").Set("compute.module", "lights").Set("compute.entry", "computeMain")
        .Wire("param.target", "target").Wire("param.lights", "light data").Wire("param.view", "view data");
    graph.Add("run", "Dispatch").Wire("pipeline", "sum").Set("groups.x", 4).Set("groups.y", 4);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "target").Wire("destination", "screen");

    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    Ok();
    for (const char* name : { "field.position", "field.range", "field.color", "field.kind", "field.cone" }) EXPECT_TRUE(Has(runtime.PortsOf("light data"), name)) << name;
    EXPECT_EQ(runtime.ValueOf("light data", "stride"), Value(48)) << "as the shader has Light";
    EXPECT_EQ(runtime.ValueOf("light data", "count"), Value(3)) << "an element for each of the ForEach's";
    EXPECT_EQ(runtime.ValueOf("view data", "stride"), Value(80)) << "a matrix, a float3, and a uint in the float3's fourth place";
    EXPECT_EQ(runtime.ValueOf("view data", "count"), Value(1));

    Frame();
    Frame();
    const auto lights = Floats("light data");
    ASSERT_GE(lights.size(), 3u * 12u);
    EXPECT_EQ(lights[1], 4.f);
    EXPECT_EQ(lights[3], 1.f);
    EXPECT_EQ(lights[4], 0.5f);
    EXPECT_EQ(lights[12], 7.f);
    kor::u32 kind = 0;
    std::memcpy(&kind, &lights[8], 4);
    EXPECT_EQ(kind, 1u);
    const auto pixel = scene->Pixel(16, 16);
    EXPECT_NEAR(pixel.x, 128, 1) << "the shader read each light, and how many there are, where the nodes put them";
    EXPECT_NEAR(pixel.y, 128, 1);
    EXPECT_NEAR(pixel.z, 64, 1);

    // A count given is how many there are: of the ForEach's, the first that many.
    graph.Find("light data")->Set("count", 2);
    runtime.Set(graph);
    Ok();
    Frame();
    Frame();
    EXPECT_EQ(runtime.ValueOf("light data", "count"), Value(2));
    EXPECT_NEAR(scene->Pixel(16, 16).z, 0, 1) << "the third, the blue one, is not in it";
}

// A camera is a View — from where its entity is, towards a step along the way it faces, up as its up — and a lens of
// its component's numbers: neither knows of a camera. Wired into a buffer field by field, they are its one element.
TEST_F(BuilderTest, ACameraIsAViewAndALensOfItsNumbers) {
    const kecs::Entity eye = scene->world.Create();
    support::Give(scene->world, eye, "PerspectiveCamera", { { "fieldOfView", { 50.f } }, { "far", { 40.f } } });
    scene->world.SetPosition(eye, { 1, 2, 3 });
    scene->world.SetRotation(eye, { -20, 30, 15 });     // rolled as well
    kgraph::Graph graph;
    graph.Add("eye", "Entity").Set("component", "PerspectiveCamera");
    graph.Add("looking at", "Add").Wire("a", "eye", "worldPosition").Wire("b", "eye", "forward");
    graph.Add("camera", "View").Wire("position", "eye", "worldPosition").Wire("target", "looking at").Wire("up", "eye", "up");
    graph.Add("seen", "MatrixMultiply").Wire("a", "lens").Wire("b", "camera");
    graph.Add("lens", "Perspective").Wire("fieldOfView", "eye", "field.fieldOfView").Wire("near", "eye", "field.near")
        .Wire("far", "eye", "field.far").Set("aspect", 2);
    graph.Add("camera data", "BufferBuilder").Set("usage", "eUniform")
        .Set("layout", Value::List { Field("viewProjection", "eMat4"), Field("position", "eVec3"), Field("far", "eFloat") })
        .Wire("field.viewProjection", "seen").Wire("field.position", "camera", "position").Wire("field.far", "lens", "far");
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    Ok();
    EXPECT_EQ(runtime.ValueOf("camera data", "count"), Value(1));
    EXPECT_EQ(runtime.ValueOf("camera data", "stride"), Value(80)) << "a matrix, then a float3 and a float in one float4";

    // The view is the entity's own, roll and all; the lens is glm's perspective of its numbers, Y pointing down.
    const kor::Mat4 placed = kgraph::ToKor(scene->world.WorldMatrix(eye));
    const kor::Mat4 view = kor::Inverse(placed);
    kor::Mat4 lens = kor::Perspective(50.f * 3.14159265f / 180.f, 2.f, 0.1f, 40.f);
    for (int column = 0; column < 4; ++column) lens[column][1] = -lens[column][1];
    const kor::Mat4 both = lens * view;
    const Value matrix = runtime.ValueOf("seen");
    ASSERT_TRUE(matrix.Items());
    for (int i = 0; i < 16; ++i) EXPECT_NEAR(*(*matrix.Items())[static_cast<std::size_t>(i)].Number(), both[i / 4][i % 4], 1e-4) << i;

    Frame();
    Frame();
    const auto floats = Floats("camera data");
    ASSERT_GE(floats.size(), 20u);
    for (int i = 0; i < 16; ++i) EXPECT_NEAR(floats[static_cast<std::size_t>(i)], both[i / 4][i % 4], 1e-4) << i;
    EXPECT_NEAR(floats[16], 1.f, 1e-5);
    EXPECT_NEAR(floats[17], 2.f, 1e-5);
    EXPECT_NEAR(floats[18], 3.f, 1e-5);
    EXPECT_FLOAT_EQ(floats[19], 40.f) << "far, in the position's fourth place";
}

// Each light's camera: a ForEach of the spots, their element worked out into a view and a lens, and a buffer of those.
// A point light is six elements, a face each: a CubeFace says which way each looks.
TEST_F(BuilderTest, CamerasOfLightsAreABufferTheSameWay) {
    Spot({ 0, 3, 0 }, 12.f, 30.f);
    Spot({ 5, 1, -2 }, 4.f, 20.f);
    Lamp({ 1, 1, 1 }, 1.f, 6.f, { -3, 2, 1 });

    kgraph::Graph graph;
    graph.Add("spots", "ForEach").Set("component", "SpotLight");
    graph.Add("spot target", "Add").Wire("a", "spots", "worldPosition").Wire("b", "spots", "forward");
    graph.Add("spot view", "View").Wire("position", "spots", "worldPosition").Wire("target", "spot target").Wire("up", "spots", "up");
    graph.Add("spot cone", "Multiply").Wire("a", "spots", "field.outerAngle").Set("b", 2);
    graph.Add("spot lens", "Perspective").Wire("fieldOfView", "spot cone").Set("near", 0.05).Wire("far", "spots", "field.range");
    graph.Add("spot seen", "MatrixMultiply").Wire("a", "spot lens").Wire("b", "spot view");
    graph.Add("spot cameras", "BufferBuilder")
        .Set("layout", Value::List { Field("viewProjection", "eMat4"), Field("position", "eVec3"), Field("far", "eFloat"), Field("fieldOfView", "eFloat") })
        .Wire("field.viewProjection", "spot seen").Wire("field.position", "spot view", "position")
        .Wire("field.far", "spot lens", "far").Wire("field.fieldOfView", "spot lens", "fieldOfView");
    graph.Add("lamps", "ForEach").Set("component", "PointLight").Set("repeat", 6);
    graph.Add("face", "CubeFace").Wire("face", "lamps", "turn");
    graph.Add("face target", "Add").Wire("a", "lamps", "worldPosition").Wire("b", "face", "direction");
    graph.Add("face view", "View").Wire("position", "lamps", "worldPosition").Wire("target", "face target").Wire("up", "face", "up");
    graph.Add("face lens", "Perspective").Set("fieldOfView", 90).Set("near", 0.05).Wire("far", "lamps", "field.range");
    graph.Add("face seen", "MatrixMultiply").Wire("a", "face lens").Wire("b", "face view");
    graph.Add("face cameras", "BufferBuilder").Set("layout", Value::List { Field("viewProjection", "eMat4") })
        .Wire("field.viewProjection", "face seen");

    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    Ok();
    EXPECT_EQ(runtime.ValueOf("spot cameras", "count"), Value(2));
    EXPECT_EQ(runtime.ValueOf("spot cameras", "stride"), Value(96));
    EXPECT_EQ(runtime.ValueOf("face cameras", "count"), Value(6)) << "one lamp, six faces";
    EXPECT_EQ(runtime.ValueOf("lamps", "count"), Value(6));
    EXPECT_EQ(runtime.ValueOf("lamps", "entities"), Value(1));

    Frame();
    Frame();
    const auto spots = Floats("spot cameras");
    ASSERT_GE(spots.size(), 2u * 24u);
    for (std::size_t i = 0; i < 2; ++i) {
        const float* element = spots.data() + i * 24;
        const kor::Vec3 at = i == 0 ? kor::Vec3(0, 3, 0) : kor::Vec3(5, 1, -2);
        EXPECT_FLOAT_EQ(element[16], at.x) << "spot " << i << "'s camera is where it is";
        EXPECT_FLOAT_EQ(element[17], at.y);
        EXPECT_FLOAT_EQ(element[18], at.z);
        EXPECT_FLOAT_EQ(element[19], i == 0 ? 12.f : 4.f) << "and sees as far as it reaches";
        EXPECT_NEAR(element[20], i == 0 ? 60.f : 40.f, 1e-3) << "as wide as its cone";
        // Straight ahead of it — down -Z, the way an entity faces — is the middle of its picture.
        const auto clip = Clip(element, at + kor::Vec3(0, 0, -2));
        EXPECT_GT(clip[2], 0.f);
        EXPECT_NEAR(clip[0] / clip[2], 0.f, 1e-4);
        EXPECT_NEAR(clip[1] / clip[2], 0.f, 1e-4);
    }

    // The lamp's six faces, each looking its own way along an axis.
    const auto faces = Floats("face cameras");
    const auto stride = static_cast<std::size_t>(*runtime.ValueOf("face cameras", "stride").Number()) / 4;
    ASSERT_GE(faces.size(), 6 * stride);
    const kor::Vec3 axes[6] { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (std::size_t face = 0; face < 6; ++face) {
        const float* element = faces.data() + face * stride;
        const auto clip = Clip(element, kor::Vec3(-3, 2, 1) + axes[face] * 2.f);
        EXPECT_GT(clip[2], 0.f) << "face " << face;
        EXPECT_NEAR(clip[0] / clip[2], 0.f, 1e-4) << "face " << face;
        EXPECT_NEAR(clip[1] / clip[2], 0.f, 1e-4) << "face " << face;
    }

    // The world changes, and the cameras follow without anything being built again.
    const auto builds = runtime.Status("spot cameras")->builds;
    scene->world.SetPosition(scene->world.All().front(), { 1, 1, 1 });
    Frame();
    Frame();
    EXPECT_FLOAT_EQ(Floats("spot cameras")[16], 1.f);
    EXPECT_EQ(runtime.Status("spot cameras")->builds, builds);
}

// What a layout cannot be is said on the node.
TEST_F(BuilderTest, WhatALayoutCannotBeIsSaid) {
    auto& runtime = *scene->runtime;
    kgraph::Graph graph;
    graph.Add("data", "BufferBuilder");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("data")->error.find("it has no fields yet"), std::string::npos) << runtime.Status("data")->error;
    graph.Find("data")->Set("layout", Value::List { Field("position", "eVec5") });
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("data")->error.find("'position' is a eVec5"), std::string::npos) << runtime.Status("data")->error;
    graph.Find("data")->Set("layout", Value::List { Field("position", "eVec3") }).Set("field.colour", 1);
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("data")->error.find("has no field 'colour'. It has: position"), std::string::npos) << runtime.Status("data")->error;
}

// What the GPU has is written back into the entities it is of: a buffer of each light's position, one along X, written
// back moves each light along X a little at a time, as each read-back arrives. One moved by something else is not put
// back where the buffer had it: setup is made again from where it is, and it goes on from there.
TEST_F(BuilderTest, WhatIsReadBackIsWrittenIntoTheEntitiesItIsOf) {
    const kecs::Entity first = Lamp({ 1, 0, 0 }, 1.f, 1.f, { 0, 2, 0 });
    const kecs::Entity second = Lamp({ 0, 1, 0 }, 1.f, 1.f, { 5, 0, 0 });
    kgraph::Graph graph;
    graph.Add("lamps", "ForEach").Set("component", "PointLight");
    graph.Add("on", "Add").Wire("a", "lamps", "position").Set("b", Value::List { 1, 0, 0 });
    graph.Add("data", "BufferBuilder").Set("layout", Value::List { Field("position", "eVec3"), Field("range", "eFloat") }).Wire("field.position", "on")
        .Wire("field.range", "lamps", "field.range");
    graph.Add("back", "WriteBack").Wire("buffer", "data").Set("forEach", "lamps").Set("stride", 16).Set("position", 0)
        .Set("fields", Value::Members { { "PointLight.range", 12 } });
    auto& runtime = *scene->runtime;
    runtime.Set(graph);
    Ok();
    EXPECT_EQ(runtime.ValueOf("data", "stride"), Value(16));
    auto& world = scene->world;
    for (int i = 0; i < 30; ++i) Frame();
    EXPECT_GT(world.PositionOf(first).x, 3.f) << "moved along X, a read-back at a time";
    EXPECT_FLOAT_EQ(world.PositionOf(first).y, 2.f);
    EXPECT_GT(world.PositionOf(second).x, 8.f) << "each its own";

    world.SetPosition(first, { 100.f, 2.f, 0.f });        // as the gizmo does
    for (int i = 0; i < 30; ++i) Frame();
    EXPECT_GT(world.PositionOf(first).x, 100.f) << "going on from where it was put, not from where the buffer had it";

    // Wrongly set: said so.
    graph.Find("back")->Set("forEach", "data");
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("back")->error.find("ForEach"), std::string::npos) << runtime.Status("back")->error;
    graph.Find("back")->Set("forEach", "lamps").Set("position", 8);
    runtime.Set(graph);
    EXPECT_NE(runtime.Status("back")->error.find("within its stride"), std::string::npos) << runtime.Status("back")->error;
}
