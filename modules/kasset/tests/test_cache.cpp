// Artifacts: kept by what they were made of, and read back as they were.

#include <gtest/gtest.h>

#include <kasset/artifact.h>
#include <kasset/cache.h>
#include <kasset/id.h>

#include <bit>
#include <format>
#include <fstream>

using namespace kasset;
namespace fs = std::filesystem;

TEST(Hash, KnownValues)
{
    // XXH64's own test values.
    EXPECT_EQ(Hash(std::string_view("")), 0xef46db3751d8e999ull);
    EXPECT_EQ(Hash(std::string_view("a")), 0xd24ec4f1a98c6e5bull);
    EXPECT_EQ(Hash(std::string_view("abc")), 0x44bc2cf5ad770999ull);
    EXPECT_EQ(Hash(std::string_view("Nobody inspects the spammish repetition")), 0xfbcea83c8a378bf1ull);
}

TEST(Key, WhatItIsMadeOfIsWhatItIs)
{
    EXPECT_EQ(Key("model@1").Add("a").Text(), Key("model@1").Add("a").Text());
    EXPECT_NE(Key("model@1").Add("a").Text(), Key("model@2").Add("a").Text());
    EXPECT_NE(Key("model@1").Add("ab").Add("c").Text(), Key("model@1").Add("a").Add("bc").Text());
    EXPECT_EQ(Key("x").Text().size(), 32u);
}

namespace {
    struct Folder : ::testing::Test {
        fs::path root;
        void SetUp() override { root = fs::temp_directory_path() / std::format("kasset-cache-{}", Id::New().Text()); fs::create_directories(root); }
        void TearDown() override { fs::remove_all(root); }
    };
}

TEST_F(Folder, AFileIsKeptByItsContentNotItsTime)
{
    const fs::path file = root / "wall.png";
    std::ofstream(file) << "picture";
    const auto first = Key("t").AddFile(file) ? Key("t").Add(*HashFile(file)).Text() : std::string();
    Key key("t");
    ASSERT_TRUE(key.AddFile(file));

    // Touched — as a checkout does — it is the same.
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::hours(1));
    Key touched("t");
    ASSERT_TRUE(touched.AddFile(file));
    EXPECT_EQ(touched.Text(), key.Text());

    // Changed, it is another.
    std::ofstream(file) << "another picture";
    Key changed("t");
    ASSERT_TRUE(changed.AddFile(file));
    EXPECT_NE(changed.Text(), key.Text());

    Key missing("t");
    EXPECT_FALSE(missing.AddFile(root / "nothing.png"));
    EXPECT_FALSE(missing.Valid());
}

TEST_F(Folder, WrittenAndReadBack)
{
    const Cache cache(root / "cache");
    const Key key = Key("model@1").Add("ship");
    EXPECT_FALSE(cache.Read("models", key, ".kmodel"));
    const std::vector<std::byte> bytes { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } };
    const auto written = cache.Write("models", key, ".kmodel", bytes);
    ASSERT_TRUE(written) << written.error();
    EXPECT_EQ(written->parent_path().filename(), "models");
    EXPECT_EQ(cache.Read("models", key, ".kmodel"), bytes);
    EXPECT_EQ(std::distance(fs::directory_iterator(root / "cache" / "models"), fs::directory_iterator()), 1) << "nothing left beside it";
}

TEST(ModelArtifact, ReadBackAsItWasMade)
{
    ModelArtifact artifact;
    Model& model = artifact.model;
    model.nodes.push_back({ "root", -1, { 1, 2, 3 }, { 0, 90, 0 }, { 1, 1, 2 }, { { "#0", 0 }, { "#2", -1 } } });
    ModelMaterial material;
    material.name = "Paint";
    material.model = ModelMaterial::Model::eClassic;
    material.albedo[1] = 0.25f;
    material.shininess = 32.f;
    material.normalMap = "textures/paint_n.png";
    material.blended = true;
    model.materials.push_back(material);
    ModelLight light;
    light.name = "Sun";
    light.kind = ModelLight::Kind::eDirectional;
    light.node = 0;
    model.lights.push_back(light);
    ModelCamera camera;
    camera.name = "View";
    camera.orthographic = true;
    model.cameras.push_back(camera);
    model.boundsMax[2] = 7.f;
    model.meshNames = { "Hull", "", "Mast" };
    model.attributes.push_back({ "POSITION", "float", 3, true });
    model.attributes.push_back({ "BONE_IDS", "uint", 4, false });
    VertexData data;
    data.vertexCount = 3;
    data.attributes.push_back({ "POSITION", AttributeType::eFloat, 3, { 1, 2, 3, 4, 5, 6, 7, 8, 9 } });
    data.indices = { 0, 1, 2 };
    data.buffers = { { "POSITION" } };
    artifact.meshes.push_back({ 2, "Hull", data });

    const auto bytes = Serialize(artifact);
    const auto read = DeserializeModel(bytes);
    ASSERT_TRUE(read) << read.error();
    EXPECT_EQ(read->meshes, artifact.meshes);
    ASSERT_EQ(read->model.nodes.size(), 1u);
    EXPECT_EQ(read->model.nodes[0].parts[1].mesh, "#2");
    EXPECT_EQ(read->model.nodes[0].rotation[1], 90.f);
    ASSERT_EQ(read->model.materials.size(), 1u);
    EXPECT_EQ(read->model.materials[0].name, "Paint");
    EXPECT_EQ(read->model.materials[0].model, ModelMaterial::Model::eClassic);
    EXPECT_EQ(read->model.materials[0].albedo[1], 0.25f);
    EXPECT_EQ(read->model.materials[0].normalMap, "textures/paint_n.png");
    EXPECT_TRUE(read->model.materials[0].blended);
    EXPECT_EQ(read->model.lights.at(0).kind, ModelLight::Kind::eDirectional);
    EXPECT_TRUE(read->model.cameras.at(0).orthographic);
    EXPECT_EQ(read->model.boundsMax[2], 7.f);
    EXPECT_EQ(read->model.meshNames, model.meshNames);
    EXPECT_EQ(read->model.attributes, model.attributes);

    auto cut = bytes;
    cut.resize(cut.size() - 5);
    EXPECT_FALSE(DeserializeModel(cut));
    auto other = bytes;
    other[4] = std::byte { 99 };
    EXPECT_FALSE(DeserializeModel(other)) << "another version";
}

TEST(MeshArtifact, ReadBackAsItWasMade)
{
    VertexData mesh;
    mesh.vertexCount = 2;
    mesh.attributes.push_back({ "POSITION", AttributeType::eFloat, 2, { 1, 2, 3, 4 } });
    mesh.attributes.push_back({ "WIND", AttributeType::eUInt, 1, { 7, 8 } });
    mesh.buffers = { { "POSITION" }, { "WIND" } };
    const auto read = DeserializeMesh(Serialize(mesh));
    ASSERT_TRUE(read) << read.error();
    EXPECT_EQ(*read, mesh);
    EXPECT_FALSE(DeserializeMesh(Serialize(ModelArtifact {}))) << "a model is no mesh";
}
