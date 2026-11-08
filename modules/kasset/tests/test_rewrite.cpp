// A file's references written as IDs and read back as where the assets are now.

#include <gtest/gtest.h>

#include <kasset/database.h>
#include <kasset/rewrite.h>

#include <format>
#include <fstream>

using namespace kasset;
namespace fs = std::filesystem;

TEST(Rewrite, OnlyStringValues)
{
    const std::string json = R"({ "a": "x", "x": ["x", 1, "y \"quoted\" x"], "n": { "x": "z" } })";
    const auto out = RewriteStrings(json, [](const std::string_view s) -> std::optional<std::string> { return s == "x" ? std::optional<std::string>("X\n") : std::nullopt; });
    EXPECT_EQ(out, R"({ "a": "X\n", "x": ["X\n", 1, "y \"quoted\" x"], "n": { "x": "z" } })");
}

TEST(Rewrite, AMovedFileIsFoundByItsId)
{
    const fs::path root = fs::temp_directory_path() / std::format("kasset-rewrite-{}", Id::New().Text());
    fs::create_directories(root / "assets" / "textures");
    std::ofstream(root / "assets" / "textures" / "wall.png") << "x";
    std::ofstream(root / "assets" / "ship.gltf") << "x";
    {
        Database assets(root);
        assets.Scan();
        assets.AddBuiltIn(Kind::eMesh, "mesh/cube", "$cube");
        const std::string scene = R"({ "mesh": "@ship.gltf#2", "picture": "@textures/wall.png", "shape": "$cube", "name": "@nothing.png", "label": "wall", "assets": "PbrMaterial @ship.gltf#material:1\nToon $cube" })";
        const std::string kept = ToIds(scene, assets);
        EXPECT_EQ(kept.find("@ship"), std::string::npos) << kept;
        EXPECT_NE(kept.find("$cube"), std::string::npos) << "the engine's own keep their names";
        EXPECT_NE(kept.find("@nothing.png"), std::string::npos) << "what is no asset is as it is";
        EXPECT_NE(kept.find("\"wall\""), std::string::npos);
        EXPECT_EQ(FromIds(kept, assets), scene) << "read back, it is as it was";
        std::ofstream(root / "scene.json") << kept;
    }
    // The picture moved, and the model renamed, both with their metas (kept under .koral/meta): read again, the scene
    // names them where they are.
    fs::create_directories(root / "assets" / "brick");
    fs::rename(root / "assets" / "textures" / "wall.png", root / "assets" / "brick" / "red.png");
    fs::create_directories(root / ".koral/meta/assets/brick");
    fs::rename(root / ".koral/meta/assets/textures/wall.png.meta", root / ".koral/meta/assets/brick/red.png.meta");
    fs::rename(root / "assets" / "ship.gltf", root / "assets" / "boat.gltf");
    fs::rename(root / ".koral/meta/assets/ship.gltf.meta", root / ".koral/meta/assets/boat.gltf.meta");
    Database assets(root);
    assets.Scan();
    assets.AddBuiltIn(Kind::eMesh, "mesh/cube", "$cube");
    std::ifstream in(root / "scene.json");
    const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t missing = 0;
    EXPECT_EQ(FromIds(kept, assets, &missing), R"({ "mesh": "@boat.gltf#2", "picture": "@brick/red.png", "shape": "$cube", "name": "@nothing.png", "label": "wall", "assets": "PbrMaterial @boat.gltf#material:1\nToon $cube" })");
    EXPECT_EQ(missing, 0u);
    fs::remove(root / "assets" / "boat.gltf");
    fs::remove(root / ".koral/meta/assets/boat.gltf.meta");
    assets.Scan();
    (void)FromIds(kept, assets, &missing);
    EXPECT_EQ(missing, 2u) << "what is gone is said of: the mesh, and the material that was of it";
    fs::remove_all(root);
}
