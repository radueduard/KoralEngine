// A scene made through the document — as the editor's panels make it — then saved, and run again from the file by
// the scene a project runs in. Offscreen, with no windowing system.

#include <gtest/gtest.h>

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
