// A scene made through the document — as the editor's panels make it — then saved, and run again from the file by
// the scene a project runs in. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>
#include <ranges>

#include <kgraph/meshes.h>
#include <keditor/editor.h>
#include <keditor/play.h>
#include <log.h>

#include "scene.h"
#include "support.h"

namespace {
    using support::Frame;
    using keditor::PlayScene;

    /** A play scene, with the screen copied where the test can read it. */
    struct Running {
        PlayScene* scene = nullptr;
        kor::Resource<kor::Buffer> readback;

        explicit Running(const std::filesystem::path& file = {}) {
            scene = support::App()->OpenOffscreen<PlayScene>({ .title = "play", .extent = { support::Size, support::Size } }, file);
            readback = kor::Buffer::RawBuilder{}.SetRawSize(support::Size * support::Size * 4).SetUsage(kor::Buffer::Usage::eTransferDst)
                .SetType(kor::Buffer::Type::eReadback).Build();
            scene->Graph().Add<support::ReadScreen>(kor::ResourceRef<const kor::Buffer>(readback));
        }
        ~Running() { support::App()->Close(*scene); Frame(); }
        [[nodiscard]] std::vector<kor::U8Vec4> Picture() const { return readback->Read<kor::U8Vec4>(support::Size * support::Size); }
        [[nodiscard]] kor::U8Vec4 Quarter(const int quarter) const { return Picture()[static_cast<std::size_t>((quarter / 2 ? 24 : 8) * support::Size + (quarter % 2 ? 24 : 8))]; }
        void ExpectQuarters(const char* when) const {
            for (int quarter = 0; quarter < 4; ++quarter) {
                const auto pixel = Quarter(quarter);
                const auto& color = scene::Colors[static_cast<std::size_t>(quarter)];
                EXPECT_NEAR(pixel.x, color[0], 1) << when << ", quarter " << quarter;
                EXPECT_NEAR(pixel.y, color[1], 1) << when << ", quarter " << quarter;
                EXPECT_NEAR(pixel.z, color[2], 1) << when << ", quarter " << quarter;
            }
        }
    };

    class DocumentTest : public ::testing::Test {
    protected:
        void SetUp() override { if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason(); }
    };
}

// The plan's gate: E4's scene built from nothing, saved, reopened, and running the same outside the editor.
TEST_F(DocumentTest, ASceneBuiltFromNothingIsSavedAndRunsTheSameFromItsFile) {
    const keditor::Project project = scene::MakeProject("gate");
    std::vector<kor::U8Vec4> made;
    {
        Running editing;
        keditor::Document& doc = editing.scene->Doc();
        EXPECT_FALSE(doc.Dirty());
        const auto logged = kor::log::LastSequence();
        scene::BuildGraph(doc);
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        // Made a step at a time, most nodes could not be built at first — an image with no size yet, a pipeline with
        // no shader. Each said so on itself; none of it went to the log.
        for (const auto& record : kor::log::HistorySince(logged)) ADD_FAILURE() << "logged while building: " << record.message;
        scene::BuildEntities(doc);
        EXPECT_TRUE(doc.Dirty());
        Frame();
        Frame();
        EXPECT_EQ(doc.Runtime().ReportOf("draw")->ran, 4u) << doc.Runtime().ReportOf("draw")->why;
        editing.ExpectQuarters("as built");
        made = editing.Picture();

        std::string error;
        ASSERT_TRUE(doc.Save(project.ScenePath("main"), error)) << error;
        EXPECT_FALSE(doc.Dirty());
    }

    // As a project is run: the file, and nothing of the session that made it.
    Running playing(project.ScenePath("main"));
    ASSERT_TRUE(playing.scene->Error().empty()) << playing.scene->Error();
    keditor::Document& doc = playing.scene->Doc();
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
    for (const auto& note : doc.Notes()) ADD_FAILURE() << note;
    Frame();
    Frame();
    playing.ExpectQuarters("from the file");
    EXPECT_EQ(playing.Picture(), made) << "the same picture, to the pixel";

    // And all of it is there to go on working with.
    EXPECT_FALSE(doc.Dirty());
    const auto entities = doc.Entities();
    ASSERT_EQ(entities.size(), 4u);
    EXPECT_EQ(doc.NameOf(entities[2]), "Quarter 2");
    EXPECT_EQ(doc.ComponentsOf(entities[2]).size(), 4u);
    EXPECT_EQ(doc.Field(entities[2], "stamp", "gain"), kgraph::Value(0.5));
    EXPECT_EQ(doc.Field(entities[2], "stamp", "target"), kgraph::Value("image 2"));
    EXPECT_EQ(doc.Field(entities[2], "Mesh", "mesh"), kgraph::Value("square"));
    EXPECT_EQ(doc.Graph().Find("run")->x, 400);
    EXPECT_NE(doc.World().Find("Hidden"), kecs::NoComponent) << "a tag nothing has yet is kept too";
    EXPECT_EQ(doc.ToJson(), [&] { std::ifstream in(project.ScenePath("main")); return std::string(std::istreambuf_iterator<char>(in), {}); }())
        << "saved again, it is the same file";
}

TEST_F(DocumentTest, AnEditShowsInTheNextFrame) {
    scene::MakeProject("edits");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    scene::BuildGraph(doc);
    const auto entities = scene::BuildEntities(doc);
    Frame();
    Frame();
    editing.ExpectQuarters("to begin with");

    // An entity's value.
    const auto revision = doc.Revision();
    EXPECT_TRUE(doc.SetField(entities[0], "stamp", "tint", kgraph::Value::List { 0, 0, 1, 1 }));
    EXPECT_GT(doc.Revision(), revision);
    // A tag, a component switched off, an entity gone.
    EXPECT_TRUE(doc.AddComponent(entities[3], "Hidden"));
    EXPECT_FALSE(doc.AddComponent(entities[3], "Hidden")) << "it has it";
    EXPECT_TRUE(doc.SetEnabled(entities[1], "show", false));
    EXPECT_FALSE(doc.IsEnabled(entities[1], "show"));
    EXPECT_TRUE(doc.DestroyEntity(entities[2]));
    Frame();
    Frame();
    const auto near = [](const kor::U8Vec4 pixel, const int r, const int g, const int b) { return std::abs(pixel.x - r) <= 1 && std::abs(pixel.y - g) <= 1 && std::abs(pixel.z - b) <= 1; };
    EXPECT_TRUE(near(editing.Quarter(0), 0, 0, 255)) << "its new colour";
    EXPECT_TRUE(near(editing.Quarter(1), 0, 0, 0)) << "switched off";
    EXPECT_TRUE(near(editing.Quarter(2), 0, 0, 0)) << "destroyed";
    EXPECT_TRUE(near(editing.Quarter(3), 0, 0, 0)) << "hidden";

    // A node's value: the backdrop every entity is stamped over.
    EXPECT_TRUE(doc.SetInput("tint backdrop", "color", kgraph::Value::List { 0.5, 0, 0, 0 }));
    Frame();
    Frame();
    EXPECT_TRUE(near(editing.Quarter(0), 128, 0, 255)) << "over a red backdrop now";

    // A wire taken off, and what that leaves is said; put back, it runs again.
    EXPECT_TRUE(doc.ClearInput("draw", "pipeline"));
    EXPECT_FALSE(doc.Runtime().Status("draw")->ok);
    EXPECT_TRUE(doc.Connect("show", "out", "draw", "pipeline"));
    EXPECT_TRUE(doc.Runtime().Ok());

    // A node taken out takes its wires with it.
    doc.RemoveNodes({ "blit" });
    EXPECT_EQ(doc.Graph().Find("blit"), nullptr);
    EXPECT_TRUE(doc.Runtime().Ok());
    ASSERT_TRUE(doc.Graph().Find("stamp")->inputs.contains("param.backdrop"));
    doc.RemoveNodes({ "backdrop" });
    EXPECT_FALSE(doc.Graph().Find("stamp")->inputs.contains("param.backdrop")) << "what was wired to it is wired to nothing";
    EXPECT_TRUE(doc.Runtime().Status("stamp")->ok) << "and the backdrop is each entity's to supply now";
    EXPECT_NE(doc.World().Describe(doc.Runtime().ComponentOf("stamp")).FindField("backdrop"), nullptr);
    EXPECT_EQ(doc.AddNode("Blit", 0, 0), "blit");
    EXPECT_EQ(doc.AddNode("Blit", 0, 0), "blit 2") << "an id of its own";
    EXPECT_EQ(doc.AddNode("NoSuchThing", 0, 0), "");
}

TEST_F(DocumentTest, AnEntitysFieldsAreReadAndSetAsValues) {
    scene::MakeProject("fields");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    scene::BuildGraph(doc);
    const kecs::Entity entity = doc.CreateEntity("One");
    EXPECT_EQ(doc.NameOf(entity), "One");
    EXPECT_TRUE(doc.Rename(entity, "Renamed"));
    EXPECT_EQ(doc.NameOf(entity), "Renamed");
    EXPECT_TRUE(doc.ComponentsOf(entity).empty()) << "its name is not shown as a component";

    // What it can be given: the pipelines' components, the mesh, and tags as they are made.
    doc.AddTag("Lit");
    const auto names = doc.ComponentNames();
    for (const char* name : { "stamp", "show", "Mesh", "Lit" }) EXPECT_NE(std::ranges::find(names, name), names.end()) << name;
    EXPECT_EQ(std::ranges::find(names, "Name"), names.end());

    ASSERT_TRUE(doc.AddComponent(entity, "stamp"));
    EXPECT_TRUE(doc.Field(entity, "stamp", "target").IsNull());
    EXPECT_EQ(doc.Field(entity, "stamp", "gain"), kgraph::Value(0.0));
    EXPECT_EQ(doc.Field(entity, "stamp", "tint"), kgraph::Value(kgraph::Value::List { 0, 0, 0, 0 }));

    std::string error;
    EXPECT_FALSE(doc.SetField(entity, "stamp", "tint", 1.0, &error));
    EXPECT_NE(error.find("4 numbers"), std::string::npos) << error;
    EXPECT_FALSE(doc.SetField(entity, "stamp", "target", "no such node", &error));
    EXPECT_NE(error.find("no such node"), std::string::npos) << error;
    EXPECT_FALSE(doc.SetField(entity, "stamp", "nothing", 1.0, &error));
    EXPECT_FALSE(doc.SetField(entity, "show", "offset", kgraph::Value::List { 0, 0 }, &error)) << "it has no such component";
    EXPECT_TRUE(doc.SetField(entity, "stamp", "target", "backdrop", &error)) << error;
    EXPECT_TRUE(doc.SetField(entity, "stamp", "target", kgraph::Value {}, &error)) << "and emptied again";
    EXPECT_TRUE(doc.Field(entity, "stamp", "target").IsNull());

    // Which nodes a field can hold the output of.
    const auto images = doc.NodesFor("Image");
    EXPECT_NE(std::ranges::find(images, "backdrop"), images.end());
    EXPECT_NE(std::ranges::find(images, "canvas view"), images.end()) << "a view of an image will do";
    EXPECT_EQ(std::ranges::find(images, "square"), images.end());
    std::vector<std::string> meshes { "square" };
    for (const auto& name : kgraph::DefaultMeshNames()) meshes.push_back(kgraph::DefaultMeshId(name));
    EXPECT_EQ(doc.NodesFor("Mesh"), meshes) << "the node that makes one, and the meshes there are without a node";

    EXPECT_TRUE(doc.RemoveComponent(entity, "stamp"));
    EXPECT_FALSE(doc.RemoveComponent(entity, "stamp"));
}

// A component that holds another entity: what a script's Entity property is.
namespace prefabs { struct Follows { kecs::Entity target; }; }
template <> struct kecs::Name<prefabs::Follows> { static constexpr std::string_view value = "Follows"; };
KECS_FIELDS(prefabs::Follows, target)

// A prefab is made of an entity and what is under it; its instances follow it, but for what each made its own.
TEST_F(DocumentTest, APrefabsInstancesFollowItButForWhatEachMadeItsOwn) {
    using kgraph::Value;
    const keditor::Project project = scene::MakeProject("prefabs");
    const auto file = project.directory / "prefabs" / "Lamp.prefab";
    const auto scenePath = project.ScenePath("main");
    {
        Running editing;
        keditor::Document& doc = editing.scene->Doc();
        doc.SetAssetDirectory(project.directory / "assets");
        kecs::World& world = doc.World();
        world.Id<prefabs::Follows>();
        doc.AddTag("Heavy");
        const kecs::Entity lamp = doc.CreateEntity("Lamp");
        const kecs::Entity bulb = doc.CreateEntity("Bulb", lamp);
        const kecs::Entity base = doc.CreateEntity("Base", lamp);
        ASSERT_TRUE(doc.AddComponent(bulb, "PointLight"));
        ASSERT_TRUE(doc.SetField(bulb, "PointLight", "range", 7.0));
        ASSERT_TRUE(doc.AddComponent(base, "Heavy"));
        ASSERT_TRUE(doc.AddComponent(lamp, "Follows"));
        std::string error;
        ASSERT_TRUE(doc.SetField(lamp, "Follows", "target", keditor::EntityValue(bulb), &error)) << error;
        EXPECT_EQ(doc.Field(lamp, "Follows", "target"), keditor::EntityValue(bulb)) << "an entity field holds another entity";

        // Saved as a prefab, it is the prefab's first instance, with nothing of its own.
        ASSERT_TRUE(doc.SaveAsPrefab(lamp, file, &error)) << error;
        EXPECT_TRUE(std::filesystem::exists(file));
        EXPECT_EQ(doc.PrefabOf(lamp), "prefabs/Lamp.prefab");
        EXPECT_TRUE(doc.OverridesOf(lamp).empty());
        EXPECT_EQ(doc.PrefabRootOf(bulb), lamp);
        EXPECT_TRUE(doc.ComponentsOf(lamp) == std::vector<std::string> { "Follows" }) << "which prefab it is of is no component to show";

        // Another instance: the same entities, and what one holds of another is its own.
        const kecs::Entity other = doc.Instantiate("prefabs/Lamp.prefab", kecs::NullEntity, &error);
        ASSERT_FALSE(other.IsNull()) << error;
        EXPECT_EQ(doc.NameOf(other), "Lamp 2");
        ASSERT_EQ(world.ChildrenOf(other).size(), 2u);
        const kecs::Entity otherBulb = world.ChildrenOf(other)[0], otherBase = world.ChildrenOf(other)[1];
        EXPECT_EQ(doc.NameOf(otherBulb), "Bulb");
        EXPECT_EQ(doc.Field(otherBulb, "PointLight", "range"), Value(7.0));
        EXPECT_EQ(doc.Field(other, "Follows", "target"), keditor::EntityValue(otherBulb)) << "its own bulb, not the first one's";
        EXPECT_TRUE(doc.OverridesOf(other).empty());

        // A field set on it is its own; the prefab changed from the first, it follows the rest.
        ASSERT_TRUE(doc.SetField(otherBulb, "PointLight", "range", 20.0));
        EXPECT_EQ(doc.OverridesOf(other).size(), 1u);
        ASSERT_TRUE(doc.SetField(bulb, "PointLight", "range", 3.0));
        ASSERT_TRUE(doc.SetField(bulb, "PointLight", "color", Value::List { 0, 1, 0 }));
        ASSERT_TRUE(doc.SetPosition(bulb, { 0, 2, 0 }));
        ASSERT_TRUE(doc.ApplyPrefab(lamp, &error)) << error;
        EXPECT_TRUE(doc.OverridesOf(lamp).empty()) << "applied, nothing of it is its own";
        EXPECT_EQ(doc.Field(otherBulb, "PointLight", "color"), Value(Value::List { 0, 1, 0 })) << "the other follows the prefab";
        EXPECT_EQ(world.PositionOf(otherBulb), (kecs::Vec3 { 0, 2, 0 }));
        EXPECT_EQ(doc.Field(otherBulb, "PointLight", "range"), Value(20.0)) << "but for what it made its own";
        EXPECT_EQ(world.PositionOf(other), (kecs::Vec3 {})) << "and where it is is its own";

        // An entity taken from it stays taken; a component given it stays.
        ASSERT_TRUE(doc.DestroyEntity(otherBase));
        ASSERT_TRUE(doc.AddComponent(otherBulb, "Heavy"));
        doc.UpdatePrefabs();
        EXPECT_EQ(world.ChildrenOf(other).size(), 1u);
        EXPECT_TRUE(std::ranges::contains(doc.ComponentsOf(otherBulb), std::string("Heavy")));

        // A copy of an instance is another instance; saved, all of it is in the file.
        const kecs::Entity copy = doc.Duplicate(other);
        EXPECT_EQ(doc.PrefabOf(copy), "prefabs/Lamp.prefab");
        EXPECT_EQ(doc.OverridesOf(copy), doc.OverridesOf(other));
        ASSERT_TRUE(doc.Save(scenePath, error)) << error;

        // Reverted, it is as the prefab is.
        ASSERT_TRUE(doc.RevertPrefab(other, &error)) << error;
        ASSERT_EQ(world.ChildrenOf(other).size(), 2u);
        EXPECT_EQ(doc.Field(world.ChildrenOf(other)[0], "PointLight", "range"), Value(3.0));
        EXPECT_FALSE(std::ranges::contains(doc.ComponentsOf(world.ChildrenOf(other)[0]), std::string("Heavy")));

        // And unlinked, it is entities like any other.
        ASSERT_TRUE(doc.Unlink(copy));
        EXPECT_TRUE(doc.PrefabOf(copy).empty());
        EXPECT_TRUE(doc.PrefabRootOf(world.ChildrenOf(copy)[0]).IsNull());

        // The prefab changed with nobody's scene open on the other: it follows when it is opened.
        ASSERT_TRUE(doc.SetField(bulb, "PointLight", "range", 5.0));
        ASSERT_TRUE(doc.ApplyPrefab(lamp, &error)) << error;
    }
    {
        Running opened(scenePath);
        keditor::Document& doc = opened.scene->Doc();
        kecs::World& world = doc.World();
        const kecs::Entity other = world.FindEntity("Lamp 2");
        ASSERT_FALSE(other.IsNull());
        EXPECT_EQ(doc.PrefabOf(other), "prefabs/Lamp.prefab");
        ASSERT_EQ(world.ChildrenOf(other).size(), 1u) << "what it took away, it still has not";
        const kecs::Entity otherBulb = world.ChildrenOf(other)[0];
        EXPECT_EQ(doc.Field(otherBulb, "PointLight", "range"), Value(20.0)) << "its own, as saved";
        const kecs::Entity lamp = world.FindEntity("Lamp");
        EXPECT_EQ(doc.Field(world.ChildrenOf(lamp)[0], "PointLight", "range"), Value(5.0)) << "the first follows what was applied after the scene was saved";
        EXPECT_FALSE(doc.Dirty()) << "made again as it is opened, it is no change of anyone's";
    }
}

// A prefab's instance under another's is part of the outer prefab: its own prefab's, with what it made its own.
TEST_F(DocumentTest, APrefabHasInstancesOfOthersInIt) {
    using kgraph::Value;
    const keditor::Project project = scene::MakeProject("nested-prefabs");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    doc.SetAssetDirectory(project.directory / "assets");
    kecs::World& world = doc.World();
    std::string error;
    // The inner one: a bulb.
    const kecs::Entity bulb = doc.CreateEntity("Bulb");
    ASSERT_TRUE(doc.AddComponent(bulb, "PointLight"));
    ASSERT_TRUE(doc.SetField(bulb, "PointLight", "range", 7.0));
    ASSERT_TRUE(doc.SaveAsPrefab(bulb, project.directory / "prefabs" / "Bulb.prefab", &error)) << error;
    // The outer one: a lamp with a bulb in it, that bulb brighter than the prefab's and moved up.
    const kecs::Entity lamp = doc.CreateEntity("Lamp");
    ASSERT_TRUE(doc.SetParent(bulb, lamp));
    ASSERT_TRUE(doc.SetField(bulb, "PointLight", "range", 9.0));
    ASSERT_TRUE(doc.SetPosition(bulb, { 0, 1, 0 }));
    ASSERT_TRUE(doc.SaveAsPrefab(lamp, project.directory / "prefabs" / "Lamp.prefab", &error)) << error;
    EXPECT_EQ(doc.PrefabOf(bulb), "prefabs/Bulb.prefab") << "the bulb is still the bulb's";

    // Another lamp: its bulb is the bulb's prefab's instance, as the lamp's prefab has it.
    const kecs::Entity other = doc.Instantiate("prefabs/Lamp.prefab", kecs::NullEntity, &error);
    ASSERT_FALSE(other.IsNull()) << error;
    ASSERT_EQ(world.ChildrenOf(other).size(), 1u);
    const kecs::Entity otherBulb = world.ChildrenOf(other)[0];
    EXPECT_EQ(doc.PrefabOf(otherBulb), "prefabs/Bulb.prefab");
    EXPECT_EQ(doc.Field(otherBulb, "PointLight", "range"), Value(9.0)) << "what the lamp's bulb made its own";
    EXPECT_EQ(world.PositionOf(otherBulb), (kecs::Vec3 { 0, 1, 0 })) << "where the lamp put it";

    // The bulb's prefab changed: every bulb follows in what none of them made its own.
    const kecs::Entity loose = doc.Instantiate("prefabs/Bulb.prefab", kecs::NullEntity, &error);
    ASSERT_FALSE(loose.IsNull()) << error;
    ASSERT_TRUE(doc.SetField(loose, "PointLight", "color", Value::List { 1, 0, 0 }));
    ASSERT_TRUE(doc.ApplyPrefab(loose, &error)) << error;
    EXPECT_EQ(doc.Field(otherBulb, "PointLight", "color"), Value(Value::List { 1, 0, 0 }));
    EXPECT_EQ(doc.Field(otherBulb, "PointLight", "range"), Value(9.0)) << "but not in what it did";

    // A bulb changed in one lamp is that lamp's own: the lamp's prefab changed, it keeps it; the other follows.
    ASSERT_TRUE(doc.SetField(otherBulb, "PointLight", "range", 15.0));
    ASSERT_TRUE(doc.SetPosition(lamp, { 3, 0, 0 }));
    ASSERT_TRUE(doc.SetField(bulb, "PointLight", "range", 11.0));
    ASSERT_TRUE(doc.ApplyPrefab(lamp, &error)) << error;
    EXPECT_EQ(doc.Field(otherBulb, "PointLight", "range"), Value(15.0));
    const kecs::Entity third = doc.Instantiate("prefabs/Lamp.prefab", kecs::NullEntity, &error);
    ASSERT_FALSE(third.IsNull()) << error;
    EXPECT_EQ(doc.Field(world.ChildrenOf(third)[0], "PointLight", "range"), Value(11.0)) << "what was applied";

    // Reverted, the lamp's bulb is as the lamp's prefab has it.
    ASSERT_TRUE(doc.RevertPrefab(other, &error)) << error;
    ASSERT_EQ(world.ChildrenOf(other).size(), 1u);
    EXPECT_EQ(doc.Field(world.ChildrenOf(other)[0], "PointLight", "range"), Value(11.0));
    EXPECT_EQ(doc.PrefabOf(world.ChildrenOf(other)[0]), "prefabs/Bulb.prefab");

    // A prefab with an instance of itself in it is made a few deep, and no more.
    ASSERT_TRUE(doc.SetParent(third, lamp));
    ASSERT_TRUE(doc.ApplyPrefab(lamp, &error)) << error;
    EXPECT_TRUE(std::ranges::any_of(doc.Notes(), [](const std::string& note) { return note.find("has itself in it") != std::string::npos; }));
}

// Nodes moved to another pass, or pasted there, are wired as passes are: what they take of an earlier pass brought in
// through a Reference, what is of their own pass wired straight. What they work out stays what it was.
TEST_F(DocumentTest, NodesMovedOrPastedToAnotherPassAreBroughtInAsPassesAre) {
    using kgraph::Value;
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    for (const char* pass : { "first", "second", "third" }) ASSERT_TRUE(doc.AddPass(pass));
    doc.AddNode("Double", 0, 0, "n", "first");
    doc.SetInput("n", "value", 2);
    doc.AddNode("Multiply", 0, 0, "double", "second");
    doc.SetInput("double", "b", 2);
    const std::string brought = doc.Import("n", "second", -260, 0);
    ASSERT_TRUE(doc.Connect(brought, "out", "double", "a"));
    doc.AddNode("Multiply", 0, 0, "twice", "second");
    doc.SetInput("twice", "b", 3);
    ASSERT_TRUE(doc.Connect("double", "out", "twice", "a"));
    doc.AddNode("Add", 0, 0, "show", "third");
    doc.SetInput("show", "b", 1);
    ASSERT_TRUE(doc.Connect(doc.Import("twice", "third", -260, 0), "out", "show", "a"));
    ASSERT_EQ(doc.Runtime().ValueOf("show"), Value(13)) << "2 * 2 * 3 + 1";
    const auto link = [&](const char* node, const char* input) {
        const kgraph::Node* found = doc.Graph().Find(node);
        if (!found) return std::string();
        const auto it = found->inputs.find(input);
        return it != found->inputs.end() && it->second.link ? it->second.link->node : std::string();
    };
    const auto passOf = [&](const std::string& id) { const kgraph::Node* found = doc.Graph().Find(id); return found ? found->pass : std::string("(gone)"); };

    // Moved to the last pass: what it took of its own pass is brought in now; what took it there takes it straight.
    ASSERT_TRUE(doc.MoveNodesToPass({ "twice" }, "third"));
    EXPECT_EQ(passOf("twice"), "third");
    EXPECT_EQ(passOf(link("twice", "a")), "third") << "a Reference in its new pass";
    ASSERT_NE(doc.Graph().Find(link("twice", "a")), nullptr);
    EXPECT_EQ(doc.Graph().Find(link("twice", "a"))->type, "Reference");
    EXPECT_EQ(link("show", "a"), "twice") << "of one pass now: wired straight";
    EXPECT_EQ(doc.Graph().Find("twice (in third)"), nullptr) << "the Reference that brought it in has nothing to bring in now, and is gone";
    EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(13)) << "and what it works out is what it was";

    // Moved to the first: what it took of that pass, straight; and the pass after takes it through a Reference.
    ASSERT_TRUE(doc.MoveNodesToPass({ "double" }, "first"));
    EXPECT_EQ(link("double", "a"), "n");
    EXPECT_EQ(doc.Graph().Find(brought), nullptr) << "the Reference to n in the second pass is wired from by nothing";
    EXPECT_EQ(doc.Graph().Find(link("twice", "a"))->type, "Reference");
    EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(13));

    // Into a pass that is not there yet: it is made, last.
    ASSERT_TRUE(doc.MoveNodesToPass({ "show" }, "fourth"));
    EXPECT_EQ(doc.Passes().back(), "fourth");
    EXPECT_EQ(doc.Graph().Find(link("show", "a"))->type, "Reference");
    EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(13));

    // Pasted into another pass: the copy brings in what it takes of the earlier ones, through one Reference however many take it.
    std::vector<kgraph::Node> copied { *doc.Graph().Find("twice"), *doc.Graph().Find("show") };
    const std::string second = "second";
    const auto made = doc.PasteNodes(copied, 0, 400, &second);
    ASSERT_EQ(made.size(), 2u);
    EXPECT_EQ(passOf(made[0]), "second");
    EXPECT_EQ(link(made[1].c_str(), "a"), made[0]) << "copied together, wired together";
    EXPECT_EQ(passOf(link(made[0].c_str(), "a")), "second");
    EXPECT_EQ(doc.Graph().Find(link(made[0].c_str(), "a"))->type, "Reference") << "what it takes of the first pass, brought in";
    EXPECT_EQ(doc.Runtime().ValueOf(made[1]), Value(13));
}

// Nodes made a function: a file of the project's, and a call in their place wired as they were. Opened in place, it is
// changed for every call; written out, a call is its nodes again.
TEST_F(DocumentTest, NodesAreMadeAFunctionThatIsOpenedChangedAndWrittenOut) {
    using kgraph::Value;
    const keditor::Project project = scene::MakeProject("functions");
    const auto scenePath = project.ScenePath("main");
    {
        Running editing;
        keditor::Document& doc = editing.scene->Doc();
        doc.SetAssetDirectory(project.directory / "assets");
        doc.AddNode("Double", 0, 0, "n");
        doc.SetInput("n", "value", 2);
        doc.AddNode("Multiply", 300, 0, "double");
        doc.SetInput("double", "b", 2);
        ASSERT_TRUE(doc.Connect("n", "out", "double", "a"));
        doc.AddNode("Multiply", 600, 0, "twice");
        doc.SetInput("twice", "b", 3);
        ASSERT_TRUE(doc.Connect("double", "out", "twice", "a"));
        doc.AddNode("Add", 900, 0, "show");
        doc.SetInput("show", "b", 1);
        ASSERT_TRUE(doc.Connect("twice", "out", "show", "a"));
        ASSERT_EQ(doc.Runtime().ValueOf("show"), Value(13));
        const auto link = [&](const std::string& node, const char* input) {
            const kgraph::Node* found = doc.Graph().Find(node);
            if (!found) return std::string("(gone)");
            const auto it = found->inputs.find(input);
            return it != found->inputs.end() && it->second.link ? it->second.link->node + "." + it->second.link->output : std::string();
        };

        // Made a function: the call is where they were, wired as they were, and works out what they did.
        std::string error;
        const std::string call = doc.MakeFunction({ "double", "twice" }, "Scale", &error);
        ASSERT_FALSE(call.empty()) << error;
        EXPECT_TRUE(std::filesystem::exists(project.directory / "functions" / "Scale.kfunc"));
        std::vector<std::string> own;
        for (const auto& name : doc.Functions()) if (!doc.IsEngineFunction(name)) own.push_back(name);
        EXPECT_EQ(own, (std::vector<std::string> { "Scale" })) << "the project's, besides the engine's";
        EXPECT_EQ(doc.Graph().Find("double"), nullptr);
        EXPECT_EQ(link(call, "a"), "n.out") << "what came in, an input";
        EXPECT_EQ(link("show", "a"), call + ".twice") << "what went out, an output";
        EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(13));
        EXPECT_TRUE(doc.Runtime().Status(call)->ok) << doc.Runtime().Status(call)->error;
        EXPECT_TRUE(doc.MakeFunction({ "show" }, "Scale", &error).empty()) << "a name it has already is refused";
        EXPECT_NE(error.find("already"), std::string::npos) << error;

        // Another call, told something else.
        const std::string other = doc.AddNode("Function", 0, 300, "other");
        doc.SetInput(other, "function", "Scale");
        doc.SetInput(other, "a", 1);
        EXPECT_EQ(doc.Runtime().ValueOf(other, "twice"), Value(6));

        // Opened in place: a page of its own, its nodes real nodes; the call is what its page's edges are wired to.
        const std::string page = doc.OpenFunction(call, &error);
        ASSERT_FALSE(page.empty()) << error;
        EXPECT_NE(std::ranges::find(doc.Passes(), page), doc.Passes().end());
        ASSERT_NE(doc.Graph().Find(call + "/twice"), nullptr);
        EXPECT_EQ(doc.Graph().Find(call + "/twice")->pass, page);
        EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(13)) << "it works out what it did, open";
        ASSERT_TRUE(doc.SetInput(call + "/twice", "b", 4));
        EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(17)) << "and what it is changed to, at once";
        ASSERT_TRUE(doc.CloseFunction(true, &error)) << error;
        EXPECT_FALSE(doc.Opened().has_value());
        EXPECT_EQ(std::ranges::find(doc.Passes(), page), doc.Passes().end()) << "its page is gone";
        EXPECT_EQ(link("show", "a"), call + ".twice");
        EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(17));
        EXPECT_EQ(doc.Runtime().ValueOf(other, "twice"), Value(8)) << "every call follows: 1 * 2 * 4";

        // Opened and put back as it was: nothing of it changed.
        ASSERT_FALSE(doc.OpenFunction(other, &error).empty()) << error;
        ASSERT_TRUE(doc.SetInput(other + "/twice", "b", 100));
        ASSERT_TRUE(doc.CloseFunction(false, &error)) << error;
        EXPECT_EQ(doc.Runtime().ValueOf(other, "twice"), Value(8));

        // Written out, a call is its nodes, wired as the call was, and no call of anything.
        ASSERT_TRUE(doc.WriteOut(other, &error)) << error;
        EXPECT_EQ(doc.Graph().Find(other), nullptr);
        ASSERT_NE(doc.Graph().Find(other + "/twice"), nullptr);
        EXPECT_EQ(doc.Runtime().ValueOf(other + "/twice"), Value(8));
        ASSERT_TRUE(doc.Save(scenePath, error)) << error;
    }
    {
        // Opened again, the call is its function's, read from the project.
        Running opened(scenePath);
        keditor::Document& doc = opened.scene->Doc();
        EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(17));
    }
}

TEST_F(DocumentTest, AProjectIsMadeAndOpened) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "project";
    std::filesystem::remove_all(directory);
    keditor::Project made;
    std::string error;
    ASSERT_TRUE(keditor::Project::Create(directory, "A Game", made, error)) << error;
    EXPECT_FALSE(keditor::Project::Create(directory, "Another", made, error)) << "there is one there";
    EXPECT_TRUE(std::filesystem::is_directory(made.ShaderDirectory()));

    keditor::Project opened;
    ASSERT_TRUE(keditor::Project::Open(directory / "project.json", opened, error)) << error;
    EXPECT_EQ(opened.name, "A Game");
    EXPECT_EQ(opened.scenes, std::vector<std::string> { "main" });
    EXPECT_EQ(opened.start, "main");
    EXPECT_FALSE(keditor::Project::Open(directory / "nowhere", opened, error));
    EXPECT_NE(error.find("no project"), std::string::npos) << error;

    // The scene it starts with is one a document opens: empty.
    Running playing(opened.ScenePath("main"));
    EXPECT_TRUE(playing.scene->Error().empty()) << playing.scene->Error();
    EXPECT_TRUE(playing.scene->Doc().Entities().empty());
    EXPECT_TRUE(playing.scene->Doc().Graph().nodes.empty());

    Running broken(directory / "scenes" / "missing.scene.json");
    EXPECT_NE(broken.scene->Error().find("cannot be read"), std::string::npos) << broken.scene->Error();
}

// Where an input is set is the node's to say, through the document; and what a wired-only input needs is made beside
// it, wired in: a VertexLayout for a pipeline's vertex layout.
TEST_F(DocumentTest, AnInputIsSetWhereTheNodeSaysAndAWiredOneHasItsNodeMade) {
    scene::MakeProject("placement");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const std::string add = doc.AddNode("Add", 0, 0);
    EXPECT_EQ(doc.Graph().Find(add)->inputs.at("a").value, kgraph::Value(0)) << "made with what it starts as";
    EXPECT_TRUE(doc.SetPlacement(add, "a", kgraph::Placement::eInspector));
    EXPECT_EQ(doc.Graph().Find(add)->placed.at("a"), "inspector");
    EXPECT_TRUE(doc.SetPlacement(add, "a", kgraph::Placement::eNode));
    EXPECT_FALSE(doc.Graph().Find(add)->placed.contains("a")) << "back to what its type says: nothing of its own";

    const std::string pipeline = doc.AddNode("GraphicsPipeline", 400, 100, "lit");
    EXPECT_FALSE(doc.SetPlacement(pipeline, "vertex.layout", kgraph::Placement::eNode)) << "wired only";
    std::string error;
    const std::string layout = doc.MakeCompanion(pipeline, "vertex.layout", &error);
    ASSERT_FALSE(layout.empty()) << error;
    EXPECT_EQ(doc.Graph().Find(layout)->type, "VertexLayout");
    EXPECT_LT(doc.Graph().Find(layout)->x, doc.Graph().Find(pipeline)->x) << "beside it, before it";
    ASSERT_TRUE(doc.Graph().Find(pipeline)->inputs.at("vertex.layout").link);
    EXPECT_EQ(doc.Graph().Find(pipeline)->inputs.at("vertex.layout").link->node, layout);
    EXPECT_TRUE(doc.MakeCompanion(add, "a", &error).empty());
    EXPECT_NE(error.find("no node of its own"), std::string::npos) << error;
}

// Every node seen at once is a layout of its own: the passes' pages side by side until a node is moved there, and a
// node moved there is not moved on its page — nor one moved on its page there. It is saved with the graph.
TEST_F(DocumentTest, EveryNodeAtOnceIsALayoutOfItsOwn) {
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    for (const char* pass : { "first", "second" }) ASSERT_TRUE(doc.AddPass(pass));
    doc.AddNode("Double", 0, 0, "a", "first");
    doc.AddNode("Double", 0, 100, "b", "first");
    doc.AddNode("Double", 0, 0, "c", "second");
    ASSERT_TRUE(doc.Graph().overview.empty());

    // At first: each pass's page where it is, beside the one before — none over another.
    auto places = doc.OverviewPlaces();
    ASSERT_EQ(places.size(), doc.Graph().nodes.size());
    EXPECT_EQ(places.at("a"), (std::pair<double, double> { places.at("a").first, 0. }));
    EXPECT_EQ(places.at("b").second - places.at("a").second, 100.) << "a page is as it is";
    EXPECT_GT(places.at("c").first, places.at("a").first + 300.) << "the second pass's page is beside the first's";

    // Moved there: there, and not on its page.
    doc.MoveInOverview({ "c" }, 50, 70);
    EXPECT_EQ(doc.Graph().Find("c")->x, 0.);
    EXPECT_EQ(doc.Graph().Find("c")->y, 0.);
    EXPECT_EQ(doc.OverviewPlaces().at("c"), (std::pair<double, double> { places.at("c").first + 50, places.at("c").second + 70 }));
    // Moved on its page: there, and not where every node is seen.
    places = doc.OverviewPlaces();
    doc.MoveNodes({ "a" }, 500, 500);
    EXPECT_EQ(doc.OverviewPlaces().at("a"), places.at("a")) << "held where it was seen once any was moved there";

    // Made where every node is seen: where it was put, there.
    doc.MakeInOverview(true);
    doc.AddNode("Double", -400, -300, "d", "second");
    doc.MakeInOverview(false);
    EXPECT_EQ(doc.OverviewPlaces().at("d"), (std::pair<double, double> { -400., -300. }));
    EXPECT_EQ(doc.OverviewPlaces().at("c"), places.at("c")) << "what was there stays";

    // Taken away, it is nowhere; saved and read back, the layout is as it was.
    doc.RemoveNodes({ "b" });
    EXPECT_FALSE(doc.Graph().overview.contains("b"));
    const std::string saved = doc.ToJson();
    EXPECT_NE(saved.find("\"overview\""), std::string::npos);
    kgraph::Graph read;
    std::string error;
    ASSERT_TRUE(kgraph::Graph::FromJson(doc.Graph().ToJson(), read, error)) << error;
    EXPECT_EQ(read.overview, doc.Graph().overview);
}

// Put in order, a page comes out as the view it is in is shaped: a tall column of what is not wired to one another is
// wrapped into columns side by side for a wide view, and left as it is for a tall one.
TEST_F(DocumentTest, AnArrangedPageIsShapedAsItsView) {
    std::vector<keditor::PlacedNode> nodes { { "source", 0.f, 0.f, 200.f, 100.f } };
    std::vector<std::pair<std::string, std::string>> wires;
    for (int i = 0; i < 16; ++i) {
        nodes.push_back({ std::format("use {}", i), 0.f, static_cast<float>(i) * 10.f, 200.f, 100.f });
        wires.emplace_back("source", nodes.back().id);
    }
    for (int i = 0; i < 6; ++i) nodes.push_back({ std::format("loose {}", i), 0.f, 0.f, 200.f, 100.f });
    const auto bounds = [&](const std::map<std::string, std::pair<double, double>>& where) {
        double right = 0, bottom = 0, left = 1e9, top = 1e9;
        for (const auto& node : nodes) {
            const auto [x, y] = where.at(node.id);
            left = std::min(left, x); top = std::min(top, y);
            right = std::max(right, x + node.width); bottom = std::max(bottom, y + node.height);
        }
        return (right - left) / (bottom - top);
    };
    const auto tall = keditor::ArrangeNodes(nodes, wires), wide = keditor::ArrangeNodes(nodes, wires, 1.8f);
    ASSERT_EQ(wide.size(), nodes.size());
    EXPECT_LT(bounds(tall), 1.) << "one column, as it was";
    EXPECT_GT(bounds(wide), 1.4) << "about as wide as the view, for how tall";
    EXPECT_LT(bounds(wide), 2.6);
    for (const auto& [from, to] : wires) EXPECT_LT(wide.at(from).first, wide.at(to).first) << from << " feeds " << to;
    std::set<std::pair<double, double>> places;
    for (const auto& at : wide | std::views::values) EXPECT_TRUE(places.insert(at).second) << "two nodes at one place";
}

// Put in order, each loop is a block of its own: what is in its frame is its nodes, and no two loops' frames are one over
// the other — though how they are wired would put others' nodes between theirs, column by column.
TEST(Arrange, EachLoopIsABlockOfItsOwn) {
    struct Box { double left, top, right, bottom; };
    const auto check = [](const std::vector<keditor::PlacedNode>& nodes, const std::vector<std::pair<std::string, std::string>>& wires, const bool inOrder) {
        const auto where = keditor::ArrangeNodes(nodes, wires, 1.6f);
        ASSERT_EQ(where.size(), nodes.size());
        const auto box = [&](const std::string& group) {
            Box b { 1e9, 1e9, -1e9, -1e9 };
            for (const auto& node : nodes)
                if (node.group == group) {
                    const auto [x, y] = where.at(node.id);
                    b = { std::min(b.left, x), std::min(b.top, y), std::max(b.right, x + node.width), std::max(b.bottom, y + node.height) };
                }
            return b;
        };
        const auto meet = [](const Box& a, const Box& b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; };
        const Box a = box("loop a"), b = box("loop b");
        EXPECT_FALSE(meet(a, b)) << "two loops are two blocks";
        for (const auto& node : nodes) {
            const auto [x, y] = where.at(node.id);
            const Box own { x, y, x + node.width, y + node.height };
            if (node.group != "loop a") EXPECT_FALSE(meet(own, a)) << node.id << " is not of loop a, and not in its frame";
            if (node.group != "loop b") EXPECT_FALSE(meet(own, b)) << node.id << " is not of loop b, and not in its frame";
        }
        if (inOrder)
            for (const auto& [from, to] : wires) EXPECT_LT(where.at(from).first, where.at(to).first) << from << " feeds " << to;
    };
    // Each loop two columns wide, with what is of neither fed from one and feeding the other beside them.
    check({ { "a1", 0, 0, 160, 80, "loop a", 0 }, { "a2", 0, 0, 160, 80, "loop a", 0 },
            { "b1", 0, 0, 160, 80, "loop b", 0 }, { "b2", 0, 0, 160, 80, "loop b", 0 },
            { "before", 0, 0, 160, 80, {}, 0 }, { "aside", 0, 0, 160, 80, {}, 0 }, { "after", 0, 0, 160, 80, {}, 0 } },
          { { "before", "a1" }, { "a1", "a2" }, { "a2", "b1" }, { "b1", "b2" }, { "a1", "aside" }, { "aside", "b2" }, { "b2", "after" } }, true);
    // Fed from each other, two loops kept apart cannot both be after the other: they are two blocks all the same.
    check({ { "a1", 0, 0, 160, 80, "loop a", 0 }, { "a2", 0, 0, 160, 80, "loop a", 0 },
            { "b1", 0, 0, 160, 80, "loop b", 0 }, { "b2", 0, 0, 160, 80, "loop b", 0 } },
          { { "a1", "b1" }, { "b1", "a2" }, { "a2", "b2" } }, false);
}

// Put in order, each node is level with what it is wired to, port with port: a wire runs straight where it can.
TEST(Arrange, AWireRunsStraightFromItsPortToTheOther) {
    const std::vector<keditor::PlacedNode> nodes { { "camera", 0, 0, 160, 80, {}, 0 }, { "draw", 0, 300, 200, 160, {}, 0 }, { "show", 0, 600, 160, 60, {}, 0 } };
    // Out of the camera's second row, into the draw's fourth; out of the draw's first, into the show's only.
    const std::vector<keditor::PlacedWire> wires { { "camera", "draw", 60.f, 120.f }, { "draw", "show", 45.f, 30.f } };
    const auto where = keditor::ArrangeNodes(nodes, wires);
    EXPECT_NEAR(where.at("camera").second + 60.0, where.at("draw").second + 120.0, 1.0) << "the camera's port level with the draw's";
    EXPECT_NEAR(where.at("draw").second + 45.0, where.at("show").second + 30.0, 1.0) << "the draw's port level with the show's";
    EXPECT_LT(where.at("camera").first + 160.0, where.at("draw").first);
}

// Put in order, a wire that runs past a column has its way kept clear through it: it does not run through the node
// that is in that column, level with where it goes.
TEST(Arrange, ALongWireGoesRoundWhatItPasses) {
    const std::vector<keditor::PlacedNode> nodes { { "a", 0, 0, 160, 100, {}, 0 }, { "b", 0, 0, 160, 100, {}, 0 }, { "c", 0, 0, 160, 140, {}, 0 } };
    // a into b into c, and a straight into c — each wire from a's middle, into c at two rows near one another.
    const std::vector<keditor::PlacedWire> wires { { "a", "b", 50.f, 50.f }, { "b", "c", 50.f, 80.f }, { "a", "c", 50.f, 50.f } };
    const auto where = keditor::ArrangeNodes(nodes, wires);
    const auto [ax, ay] = where.at("a");
    const auto [bx, by] = where.at("b");
    const auto [cx, cy] = where.at("c");
    ASSERT_LT(ax, bx);
    ASSERT_LT(bx, cx);
    // Where the straight line from a's port to c's is, across b's column.
    const auto lineAt = [&](const double x) { return ay + 50.0 + (cy + 50.0 - ay - 50.0) * (x - ax - 160.0) / (cx - ax - 160.0); };
    for (const double x : { bx, bx + 80.0, bx + 160.0 })
        EXPECT_TRUE(lineAt(x) < by || lineAt(x) > by + 100.0) << "the wire from a to c runs clear of b, at " << x;
}

// Put in order, a page's wires cross as few times as they can: each column is ordered by what it is wired to on either
// side, its first one too — not only by what comes before it.
TEST(Arrange, WiresCrossAsFewTimesAsTheyCan) {
    // How many wires cross, drawn straight from the middle of one's right side to the middle of the other's left.
    const auto crossings = [](const std::vector<keditor::PlacedNode>& nodes, const std::vector<std::pair<std::string, std::string>>& wires) {
        const auto where = keditor::ArrangeNodes(nodes, wires);
        const auto of = [&](const std::string& id) { return *std::ranges::find(nodes, id, &keditor::PlacedNode::id); };
        struct Line { double x1, y1, x2, y2; std::string a, b; };
        std::vector<Line> lines;
        for (const auto& [from, to] : wires) {
            const auto [fx, fy] = where.at(from);
            const auto [tx, ty] = where.at(to);
            lines.push_back({ fx + of(from).width, fy + of(from).height * 0.5, tx, ty + of(to).height * 0.5, from, to });
        }
        const auto side = [](const Line& l, const double x, const double y) { return (l.x2 - l.x1) * (y - l.y1) - (l.y2 - l.y1) * (x - l.x1); };
        int crossed = 0;
        for (std::size_t i = 0; i < lines.size(); ++i)
            for (std::size_t j = i + 1; j < lines.size(); ++j) {
                const Line& p = lines[i];
                const Line& q = lines[j];
                if (p.a == q.a || p.b == q.b || p.a == q.b || p.b == q.a) continue;     // meeting at a node is no crossing
                if (side(p, q.x1, q.y1) * side(p, q.x2, q.y2) < 0 && side(q, p.x1, p.y1) * side(q, p.x2, p.y2) < 0) ++crossed;
            }
        return crossed;
    };
    // Six into six, each to another than the one across from it: none need cross.
    std::vector<keditor::PlacedNode> nodes;
    std::vector<std::pair<std::string, std::string>> wires;
    const int to[] { 3, 0, 5, 1, 4, 2 };
    for (int i = 0; i < 6; ++i) {
        nodes.push_back({ std::format("s{}", i), 0, static_cast<float>(i) * 100.f, 160, 80, {}, 0 });
        nodes.push_back({ std::format("t{}", i), 300, static_cast<float>(i) * 100.f, 160, 80, {}, 0 });
    }
    for (int i = 0; i < 6; ++i) wires.emplace_back(std::format("s{}", i), std::format("t{}", to[i]));
    EXPECT_EQ(crossings(nodes, wires), 0);

    // Three into two, the outer two to one and the middle one to the other: none cross once the first column is ordered
    // by what it feeds — which the order it starts in, top to bottom, does not give.
    EXPECT_EQ(crossings({ { "a", 0, 0, 160, 80, {}, 0 }, { "b", 0, 100, 160, 80, {}, 0 }, { "c", 0, 200, 160, 80, {}, 0 },
                          { "x", 300, 0, 160, 80, {}, 0 }, { "y", 300, 100, 160, 80, {}, 0 } },
                        { { "a", "y" }, { "b", "x" }, { "c", "y" } }), 0);

    // A wire across a column is ordered with it: through it, it crosses nothing that need not be crossed.
    EXPECT_EQ(crossings({ { "a", 0, 0, 160, 80, {}, 0 }, { "b", 0, 100, 160, 80, {}, 0 },
                          { "m", 300, 0, 160, 80, {}, 0 }, { "n", 300, 100, 160, 80, {}, 0 },
                          { "z", 600, 0, 160, 80, {}, 0 }, { "w", 600, 100, 160, 80, {}, 0 } },
                        { { "a", "n" }, { "n", "w" }, { "b", "m" }, { "m", "z" }, { "b", "w" } }), 0);
}

// A wire let go on a node is offered every port of it that would take it — shown on the node or not — and, greyed, the
// rest with why; the other way, from an input, the node's outputs, what it gives of itself among them.
TEST_F(DocumentTest, AWireLetGoOnANodeIsOfferedItsPorts) {
    scene::MakeProject("port choices");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const std::string picture = doc.AddNode("Image", 0, 0, "picture");
    const std::string other = doc.AddNode("Image", 0, 200, "other");
    const std::string blit = doc.AddNode("Blit", 300, 0, "blit");
    Frame();

    const auto choices = keditor::PortChoices(doc, picture, "out", true, blit);
    const auto find = [&](const std::string& port) { return std::ranges::find(choices, port, &keditor::PortChoice::port); };
    ASSERT_NE(find("source"), choices.end());
    EXPECT_TRUE(find("source")->fits);
    ASSERT_NE(find("sourceMip"), choices.end()) << "an input not shown on the node is offered too";
    EXPECT_FALSE(find("sourceMip")->fits);
    EXPECT_NE(find("sourceMip")->why.find("value"), std::string::npos) << find("sourceMip")->why;
    EXPECT_TRUE(choices.front().fits) << "what fits comes first";
    EXPECT_TRUE(find("source")->wiredFrom.empty());

    ASSERT_TRUE(doc.Connect(other, "out", blit, "source"));
    Frame();
    const auto again = keditor::PortChoices(doc, picture, "out", true, blit);
    const auto source = std::ranges::find(again, std::string("source"), &keditor::PortChoice::port);
    ASSERT_NE(source, again.end());
    EXPECT_EQ(source->wiredFrom, "out of other") << "one wired says from what; chosen, the new wire takes its place";

    // From the Blit's source, onto an Image: its outputs, `out` fitting and its size not.
    const auto outputs = keditor::PortChoices(doc, blit, "source", false, picture);
    const auto out = std::ranges::find(outputs, std::string("out"), &keditor::PortChoice::port);
    const auto size = std::ranges::find(outputs, std::string("size"), &keditor::PortChoice::port);
    ASSERT_NE(out, outputs.end());
    EXPECT_TRUE(out->fits);
    ASSERT_NE(size, outputs.end()) << "what a node gives of itself is offered";
    EXPECT_FALSE(size->fits);

    EXPECT_TRUE(keditor::PortChoices(doc, blit, "source", false, blit).empty()) << "none onto its own node";
}

// What a node costs: a pass's own; a Function's call's and a loop's every pass inside added up; nothing for the rest.
TEST(Cost, ACallOrALoopIsWhatItsPassesAddUpTo) {
    std::map<std::string, kgraph::Runtime::Timing, std::less<>> timings;
    timings["blit"] = { .gpuMs = 0.5, .gpuMeasured = true, .runs = 1 };
    timings["bloom/down"] = { .gpuMs = 1.0, .gpuMeasured = true, .runs = 1 };
    timings["bloom/up"] = { .gpuMs = 2.0, .gpuMeasured = true, .runs = 1 };
    timings["bloomy"] = { .gpuMs = 8.0, .gpuMeasured = true, .runs = 1 };
    timings["down [0]"] = { .gpuMs = 0.25, .gpuMeasured = true, .runs = 1 };
    timings["down [1]"] = { .gpuMs = 0.25, .gpuMeasured = true, .runs = 1 };
    ASSERT_TRUE(keditor::CostOf(timings, "blit"));
    EXPECT_DOUBLE_EQ(keditor::CostOf(timings, "blit")->gpuMs, 0.5);
    ASSERT_TRUE(keditor::CostOf(timings, "bloom"));
    EXPECT_DOUBLE_EQ(keditor::CostOf(timings, "bloom")->gpuMs, 3.0) << "a call's passes, and not another node whose name starts the same";
    EXPECT_EQ(keditor::CostOf(timings, "bloom")->runs, 2u);
    EXPECT_DOUBLE_EQ(keditor::CostOf(timings, "down")->gpuMs, 0.5) << "a loop's turns";
    EXPECT_FALSE(keditor::CostOf(timings, "image"));
}

// The list of nodes: the functions first, each under what it is for and found by its tags; then the scene's and
// values' nodes; then Vulkan's own.
TEST(NodeList, FunctionsComeFirstUnderWhatTheyAreForAndVulkansOwnLast) {
    kgraph::Preset blur { .name = "GaussianBlur", .type = "Function", .doc = "Blurs a picture.", .group = "Post", .keywords = "blur soften gaussian" };
    blur.inputs = { { "function", kgraph::Value("GaussianBlur") } };
    const auto found = keditor::FindNodeTypes("soften", { blur });
    ASSERT_FALSE(found.empty()) << "found by a tag";
    EXPECT_EQ(found.front()->name, "GaussianBlur");
    EXPECT_EQ(found.front()->category, "Post");

    const std::map<std::string, std::size_t> counts { { "Passes", 3 }, { "Math", 2 }, { "Post", 1 }, { "Physics", 2 }, { "Functions", 1 }, { "Resources", 4 } };
    const std::vector<std::string> sections = keditor::NodeListSections(counts);
    const std::vector<std::string> expected { "#Functions", "Physics", "Post", "Functions", "#Scene and values", "Math", "#Vulkan", "Resources", "Passes" };
    EXPECT_EQ(sections, expected);
    const auto fixed = keditor::NodeListSections({ { "Post", 1 }, { "Physics", 1 } }, kgraph::PassRate::eFixed);
    ASSERT_GE(fixed.size(), 2u);
    EXPECT_EQ(fixed[1], "Physics") << "on the fixed step, what simulates first";
    EXPECT_EQ(keditor::NodeListSections({ { "Math", 1 } }).front(), "#Scene and values") << "a heading only over something";
}

// What a function is for is in its file, beside its graph: the engine's are offered under it, with what they do.
TEST_F(DocumentTest, AnEngineFunctionSaysWhatItIsFor) {
    scene::MakeProject("function purposes");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const keditor::FunctionAbout about = doc.AboutFunction("XpbdStep");
    EXPECT_EQ(about.purpose, "Physics");
    EXPECT_FALSE(about.description.empty());
    EXPECT_TRUE(std::ranges::contains(about.tags, std::string("rigid body")));
    const auto presets = doc.Presets();
    const auto call = std::ranges::find(presets, std::string("XpbdStep"), &kgraph::Preset::name);
    ASSERT_NE(call, presets.end());
    EXPECT_EQ(call->group, "Physics");
    const auto found = keditor::FindNodeTypes("prefix sum", presets);
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(found.front()->name, "Scan") << "found by what it does, not only its name";
}

// A Bundle let go on a buffer is offered first as what it fills by name, with how many of its members match.
TEST_F(DocumentTest, ABundleLetGoOnABufferFillsItByName) {
    scene::MakeProject("bundle choices");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const std::string lamps = doc.AddNode("ForEach", 0, 0, "lamps");
    ASSERT_TRUE(doc.SetInput(lamps, "component", kgraph::Value("PointLight")));
    const std::string data = doc.AddNode("BufferBuilder", 300, 0, "lamp data");
    ASSERT_TRUE(doc.SetInput(data, "layout", kgraph::Value(kgraph::Value::List {
        kgraph::Value::Members { { "name", "color" }, { "type", "eVec3" } }, kgraph::Value::Members { { "name", "range" }, { "type", "eFloat" } },
        kgraph::Value::Members { { "name", "glow" }, { "type", "eFloat" } } })));
    Frame();

    const auto choices = keditor::PortChoices(doc, lamps, "element", true, data);
    ASSERT_FALSE(choices.empty());
    EXPECT_EQ(choices.front().port, "fields");
    EXPECT_TRUE(choices.front().byName);
    EXPECT_NE(choices.front().label.find("By name: 2 of"), std::string::npos) << choices.front().label;
    EXPECT_TRUE(std::ranges::none_of(choices, [](const keditor::PortChoice& c) { return c.fits && c.port == "field.color"; })) << "a Bundle goes only where one is taken";
}

// A call opened and kept is given what it was given, at every port: not only the first of its function's inputs.
TEST_F(DocumentTest, AFunctionOpenedAndKeptLeavesItsCallWiredAsItWas) {
    using kgraph::Value;
    const keditor::Project project = scene::MakeProject("functions-kept");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    doc.SetAssetDirectory(project.directory / "assets");
    doc.AddNode("Double", 0, 0, "n");
    doc.SetInput("n", "value", 2);
    doc.AddNode("Double", 0, 100, "m");
    doc.SetInput("m", "value", 5);
    doc.AddNode("Multiply", 300, 0, "times");
    ASSERT_TRUE(doc.Connect("n", "out", "times", "a"));
    ASSERT_TRUE(doc.Connect("m", "out", "times", "b"));
    doc.AddNode("Add", 600, 0, "plus");
    ASSERT_TRUE(doc.Connect("times", "out", "plus", "a"));
    doc.SetInput("plus", "b", 1);
    doc.AddNode("Add", 900, 0, "show");
    ASSERT_TRUE(doc.Connect("plus", "out", "show", "a"));
    std::string error;
    const std::string call = doc.MakeFunction({ "times", "plus" }, "Affine", &error);
    ASSERT_FALSE(call.empty()) << error;
    ASSERT_EQ(doc.Runtime().ValueOf("show"), Value(11));
    const auto inputs = doc.Graph().Find(call)->inputs;
    ASSERT_EQ(inputs.size(), 3u) << "function, a, b";

    ASSERT_FALSE(doc.OpenFunction(call, &error).empty()) << error;
    ASSERT_TRUE(doc.CloseFunction(true, &error)) << error;
    const auto after = doc.Graph().Find(call)->inputs;
    EXPECT_EQ(after.size(), inputs.size());
    for (const auto& [port, input] : inputs) {
        ASSERT_TRUE(after.contains(port)) << port << " was lost";
        EXPECT_EQ(after.at(port).link.has_value(), input.link.has_value()) << port;
    }
    EXPECT_EQ(doc.Runtime().ValueOf("show"), Value(11));
}

// What is set of an input is what the input takes, whatever the value given: an Int's a whole number, a UInt's none or more,
// a Float's a float's, a vector's as many as it has — a Random's own `type` among them, and its kind followed when Auto.
TEST_F(DocumentTest, WhatIsSetIsWhatTheInputTakes) {
    using kgraph::Value;
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const auto given = [&](const char* node, const char* input) {
        const kgraph::Node* found = doc.Graph().Find(node);
        const auto it = found ? found->inputs.find(input) : decltype(found->inputs.end()) {};
        return found && it != found->inputs.end() ? it->second.value : Value();
    };
    doc.AddNode("Int", 0, 0, "whole");
    doc.SetInput("whole", "value", 2.6);
    EXPECT_EQ(given("whole", "value"), Value(3.0));
    doc.AddNode("UInt", 0, 0, "count");
    doc.SetInput("count", "value", -4);
    EXPECT_EQ(given("count", "value"), Value(0.0));
    doc.AddNode("Float", 0, 0, "single");
    doc.SetInput("single", "value", 0.1);
    EXPECT_EQ(given("single", "value"), Value(static_cast<double>(0.1f)));
    doc.AddNode("Double", 0, 0, "precise");
    doc.SetInput("precise", "value", 0.1);
    EXPECT_EQ(given("precise", "value"), Value(0.1));
    doc.AddNode("Add", 0, 0, "any");
    doc.SetInput("any", "a", 2.6);
    EXPECT_EQ(given("any", "a"), Value(2.6)) << "what takes any kind, as it is given";

    doc.AddNode("RandomUniform", 0, 0, "dice");
    doc.SetInput("dice", "min", 1);
    doc.SetInput("dice", "max", 6);
    ASSERT_TRUE(doc.Connect("dice", "out", "count", "value"));
    const Value rolled = doc.Runtime().ValueOf("dice");
    ASSERT_TRUE(rolled.Number()) << rolled.ToJson();
    EXPECT_EQ(*rolled.Number(), std::trunc(*rolled.Number())) << "of the UInt it is wired to: whole";
}

// The engine's Bloom opened in place: every node of its page as it runs in the call, none of them failing.
TEST_F(DocumentTest, TheEnginesBloomOpenedInPlaceHasNothingWrong) {
    using kgraph::Value;
    const keditor::Project project = scene::MakeProject("bloom-opened");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    doc.SetAssetDirectory(project.directory / "assets");
    doc.AddNode("Image", 0, 0, "picture");
    doc.SetInput("picture", "format", "eRGBA16_SFLOAT");
    doc.SetInput("picture", "extent", Value(Value::List { 64, 64, 1 }));
    doc.AddNode("Clear", 300, 0, "paint");
    doc.Connect("picture", "out", "paint", "image");
    doc.SetInput("paint", "color", Value(Value::List { 4, 4, 4, 1 }));
    doc.AddNode("Sampler", 0, 200, "linear");
    doc.SetInput("linear", "minFilter", "eLinear");
    doc.SetInput("linear", "magFilter", "eLinear");
    const std::string call = doc.AddNode("Function", 600, 0, "bloom");
    ASSERT_TRUE(doc.SetInput(call, "function", "Bloom"));
    ASSERT_TRUE(doc.Connect("picture", "out", call, "picture"));
    ASSERT_TRUE(doc.Connect("linear", "out", call, "sampler"));
    for (int i = 0; i < 3; ++i) support::Frame();
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << "called: " << problem;
    std::string error;
    const std::string page = doc.OpenFunction(call, &error);
    ASSERT_FALSE(page.empty()) << error;
    for (int i = 0; i < 3; ++i) support::Frame();
    for (const auto& node : doc.Graph().nodes) {
        if (node.pass != page) continue;
        const auto* status = doc.Runtime().Status(node.id);
        if (!status) { ADD_FAILURE() << node.id << " (" << node.type << "): no status"; continue; }
        EXPECT_TRUE(status->ok) << node.id << " (" << node.type << "): " << status->error;
        EXPECT_TRUE(status->note.empty()) << node.id << " (" << node.type << "): " << status->note;
    }
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << "open: " << problem;
    // Kept, it is the function it was: its loops name their own nodes, not the call's.
    ASSERT_TRUE(doc.CloseFunction(true, &error)) << error;
    for (int i = 0; i < 3; ++i) support::Frame();
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << "kept: " << problem;
    const auto kept = doc.Graph().functions.at("Bloom");
    ASSERT_NE(kept->Find("level above"), nullptr);
    EXPECT_EQ(kept->Find("level above")->inputs.at("of").value, Value("down level"));
    // Written out, its nodes are the graph's, and still work.
    ASSERT_TRUE(doc.WriteOut(call, &error)) << error;
    for (int i = 0; i < 3; ++i) support::Frame();
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << "written out: " << problem;
    EXPECT_EQ(doc.Graph().Find("bloom/level above")->inputs.at("of").value, Value("bloom/down level"));
}

// A model's materials are imported as what its file reports them: an OBJ's the classic way, as BlinnPhongMaterials, each
// an asset its things follow. Changed in the asset, every thing that follows it is — but for what one made its own,
// which it keeps until it is applied to the asset, reverted, or the thing unlinked. What was changed of a model's is
// kept beside it.
TEST_F(DocumentTest, AModelsMaterialsAreAssetsItsThingsFollow) {
    const auto project = scene::MakeProject("materials");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const auto models = project.directory / "assets" / "models";
    std::filesystem::create_directories(models);
    {
        std::ofstream mtl(models / "two.mtl");
        mtl << "newmtl red\nKd 1 0 0\nKs 0.5 0.5 0.5\nNs 32\nillum 2\nnewmtl glass\nKd 0.2 0.4 0.8\nd 0.25\nillum 2\n";
        std::ofstream obj(models / "two.obj");
        obj << "mtllib two.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
               "o a\nusemtl red\nf 1 2 3\no b\nusemtl red\nf 2 4 3\no pane\nusemtl glass\nf 1 2 4\n";
    }
    doc.SetAssetDirectory(project.directory / "assets");
    const kecs::Entity root = doc.ImportModel(models / "two.obj");
    ASSERT_FALSE(root.IsNull());
    for (int i = 0; i < 400 && (i < 2 || doc.Reading() > 0); ++i) Frame();

    const kecs::ComponentId mesh = doc.World().Find("Mesh");
    std::map<std::string, kecs::Entity> drawn;
    for (const kecs::Entity entity : doc.World().All())
        if (doc.World().Has(entity, mesh)) drawn[doc.NameOf(entity)] = entity;
    ASSERT_EQ(drawn.size(), 3u) << "a thing for each of the file's";
    const kecs::Entity a = drawn.begin()->second, b = std::next(drawn.begin())->second;
    kecs::Entity pane = kecs::NullEntity;
    for (const auto& [name, entity] : drawn) if (name.find("pane") != std::string::npos) pane = entity;
    ASSERT_FALSE(pane.IsNull());

    const std::string red = doc.MaterialAssetOfEntity(a, "BlinnPhongMaterial");
    ASSERT_FALSE(red.empty()) << "an OBJ's material is classic: a BlinnPhongMaterial";
    EXPECT_EQ(red.substr(0, red.find('#')), "@models/two.obj");
    EXPECT_EQ(doc.MaterialAssetOfEntity(b, "BlinnPhongMaterial"), red) << "two things of one material follow one asset";
    EXPECT_NE(doc.MaterialAssetOfEntity(pane, "BlinnPhongMaterial"), red);
    EXPECT_EQ(doc.Field(a, "BlinnPhongMaterial", "diffuse"), kgraph::Value(kgraph::Value::List { 1.0, 0.0, 0.0, 1.0 }));
    EXPECT_EQ(doc.Field(a, "BlinnPhongMaterial", "shininess"), kgraph::Value(32.0));
    EXPECT_TRUE(doc.World().Has(pane, doc.World().Find("Blended"))) << "see-through by its opacity: drawn over what is behind it";
    EXPECT_FALSE(doc.World().Has(a, doc.World().Find("Blended")));
    ASSERT_NE(doc.MaterialAssetOf(red), nullptr);
    EXPECT_EQ(doc.MaterialAssetOf(red)->kind, "BlinnPhongMaterial");

    // Changed in the asset: in everything that follows it, and kept beside the model.
    ASSERT_TRUE(doc.SetMaterialAssetField(red, "shininess", kgraph::Value(64)));
    EXPECT_EQ(doc.Field(a, "BlinnPhongMaterial", "shininess"), kgraph::Value(64.0));
    EXPECT_EQ(doc.Field(b, "BlinnPhongMaterial", "shininess"), kgraph::Value(64.0));
    EXPECT_TRUE(std::filesystem::exists(models / "two.obj.materials.json"));

    // Changed on a thing: its own, which the asset no longer sets.
    ASSERT_TRUE(doc.SetField(a, "BlinnPhongMaterial", "shininess", kgraph::Value(8)));
    EXPECT_EQ(doc.MaterialOverridesOf(a, "BlinnPhongMaterial"), std::vector<std::string> { "shininess" });
    ASSERT_TRUE(doc.SetMaterialAssetField(red, "shininess", kgraph::Value(16)));
    EXPECT_EQ(doc.Field(a, "BlinnPhongMaterial", "shininess"), kgraph::Value(8.0)) << "its own";
    EXPECT_EQ(doc.Field(b, "BlinnPhongMaterial", "shininess"), kgraph::Value(16.0));

    // Applied: the asset's, and so every thing's that follows it.
    ASSERT_TRUE(doc.ApplyMaterial(a, "BlinnPhongMaterial"));
    EXPECT_TRUE(doc.MaterialOverridesOf(a, "BlinnPhongMaterial").empty());
    EXPECT_EQ(doc.Field(b, "BlinnPhongMaterial", "shininess"), kgraph::Value(8.0));

    // Reverted: as its asset is.
    ASSERT_TRUE(doc.SetField(b, "BlinnPhongMaterial", "specular", kgraph::Value(kgraph::Value::List { 1.0, 1.0, 1.0 })));
    ASSERT_TRUE(doc.RevertMaterial(b, "BlinnPhongMaterial"));
    EXPECT_TRUE(doc.MaterialOverridesOf(b, "BlinnPhongMaterial").empty());
    EXPECT_EQ(doc.Field(b, "BlinnPhongMaterial", "specular"), doc.Field(a, "BlinnPhongMaterial", "specular"));

    // Unlinked: its own altogether.
    ASSERT_TRUE(doc.UnlinkMaterial(b, "BlinnPhongMaterial"));
    EXPECT_TRUE(doc.MaterialAssetOfEntity(b, "BlinnPhongMaterial").empty());
    ASSERT_TRUE(doc.SetMaterialAssetField(red, "shininess", kgraph::Value(100)));
    EXPECT_EQ(doc.Field(b, "BlinnPhongMaterial", "shininess"), kgraph::Value(8.0));
    EXPECT_EQ(doc.Field(a, "BlinnPhongMaterial", "shininess"), kgraph::Value(100.0));

    // Made a material of its own again, an alpha mode sets what it is tagged.
    ASSERT_TRUE(doc.SetField(pane, "BlinnPhongMaterial", "alphaMode", kgraph::Value(0)));
    EXPECT_FALSE(doc.World().Has(pane, doc.World().Find("Blended")));
}

// A thing of a model's mesh whose material is its own — as a scene saved before materials were their own is migrated —
// follows the material its mesh has in the file once the model is read. What it set that the file says otherwise is
// kept as its own; what is only written otherwise (a flat normal map as white) is the file's.
TEST_F(DocumentTest, AThingOfAModelsMeshFollowsTheModelsMaterial) {
    const auto project = scene::MakeProject("follow materials");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const auto models = project.directory / "assets" / "models";
    std::filesystem::create_directories(models);
    {
        std::ofstream mtl(models / "one.mtl");
        mtl << "newmtl red\nKd 1 0 0\nillum 2\n";
        std::ofstream obj(models / "one.obj");
        obj << "mtllib one.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\no a\nusemtl red\nf 1 2 3\no b\nusemtl red\nf 2 4 3\n";
    }
    doc.SetAssetDirectory(project.directory / "assets");
    doc.SetImportKind(kgraph::ModelMaterial::Model::eClassic, "PbrMaterial");
    ASSERT_FALSE(doc.ImportModel(models / "one.obj").IsNull());
    for (int i = 0; i < 400 && (i < 2 || doc.Reading() > 0); ++i) Frame();
    const kecs::ComponentId mesh = doc.World().Find("Mesh");
    std::vector<kecs::Entity> drawn;
    for (const kecs::Entity entity : doc.World().All()) if (doc.World().Has(entity, mesh)) drawn.push_back(entity);
    ASSERT_EQ(drawn.size(), 2u);
    const kecs::Entity a = drawn[0], b = drawn[1];
    const std::string red = doc.MaterialAssetOfEntity(a, "PbrMaterial");
    ASSERT_FALSE(red.empty());

    // Its own: a roughness the file does not give, and a flat normal map written as white.
    ASSERT_TRUE(doc.UnlinkMaterial(a, "PbrMaterial"));
    ASSERT_TRUE(doc.SetField(a, "PbrMaterial", "roughness", kgraph::Value(0.125)));
    ASSERT_TRUE(doc.SetField(a, "PbrMaterial", "normalMap", kgraph::Value("$white")));
    doc.FollowModelMaterial(a);
    doc.FollowModelMaterial(b);     // follows already: nothing
    EXPECT_GT(doc.Reading(), 0u) << "until its model is read";
    for (int i = 0; i < 400 && doc.Reading() > 0; ++i) Frame();

    EXPECT_EQ(doc.MaterialAssetOfEntity(a, "PbrMaterial"), red) << "the file's material, shared";
    EXPECT_EQ(doc.MaterialOverridesOf(a, "PbrMaterial"), std::vector<std::string> { "roughness" }) << "what it set otherwise, its own";
    EXPECT_EQ(doc.Field(a, "PbrMaterial", "roughness"), kgraph::Value(0.125));
    EXPECT_EQ(doc.Field(a, "PbrMaterial", "normalMap"), doc.Field(b, "PbrMaterial", "normalMap")) << "white is the flat normal, as the file has it";
    EXPECT_TRUE(doc.MaterialOverridesOf(b, "PbrMaterial").empty());
}

// Everything a model file has is imported as it has it: its meshes and materials, and its lights and cameras — each on
// the thing of the file's it is on, which is where it is and which way it faces, not a thing of its own beside it.
TEST_F(DocumentTest, AModelsLightsAndCamerasAreImportedOnTheirThings) {
    const auto project = scene::MakeProject("lights and cameras");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    const auto models = project.directory / "assets" / "models";
    std::filesystem::create_directories(models);
    std::ofstream(models / "room.gltf") << R"GLTF({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_lights_punctual"],
  "extensions": {"KHR_lights_punctual": {"lights": [{"type": "point", "color": [1.0, 0.5, 0.25], "intensity": 7.0, "range": 9.0}]}},
  "scene": 0,
  "scenes": [{"nodes": [0, 1, 2, 3]}],
  "nodes": [
    {"name": "floor", "mesh": 0},
    {"name": "eye", "translation": [1.0, 2.0, 3.0], "camera": 0},
    {"name": "plan", "translation": [0.0, 10.0, 0.0], "camera": 1},
    {"name": "lamp", "translation": [0.0, 4.0, 0.0], "extensions": {"KHR_lights_punctual": {"light": 0}}}
  ],
  "cameras": [
    {"type": "perspective", "perspective": {"yfov": 0.8, "znear": 0.05, "zfar": 500.0}},
    {"type": "orthographic", "orthographic": {"xmag": 4.0, "ymag": 3.0, "znear": 0.1, "zfar": 50.0}}
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]}]
})GLTF";
    doc.SetAssetDirectory(project.directory / "assets");
    const kecs::Entity root = doc.ImportModel(models / "room.gltf");
    ASSERT_FALSE(root.IsNull());
    for (int i = 0; i < 400 && (i < 2 || doc.Reading() > 0); ++i) Frame();

    const auto named = [&](const std::string& name) {
        std::vector<kecs::Entity> found;
        for (const kecs::Entity entity : doc.World().All()) if (doc.NameOf(entity) == name) found.push_back(entity);
        return found;
    };
    ASSERT_EQ(named("eye").size(), 1u) << "the camera is on its thing, not a thing of its own beside it";
    ASSERT_EQ(named("lamp").size(), 1u);
    const kecs::Entity eye = named("eye")[0], plan = named("plan")[0], lamp = named("lamp")[0];

    ASSERT_TRUE(doc.World().Has(eye, doc.World().Find("Camera")));
    EXPECT_NEAR(*doc.Field(eye, "Camera", "fieldOfView").Number(), 0.8 * 180.0 / std::numbers::pi, 1e-3) << "top to bottom, as the file has it";
    EXPECT_NEAR(*doc.Field(eye, "Camera", "near").Number(), 0.05, 1e-6);
    EXPECT_NEAR(*doc.Field(eye, "Camera", "far").Number(), 500.0, 1e-3);
    EXPECT_EQ(doc.Field(eye, "Camera", "orthographic"), kgraph::Value(false));
    EXPECT_EQ(doc.World().PositionOf(eye), (kecs::Vec3 { 1.f, 2.f, 3.f })) << "where its thing is";

    ASSERT_TRUE(doc.World().Has(plan, doc.World().Find("Camera")));
    EXPECT_EQ(doc.Field(plan, "Camera", "orthographic"), kgraph::Value(true));
    EXPECT_NEAR(*doc.Field(plan, "Camera", "height").Number(), 6.0, 1e-6);

    ASSERT_TRUE(doc.World().Has(lamp, doc.World().Find("PointLight")));
    EXPECT_NEAR(*doc.Field(lamp, "PointLight", "intensity").Number(), 7.0, 1e-6);
    EXPECT_NEAR(*doc.Field(lamp, "PointLight", "range").Number(), 9.0, 1e-6);
    EXPECT_EQ(doc.World().PositionOf(lamp), (kecs::Vec3 { 0.f, 4.f, 0.f }));
    EXPECT_FALSE(named("floor").empty()) << "and its mesh";
}

// What a reported model is imported as is the project's to say — for every file, or for one.
TEST_F(DocumentTest, WhatAMaterialIsImportedAsIsAChoice) {
    scene::MakeProject("import kinds");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    using Model = kgraph::ModelMaterial::Model;
    EXPECT_EQ(doc.ImportKind(Model::ePbr), "PbrMaterial");
    EXPECT_EQ(doc.ImportKind(Model::eClassic), "BlinnPhongMaterial");
    doc.SetImportKind(Model::eClassic, "PbrMaterial", "models/old.obj");
    EXPECT_EQ(doc.ImportKind(Model::eClassic, "models/old.obj"), "PbrMaterial");
    EXPECT_EQ(doc.ImportKind(Model::eClassic, "models/other.obj"), "BlinnPhongMaterial");
    EXPECT_EQ(doc.ImportKind(Model::ePbr, "models/old.obj"), "PbrMaterial") << "what that file does not say is every file's";
    doc.SetImportKind(Model::eClassic, "", "models/old.obj");
    EXPECT_EQ(doc.ImportKind(Model::eClassic, "models/old.obj"), "BlinnPhongMaterial");
}
