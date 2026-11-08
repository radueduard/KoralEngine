// Metas: written as JSON and read back, and what is no meta.

#include <gtest/gtest.h>

#include <kasset/meta.h>

#include <format>

using namespace kasset;

TEST(Meta, KindsOfFiles)
{
    EXPECT_EQ(KindOfFile("models/ship/Ship.GLTF"), Kind::eModel);
    EXPECT_EQ(KindOfFile("wall.png"), Kind::eTexture);
    EXPECT_EQ(KindOfFile("sky.hdr"), Kind::eTexture);
    EXPECT_EQ(KindOfFile("lit.slang"), Kind::eShader);
    EXPECT_EQ(KindOfFile("Spin.cs"), Kind::eScript);
    EXPECT_EQ(KindOfFile("Spin.kt"), Kind::eScript);
    EXPECT_EQ(KindOfFile("main.scene.json"), Kind::eScene);
    EXPECT_EQ(KindOfFile("Tree.prefab"), Kind::ePrefab);
    EXPECT_EQ(KindOfFile("bloom.kfunc"), Kind::eFunction);
    EXPECT_EQ(KindOfFile("Red.kmat"), Kind::eMaterial);
    EXPECT_EQ(KindOfFile("sphere.kgen"), Kind::eGenerated);
    EXPECT_FALSE(KindOfFile("Ship.bin"));
    EXPECT_FALSE(KindOfFile("Ship.mtl"));
    EXPECT_FALSE(KindOfFile("wall.png.meta"));
    EXPECT_FALSE(KindOfFile("project.json"));
}

TEST(Meta, KeptInTheProjectsMetaFolder)
{
    // From the project's folder, and whole: in the same folders under .koral/meta.
    EXPECT_EQ(MetaPathOf({}, "assets/wall.png"), std::filesystem::path(".koral/meta/assets/wall.png.meta"));
    const std::filesystem::path root = std::filesystem::path("C:/game").lexically_normal();
    EXPECT_EQ(MetaPathOf(root, root / "scripts/solver/Crew.kt"), root / ".koral/meta/scripts/solver/Crew.kt.meta");
    EXPECT_EQ(FileOfMeta(root, root / ".koral/meta/scripts/solver/Crew.kt.meta"), root / "scripts/solver/Crew.kt");
    // A generator is its own; and where metas were kept before, beside their files.
    EXPECT_EQ(MetaPathOf(root, "meshes/sphere.kgen"), std::filesystem::path("meshes/sphere.kgen"));
    EXPECT_EQ(MetaBeside("assets/wall.png"), std::filesystem::path("assets/wall.png.meta"));
    EXPECT_TRUE(IsMeta("wall.png.meta"));
}

TEST(Meta, WrittenAndReadBack)
{
    Meta meta;
    meta.id = Id::New();
    meta.kind = Kind::eModel;
    meta.producer = "model@1";
    meta.parameters = R"({ "layout": "positions apart", "materialKind": { "pbr": "PbrMaterial" } })";
    meta.parts = { { Id::New(), Kind::eMesh, "Hull \"main\"", 0 }, { Id::New(), Kind::eMaterial, "Paint", 0 }, { Id::New(), Kind::eShaderEntry, "vertex", -1 } };
    meta.overrides = R"({ "Paint": { "roughness": 0.25 } })";

    const auto read = Read(Write(meta));
    ASSERT_TRUE(read) << read.error();
    EXPECT_EQ(*read, meta);
}

TEST(Meta, ItsLeastIsAnIdAndAKind)
{
    Meta meta;
    meta.id = Id::New();
    const std::string text = Write(meta);
    EXPECT_EQ(text.find("parts"), std::string::npos);
    EXPECT_EQ(text.find("parameters"), std::string::npos);
    const auto read = Read(text);
    ASSERT_TRUE(read) << read.error();
    EXPECT_EQ(*read, meta);
}

TEST(Meta, WhatIsNoMeta)
{
    EXPECT_FALSE(Read("not json"));
    EXPECT_FALSE(Read("[]"));
    EXPECT_FALSE(Read(R"({ "kind": "mesh" })"));
    EXPECT_FALSE(Read(R"({ "id": "12", "kind": "mesh" })"));
    EXPECT_FALSE(Read(std::format(R"({{ "id": "{}", "kind": "spaceship" }})", Id::New().Text())));
    const std::string part = Id::New().Text();
    EXPECT_FALSE(Read(std::format(R"({{ "id": "{}", "kind": "model", "parts": [ {{ "id": "{}", "kind": "mesh" }}, {{ "id": "{}", "kind": "mesh" }} ] }})", Id::New().Text(), part, part)));
}

TEST(Meta, FindingParts)
{
    Meta meta;
    meta.parts = { { Id::New(), Kind::eMesh, "Wheel", 0 }, { Id::New(), Kind::eMesh, "Wheel", 1 }, { Id::New(), Kind::eMaterial, "Wheel", 0 } };
    EXPECT_EQ(meta.FindPart(Kind::eMesh, "Wheel"), &meta.parts[0]);
    EXPECT_EQ(meta.FindPart(Kind::eMesh, "Wheel", 1), &meta.parts[1]);
    EXPECT_EQ(meta.FindPart(Kind::eMesh, "Wheel", 2), nullptr);
    EXPECT_EQ(meta.FindPart(Kind::eMaterial, "Wheel"), &meta.parts[2]);
    EXPECT_EQ(meta.PartAt(Kind::eMesh, 1), &meta.parts[1]);
    EXPECT_EQ(meta.FindPart(meta.parts[2].id), &meta.parts[2]);
}
