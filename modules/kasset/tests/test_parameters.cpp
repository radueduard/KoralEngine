// A producer's parameters in a meta: set, read back, taken away; and the importers' settings as JSON.

#include <gtest/gtest.h>

#include <kasset/parameters.h>

using namespace kasset;

TEST(Parameters, SetReadAndTakenAway)
{
    Meta meta;
    meta.id = Id::New();
    EXPECT_EQ(ParameterOf(meta, "mesh"), "");
    EXPECT_TRUE(SetParameter(meta, "mesh", R"({ "leaveOut": ["TANGENT"] })"));
    EXPECT_FALSE(SetParameter(meta, "mesh", R"({ "leaveOut": ["TANGENT"] })")) << "the same is no change";
    EXPECT_TRUE(SetParameter(meta, "materialKinds", R"({ "pbr": "Mine" })"));
    EXPECT_EQ(ParameterOf(meta, "mesh"), R"({ "leaveOut": ["TANGENT"] })");

    // Written and read as a meta, it is the same.
    const auto read = Read(Write(meta));
    ASSERT_TRUE(read) << read.error();
    EXPECT_EQ(ParameterOf(*read, "materialKinds"), R"({ "pbr": "Mine" })");

    EXPECT_TRUE(SetParameter(meta, "mesh", ""));
    EXPECT_EQ(ParameterOf(meta, "mesh"), "");
    EXPECT_TRUE(SetParameter(meta, "materialKinds", ""));
    EXPECT_TRUE(meta.parameters.empty()) << "nothing left is no parameters at all";
    EXPECT_FALSE(SetParameter(meta, "nothing", ""));
}

TEST(Parameters, TheImportersSettings)
{
    MeshImport mesh;
    mesh.leaveOut = { "BONE_IDS", "BONE_WEIGHTS" };
    mesh.buffers = { { "POSITION" }, { "NORMAL", "UV" } };
    EXPECT_EQ(MeshImportFrom(ToJson(mesh)), mesh);
    EXPECT_EQ(MeshImportFrom("{}"), MeshImport {});
    EXPECT_FALSE(MeshImportFrom(R"({ "leaveOut": [1] })"));

    const TextureImport texture { TextureFormat::eBC5, TextureQuality::eBest };
    EXPECT_EQ(TextureImportFrom(ToJson(texture)), texture);
    EXPECT_FALSE(TextureImportFrom(R"({ "format": "BC99" })"));

    const std::map<std::string, std::string> kinds { { "pbr", "PbrMaterial" }, { "classic", "Toon" } };
    EXPECT_EQ(KindsFrom(ToJson(kinds)), kinds);
}
