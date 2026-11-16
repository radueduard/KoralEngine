// A skinned mesh drawn bent by its joints: the model's nodes made entities, its mesh given a Skin, and a shader that reads
// the runtime's JOINT_MATRICES where its instance's JOINTS say. Offscreen.

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <kgraph/animation.h>
#include <kgraph/entities.h>
#include <kgraph/runtime.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using kgraph::Value;

    std::string Base64(const std::vector<std::byte>& bytes)
    {
        static constexpr char Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            std::uint32_t n = 0;
            for (std::size_t k = 0; k < 3; ++k) n = n << 8 | (i + k < bytes.size() ? static_cast<std::uint32_t>(bytes[i + k]) : 0u);
            for (std::size_t k = 0; k < 4; ++k) out += i + k <= bytes.size() ? Digits[n >> (18 - 6 * k) & 63] : '=';
        }
        return out;
    }

    /** A square from (0.25, 0.25) to (0.75, 0.75), every corner the second joint's — `tip`, under `root` — bound where it is. */
    void WriteSkinnedSquare(const std::filesystem::path& file)
    {
        std::vector<std::byte> bytes;
        const auto put = [&](const auto& value) { const auto* at = reinterpret_cast<const std::byte*>(&value); bytes.insert(bytes.end(), at, at + sizeof value); };
        for (const float v : { 0.25f, 0.25f, 0.f, 0.75f, 0.25f, 0.f, 0.75f, 0.75f, 0.f, 0.25f, 0.75f, 0.f }) put(v);    // 0: positions, 48
        for (int i = 0; i < 4; ++i) for (const std::uint16_t j : { 1, 0, 0, 0 }) put(j);                               // 48: joints, 32
        for (int i = 0; i < 4; ++i) for (const float w : { 1.f, 0.f, 0.f, 0.f }) put(w);                                // 80: weights, 64
        for (const std::uint16_t i : { 0, 1, 2, 0, 2, 3 }) put(i);                                                      // 144: indices, 12
        for (int j = 0; j < 2; ++j) for (const float m : { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f }) put(m);   // 156: binds, 128
        std::ofstream(file) << std::format(R"({{
          "asset": {{ "version": "2.0" }}, "scene": 0, "scenes": [ {{ "nodes": [ 0, 1 ] }} ],
          "nodes": [ {{ "name": "body", "mesh": 0, "skin": 0 }}, {{ "name": "root", "children": [ 2 ] }}, {{ "name": "tip" }} ],
          "meshes": [ {{ "primitives": [ {{ "attributes": {{ "POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2 }}, "indices": 3 }} ] }} ],
          "skins": [ {{ "joints": [ 1, 2 ], "inverseBindMatrices": 4 }} ],
          "buffers": [ {{ "byteLength": {}, "uri": "data:application/octet-stream;base64,{}" }} ],
          "bufferViews": [ {{ "buffer": 0, "byteOffset": 0, "byteLength": 48 }}, {{ "buffer": 0, "byteOffset": 48, "byteLength": 32 }},
                           {{ "buffer": 0, "byteOffset": 80, "byteLength": 64 }}, {{ "buffer": 0, "byteOffset": 144, "byteLength": 12 }},
                           {{ "buffer": 0, "byteOffset": 156, "byteLength": 128 }} ],
          "accessors": [ {{ "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [ 0.25, 0.25, 0 ], "max": [ 0.75, 0.75, 0 ] }},
                         {{ "bufferView": 1, "componentType": 5123, "count": 4, "type": "VEC4" }},
                         {{ "bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC4" }},
                         {{ "bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR" }},
                         {{ "bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4" }} ]
        }})", bytes.size(), Base64(bytes));
    }

    class SkinningTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "skinning", .extent = { support::Size, support::Size } });
            ASSERT_NE(scene, nullptr);
        }
        void TearDown() override {
            if (scene) { scene->runtime->SetAssetDirectory({}); support::App()->Close(*scene); Frame(); }
            std::filesystem::remove_all(root);
        }
        [[nodiscard]] bool Red(const kor::u32 x, const kor::u32 y) const { return scene->Pixel(x, y).x > 200; }
        GraphScene* scene = nullptr;
        std::filesystem::path root = std::filesystem::temp_directory_path() / "kgraph-skinning";
    };
}

TEST_F(SkinningTest, ASkinnedMeshIsDrawnWhereItsJointsAre) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteSkinnedSquare(root / "square.gltf");
    kgraph::Runtime& runtime = *scene->runtime;
    kecs::World& world = scene->world;
    runtime.SetAssetDirectory(root);
    std::shared_ptr<const kgraph::Model> model;
    std::string error;
    for (int i = 0; i < 400 && !(model = runtime.ModelOf("square.gltf", &error)) && error.empty(); ++i) Frame();
    ASSERT_TRUE(model) << error;
    ASSERT_EQ(model->skins.size(), 1u);

    // Made as a document makes a model: an entity a node, under one for the whole; the skinned part a Skin.
    const kecs::Entity whole = world.Create();
    world.Rename(whole, "square");
    std::vector<kecs::Entity> made;
    kecs::Entity body, tip;
    for (std::size_t i = 0; i < model->nodes.size(); ++i) {
        const auto& node = model->nodes[i];
        const kecs::Entity entity = world.Create();
        world.Rename(entity, node.name.empty() ? std::format("Node {}", i) : node.name);
        world.SetParent(entity, node.parent >= 0 ? made[static_cast<std::size_t>(node.parent)] : whole);
        world.SetPosition(entity, { node.position[0], node.position[1], node.position[2] });
        world.SetRotation(entity, { node.rotation[0], node.rotation[1], node.rotation[2] });
        world.SetScale(entity, { node.scale[0], node.scale[1], node.scale[2] });
        made.push_back(entity);
        if (node.name == "tip") tip = entity;
        for (const auto& part : node.parts) {
            ASSERT_EQ(part.skin, 0);
            body = entity;
            world.Add<kgraph::Mesh>(entity, { runtime.ResourceOf(part.mesh.starts_with('@') ? part.mesh : "@square.gltf" + part.mesh) });
            world.Add<kgraph::Skin>(entity, kgraph::Skin { .model = "@square.gltf", .skin = part.skin, .root = whole });
        }
    }
    ASSERT_FALSE(body.IsNull());
    ASSERT_FALSE(tip.IsNull());

    kgraph::Graph graph;
    graph.Add("canvas", "Image").Set("format", "eRGBA8_UNORM").Set("extent", Value::List { support::Size, support::Size })
        .Set("usage", Value::List { "eColorAttachment", "eTransferSrc" });
    graph.Add("canvas view", "ImageView").Wire("image", "canvas");
    graph.Add("target", "Framebuffer").Set("color[0].name", "color").Wire("color[0].view", "canvas view");
    graph.Add("skinned", "GraphicsPipeline").Set("vertex.module", "skinned").Set("vertex.entry", "vertexMain")
        .Set("fragment.module", "skinned").Set("fragment.entry", "fragmentMain").Wire("framebuffer", "target");
    graph.Add("drawn", "ForEach").Set("component", "Mesh").Set("with", Value::List { "Skin" });
    graph.Add("draw", "Draw").Wire("pipeline", "skinned").Wire("mesh", "drawn", "field.mesh").Set("batched", true);
    graph.Add("screen", "Screen");
    graph.Add("show", "Blit").Wire("source", "canvas").Wire("destination", "screen");
    runtime.Set(graph);
    const auto frames = [&] { for (int i = 0; i < 6; ++i) Frame(); for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem; };
    frames();
    ASSERT_TRUE(runtime.Ok());

    // As bound: where its vertices are.
    EXPECT_TRUE(Red(24, 24)) << "the square, as it was bound";
    EXPECT_FALSE(Red(8, 24));
    // Its joint moved a step to the left: the square with it, its entity where it was.
    world.SetPosition(tip, { -1.f, 0.f, 0.f });
    frames();
    EXPECT_FALSE(Red(24, 24)) << "bent away";
    EXPECT_TRUE(Red(8, 24)) << "to where its joint is";
    // The whole moved: the joint and the body with it — once, not twice.
    world.SetPosition(whole, { 0.5f, 0.f, 0.f });
    frames();
    EXPECT_TRUE(Red(16, 24));
    EXPECT_FALSE(Red(8, 24));
    EXPECT_FALSE(Red(28, 24));
}
