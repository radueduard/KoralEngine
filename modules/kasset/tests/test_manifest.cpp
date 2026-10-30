// What a game is made of: what its scenes reach, and nothing they do not.

#include <gtest/gtest.h>

#include <kasset/database.h>
#include <kasset/manifest.h>

#include <algorithm>
#include <format>
#include <fstream>

using namespace kasset;
namespace fs = std::filesystem;

TEST(Manifest, WhatTheScenesReachAndNothingElse)
{
    const fs::path root = fs::temp_directory_path() / std::format("kasset-manifest-{}", Id::New().Text());
    const auto file = [&](const fs::path& path, const std::string_view text = "x") {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path) << text;
    };
    file("project.json", "{}");
    file("assets/models/ship/Ship.gltf", R"({ "buffers": [ { "uri": "Ship.bin" } ] })");
    file("assets/models/ship/Ship.bin");
    file("assets/textures/wall.png");
    file("assets/textures/unused.png");
    file("assets/Red.kmat", R"({ "values": { "baseColorMap": "@textures/wall.png" } })");
    file("prefabs/Boat.prefab", R"({ "root": { "mesh": "@models/ship/Ship.gltf#0" } })");
    file("scenes/main.scene.json", R"({ "world": { "a": "@Red.kmat", "b": "prefab:Boat.prefab" } })");
    file("scenes/other.scene.json", R"({ "world": { "a": "@textures/unused.png" } })");
    file("shaders/lit.slang");
    file("scripts/Spin.cs");
    Database assets(root);
    assets.Scan();
    const Id main = assets.FindFile("scenes/main.scene.json")->meta.id;

    const Manifest manifest = Reach(assets, { main });
    const auto has = [&](const char* path) { return std::ranges::contains(manifest.files, fs::path(path)); };
    for (const char* path : { "scenes/main.scene.json", "scenes/main.scene.json.meta", "assets/Red.kmat", "assets/textures/wall.png", "assets/textures/wall.png.meta",
                              "prefabs/Boat.prefab", "assets/models/ship/Ship.gltf", "assets/models/ship/Ship.gltf.meta", "assets/models/ship/Ship.bin",
                              "project.json", "shaders/lit.slang", "scripts/Spin.cs" })
        EXPECT_TRUE(has(path)) << path;
    EXPECT_FALSE(has("assets/textures/unused.png")) << "what no scene of it has";
    EXPECT_FALSE(has("scenes/other.scene.json"));
    EXPECT_EQ(manifest.assets.front(), main);

    const auto read = ReadManifest(Write(manifest));
    ASSERT_TRUE(read) << read.error();
    EXPECT_EQ(read->files, manifest.files);
    EXPECT_EQ(read->assets, manifest.assets);
    fs::remove_all(root);
}
