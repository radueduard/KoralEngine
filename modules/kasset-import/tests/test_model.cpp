// A model imported: read by its importer once, then from the cache, for as long as it and its settings are as they were.

#include <gtest/gtest.h>

#include <kasset/id.h>
#include <kasset/import.h>

#include <algorithm>
#include <format>
#include <fstream>

using namespace kasset;
namespace fs = std::filesystem;

namespace {
    struct Folder : ::testing::Test {
        fs::path root;
        void SetUp() override { root = fs::temp_directory_path() / std::format("kasset-import-{}", Id::New().Text()); fs::create_directories(root); }
        void TearDown() override { fs::remove_all(root); }

        /** A triangle and a quad, of a material that names a picture. */
        fs::path Crate()
        {
            std::ofstream(root / "crate.obj") << "mtllib crate.mtl\n"
                                                 "o Triangle\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvn 0 0 1\nusemtl wood\nf 1/1/1 2/2/1 3/3/1\n"
                                                 "o Quad\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\nf 4/1/1 5/2/1 6/3/1\nf 4/1/1 6/3/1 7/3/1\n";
            std::ofstream(root / "crate.mtl") << "newmtl wood\nKd 0.5 0.25 0.125\nmap_Kd textures/wood.png\n";
            return root / "crate.obj";
        }

        /**
         * A triangle bent by two joints — a root, and a tip a metre above it, two corners the root's and the third the
         * tip's — and a clip of a second that turns the tip a quarter about z. Its buffer in the file, as base64.
         */
        fs::path Bent()
        {
            std::vector<std::byte> bytes;
            const auto put = [&](const auto& value) { const auto* at = reinterpret_cast<const std::byte*>(&value); bytes.insert(bytes.end(), at, at + sizeof value); };
            for (const float v : { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f }) put(v);                       // 0: positions, 36
            for (const std::uint16_t j : { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 }) put(j);                        // 36: joints, 24
            for (const float w : { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f }) put(w);        // 60: weights, 48
            for (const std::uint16_t i : { 0, 1, 2, 0 }) put(i);                                                // 108: indices, 6 (+2)
            for (const float m : { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
                                   1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 1.f }) put(m);   // 116: inverse binds, 128
            for (const float t : { 0.f, 1.f }) put(t);                                                          // 244: times, 8
            for (const float q : { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.70710678f, 0.70710678f }) put(q);           // 252: rotations, 32
            static constexpr char Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string base64;
            for (std::size_t i = 0; i < bytes.size(); i += 3) {
                std::uint32_t n = 0;
                for (std::size_t k = 0; k < 3; ++k) n = n << 8 | (i + k < bytes.size() ? static_cast<std::uint32_t>(bytes[i + k]) : 0u);
                for (std::size_t k = 0; k < 4; ++k) base64 += i + k <= bytes.size() ? Digits[n >> (18 - 6 * k) & 63] : '=';
            }
            std::ofstream(root / "bent.gltf") << std::format(R"({{
              "asset": {{ "version": "2.0" }}, "scene": 0, "scenes": [ {{ "nodes": [ 0, 1 ] }} ],
              "nodes": [ {{ "name": "body", "mesh": 0, "skin": 0 }}, {{ "name": "root", "children": [ 2 ] }}, {{ "name": "tip", "translation": [ 0, 1, 0 ] }} ],
              "meshes": [ {{ "primitives": [ {{ "attributes": {{ "POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2 }}, "indices": 3 }} ] }} ],
              "skins": [ {{ "joints": [ 1, 2 ], "inverseBindMatrices": 4 }} ],
              "animations": [ {{ "name": "bend", "samplers": [ {{ "input": 5, "output": 6 }} ], "channels": [ {{ "sampler": 0, "target": {{ "node": 2, "path": "rotation" }} }} ] }} ],
              "buffers": [ {{ "byteLength": {}, "uri": "data:application/octet-stream;base64,{}" }} ],
              "bufferViews": [ {{ "buffer": 0, "byteOffset": 0, "byteLength": 36 }}, {{ "buffer": 0, "byteOffset": 36, "byteLength": 24 }},
                               {{ "buffer": 0, "byteOffset": 60, "byteLength": 48 }}, {{ "buffer": 0, "byteOffset": 108, "byteLength": 6 }},
                               {{ "buffer": 0, "byteOffset": 116, "byteLength": 128 }}, {{ "buffer": 0, "byteOffset": 244, "byteLength": 8 }},
                               {{ "buffer": 0, "byteOffset": 252, "byteLength": 32 }} ],
              "accessors": [ {{ "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] }},
                             {{ "bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4" }},
                             {{ "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" }},
                             {{ "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }},
                             {{ "bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4" }},
                             {{ "bufferView": 5, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [ 0 ], "max": [ 1 ] }},
                             {{ "bufferView": 6, "componentType": 5126, "count": 2, "type": "VEC4" }} ]
            }})", bytes.size(), base64);
            return root / "bent.gltf";
        }
    };
}

TEST_F(Folder, WhatAModelNamesBesideIt)
{
    std::ofstream(root / "lamp.gltf") << R"({ "buffers": [ { "uri": "data/lamp.bin" }, { "uri": "data:application/octet-stream;base64,AAAA" } ],
                                             "images": [ { "uri": "lamp%20glow.png" }, { "uri": "../outside.png" }, { "uri": "https://x/y.png" } ] })";
    EXPECT_EQ(NamedBeside(root / "lamp.gltf"), (std::vector<fs::path> { "data/lamp.bin", "lamp glow.png" }));
    EXPECT_EQ(NamedBeside(Crate()), (std::vector<fs::path> { "crate.mtl", "textures/wood.png" }));
}

// An FBX names its pictures as text among what it holds, as Windows writes them, and where it was exported from too.
TEST_F(Folder, WhatAnFbxNamesBesideIt)
{
    const auto string = [](const std::string_view text) {
        const auto length = static_cast<std::uint32_t>(text.size());
        std::string bytes = "S";
        bytes.append(reinterpret_cast<const char*>(&length), sizeof length);
        return bytes.append(text);
    };
    std::ofstream(root / "bistro.fbx", std::ios::binary) << "Kaydara FBX Binary  " << string("Model::Lamp") << string("Textures\\Lamp_BaseColor.dds")
                                                         << string("Textures/Lamp_BaseColor.dds") << string("C:\\Bistro\\Textures\\Lamp_Normal.dds")
                                                         << string("..\\outside.png");
    EXPECT_EQ(NamedBeside(root / "bistro.fbx"), (std::vector<fs::path> { "Textures/Lamp_BaseColor.dds" }));
    std::ofstream(root / "plain.fbx") << "; FBX 7.4.0 project file\nRelativeFilename: \"maps\\wood.png\"\nModel: \"Crate\"\n";
    EXPECT_EQ(NamedBeside(root / "plain.fbx"), (std::vector<fs::path> { "maps/wood.png" }));
}

TEST_F(Folder, AModelIsReadAsTheFileHasIt)
{
    const auto read = ReadModel(Crate(), {});
    ASSERT_TRUE(read) << read.error();
    ASSERT_EQ(read->meshes.size(), 2u);
    for (const auto& mesh : read->meshes) EXPECT_EQ(mesh.data.Problem(), "") << mesh.name;
    ASSERT_FALSE(read->model.materials.empty());
    // Named as the file names it: from the model's folder, not as a project would.
    const auto wood = std::ranges::find(read->model.materials, std::string("wood"), &ModelMaterial::name);
    ASSERT_NE(wood, read->model.materials.end());
    EXPECT_EQ(wood->albedoMap, "textures/wood.png");
    bool parts = false;
    for (const auto& node : read->model.nodes)
        for (const auto& part : node.parts) { EXPECT_TRUE(part.mesh.starts_with('#')) << part.mesh; parts = true; }
    EXPECT_TRUE(parts);
    EXPECT_FALSE(ReadModel(root / "nothing.obj", {}));
}

// A skinned mesh: what bends it, its joints the nodes they are, each with its inverse bind; and its clips, by node.
TEST_F(Folder, AModelsSkinsAndClipsAreRead)
{
    const auto read = ReadModel(Bent(), {});
    ASSERT_TRUE(read) << read.error();
    const Model& model = read->model;
    const auto nodeNamed = [&](const std::string& name) {
        const auto found = std::ranges::find(model.nodes, name, &ModelNode::name);
        return found == model.nodes.end() ? -1 : static_cast<int>(found - model.nodes.begin());
    };
    ASSERT_GE(nodeNamed("root"), 0);
    ASSERT_GE(nodeNamed("tip"), 0);
    ASSERT_EQ(model.skins.size(), 1u);
    const ModelSkin& skin = model.skins.front();
    ASSERT_EQ(skin.joints.size(), 2u);
    EXPECT_EQ(skin.joints[0].node, nodeNamed("root"));
    EXPECT_EQ(skin.joints[1].node, nodeNamed("tip"));
    EXPECT_FLOAT_EQ(skin.joints[1].inverseBind[13], -1.f) << "column-major: its translation last";
    EXPECT_FLOAT_EQ(skin.joints[1].inverseBind[0], 1.f);
    bool skinned = false;
    for (const auto& node : model.nodes)
        for (const auto& part : node.parts) skinned |= part.skin == 0;
    EXPECT_TRUE(skinned);
    ASSERT_EQ(read->meshes.size(), 1u);
    EXPECT_TRUE(read->meshes.front().data.Find("BONE_IDS"));
    EXPECT_TRUE(read->meshes.front().data.Find("BONE_WEIGHTS"));

    ASSERT_EQ(model.animations.size(), 1u);
    const ModelAnimation& bend = model.animations.front();
    EXPECT_EQ(bend.name, "bend");
    EXPECT_NEAR(bend.duration, 1.f, 1e-4f);
    const auto tip = std::ranges::find(bend.channels, nodeNamed("tip"), &ModelAnimation::Channel::node);
    ASSERT_NE(tip, bend.channels.end());
    ASSERT_EQ(tip->rotationTimes.size(), 2u);
    EXPECT_NEAR(tip->rotationTimes[1], 1.f, 1e-4f);
    ASSERT_EQ(tip->rotations.size(), 8u);
    EXPECT_NEAR(tip->rotations[6], 0.70710678f, 1e-4f) << "z";
    EXPECT_NEAR(tip->rotations[7], 0.70710678f, 1e-4f) << "w";

    // Kept as read, through the cache.
    const auto again = DeserializeModel(Serialize(*read));
    ASSERT_TRUE(again) << again.error();
    ASSERT_EQ(again->model.skins.size(), 1u);
    EXPECT_FLOAT_EQ(again->model.skins[0].joints[1].inverseBind[13], -1.f);
    ASSERT_EQ(again->model.animations.size(), 1u);
    EXPECT_EQ(again->model.animations[0].channels.size(), bend.channels.size());
}

TEST_F(Folder, ReadOnceThenFromTheCache)
{
    const fs::path crate = Crate();
    const Cache cache(root / "cache");
    const auto first = ImportModel(crate, {}, &cache);
    ASSERT_TRUE(first) << first.error();
    EXPECT_TRUE(fs::exists(cache.PathOf("models", ModelKey(crate, {}), ".kmodel")));

    // What is in the cache is what is read: one kept there by hand, marked, is what comes back.
    ModelArtifact marked = *first;
    marked.meshes.front().name = "from the cache";
    ASSERT_TRUE(cache.Write("models", ModelKey(crate, {}), ".kmodel", Serialize(marked)));
    const auto second = ImportModel(crate, {}, &cache);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->meshes.front().name, "from the cache");

    // Other settings are another artifact; the file's materials changed, too.
    MeshImport without;
    without.leaveOut = { "NORMAL" };
    const auto other = ImportModel(crate, without, &cache);
    ASSERT_TRUE(other);
    EXPECT_NE(other->meshes.front().name, "from the cache");
    EXPECT_FALSE(other->meshes.front().data.Find("NORMAL"));
    const std::string before = ModelKey(crate, {}).Text();
    std::ofstream(root / "crate.mtl") << "newmtl wood\nKd 1 1 1\n";
    EXPECT_NE(ModelKey(crate, {}).Text(), before) << "what it names beside it is what it is made of";
    std::ofstream(root / "textures.png") << "x";
    fs::create_directories(root / "textures");
    std::ofstream(root / "textures" / "wood.png") << "a picture changed is the same model";
    const std::string withMaterials = ModelKey(crate, {}).Text();
    std::ofstream(root / "textures" / "wood.png") << "changed again";
    EXPECT_EQ(ModelKey(crate, {}).Text(), withMaterials);

    // An artifact that is not one is made again in its place.
    const auto garbage = std::vector<std::byte>(16, std::byte { 7 });
    ASSERT_TRUE(cache.Write("models", ModelKey(crate, {}), ".kmodel", garbage));
    const auto again = ImportModel(crate, {}, &cache);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->meshes.size(), 2u);
    EXPECT_NE(cache.Read("models", ModelKey(crate, {}), ".kmodel"), garbage);
}
