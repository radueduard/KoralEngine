// A scene made through the document — as the editor's panels make it — then saved, and run again from the file by
// the scene a project runs in. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <set>
#include <ranges>

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
    EXPECT_EQ(doc.NodesFor("Mesh"), (std::vector<std::string> { "square", "$cube", "$sphere", "$plane", "$cylinder", "$cone", "$capsule", "$torus" }))
        << "the node that makes one, and the meshes there are without a node";

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
    doc.AddNode("Number", 0, 0, "n", "first");
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
        doc.AddNode("Number", 0, 0, "n");
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
        EXPECT_EQ(doc.Functions(), (std::vector<std::string> { "Scale" }));
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
    doc.AddNode("Number", 0, 0, "a", "first");
    doc.AddNode("Number", 0, 100, "b", "first");
    doc.AddNode("Number", 0, 0, "c", "second");
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
    doc.AddNode("Number", -400, -300, "d", "second");
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
