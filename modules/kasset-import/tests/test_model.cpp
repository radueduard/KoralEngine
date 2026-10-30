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
    };
}

TEST_F(Folder, WhatAModelNamesBesideIt)
{
    std::ofstream(root / "lamp.gltf") << R"({ "buffers": [ { "uri": "data/lamp.bin" }, { "uri": "data:application/octet-stream;base64,AAAA" } ],
                                             "images": [ { "uri": "lamp%20glow.png" }, { "uri": "../outside.png" }, { "uri": "https://x/y.png" } ] })";
    EXPECT_EQ(NamedBeside(root / "lamp.gltf"), (std::vector<fs::path> { "data/lamp.bin", "lamp glow.png" }));
    EXPECT_EQ(NamedBeside(Crate()), (std::vector<fs::path> { "crate.mtl", "textures/wood.png" }));
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
