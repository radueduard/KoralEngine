// The editor's interface, offscreen: the panels over a scene, showing what the document holds and following it as it
// changes. With KEDITOR_SHEET set to a file's path, the editor as it looks with the tests' scene in it is written
// there (a PPM), to be looked at.

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

#include <keditor/editor.h>
#include <koralUI.h>

#include "scene.h"
#include "support.h"

namespace {
    using support::Frame;
    constexpr kor::UVec2 Extent { 1500, 900 };

    class EditorTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
        }
        void Open(const std::filesystem::path& project = {}) {
            editor = support::App()->OpenOffscreen<keditor::EditorScene>({ .title = "editor", .extent = Extent }, project);
            ASSERT_NE(editor, nullptr);
            Frame();
            Frame();
        }
        void TearDown() override {
            if (editor) { support::App()->Close(*editor); Frame(); Frame(); }
        }
        keditor::Session& Session() { return editor->CurrentSession(); }
        keditor::Document& Doc() { return Session().Doc(); }
        /** Whether the interface shows @p text anywhere, as the whole of something or part of it. */
        [[nodiscard]] bool Shows(const std::string_view text) {
            for (int i = 0; i < 4; ++i) Frame();
            return std::ranges::any_of(kui::debug::Texts(editor->Interface()), [&](const std::string& shown) { return shown.find(text) != std::string::npos; });
        }
        void Click(const kor::Vec2 at) {
            auto& input = editor->SceneInput();
            input.FeedMousePosition(at);
            Frame();
            input.FeedMouseButton(kor::MouseButton::eLeft, true);
            Frame();
            input.FeedMouseButton(kor::MouseButton::eLeft, false);
            Frame();
            Frame();
        }
        void RightClick(const kor::Vec2 at) {
            auto& input = editor->SceneInput();
            input.FeedMousePosition(at);
            Frame();
            input.FeedMouseButton(kor::MouseButton::eRight, true);
            Frame();
            input.FeedMouseButton(kor::MouseButton::eRight, false);
            Frame();
            Frame();
        }
        /** Writes the editor as it looks now to `<KEDITOR_SHEET><name>.ppm`, when that is set. */
        void Sheet(const std::string& name) {
            const char* sheet = std::getenv("KEDITOR_SHEET");
            if (!sheet) return;
            const auto readback = kor::Buffer::RawBuilder{}.SetRawSize(Extent.x * Extent.y * 4).SetUsage(kor::Buffer::Usage::eTransferDst)
                .SetType(kor::Buffer::Type::eReadback).Build();
            auto& pass = editor->Graph().Add<support::ReadScreen>(kor::ResourceRef<const kor::Buffer>(readback));
            for (int i = 0; i < 4; ++i) Frame();
            const auto pixels = readback->Read<kor::U8Vec4>(Extent.x * Extent.y);
            std::ofstream out(std::string(sheet) + name + ".ppm", std::ios::binary);
            out << "P6\n" << Extent.x << " " << Extent.y << "\n255\n";
            for (const auto& pixel : pixels) out.put(static_cast<char>(pixel.x)).put(static_cast<char>(pixel.y)).put(static_cast<char>(pixel.z));
            editor->Graph().Remove(pass);
            Frame();
        }
        keditor::EditorScene* editor = nullptr;
    };
}

TEST_F(EditorTest, ThePanelsShowTheSceneAndFollowIt) {
    scene::MakeProject("editor");
    Open();
    EXPECT_TRUE(Shows("No entities yet."));
    EXPECT_TRUE(Shows("Nothing is wrong."));

    // A node, half made: it is on the graph, and what it lacks is said under Problems.
    Doc().AddNode("Dispatch", 40, 40, "canvas");
    EXPECT_TRUE(Shows("canvas"));
    EXPECT_FALSE(Shows("Nothing is wrong."));
    EXPECT_TRUE(Shows("canvas: 'pipeline' needs a ComputePipeline wired in")) << "its problem, by name";

    // The whole scene, as the tests build it.
    Doc().RemoveNodes({ "canvas" });
    scene::BuildGraph(Doc());
    const auto entities = scene::BuildEntities(Doc());
    EXPECT_TRUE(Shows("Nothing is wrong."));
    for (const char* node : { "stamp", "show", "draw", "tint backdrop", "square" }) EXPECT_TRUE(Shows(node)) << node;
    EXPECT_TRUE(Shows("Quarter 2")) << "in the outliner";

    // A node picked: the inspector has its inputs, by name, and what the runtime knows of it.
    Session().Select(std::string("run"));
    EXPECT_TRUE(Shows("Runs for 4 entities."));
    for (const char* input : { "pipeline", "cover", "groupSize.x", "with" }) EXPECT_TRUE(Shows(input)) << input;
    EXPECT_TRUE(Shows("from stamp")) << "what an input is wired to";
    Session().Select(std::string("stamp"));
    EXPECT_TRUE(Shows("param.backdrop")) << "a pipeline's inputs are its shader's";
    EXPECT_TRUE(Shows("Entities run it by having the component 'stamp'."));
    Session().Select(std::string("canvas"));
    EXPECT_TRUE(Shows("eRGBA8_UNORM")) << "an enumeration, by name";
    EXPECT_TRUE(Shows("eColorAttachment")) << "flags, each to tick";

    // An entity picked: its components and their fields.
    Session().Select(entities[2]);
    for (const char* text : { "Quarter 2", "stamp", "target", "tint", "gain", "offset", "Mesh", "Lit" }) EXPECT_TRUE(Shows(text)) << text;
    EXPECT_TRUE(Shows("image 2")) << "the node its image comes from";
    EXPECT_TRUE(Shows("0.50")) << "its gain";

    // What goes wrong with an entity is said too.
    Doc().SetField(entities[0], "show", "source", kgraph::Value {});
    EXPECT_TRUE(Shows("1 passed by."));
    EXPECT_TRUE(Shows("'source' has nothing in it"));
    Doc().SetField(entities[0], "show", "source", "image 0");
    EXPECT_FALSE(Shows("passed by."));

    Session().Select(std::string("stamp"));
    Sheet("node");
    Session().Select(entities[2]);
    Sheet("entity");
}

TEST_F(EditorTest, AProjectIsMadeSavedAndOpenedAgain) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "from-the-editor";
    std::filesystem::remove_all(directory);
    {
        scene::MakeProject("scratch");      // only for its shaders, found by name
        Open();
        scene::BuildGraph(Doc());
        scene::BuildEntities(Doc());
        EXPECT_FALSE(Session().Save()) << "there is nowhere to save it yet";
        EXPECT_TRUE(Session().Failed());
        EXPECT_TRUE(Shows("no project"));

        EXPECT_TRUE(Session().RunCommand().empty());
        EXPECT_FALSE(Session().Run()) << "nor anything to run";
        ASSERT_TRUE(Session().NewProject(directory)) << Session().Status();
        // What running it runs: the player built beside these tests' programs, on the project's folder.
        EXPECT_NE(Session().RunCommand().find("koral-play"), std::string::npos) << Session().RunCommand();
        EXPECT_NE(Session().RunCommand().find(directory.string()), std::string::npos) << Session().RunCommand();
        EXPECT_TRUE(Shows("Saved main.scene.json."));
        EXPECT_FALSE(Doc().Dirty());
        EXPECT_TRUE(std::filesystem::exists(directory / "project.json"));
        support::App()->Close(*editor);
        editor = nullptr;
        Frame();
        Frame();
    }
    for (const char* shader : { "stamp.slang", "show.slang" })
        std::filesystem::copy_file(std::filesystem::path(KEDITOR_TEST_SHADERS) / shader, directory / "shaders" / shader);

    // Another session, opened on the project's folder.
    Open(directory);
    EXPECT_TRUE(Session().HasProject()) << Session().Status();
    EXPECT_TRUE(Shows("Opened from-the-editor."));
    EXPECT_TRUE(Shows("Quarter 3"));
    EXPECT_TRUE(Shows("Nothing is wrong."));
    EXPECT_EQ(Doc().Entities().size(), 4u);
    EXPECT_EQ(Doc().Runtime().ReportOf("draw")->ran, 4u);

    // Control+S, with a change to save.
    Doc().CreateEntity("One more");
    EXPECT_TRUE(Doc().Dirty());
    auto& input = editor->SceneInput();
    input.FeedKey(kor::Key::eLeftControl, true);
    Frame();
    input.FeedKey(kor::Key::eS, true);
    Frame();
    input.FeedKey(kor::Key::eS, false);
    input.FeedKey(kor::Key::eLeftControl, false);
    Frame();
    EXPECT_FALSE(Doc().Dirty()) << Session().Status();

    EXPECT_FALSE(Session().OpenProject(directory / "nowhere"));
    EXPECT_TRUE(Shows("no project"));
    EXPECT_EQ(Doc().Entities().size(), 5u) << "what was open is still open";
}

// With the pointer, in a window of a size the panels' places are known for.
TEST_F(EditorTest, ThePointerPicksMakesAndAdds) {
    scene::MakeProject("clicks");
    Open();
    scene::BuildGraph(Doc());
    const auto entities = scene::BuildEntities(Doc());
    ASSERT_TRUE(Shows("Quarter 1"));

    // An entity in the outliner: picked, and the inspector is its.
    Click({ 95.f, 157.f });
    EXPECT_EQ(Session().selectedEntity, entities[1]);
    EXPECT_TRUE(Shows("Destroy the entity"));

    // The button over the list makes one, and picks it.
    Click({ 85.f, 85.f });
    ASSERT_EQ(Doc().Entities().size(), 5u);
    EXPECT_EQ(Session().selectedEntity, Doc().Entities().back());
    EXPECT_TRUE(Shows("Entity")) << "its name, to change";

    // A node on the graph: picked instead.
    Click({ 530.f, 198.f });
    EXPECT_EQ(Session().selectedNode, "stamp");
    EXPECT_TRUE(Session().selectedEntity.IsNull());
    EXPECT_TRUE(Shows("Entities run it by having the component 'stamp'."));

    // A right click on the graph where nothing is: every kind of node, to add one there.
    RightClick({ 880.f, 300.f });
    EXPECT_TRUE(Shows("Add a node"));
    for (const char* kind : { "ComputePipeline", "Framebuffer", "Dispatch", "Draw", "Multiply", "Primitive" }) EXPECT_TRUE(Shows(kind)) << kind;
    Sheet("add");

    // One of them picked: it is on the graph where the click was, and the inspector is its.
    const auto nodes = Doc().Graph().nodes.size();
    Click({ 393.f, 291.f });
    ASSERT_EQ(Doc().Graph().nodes.size(), nodes + 1);
    EXPECT_EQ(Doc().Graph().nodes.back().type, "Dispatch");
    EXPECT_EQ(Session().selectedNode, "dispatch");
    EXPECT_FALSE(Shows("Add a node")) << "and the dialog is gone";
    EXPECT_TRUE(Shows("dispatch: 'pipeline' needs a ComputePipeline wired in"));
}

TEST_F(EditorTest, TheMenusAndButtonsDoWhatTheySay) {
    scene::MakeProject("clicks");
    Open();
    // The Entity menu, at the top of the window: its first line makes one, and picks it.
    ASSERT_TRUE(Shows("Entity"));
    const auto before = Doc().Entities().size();
    Session().Select(Doc().CreateEntity("Made by hand"));
    EXPECT_TRUE(Shows("Made by hand"));
    EXPECT_TRUE(Shows("Destroy the entity")) << "picked: the inspector is its";
    EXPECT_EQ(Doc().Entities().size(), before + 1);
}
