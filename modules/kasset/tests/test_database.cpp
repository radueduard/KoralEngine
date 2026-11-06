// A project's assets on disk: found, given metas, followed when moved, and named by the old references.

#include <gtest/gtest.h>

#include <kasset/database.h>

#include <fstream>
#include <format>

using namespace kasset;
namespace fs = std::filesystem;

namespace {
    /** A project folder of its own, taken away after. */
    struct Project : ::testing::Test {
        fs::path root;

        void SetUp() override
        {
            root = fs::temp_directory_path() / std::format("kasset-{}", Id::New().Text());
            fs::create_directories(root);
        }
        void TearDown() override { fs::remove_all(root); }

        void File(const fs::path& path, const std::string_view text = "x")
        {
            fs::create_directories((root / path).parent_path());
            std::ofstream(root / path, std::ios::binary) << text;
        }
    };
}

TEST_F(Project, EveryAssetIsGivenAMeta)
{
    File("assets/textures/wall.png");
    File("assets/models/ship/Ship.gltf");
    File("assets/models/ship/Ship.bin");
    File("shaders/lit.slang");
    File("scripts/Spin.cs");
    File("scenes/main.scene.json");
    File("project.json");
    File(".vscode/settings.json");
    File("cache/textures/abc.ktx2");
    File("scripts/bin/Debug/x.png");

    Database database(root);
    EXPECT_TRUE(database.Scan().empty());
    EXPECT_EQ(database.Size(), 5u);
    EXPECT_TRUE(fs::exists(root / "assets/textures/wall.png.meta"));
    EXPECT_FALSE(fs::exists(root / "assets/models/ship/Ship.bin.meta"));
    EXPECT_FALSE(fs::exists(root / "cache/textures/abc.ktx2.meta"));
    EXPECT_FALSE(fs::exists(root / "scripts/bin/Debug/x.png.meta"));

    const Entry* wall = database.FindFile("assets/textures/wall.png");
    ASSERT_TRUE(wall);
    EXPECT_EQ(wall->meta.kind, Kind::eTexture);
    EXPECT_EQ(database.FindFile(root / "assets/textures/wall.png"), wall);
    EXPECT_EQ(database.Find(wall->meta.id), wall);
}

TEST_F(Project, AnIdStaysAcrossScans)
{
    File("assets/wall.png");
    Id id;
    {
        Database database(root);
        database.Scan();
        id = database.FindFile("assets/wall.png")->meta.id;
    }
    Database again(root);
    EXPECT_TRUE(again.Scan().empty());
    EXPECT_EQ(again.FindFile("assets/wall.png")->meta.id, id);
}

TEST_F(Project, AFileMovedWithItsMetaKeepsItsId)
{
    File("assets/wall.png");
    Database database(root);
    database.Scan();
    const Id id = database.FindFile("assets/wall.png")->meta.id;

    fs::create_directories(root / "assets/textures");
    fs::rename(root / "assets/wall.png", root / "assets/textures/brick.png");
    fs::rename(root / "assets/wall.png.meta", root / "assets/textures/brick.png.meta");
    EXPECT_TRUE(database.Scan().empty());
    EXPECT_EQ(database.FindFile("assets/textures/brick.png")->meta.id, id);
}

TEST_F(Project, AFileMovedWithoutItsMetaIsFoundByItsName)
{
    File("assets/wall.png");
    Database database(root);
    database.Scan();
    const Id id = database.FindFile("assets/wall.png")->meta.id;

    fs::create_directories(root / "assets/textures");
    fs::rename(root / "assets/wall.png", root / "assets/textures/wall.png");      // as `git mv` of the file alone does
    EXPECT_TRUE(database.Scan().empty());
    EXPECT_EQ(database.FindFile("assets/textures/wall.png")->meta.id, id);
    EXPECT_FALSE(fs::exists(root / "assets/wall.png.meta"));
    EXPECT_TRUE(fs::exists(root / "assets/textures/wall.png.meta"));
}

TEST_F(Project, AMetaOfNoFileIsSaidOf)
{
    File("assets/wall.png");
    Database database(root);
    database.Scan();
    fs::remove(root / "assets/wall.png");
    const auto problems = database.Scan();
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("gone"), std::string::npos);
    EXPECT_EQ(database.Size(), 0u);
}

TEST_F(Project, ACopyIsAnotherAsset)
{
    File("assets/a/wall.png");
    Database database(root);
    database.Scan();
    fs::create_directories(root / "assets/b");
    fs::copy_file(root / "assets/a/wall.png", root / "assets/b/wall.png");
    fs::copy_file(root / "assets/a/wall.png.meta", root / "assets/b/wall.png.meta");

    EXPECT_EQ(database.Scan().size(), 1u);
    const Entry* a = database.FindFile("assets/a/wall.png");
    const Entry* b = database.FindFile("assets/b/wall.png");
    ASSERT_TRUE(a && b);
    EXPECT_NE(a->meta.id, b->meta.id);
    EXPECT_TRUE(Database(root).Scan().empty());     // and stays so
}

TEST_F(Project, AModelsPartsAreFoundAgainByName)
{
    File("assets/Ship.gltf");
    Database database(root);
    database.Scan();
    const Id ship = database.FindFile("assets/Ship.gltf")->meta.id;

    const auto first = database.RecordParts(ship, Kind::eMesh, { "Hull", "Wheel", "Wheel", "Mast" });
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->gone.empty());

    // Blender wrote them in another order, and the mast is gone.
    const auto second = database.RecordParts(ship, Kind::eMesh, { "Wheel", "Hull", "Wheel" });
    ASSERT_TRUE(second);
    EXPECT_EQ(second->ids[0], first->ids[1]);
    EXPECT_EQ(second->ids[1], first->ids[0]);
    EXPECT_EQ(second->ids[2], first->ids[2]);
    ASSERT_EQ(second->gone.size(), 1u);
    EXPECT_EQ(second->gone[0].name, "Mast");

    // And it is so in the meta, read again.
    Database again(root);
    again.Scan();
    const Meta& meta = again.FindFile("assets/Ship.gltf")->meta;
    EXPECT_EQ(meta.PartAt(Kind::eMesh, 1)->id, first->ids[0]);
    EXPECT_EQ(meta.FindPart(Kind::eMesh, "Mast"), nullptr);
}

TEST_F(Project, APartRenamedInItsFileIsFoundWhereItWas)
{
    File("assets/Ship.gltf");
    Database database(root);
    database.Scan();
    const Id ship = database.FindFile("assets/Ship.gltf")->meta.id;
    const auto first = database.RecordParts(ship, Kind::eMesh, { "Hull", "Sail" });
    const auto second = database.RecordParts(ship, Kind::eMesh, { "Hull", "Mainsail" });
    ASSERT_TRUE(first && second);
    EXPECT_EQ(second->ids, first->ids);
    EXPECT_TRUE(second->gone.empty());
}

TEST_F(Project, OldReferencesAreTranslated)
{
    File("assets/textures/wall.png");
    File("assets/sponza/Sponza.gltf");
    File("assets/Red.kmat");
    File("shaders/lit.slang");
    File("scripts/Spin.cs");
    File("prefabs/Tree.prefab");
    File("scenes/main.scene.json");
    Database database(root);
    database.Scan();
    const Id cube = database.AddBuiltIn(Kind::eMesh, "mesh/cube", "$cube");
    const Id gold = database.AddBuiltIn(Kind::eMaterial, "material/Gold", "material:Gold");

    const auto roundTrip = [&](const std::string_view legacy) {
        const auto reference = database.FromLegacy(legacy);
        EXPECT_TRUE(reference) << legacy;
        if (reference) EXPECT_EQ(database.ToLegacy(*reference), legacy);
        return reference.value_or(Reference {});
    };
    EXPECT_EQ(roundTrip("@textures/wall.png").asset, database.FindFile("assets/textures/wall.png")->meta.id);
    EXPECT_EQ(roundTrip("@Red.kmat").asset, database.FindFile("assets/Red.kmat")->meta.id);
    const Reference mesh = roundTrip("@sponza/Sponza.gltf#3");
    const Reference material = roundTrip("@sponza/Sponza.gltf#material:2");
    EXPECT_EQ(mesh.asset, material.asset);
    EXPECT_NE(mesh.part, material.part);
    EXPECT_EQ(database.FromLegacy("@sponza/Sponza.gltf#3"), mesh);    // the same part each time
    EXPECT_EQ(roundTrip("$cube").asset, cube);
    EXPECT_EQ(roundTrip("material:Gold").asset, gold);
    EXPECT_EQ(roundTrip("material:Gold").asset, Id::Named("material/Gold"));
    roundTrip("shader:lit");
    EXPECT_TRUE(roundTrip("shader:lit#vertex").part);
    roundTrip("script:Spin.cs");
    roundTrip("prefab:Tree.prefab");
    roundTrip("scene:main");

    // Its parts, given IDs, are named when the file is read: the IDs stay.
    const auto parts = database.RecordParts(mesh.asset, Kind::eMesh, { "a", "b", "c", "d" });
    ASSERT_TRUE(parts);
    EXPECT_EQ(parts->ids[3], mesh.part);

    EXPECT_FALSE(database.FromLegacy("@elsewhere/nothing.png"));
    EXPECT_FALSE(database.FromLegacy("$script/mine"));
    EXPECT_FALSE(database.FromLegacy("@sponza/Sponza.gltf#x"));
    EXPECT_FALSE(database.FromLegacy("Albedo"));
}

TEST_F(Project, AFileAddedLater)
{
    Database database(root);
    database.Scan();
    File("assets/wall.png");
    const auto added = database.Add(root / "assets/wall.png");
    ASSERT_TRUE(added) << added.error();
    EXPECT_EQ(database.Add("assets/wall.png").value(), *added);
    EXPECT_FALSE(database.Add(fs::temp_directory_path() / "outside.png"));
    File("assets/Ship.bin");
    EXPECT_FALSE(database.Add("assets/Ship.bin"));
}

TEST_F(Project, WhatNamesWhat)
{
    File("assets/wall.png");
    File("assets/Ship.gltf");
    File("assets/unused.png");
    Database database(root);
    database.Scan();
    const Id wall = database.FindFile("assets/wall.png")->meta.id;
    const Id ship = database.FindFile("assets/Ship.gltf")->meta.id;
    const auto hull = database.PartAt(ship, Kind::eMesh, 0);
    ASSERT_TRUE(hull);
    // A scene saved before IDs, by the old names; a prefab saved since, by the IDs; a material of the project's.
    File("scenes/main.scene.json", R"({ "world": { "a": "@wall.png", "b": "@Ship.gltf#0", "label": "@wall.png is a picture" } })");
    File("prefabs/Boat.prefab", std::format(R"({{ "root": {{ "mesh": "{}" }} }})", Reference { ship, *hull }.Text()));
    File("assets/Red.kmat", R"({ "values": { "baseColorMap": "@wall.png" } })");
    File("prefabs/Painted.prefab", R"({ "root": { "assets": "PbrMaterial @Ship.gltf#material:0" } })");
    database.Scan();
    database.IndexReferences();

    const Id scene = database.FindFile("scenes/main.scene.json")->meta.id;
    const Id boat = database.FindFile("prefabs/Boat.prefab")->meta.id;
    const Id red = database.FindFile("assets/Red.kmat")->meta.id;
    EXPECT_EQ(database.Users(wall), (std::vector<Id> { red, scene }));
    const Id painted = database.FindFile("prefabs/Painted.prefab")->meta.id;
    EXPECT_EQ(database.Users(ship), (std::vector<Id> { boat, painted, scene })) << "a part named is its asset used — by a material's link too";
    EXPECT_TRUE(database.Users(database.FindFile("assets/unused.png")->meta.id).empty());
    EXPECT_EQ(database.Uses(boat), (std::vector<Reference> { { ship, *hull } }));

    // Lookup changes nothing: a part with no ID yet is its asset's.
    EXPECT_EQ(database.Lookup("@Ship.gltf#7"), (Reference { ship, {} }));
    EXPECT_EQ(database.FindFile("assets/Ship.gltf")->meta.parts.size(), 1u);
    EXPECT_FALSE(database.Lookup("@nothing.png"));
    EXPECT_FALSE(database.Lookup(Id::New().Text())) << "an ID of no asset";

    // Saved again, naming nothing: it is read again.
    File("assets/Red.kmat", R"({ "values": {} })");
    database.Reindex(red);
    EXPECT_EQ(database.Users(wall), (std::vector<Id> { scene }));
}

TEST_F(Project, AnAssetMovedGoesWithItsMetaAndWhatItNames)
{
    File("assets/models/ship/Ship.gltf", R"({ "buffers": [ { "uri": "Ship.bin" } ], "images": [ { "uri": "textures/hull.png" } ] })");
    File("assets/models/ship/Ship.bin");
    File("assets/models/ship/textures/hull.png");
    File("assets/wall.png");
    Database database(root);
    database.Scan();
    const Id ship = database.FindFile("assets/models/ship/Ship.gltf")->meta.id;
    const Id hull = database.FindFile("assets/models/ship/textures/hull.png")->meta.id;

    ASSERT_TRUE(database.Move(ship, "assets/models/boat/Boat.gltf"));
    EXPECT_EQ(database.Find(ship)->file, "assets/models/boat/Boat.gltf");
    EXPECT_EQ(database.FindFile("assets/models/boat/Boat.gltf")->meta.id, ship);
    EXPECT_FALSE(database.FindFile("assets/models/ship/Ship.gltf"));
    for (const char* moved : { "Boat.gltf", "Boat.gltf.meta", "Ship.bin", "textures/hull.png", "textures/hull.png.meta" })
        EXPECT_TRUE(fs::exists(root / "assets/models/boat" / moved)) << moved;
    EXPECT_EQ(database.Find(hull)->file, "assets/models/boat/textures/hull.png") << "a picture it names went with it, and is the same asset";
    EXPECT_TRUE(Database(root).Scan().empty()) << "as the folders have it";

    const Id wall = database.FindFile("assets/wall.png")->meta.id;
    EXPECT_FALSE(database.Move(wall, "assets/models/boat/Boat.gltf")) << "not over what is there";
    EXPECT_FALSE(database.Move(wall, "assets/wall.obj")) << "not another kind of file";
    EXPECT_FALSE(database.Move(wall, fs::temp_directory_path() / "out.png")) << "not out of the project";
    ASSERT_TRUE(database.Move(wall, root / "assets/textures/wall.png"));
    EXPECT_EQ(database.Lookup("@textures/wall.png"), (Reference { wall, {} }));
}

TEST_F(Project, WhatNamesAFileByWhereItIsNamesItByItsIdBeforeItMoves)
{
    File("assets/wall.png");
    File("scenes/main.scene.json", R"({ "a": "@wall.png", "b": "@other.png" })");
    Database database(root);
    database.Scan();
    database.IndexReferences();
    const Id wall = database.FindFile("assets/wall.png")->meta.id;
    const auto written = database.KeepUsersByIds(wall);
    ASSERT_TRUE(written) << written.error();
    ASSERT_EQ(written->size(), 1u);
    std::ifstream in(root / "scenes/main.scene.json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, std::format(R"({{ "a": "{}", "b": "@other.png" }})", wall.Text()));
    ASSERT_TRUE(database.Move(wall, "assets/brick.png"));
    EXPECT_EQ(database.ToLegacy({ wall, {} }), "@brick.png");
}

// A module's assets are the project's too: found after its own, by the IDs their metas give them, named from the
// project's folder — and its scenes, for trying it out, are not among them.
TEST_F(Project, AModulesAssetsAreTheProjectsByTheirIds)
{
    const fs::path game = root / "game", physics = root / "physics";
    const auto file = [](const fs::path& path, const std::string_view text = "x") {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    };
    file(game / "project.json");
    file(game / "assets/textures/grass.png");
    file(physics / "project.json");
    file(physics / "assets/textures/rubber.png");
    file(physics / "scripts/RigidBody.kt");
    file(physics / "scenes/test.scene.json");

    // The module on its own, first: its metas, its IDs.
    Database own(physics);
    EXPECT_TRUE(own.Scan().empty());
    const Entry* rubberOwn = own.FindFile("assets/textures/rubber.png");
    ASSERT_NE(rubberOwn, nullptr);
    const Id rubber = rubberOwn->meta.id;

    Database assets(game);
    assets.SetModules({ physics });
    for (const auto& problem : assets.Scan()) ADD_FAILURE() << problem;
    const Entry* found = assets.Find(rubber);
    ASSERT_NE(found, nullptr) << "a module's asset, by the ID it has in the module";
    EXPECT_EQ(found->file.generic_string(), "../physics/assets/textures/rubber.png");
    EXPECT_EQ(assets.ModuleOf(physics / "assets/textures/rubber.png"), physics);
    EXPECT_FALSE(assets.ModuleOf(game / "assets/textures/grass.png"));
    EXPECT_NE(assets.FindFile(physics / "scripts/RigidBody.kt"), nullptr) << "its scripts too";
    EXPECT_EQ(assets.FindFile(physics / "scenes/test.scene.json"), nullptr) << "but not its scenes";
    EXPECT_EQ(assets.FindFile(physics / "project.json"), nullptr);
    EXPECT_NE(assets.FindFile(game / "assets/textures/grass.png"), nullptr) << "and the project's own as before";

    // What the runtime loads it by is its whole path; and that is found again as the same asset.
    const std::string named = assets.ToLegacy({ rubber, {} });
    EXPECT_EQ(named, "@" + (physics / "assets/textures/rubber.png").lexically_normal().generic_string());
    const auto again = assets.FromLegacy(named);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->asset, rubber);
}
