// The editor's interface, offscreen: the panels over a scene, showing what the document holds and following it as it
// changes. With KEDITOR_SHEET set to a file's path, the editor as it looks with the tests' scene in it is written
// there (a PPM), to be looked at.

#include <gtest/gtest.h>
#include <array>

#include <kasset/parameters.h>

#include <kui/icons.h>
#include <functional>
#include <optional>

#include <cmath>

#include <keditor/starter.h>
#include <kecs/json.h>
#include <kscript/declarations.h>
#include <kgraph/scene.h>

#include <cstdlib>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>

#include <capiInterop.h>
#include <image.h>
#include <keditor/editor.h>
#include <keditor/scripting.h>
#include <log.h>
#include <koralUI.h>

#include "scene.h"
#include "support.h"

namespace {
    // (The panels are where the editor puts them, not where whoever runs the tests left them.)
    [[maybe_unused]] const int noLayout = setenv("KORAL_ENGINE_NO_LAYOUT", "1", 1);
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
        /**
         * Frames until nothing more arrives to be shown: the Assets panel's pictures of meshes, each a few frames apart,
         * have all come — the interface is not built again for a dozen frames running.
         */
        void Settle() {
            // Still for a dozen frames and for half a second: what the editor makes a few at a time (the nodes'
            // pictures, one an eighth of a second) goes by the clock, and frames can be quicker than that.
            auto since = std::chrono::steady_clock::now();
            for (int frame = 0, still = 0; frame < 1000 && (still < 12 || std::chrono::steady_clock::now() - since < std::chrono::milliseconds(500)); ++frame) {
                const auto before = Session().Version();
                Frame();
                if (Session().Version() == before) ++still;
                else { still = 0; since = std::chrono::steady_clock::now(); }
            }
        }
        /** Whether the interface shows @p text anywhere, as the whole of something or part of it. */
        [[nodiscard]] bool Shows(const std::string_view text) {
            for (int i = 0; i < 4; ++i) Frame();
            return std::ranges::any_of(kui::debug::Texts(editor->Interface()), [&](const std::string& shown) { return shown.find(text) != std::string::npos; });
        }
        /** Whether something is wrong now that says @p text: what the editor logs as it comes. */
        [[nodiscard]] bool HasProblem(const std::string_view text) {
            for (int i = 0; i < 4; ++i) Frame();
            return std::ranges::any_of(Session().Problems(), [&](const auto& problem) { return problem.text.find(text) != std::string::npos; });
        }
        [[nodiscard]] bool NothingIsWrong() {
            for (int i = 0; i < 4; ++i) Frame();
            return Session().Problems().empty();
        }
        /** Where something that shows @p text is on the screen — its box, top-left and bottom-right; none when it is shown nowhere. */
        std::optional<std::pair<kor::Vec2, kor::Vec2>> Where(const std::string_view text) {
            std::optional<std::pair<kor::Vec2, kor::Vec2>> found;
            std::function<void(kui::RenderObject&)> visit = [&](kui::RenderObject& o) {
                if (!found && o.DebugText() == text) found = std::pair { o.ToGlobal({ 0.f, 0.f }), o.ToGlobal(o.Size()) };
                o.VisitChildren(visit);
            };
            if (auto* root = editor->Interface().RootRenderObject()) visit(*root);
            return found;
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
    EXPECT_TRUE(NothingIsWrong());

    // A node, half made: it is on the graph, and what it lacks is said under Problems.
    Doc().AddNode("Dispatch", 40, 40, "canvas");
    EXPECT_TRUE(Shows("canvas"));
    EXPECT_FALSE(NothingIsWrong());
    EXPECT_TRUE(HasProblem("canvas: 'pipeline' needs a ComputePipeline wired in")) << "its problem, by name";

    // The whole scene, as the tests build it.
    Doc().RemoveNodes({ "canvas" });
    scene::BuildGraph(Doc());
    const auto entities = scene::BuildEntities(Doc());
    EXPECT_TRUE(NothingIsWrong());
    for (const char* node : { "stamp", "show", "draw", "tint backdrop", "square" }) EXPECT_TRUE(Shows(node)) << node;
    EXPECT_TRUE(Shows("Quarter 2")) << "in the outliner";

    // A node picked: the inspector has its inputs, by name, and what the runtime knows of it.
    Session().Select(std::string("run"));
    EXPECT_TRUE(Shows("Runs for 4 entities."));
    for (const char* input : { "pipeline", "cover", "groupSize", "target", "tint" }) EXPECT_TRUE(Shows(input)) << input;
    EXPECT_FALSE(Shows("groupSize.x")) << "the parts of one thing are one line, under its name";
    EXPECT_TRUE(Shows("Indirect")) << "what it can do without is there too, in a part of its own";
    Session().OpenSection("run", "Indirect", true);
    EXPECT_TRUE(Shows("indirectOffset")) << "as it starts, opened";
    EXPECT_FALSE(Doc().Graph().Find("run")->inputs.contains("indirectOffset")) << "shown, it is not given";
    EXPECT_FALSE(Shows("more it can be given")) << "no menu of what else it takes: all of it is there";
    EXPECT_TRUE(Shows("from stamp")) << "what an input is wired to";
    EXPECT_TRUE(Shows("from lit")) << "and what each element gives it";
    Session().Select(std::string("lit"));
    EXPECT_TRUE(Shows("Lit")) << "the components a ForEach asks for, to tick";
    Session().Select(std::string("stamp"));
    EXPECT_FALSE(Shows("compute.module")) << "a pipeline's shader is a slot, not an input";
    EXPECT_TRUE(Shows("computeMain")) << "and the entry point in it";
    EXPECT_TRUE(Shows("backdrop")) << "a pipeline's inputs are its shader's, by the shader's own names";
    EXPECT_FALSE(Shows("param.backdrop")) << "what kind of input it is is no part of what it is called";
    EXPECT_TRUE(Shows("What its draws take from each entity is the component 'stamp'"));
    Session().Select(std::string("canvas"));
    EXPECT_TRUE(Shows("eRGBA8_UNORM")) << "an enumeration, by name";
    EXPECT_FALSE(Shows("eColorAttachment")) << "no usage to tick: an image is made for what uses it";
    EXPECT_TRUE(Shows("extent")) << "its size, on one line";
    EXPECT_TRUE(Shows("mipLevels")) << "an image is made with its format and size, and the rest is there as it starts";
    EXPECT_TRUE(Doc().Runtime().Ok());
    EXPECT_FALSE(Doc().Graph().Find("canvas")->inputs.contains("mipLevels")) << "nothing is given until it is set";

    // A graphics pipeline is drawn one of two ways, and shows the inputs of the one it is.
    Session().Select(std::string("show"));
    for (const char* input : { "stages", "eVertex", "vertex.layout", "framebuffer", "source" }) EXPECT_TRUE(Shows(input)) << input;
    EXPECT_TRUE(Shows("Add a geometry shader") && Shows("rasterizationState")) << "what it can do without, as it starts";
    EXPECT_FALSE(Shows("vertex.module") || Shows("fragment.entry")) << "its shaders are slots, not inputs";
    EXPECT_FALSE(Shows("No mesh shader")) << "and nothing of the other way of drawing";
    const std::string drawn = Doc().AddNode("GraphicsPipeline", 900, 40, "drawn");
    Session().Select(drawn);
    EXPECT_TRUE(Shows("No vertex shader"));
    Doc().SetInput(drawn, "stages", "eMesh");
    EXPECT_TRUE(Shows("No mesh shader")) << "with mesh shaders, it has a mesh shader";
    EXPECT_FALSE(Shows("No vertex shader") || Shows("Add a geometry shader")) << "and no vertex shader";
    Doc().RemoveNodes({ drawn });

    // An entity picked: its components and their fields.
    Session().Select(entities[2]);
    for (const char* text : { "Quarter 2", "stamp", "target", "tint", "gain", "offset", "Mesh", "Lit" }) EXPECT_TRUE(Shows(text)) << text;
    EXPECT_TRUE(Shows("image 2")) << "the node its image comes from";
    EXPECT_TRUE(Shows("0.50")) << "its gain";

    // What goes wrong with an entity is said too.
    Doc().SetField(entities[0], "show", "source", kgraph::Value {});
    EXPECT_TRUE(HasProblem("1 passed by."));
    EXPECT_TRUE(HasProblem("'source' has nothing in it"));
    Doc().SetField(entities[0], "show", "source", "image 0");
    EXPECT_FALSE(HasProblem("passed by."));

    Session().Select(std::string("stamp"));
    Sheet("node");
    Session().Select(entities[2]);
    Sheet("entity");
}

TEST_F(EditorTest, WhatGoesWrongIsLogged) {
    scene::MakeProject("editor");
    Open();
    // What the editor says at its foot is logged, once however often it is said, and as an error when it went wrong.
    auto before = kor::log::LastSequence();
    Session().Say("Made nothing in particular.");
    Session().Say("Made nothing in particular.");
    Session().Say("It could not be done.", true);
    const auto said = kor::log::HistorySince(before);
    ASSERT_EQ(said.size(), 2u);
    EXPECT_EQ(said[0].level, kor::log::Level::eInfo);
    EXPECT_EQ(said[0].message, "Made nothing in particular.");
    EXPECT_EQ(said[1].level, kor::log::Level::eError);

    // A problem is logged as an error when it comes, and once it is mended, that it is.
    const auto logged = [&](const std::string_view text) {
        for (int i = 0; i < 30; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); Frame(); }
        return std::ranges::count_if(kor::log::HistorySince(before), [&](const auto& record) { return record.message.find(text) != std::string::npos; });
    };
    before = kor::log::LastSequence();
    Doc().AddNode("Dispatch", 40, 40, "canvas");
    EXPECT_EQ(logged("canvas: 'pipeline' needs a ComputePipeline wired in"), 1) << "once, however long it stays wrong";
    const auto since = kor::log::HistorySince(before);
    const auto error = std::ranges::find_if(since, [](const auto& record) { return record.message.starts_with("canvas: 'pipeline'"); });
    ASSERT_NE(error, since.end());
    EXPECT_EQ(error->level, kor::log::Level::eError);
    Doc().RemoveNodes({ "canvas" });
    EXPECT_EQ(logged("Resolved: canvas: 'pipeline' needs a ComputePipeline wired in"), 1);

    // The Log panel, from the rail down the side, lists them, the latest in sight.
    Click({ 20.f, 787.f });
    EXPECT_TRUE(Shows("Resolved: canvas: 'pipeline' needs"));
    EXPECT_TRUE(Shows("Errors  "));
    Sheet("log");
}

// A Draw or a Dispatch run once an element says how many it runs a frame, and what it would take to be one: what of it is
// each element's, which its shader would read from a buffer of instances. One whose shader reads them so already is
// offered `batched`, and taken up it is one draw of them all.
TEST_F(EditorTest, ADrawOfEachElementIsOfferedBeingOne) {
    scene::MakeProject("editor");
    Open();
    scene::BuildGraph(Doc());
    scene::BuildEntities(Doc());
    EXPECT_TRUE(NothingIsWrong());

    Session().Select(std::string("draw"));
    EXPECT_TRUE(Shows("4 draws a frame, one an element."));
    EXPECT_TRUE(Shows("'param.source', 'push.offset', which are each element's now")) << "what its shader takes of each, by name";
    EXPECT_FALSE(Shows("Batch: one draw of them all")) << "not offered: its shader takes them as parameters and push constants";

    Session().Select(std::string("run"));
    EXPECT_TRUE(Shows("4 dispatches a frame, one an element."));
    EXPECT_TRUE(Shows("wired to a Filter's entities ('entities'), it would run once for all of them"));

    // Its shader made to read each instance's from a buffer of them, and nothing of each element wired but its mesh.
    Doc().SetInput("show", "vertex.module", "showBatched");
    Doc().SetInput("show", "fragment.module", "showBatched");
    EXPECT_TRUE(Doc().ClearInput("draw", "param.source"));
    EXPECT_TRUE(Doc().ClearInput("draw", "push.offset"));
    Session().Select(std::string("draw"));
    EXPECT_TRUE(Shows("Batch: one draw of them all")) << "offered, at a click";
    Session().Batch("draw");
    EXPECT_EQ(Doc().Graph().Find("draw")->inputs.at("batched").value, kgraph::Value(true));
    EXPECT_TRUE(Shows("Batched: one draw of 4 elements."));
    EXPECT_FALSE(Shows("Batch: one draw of them all"));
    EXPECT_TRUE(NothingIsWrong());
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
    EXPECT_TRUE(NothingIsWrong());
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

    // The project's shaders, for a pipeline to name: its modules, and the entry points of each.
    EXPECT_EQ(Session().ShaderModules(), (std::vector<std::string> { "show", "stamp" }));
    EXPECT_EQ(Session().EntryPoints("show"), (std::vector<std::string> { "vertexMain", "fragmentMain" }));
    EXPECT_EQ(Session().EntryPoints("stamp"), std::vector<std::string> { "computeMain" });
    EXPECT_TRUE(Session().EntryPoints("no such module").empty());
    // Listed among the assets by entry, each of its module and stage.
    {
        const auto assets = Session().Assets();
        const auto entry = [&](const std::string& id) { return std::ranges::find(assets, id, &keditor::Session::Asset::id); };
        ASSERT_NE(entry("shader:show#vertexMain"), assets.end());
        ASSERT_NE(entry("shader:show#fragmentMain"), assets.end());
        ASSERT_NE(entry("shader:stamp#computeMain"), assets.end());
        ASSERT_NE(entry("shader:show"), assets.end()) << "the module, its entries under it";
        EXPECT_TRUE(entry("shader:show")->stage.empty());
        EXPECT_EQ(entry("shader:show#vertexMain")->parent, "shader:show");
        EXPECT_LT(entry("shader:show"), entry("shader:show#vertexMain")) << "the module first";
        EXPECT_EQ(entry("shader:show#fragmentMain")->group, "show");
        EXPECT_EQ(entry("shader:show#fragmentMain")->label, "fragmentMain");
        EXPECT_EQ(entry("shader:show#fragmentMain")->stage, "fragment");
        EXPECT_EQ(entry("shader:stamp#computeMain")->stage, "compute");
        EXPECT_LT(entry("shader:show#vertexMain"), entry("shader:show#fragmentMain")) << "as the module declares them";
    }

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

    // The button over the list offers what there is to make — an empty entity first — and what is made is picked.
    Click({ 80.f, 91.f });
    EXPECT_TRUE(Shows("Empty entity"));
    Click({ 75.f, 137.f });
    ASSERT_EQ(Doc().Entities().size(), 5u);
    EXPECT_EQ(Session().selectedEntity, Doc().Entities().back());
    EXPECT_TRUE(Shows("Entity")) << "its name, to change";

    // A node on the graph: picked instead. (Where it is on the screen is where the graph's view has it.)
    const auto onGraph = [&](const std::string& id) {
        const kgraph::Node* node = Doc().Graph().Find(id);
        return Session().graphView->ToView({ static_cast<float>(node->x) + 30.f, static_cast<float>(node->y) + 12.f });
    };
    Click(onGraph("stamp"));
    EXPECT_EQ(Session().selectedNode, "stamp");
    EXPECT_TRUE(Session().selectedEntity.IsNull());
    EXPECT_TRUE(Shows("What its draws take from each entity is the component 'stamp'"));

    // A right click on the graph where nothing is, and the first line of its menu: every kind of node, to add one there.
    const auto& view = *Session().graphView;
    const kor::Vec2 nowhere = view.origin + kor::Vec2(view.size.x * 0.5f, view.size.y * 0.5f + 150.f);     // under the few nodes there are
    RightClick(nowhere);
    // The graph's own menu: a node there, a note, what was copied, the page put in order.
    EXPECT_TRUE(Shows("Add node..."));
    EXPECT_TRUE(Shows("Arrange this page"));
    EXPECT_FALSE(Shows("Add a node"));
    Click(nowhere + kor::Vec2(40.f, 6.f + 18.f));      // its first line
    EXPECT_TRUE(Shows("Add a node"));
    for (const char* kind : { "ComputePipeline", "Framebuffer", "Dispatch", "Draw", "Multiply", "Primitive", "Smoothstep", "Select", "GenerateMipmaps" }) EXPECT_TRUE(Shows(kind)) << kind;
    for (const char* category : { "Resources", "Pipelines", "Values", "Math", "Vectors", "Logic", "Passes" }) EXPECT_TRUE(Shows(category)) << category;
    Sheet("add");

    // Looked for by what it does, not what it is called: typed, the list is the kinds that fit, best first.
    const auto type = [&](const std::string& text) {
        for (const char c : text) { editor->SceneInput().FeedText(std::u32string(1, static_cast<char32_t>(c))); Frame(); }
        for (int i = 0; i < 3; ++i) Frame();
    };
    type("run compute");
    EXPECT_TRUE(Shows("ComputePass   ·   Passes") && Shows("Dispatch   ·   Passes")) << "found by other words for them";
    EXPECT_FALSE(Shows("GenerateMipmaps") || Shows("Smoothstep")) << "and what does not fit is gone";
    Sheet("find");
    // Enter adds the first: it is on the graph where the click was, and the inspector is its.
    const auto nodes = Doc().Graph().nodes.size();
    editor->SceneInput().FeedKey(kor::Key::eEnter, true);
    Frame();
    editor->SceneInput().FeedKey(kor::Key::eEnter, false);
    for (int i = 0; i < 3; ++i) Frame();
    ASSERT_EQ(Doc().Graph().nodes.size(), nodes + 1);
    EXPECT_EQ(Doc().Graph().nodes.back().type, "ComputePass") << "a pipeline and its dispatch, the first that fits";
    EXPECT_EQ(Session().selectedNode, "computepass");
    EXPECT_FALSE(Shows("Add a node")) << "and the dialog is gone";
    EXPECT_TRUE(Shows("computepass")) << "the inspector is its";
}

TEST_F(EditorTest, TheMenusAndButtonsDoWhatTheySay) {
    scene::MakeProject("clicks");
    Open();
    // An entity made is in the outliner, and picked.
    ASSERT_TRUE(Shows("No entities yet."));
    const auto before = Doc().Entities().size();
    Session().Select(Doc().CreateEntity("Made by hand"));
    EXPECT_TRUE(Shows("Made by hand"));
    EXPECT_TRUE(Shows("Destroy the entity")) << "picked: the inspector is its";
    EXPECT_EQ(Doc().Entities().size(), before + 1);
}

// Settings, Theme: the editor is in Koral's dark until another is chosen, and then in that one — Windows' light, here.
// A test's editor keeps nothing of this user's, so their theme is as it was.
TEST_F(EditorTest, AThemeChosenIsTheOneTheEditorIsIn) {
    Open();
    EXPECT_EQ(editor->Interface().GetTheme().design, kui::ThemeDesign::eKoral);
    EXPECT_TRUE(editor->Interface().GetTheme().IsDark());
    Session().ChooseTheme(kui::ThemeDesign::eFluent, false);
    Frame();
    EXPECT_EQ(editor->Interface().GetTheme().design, kui::ThemeDesign::eFluent);
    EXPECT_FALSE(editor->Interface().GetTheme().IsDark());
    EXPECT_FALSE(Session().themeChosen) << "put on, and done with";
}

// For looking at: the inspector of one node of each kind, and a node being dragged.
TEST_F(EditorTest, DISABLED_Sheets) {
    const auto project = scene::MakeProject("sheets");
    Open();
    scene::BuildGraph(Doc());
    scene::BuildEntities(Doc());
    std::string error;
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "keditor-tests" / "sheets-project");
    ASSERT_TRUE(Session().NewProject(std::filesystem::temp_directory_path() / "keditor-tests" / "sheets-project")) << Session().Status();
    for (const char* shader : { "stamp.slang", "show.slang" })
        std::filesystem::copy_file(std::filesystem::path(KEDITOR_TEST_SHADERS) / shader, Session().CurrentProject().ShaderDirectory() / shader,
                                   std::filesystem::copy_options::overwrite_existing);
    for (const char* node : { "show", "stamp", "canvas", "tint backdrop", "draw", "run", "target", "square" }) {
        Session().Select(std::string(node));
        for (int i = 0; i < 4; ++i) Frame();
        Sheet(std::string("node-") + node);
    }
    Session().Show("draw", "cullIndex");
    Session().Show("draw", "command");
    Session().Show("target", "color[0].clear");
    Session().Select(std::string("draw"));
    for (int i = 0; i < 4; ++i) Frame();
    Sheet("node-draw-more");
    Session().Select(std::string("target"));
    for (int i = 0; i < 4; ++i) Frame();
    Sheet("node-target-more");

    {
        const std::string image = Doc().AddNode("Image", 660, 40, "fresh image");
        const std::string half = Doc().AddNode("Divide", 860, 60, "half");
        Doc().Connect(image, "size", half, "a");
        Session().Show(image, "out:mipLevels");
        Session().Select(image);
        for (int i = 0; i < 4; ++i) Frame();
        Sheet("node-fresh-image");
    }
    auto& input = editor->SceneInput();
    input.FeedMousePosition({ 330.f, 77.f });
    Frame();
    input.FeedMouseButton(kor::MouseButton::eLeft, true);
    Frame();
    for (int i = 1; i <= 10; ++i) { input.FeedMousePosition({ 330.f + i * 12.f, 77.f + i * 14.f }); Frame(); }
    Sheet("drag-mid");
    input.FeedMouseButton(kor::MouseButton::eLeft, false);
    for (int i = 0; i < 3; ++i) Frame();
    Sheet("drag-after");
}

// A pipeline can name a shader before the file is written: it says what is missing, and builds once it is there.
TEST_F(EditorTest, AShaderNamedBeforeItsFileIsWrittenIsPickedUpWhenItIs) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "late-shader";
    std::filesystem::remove_all(directory);
    static int run = 0;
    const std::string module = std::format("late{}", run++);   // the framework keeps a shader by name, for the life of the program
    Open();
    const auto pipeline = Doc().AddNode("ComputePipeline", 0, 0, "p");
    Doc().SetInput(pipeline, "compute.module", module);
    const auto* status = Doc().Runtime().Status(pipeline);
    ASSERT_FALSE(status->ok);
    EXPECT_NE(status->error.find("there is no " + module + ".slang"), std::string::npos) << status->error;
    EXPECT_NE(status->error.find("shaders folder"), std::string::npos) << "and where it goes: " << status->error;

    ASSERT_TRUE(Session().NewProject(directory)) << Session().Status();
    EXPECT_FALSE(Doc().Runtime().Status(pipeline)->ok) << "a project, and still no file";
    { std::ofstream(directory / "shaders" / (module + ".slang")) << "[shader(\"compute\")] [numthreads(1, 1, 1)] void computeMain() {}\n"; }
    // Nothing is touched in the graph: the file turning up is enough.
    bool built = false;
    for (int i = 0; i < 200 && !built; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Frame();
        built = Doc().Runtime().Status(pipeline)->ok;
    }
    EXPECT_TRUE(built) << Doc().Runtime().Status(pipeline)->error;
    EXPECT_TRUE(NothingIsWrong());
    EXPECT_EQ(Session().EntryPoints(module), std::vector<std::string> { "computeMain" });
}

// An image's usage is ticked a flag at a time, in a scene that is running, with nothing waiting between frames. What
// it is ticked only adds to what it is made for: what uses it — a framebuffer drawing into it, a shader sampling it, an
// entity's shader writing it — has it made for that, ticked or not. None of it reaches the GPU wrong. Ticking
// eTransferSrc alone on a new image once took the program down.
TEST_F(EditorTest, AnImagesUsageIsChangedUnderARunningSceneWithoutHarm) {
    scene::MakeProject("usage");
    Open();
    using kgraph::Value;
    scene::BuildGraph(Doc());
    scene::BuildEntities(Doc());
    const auto frames = [&](int n) { for (int i = 0; i < n; ++i) support::App()->Frame(); };
    const auto logged = kor::log::LastSequence();
    frames(4);
    const auto error = [&](const char* node) { const auto* status = Doc().Runtime().Status(node); return status->ok ? std::string() : status->error; };

    // A new image, with nothing set but one flag ticked, then unticked: made either way, to be copied from.
    const auto fresh = Doc().AddNode("Image", 0, 0, "fresh");
    Doc().SetInput(fresh, "usage", Value::List { "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Status(fresh)->ok) << error("fresh");
    Doc().SetInput(fresh, "usage", Value::List {});
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Status(fresh)->ok) << error("fresh");
    Doc().SetInput(fresh, "extent.x", 16);
    Doc().SetInput(fresh, "extent.y", 16);
    Doc().SetInput(fresh, "usage", Value::List { "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Status(fresh)->ok) << "a size across and down is a size: " << error("fresh");

    // The image a framebuffer draws into, without the usage that takes ticked: it is made for it all the same.
    Doc().SetInput("canvas", "usage", Value::List { "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Ok()) << error("target");
    EXPECT_TRUE(Doc().Runtime().Status("draw")->ok);

    // What a shader samples, and what it writes, likewise: on the pipeline, or on the entity that supplies it.
    Doc().SetInput("backdrop", "usage", Value::List { "eTransferDst" });
    Doc().SetInput("image 0", "usage", Value::List { "eSampled", "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Ok()) << error("stamp");
    EXPECT_EQ(Doc().Runtime().ReportOf("run")->skipped, 0u) << "the entity's image is made to be written: " << Doc().Runtime().ReportOf("run")->why;

    // None of it was the GPU's to complain of.
    for (const auto& record : kor::log::HistorySince(logged))
        EXPECT_EQ(record.message.find("VUID"), std::string::npos) << record.message;
}

namespace {
    /** A scene opened after the one being edited, so updated after it: what it does, it does once that scene has recorded its frame. */
    class Late final : public kor::Scene {
    public:
        void Update() override { if (auto run = std::exchange(next, {})) run(); }
        std::function<void()> next;
    };
}

// An edit made from the interface is made in the middle of a frame: the editor's window is updated after the scene
// it shows has recorded what it draws with, and before the frame is finished. What the edit replaces has to outlast
// that frame. Ticking a usage flag in the inspector once freed the image the frame was still drawing with.
TEST_F(EditorTest, AnEditMadeMidFrameDoesNotFreeWhatTheFrameIsDrawingWith) {
    scene::MakeProject("midframe");
    Open();
    using kgraph::Value;
    scene::BuildGraph(Doc());
    scene::BuildEntities(Doc());
    auto* late = support::App()->OpenOffscreen<Late>({ .title = "late", .extent = { 8, 8 } });
    const auto frames = [&](int n) { for (int i = 0; i < n; ++i) support::App()->Frame(); };
    const auto midFrame = [&](std::function<void()> edit) { late->next = std::move(edit); frames(3); };
    frames(4);

    for (int round = 0; round < 8; ++round) {
        // The image the scene draws into and shows, built again: with another usage, another size.
        midFrame([&] { Doc().SetInput("canvas", "usage", round % 2 ? Value::List { "eColorAttachment", "eTransferSrc" } : Value::List { "eColorAttachment", "eTransferSrc", "eSampled" }); });
        midFrame([&] { Doc().SetInput("canvas", "extent.x", 32 + round); });
        // An image entities hold and two passes use; a pipeline; a pass.
        midFrame([&] { Doc().SetInput("image 1", "extent.x", 16 + round); });
        midFrame([&] { Doc().SetInput("backdrop", "extent.x", 4 + round); });
        midFrame([&] { Doc().SetInput("show", "rasterizationState", Value(Value::Members { { "lineWidth", 1.0 + round } })); });
        midFrame([&] { Doc().SetInput("run", "groupSize.x", 8 + round % 2); });
        // An entity's resource swapped, and an entity gone.
        midFrame([&] { Doc().SetField(Doc().Entities().front(), "show", "source", round % 2 ? "image 0" : "image 1"); });
    }
    EXPECT_TRUE(Doc().Runtime().Ok());
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    midFrame([&] { Doc().DestroyEntity(Doc().Entities().back()); });
    midFrame([&] { Doc().RemoveNodes({ "draw", "show" }); });
    // And the whole scene replaced: opened anew, as the File menu does it.
    midFrame([&] { Session().Play().New(); });
    frames(3);
    support::App()->Close(*late);
    frames(1);
}

// A node added is one that works, with what it needs already set to something sensible; what else it has shows as
// what it is, not as nothing; and what it gives besides what it makes can be put on it, to wire from.
TEST_F(EditorTest, ANewNodeStartsAsSomethingThatWorksAndGivesWhatItIsMadeOf) {
    Open();
    const std::string image = Doc().AddNode("Image", 40, 40);
    Session().Select(image);
    const auto* status = Doc().Runtime().Status(image);
    EXPECT_TRUE(status->ok) << "an image from the first, not a list of things to fill in: " << status->error;
    const auto& inputs = Doc().Graph().Find(image)->inputs;
    EXPECT_EQ(inputs.at("format").value, kgraph::Value("eRGBA8_UNORM"));
    EXPECT_EQ(inputs.at("extent.x").value, kgraph::Value(512));
    EXPECT_FALSE(inputs.contains("usage")) << "it is made for what uses it";
    EXPECT_FALSE(inputs.contains("mipLevels")) << "what it can do without is not set for it";
    EXPECT_TRUE(Shows("512"));
    EXPECT_TRUE(Shows("eRGBA8_UNORM"));

    // Other kinds likewise: each is right as it comes.
    for (const char* type : { "Sampler", "Buffer", "Primitive", "Double", "Vec3", "Color" }) {
        const std::string id = Doc().AddNode(type, 0, 0);
        EXPECT_TRUE(Doc().Runtime().Status(id)->ok) << type << ": " << Doc().Runtime().Status(id)->error;
    }
    const std::string clear = Doc().AddNode("Clear", 300, 40);
    EXPECT_EQ(Doc().Graph().Find(clear)->inputs.at("color").value, kgraph::Value(kgraph::Value::List { 0, 0, 0, 1 }));
    Doc().Connect(image, "out", clear, "image");
    EXPECT_TRUE(Doc().Runtime().Status(clear)->ok) << Doc().Runtime().Status(clear)->error;

    // What it gives besides the image: in the inspector, every one, with what it is now; put on the node when asked.
    Session().Select(image);
    EXPECT_FALSE(Shows("Add output")) << "no menu of what it gives: all of it is there";
    EXPECT_TRUE(Shows("width"));
    EXPECT_TRUE(Shows("What it gives"));
    EXPECT_EQ(std::ranges::count(kui::debug::Texts(editor->Interface()), std::string("extent")), 1) << "its extent once: an input, given as an output from its own row";
    Session().Show(image, "out:width");
    EXPECT_TRUE(Session().Asked(image, "out:width"));
    // Wired from, it stays on the node without being asked for: the wire is in the file.
    const std::string half = Doc().AddNode("Divide", 300, 200);
    Doc().Connect(image, "size", half, "a");
    Doc().SetInput(half, "b", 2);
    EXPECT_TRUE(Shows("size"));
    EXPECT_EQ(Doc().Runtime().ValueOf(half), kgraph::Value(kgraph::Value::List { 256, 256 }));
}

// What was put on a node comes off it again, and an input in parts is wired whole: the groups of a dispatch are an
// image's extent over the pipeline's numThreads, one wire each.
TEST_F(EditorTest, OutputsComeOffAgainAndAnInputInPartsIsWiredWhole) {
    Open();
    const std::string image = Doc().AddNode("Image", 40, 40);
    const std::string run = Doc().AddNode("Dispatch", 600, 40);
    const std::string over = Doc().AddNode("Divide", 300, 200);

    // Every output is in the inspector: what it works out under What it gives, saying what it is wired to; one that is an
    // input as it is set, from that input's row. Wired from, it takes its wires with it when it comes off.
    Session().Select(image);
    EXPECT_TRUE(Shows("width"));
    EXPECT_FALSE(Shows("wired to"));
    Doc().Connect(image, "width", over, "b");
    EXPECT_TRUE(Shows("wired to 1 input"));
    Doc().Connect(image, "extent", over, "a");
    Doc().Connect(image, "extent", run, "groupSize");
    EXPECT_EQ(Doc().DisconnectOutput(image, "extent"), 2u);
    EXPECT_FALSE(Doc().Graph().Find(over)->inputs.contains("a"));
    EXPECT_FALSE(Doc().Graph().Find(run)->inputs.contains("groupSize"));
    EXPECT_EQ(Doc().DisconnectOutput(image, "extent"), 0u);
    EXPECT_EQ(Doc().DisconnectOutput(image, "width"), 1u);
    EXPECT_FALSE(Shows("wired to"));

    // The whole of an input in parts is wired instead of them; given, its parts are gone from the node and the panel.
    Session().Select(run);
    Doc().SetInput(run, "groups.x", 4);
    EXPECT_TRUE(Shows("groups"));
    Doc().Connect(image, "extent", over, "a");
    Doc().SetInput(over, "b", 8);
    Doc().Connect(over, "out", run, "groups");
    EXPECT_FALSE(Doc().Graph().Find(run)->inputs.contains("groups.x")) << "the whole or its parts, not both";
    EXPECT_TRUE(Shows("from " + over));
    Sheet("whole");
    Session().Select(image);
    Sheet("outputs");
    // And a part given again takes the whole away.
    Doc().SetInput(run, "groups.y", 2);
    EXPECT_FALSE(Doc().Graph().Find(run)->inputs.contains("groups"));
}

// A triangle from nothing but nodes: a picture, a framebuffer of it, a pipeline whose vertex shader makes its own
// vertices, and a draw. No mesh, no vertex layout, no entity.
// A buffer a shader lays out as one matrix — a camera's block — is a table of its sixteen numbers, readable.
TEST_F(EditorTest, AMatrixBufferIsATableThatReads) {
    const auto project = scene::MakeProject("camera buffer");
    { std::ofstream(project.ShaderDirectory() / "simple.slang") << R"slang(module simple;
struct Camera { float4x4 viewProjection; };
ConstantBuffer<Camera> camera;
[shader("vertex")]
float4 vertex(float3 position : POSITION) : SV_Position { return mul(float4(position, 1.0), camera.viewProjection); }
)slang"; }
    Open();
    const std::string data = Doc().AddNode("ShaderBuffer", 40, 40, "camera data");
    Doc().SetInput(data, "module", "simple");
    Doc().SetInput(data, "entry", "vertex");
    Doc().SetInput(data, "parameter", "camera");
    Doc().SetInput(data, "field.viewProjection", kgraph::Value::List { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5, 0.25, 0, 1 });
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(Session().View(data));
    for (int i = 0; i < 60 && Session().ViewSays() == "Not copied out yet."; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); Frame(); }
    for (int i = 0; i < 4; ++i) Frame();
    Sheet("camera-buffer");
}

TEST_F(EditorTest, ATriangleIsDrawnWithNodesAndNoMesh) {
    const auto project = scene::MakeProject("triangle");
    std::filesystem::copy_file(std::filesystem::path(KGRAPH_TEST_SHADERS) / "triangle.slang", project.ShaderDirectory() / "triangle.slang");
    Open();
    const std::string image = Doc().AddNode("Image", 40, 40);
    Doc().SetInput(image, "usage", kgraph::Value::List { "eColorAttachment", "eTransferSrc", "eSampled" });
    const std::string view = Doc().AddNode("ImageView", 240, 40);
    Doc().Connect(image, "out", view, "image");
    const std::string target = Doc().AddNode("Framebuffer", 440, 40);
    Doc().Connect(view, "out", target, "color[0].view");
    const std::string pipeline = Doc().AddNode("GraphicsPipeline", 440, 240);
    EXPECT_FALSE(Doc().Graph().Find(pipeline)->inputs.contains("vertex.layout")) << "a layout is for a shader that reads vertices: not something every pipeline is given";
    Doc().SetInput(pipeline, "vertex.module", "triangle");
    Doc().SetInput(pipeline, "vertex.entry", "vertexMain");
    Doc().SetInput(pipeline, "fragment.module", "triangle");
    Doc().SetInput(pipeline, "fragment.entry", "fragmentMain");
    Doc().Connect(target, "out", pipeline, "framebuffer");
    const std::string draw = Doc().AddNode("Draw", 640, 240);
    Doc().Connect(pipeline, "out", draw, "pipeline");
    const std::string screen = Doc().AddNode("Screen", 840, 40);
    const std::string show = Doc().AddNode("Blit", 840, 240);
    Doc().Connect(image, "out", show, "source");
    Doc().Connect(screen, "out", show, "destination");
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(NothingIsWrong());
    EXPECT_EQ(Doc().Runtime().ReportOf(draw), nullptr) << "it draws once: nothing it is given follows a ForEach";

    // The draw has the rest there as it starts, each in its part: how many vertices, where, over what. A part of nothing
    // given is closed; what does nothing as it is set — an indirect draw's buffer — is not there, and it says so.
    Session().Select(draw);
    EXPECT_FALSE(Shows("more it can be given"));
    EXPECT_TRUE(Shows("What is drawn"));
    EXPECT_TRUE(Shows("6, as they start"));
    EXPECT_FALSE(Shows("vertexCount")) << "closed";
    Session().OpenSection(draw, "What is drawn", true);
    EXPECT_TRUE(Shows("vertexCount"));
    EXPECT_FALSE(Doc().Graph().Find(draw)->inputs.contains("vertexCount"));
    EXPECT_FALSE(Shows("indirectOffset"));
    EXPECT_TRUE(Shows("4 more for indirect draws, 2 more for draws counted by a buffer")) << "what it leaves out, and why";
    Doc().SetInput(draw, "command", "eDrawIndirect");
    Session().OpenSection(draw, "Indirect", true);
    EXPECT_TRUE(Shows("indirectOffset")) << "an indirect draw has its buffer's inputs";
    EXPECT_FALSE(Shows("vertexCount")) << "and none of a draw of vertices'";
    EXPECT_TRUE(Shows("for draws of vertices"));
    Doc().SetInput(draw, "command", "eAuto");
    Doc().SetInput(draw, "scissor", kgraph::Value::List { 0, 0, 256, 512 });
    EXPECT_TRUE(Shows("scissor"));
    Sheet("triangle");
    Doc().SetInput(draw, "cullMode", "eBack");
    EXPECT_TRUE(Doc().Runtime().Status(draw)->ok) << Doc().Runtime().Status(draw)->error;
    EXPECT_TRUE(Shows("eBack"));
}

// A shader's buffer is mapped with two nodes: one that has the fields of the shader's struct to be wired to, and one
// that has the fields of a component to wire from. Both show them as ports as soon as they know what they are of.
TEST_F(EditorTest, AShadersBufferIsFilledFromAComponentsFields) {
    const auto project = scene::MakeProject("lights");
    std::filesystem::copy_file(std::filesystem::path(KGRAPH_TEST_SHADERS) / "lights.slang", project.ShaderDirectory() / "lights.slang");
    Open();
    const kecs::Entity lamp = Doc().CreateEntity("Lamp");
    Doc().AddComponent(lamp, "PointLight");
    const std::string lamps = Doc().AddNode("ForEach", 40, 40);
    Doc().SetInput(lamps, "component", "PointLight");
    const std::string buffer = Doc().AddNode("ShaderBuffer", 340, 40);
    Session().Select(buffer);
    EXPECT_TRUE(Shows("the shader module that declares the buffer")) << "what it still needs is said";
    Doc().SetInput(buffer, "module", "lights");
    Doc().SetInput(buffer, "entry", "computeMain");
    EXPECT_TRUE(Shows("'computeMain' of lights uses: ")) << "the buffers there are to choose from: " << Doc().Runtime().Status(buffer)->error;
    Doc().SetInput(buffer, "parameter", "lights");
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    for (const char* field : { "position", "range", "color", "kind", "cone" }) EXPECT_TRUE(Shows(field)) << field;
    Session().Select(std::string());
    EXPECT_TRUE(Shows("cone")) << "the buffer's field, a port on its node, with nothing selected";
    EXPECT_TRUE(Shows("PointLight")) << "the ForEach's component, one port: a Bundle of its fields";
    EXPECT_FALSE(Shows("intensity")) << "a field of it is on the node only once wired";

    EXPECT_TRUE(Doc().Connect(lamps, "field.color", buffer, "field.color")) << "a field is wired by its name all the same";
    EXPECT_TRUE(Doc().Connect(lamps, "worldPosition", buffer, "field.position"));
    EXPECT_TRUE(NothingIsWrong());
    EXPECT_EQ(Doc().Runtime().ValueOf(buffer, "count"), kgraph::Value(1));
    Doc().AddComponent(Doc().CreateEntity("Another"), "PointLight");
    Frame();
    Frame();
    EXPECT_EQ(Doc().Runtime().ValueOf(buffer, "count"), kgraph::Value(2)) << "an element for each entity that has the component";
    Sheet("buffers");
}

// A ForEach's inspector is what it is of, as one thing: the components each one has — its own and `with`'s alike — those
// none has, and its conditions, each a list above a dropdown with a search; and each condition a section with its inputs.
TEST_F(EditorTest, ALoopsInspectorSaysWhatItIsOf) {
    scene::MakeProject("loops");
    Open();
    Doc().AddComponent(Doc().CreateEntity("Lamp"), "PointLight");
    const std::string lamps = Doc().AddNode("ForEach", 40, 40);
    Doc().SetInput(lamps, "with", kgraph::Value(kgraph::Value::List { "PointLight" }));
    Session().Select(lamps);
    Frame();
    for (const char* said : { "Has", "Has not", "Conditions", "Add a component each one has", "Ban a component", "Add a condition each one meets", "1 entity now" })
        EXPECT_TRUE(Shows(said)) << said;
    EXPECT_TRUE(Shows("PointLight")) << "what it has, listed";
    EXPECT_TRUE(NothingIsWrong()) << "a ForEach of `with` alone is one";

    // A condition: its own section, with its inputs; and what it reads, which each one has to have.
    Doc().SetInput(lamps, "conditions", kgraph::Value(kgraph::Value::List { "HasAttributes" }));
    Frame();
    EXPECT_TRUE(Shows("HasAttributes"));
    EXPECT_TRUE(Shows("required")) << "its input, in its section";
    EXPECT_TRUE(Shows("for HasAttributes")) << "the Mesh it reads, listed as needed for it";
    EXPECT_TRUE(Shows("0 entities now")) << "a lamp has no mesh";
    Sheet("loop");
}

// The outliner is the hierarchy: an entity under another shows under it, folds away with it, and moves with it.
// Where each is, is in the inspector, and is no component.
TEST_F(EditorTest, EntitiesAreATreeEachWithATransform) {
    Open();
    const kecs::Entity ship = Doc().CreateEntity("Ship");
    const kecs::Entity turret = Doc().CreateEntity("Turret", ship);
    const kecs::Entity barrel = Doc().CreateEntity("Barrel", turret);
    const kecs::Entity rock = Doc().CreateEntity("Rock");
    EXPECT_EQ(Doc().Entities(), (std::vector<kecs::Entity> { ship, turret, barrel, rock })) << "parents before their children";
    for (const char* name : { "Ship", "Turret", "Barrel", "Rock" }) EXPECT_TRUE(Shows(name)) << name;

    // Folded, what is under it is out of the way.
    Session().Fold(ship, true);
    EXPECT_FALSE(Shows("Turret"));
    EXPECT_TRUE(Shows("Rock"));
    Session().Fold(ship, false);
    EXPECT_TRUE(Shows("Barrel"));

    // Its transform, in the inspector; a child is placed relative to its parent.
    Doc().SetPosition(ship, { 10, 0, 0 });
    Doc().SetPosition(turret, { 0, 2, 0 });
    Doc().SetRotation(turret, { 0, 45, 0 });
    Session().Select(turret);
    for (const char* row : { "Position", "Rotation", "Scale", "Transform", "Under" }) EXPECT_TRUE(Shows(row)) << row;
    EXPECT_FALSE(Shows("(nothing: it is at the top)")) << "what it is under is said: the ship";
    EXPECT_TRUE(Shows("45.00"));
    EXPECT_TRUE(Doc().ComponentsOf(turret).empty()) << "none of it is a component";
    const kecs::Mat4 world = Doc().World().WorldMatrix(turret);
    EXPECT_NEAR(world.At(3, 0), 10.f, 1e-4f);
    EXPECT_NEAR(world.At(3, 1), 2.f, 1e-4f);

    // Moved to the top, it stays where it is in the world; and nothing goes under what is under it.
    EXPECT_FALSE(Doc().SetParent(ship, barrel));
    EXPECT_TRUE(Doc().SetParent(turret, kecs::NullEntity));
    EXPECT_NEAR(Doc().World().PositionOf(turret).x, 10.f, 1e-4f);
    EXPECT_TRUE(Shows("(nothing: it is at the top)"));
    EXPECT_EQ(Doc().World().ParentOf(barrel), turret) << "what was under it went with it";
    EXPECT_TRUE(Doc().SetParent(turret, ship, 0));

    // A Mesh is one of the default meshes, without a node for any.
    Doc().AddComponent(barrel, "Mesh");
    std::string error;
    EXPECT_TRUE(Doc().SetField(barrel, "Mesh", "mesh", kgraph::Value("$cylinder"), &error)) << error;
    Session().Select(barrel);
    EXPECT_TRUE(Shows("cylinder"));
    Sheet("tree");

    // What it is made from is shown under it, and tuned, it is still that shape, and can go back to how it starts.
    EXPECT_TRUE(Shows("segments"));
    EXPECT_FALSE(Shows("Back to how it starts"));
    EXPECT_TRUE(Doc().SetField(barrel, "Mesh", "mesh", kgraph::Value("$cylinder?radius=0.2&segments=12"), &error)) << error;
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_TRUE(Shows("cylinder"));
    EXPECT_TRUE(Shows("Back to how it starts"));
    Sheet("mesh-parameters");

    // Deleted, what is under it goes too.
    Doc().DestroyEntity(ship);
    EXPECT_EQ(Doc().Entities(), std::vector<kecs::Entity> { rock });
    EXPECT_FALSE(Shows("Barrel"));
}

// The scene a project starts from: meshes in a colour each, lit by the sun, by nodes alone and the one standard shader.
TEST_F(EditorTest, TheStandardSceneDrawsMeshesInAColourEach) {
    const auto project = scene::MakeProject("standard");
    EXPECT_EQ(keditor::WriteStandardShaders(project.ShaderDirectory()), std::vector<std::string> { "default.slang" });
    EXPECT_TRUE(keditor::WriteStandardShaders(project.ShaderDirectory()).empty()) << "one that is there is left as it is";
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddStandardGraph(Doc(), error)) << error;
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(Doc().Runtime().Ok());
    EXPECT_EQ(Doc().Passes(), (std::vector<std::string> { "Draw", "Blit" }));

    // The pipeline's component is what makes an entity drawn: what its shader's `color` asks of each entity.
    const kecs::ComponentId renderer = Doc().Runtime().ComponentOf("default");
    ASSERT_NE(renderer, kecs::NoComponent);
    std::vector<std::string> fields;
    for (const auto& field : Doc().World().Describe(renderer).fields) fields.push_back(field.name);
    EXPECT_EQ(fields, std::vector<std::string> { "color" });

    const kecs::Entity sphere = keditor::AddStandardScene(Doc());
    for (const char* name : { "Camera", "Sun", "Sphere", "Cylinder" }) EXPECT_TRUE(Shows(name)) << name;
    EXPECT_TRUE(NothingIsWrong());
    ASSERT_NE(Doc().Runtime().ReportOf("draw"), nullptr);
    EXPECT_EQ(Doc().Runtime().ReportOf("draw")->ran, 2u) << "the sphere and the cylinder: " << Doc().Runtime().ReportOf("draw")->why;
    Session().Select(sphere);
    Sheet("standard");
}

// The scene to start from instead: meshes lit under a sky, by nodes alone and the lit graph's three shaders.
TEST_F(EditorTest, TheLitSceneDrawsLitMeshesUnderASky) {
    const auto project = scene::MakeProject("lit");
    EXPECT_EQ(keditor::WriteLitShaders(project.ShaderDirectory()).size(), 3u);
    EXPECT_TRUE(keditor::WriteLitShaders(project.ShaderDirectory()).empty()) << "one that is there is left as it is";
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    ASSERT_TRUE(Doc().Runtime().Ok());

    // What its push constants ask of each entity is its PbrMaterial's, wired from a Filter of what has one: the pipeline
    // has no parameters of its own. Where it is is the engine's to say.
    EXPECT_EQ(Doc().Runtime().ComponentOf("lit"), kecs::NoComponent);
    const kgraph::Node* drawn = Doc().Graph().Find("drawn");
    ASSERT_NE(drawn, nullptr);
    EXPECT_EQ(drawn->inputs.at("with").value, kgraph::Value(kgraph::Value::List { "Mesh", "PbrMaterial" }));
    EXPECT_EQ(Doc().World().Find("MeshRenderer"), kecs::NoComponent) << "there is no renderer but what a shader makes";
    EXPECT_EQ(Doc().World().Find("Spin"), kecs::NoComponent) << "what a thing does is a script's";
    EXPECT_EQ(Doc().Runtime().ComponentOf("sky"), kecs::NoComponent);

    const kecs::Entity sphere = keditor::AddLitScene(Doc());
    for (const char* name : { "Sky", "Camera", "Sun", "Sphere" }) EXPECT_TRUE(Shows(name)) << name;
    EXPECT_TRUE(NothingIsWrong());
    ASSERT_NE(Doc().Runtime().ReportOf("draw scene"), nullptr);
    EXPECT_EQ(Doc().Runtime().ReportOf("draw scene")->ran, 1u) << Doc().Runtime().ReportOf("draw scene")->why;
    Session().Select(sphere);
    Sheet("lit");
}

// The Scene panel shows the scene other ways than lit: a channel of its materials — what the shaders that import
// koral_debug show — the editor's own ways from its graph in a file (edges, clay, normals...), a DebugView of the scene's
// graph; and a grid under it. None of it reaches the Game panel.
TEST_F(EditorTest, TheSceneViewShowsTheSceneOtherWaysAndAGrid) {
    const auto project = scene::MakeProject("views");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    const kecs::Entity sphere = keditor::AddLitScene(Doc());
    Session().Frame(sphere);
    for (int i = 0; i < 8; ++i) Frame();
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    keditor::Overlay* over = Session().Play().Over();
    ASSERT_NE(over, nullptr);
    for (const auto& problem : over->ViewProblems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(Doc().Runtime().EditorView()) << "the scene's runtime is the scene view's";

    const auto named = [&](const std::string& name) -> std::optional<keditor::Session::ViewMode> {
        for (const auto& choice : Session().ViewModes())
            if (choice.name == name) return choice.mode;
        return std::nullopt;
    };
    for (const char* name : { "Lit", "Unlit (albedo)", "Normals", "Roughness", "Wireframe", "Clay", "Normals (geometry)", "UV checker", "Overdraw", "Entities" })
        EXPECT_TRUE(named(name)) << name;
    EXPECT_EQ(Session().CurrentViewMode().kind, keditor::Session::ViewMode::Kind::eLit);
    EXPECT_FALSE(Session().ViewModePicture().Get()) << "lit, the scene's own picture is all there is";
    Sheet("view-lit");

    // A channel of the materials: the lit shader shows its albedo; the sky's does not import koral_debug.
    Session().SetViewMode(*named("Unlit (albedo)"));
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_EQ(Doc().Runtime().ShaderDebugView(), static_cast<int>(kgraph::ShaderDebugChannel::eAlbedo));
    const auto without = Session().WithoutMaterialViews();
    EXPECT_EQ(std::ranges::find(without, "lit"), without.end());
    EXPECT_NE(std::ranges::find(without, "sky"), without.end());
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_FALSE(Session().ViewModePicture().Get()) << "drawn by the scene's own passes";
    Sheet("view-albedo");

    // The editor's ways: a picture of their own in the scene's place.
    for (const char* name : { "Clay", "Wireframe", "Normals (geometry)", "UV checker", "Overdraw", "Entities" }) {
        Session().SetViewMode(*named(name));
        for (int i = 0; i < 6; ++i) Frame();
        EXPECT_EQ(Doc().Runtime().ShaderDebugView(), 0) << name;
        EXPECT_TRUE(Session().ViewModePicture().Get()) << name;
        for (const auto& problem : over->ViewProblems()) ADD_FAILURE() << name << ": " << problem;
        Sheet(std::string("view-") + name);
    }

    // A DebugView of the scene's graph is listed, and shown in its place.
    ASSERT_EQ(Doc().AddNode("DebugView", 1200, 600, "picture view", "scene"), "picture view");
    ASSERT_TRUE(Doc().SetInput("picture view", "name", "The picture"));
    ASSERT_TRUE(Doc().Connect("color", "out", "picture view", "picture"));
    for (int i = 0; i < 3; ++i) Frame();
    const auto graphView = named("The picture");
    ASSERT_TRUE(graphView);
    EXPECT_EQ(graphView->kind, keditor::Session::ViewMode::Kind::eGraph);
    Session().SetViewMode(*graphView);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(Doc().Runtime().ShownDebugView(), "picture view");
    EXPECT_EQ(Session().ViewModePicture().Get(), Doc().Runtime().ImageOf("color").Get());
    // Taken away, the scene is lit again.
    Doc().RemoveNodes({ "picture view" });
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(Session().CurrentViewMode().kind, keditor::Session::ViewMode::Kind::eLit);

    // The grid: over the scene while it is shown, set as the user likes.
    keditor::Grid grid = Session().SceneGrid();
    grid.shown = true;
    grid.cell = 0.5f;
    Session().SetGrid(grid);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_TRUE(Session().GridPicture().Get());
    Sheet("view-grid");
    grid.shown = false;
    Session().SetGrid(grid);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(Session().GridPicture().Get());

    // Played, the Game panel shows the scene lit, whatever the Scene panel shows.
    Session().SetViewMode(*named("Normals"));
    Session().StartPlaying();
    for (int i = 0; i < 6; ++i) Frame();
    ASSERT_NE(Session().Game(), nullptr);
    ASSERT_NE(Session().Game()->Runtime(), nullptr);
    EXPECT_EQ(Session().Game()->Runtime()->ShaderDebugView(), 0);
    EXPECT_FALSE(Session().Game()->Runtime()->EditorView());
    EXPECT_EQ(Doc().Runtime().ShaderDebugView(), static_cast<int>(kgraph::ShaderDebugChannel::eNormal));
    Session().Stop();
    Frame();
}

// The flow is of what comes before: a node set down on a later page than one that uses it — an Image made on the last
// page and cleared on an earlier one — is no step back to that page, and a Switch is a step after what it chooses from,
// wherever its node is. However the graph is wired, the flow has no way round.
TEST_F(EditorTest, ThePassFlowHasNoWayRound) {
    const auto project = scene::MakeProject("flow dag");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    keditor::AddLitScene(Doc());
    ASSERT_TRUE(Doc().AddPass("other"));
    ASSERT_EQ(Doc().AddNode("Image", 40, 40, "late picture", "other"), "late picture");
    Doc().SetInput("late picture", "format", "eRGBA8_UNORM");
    Doc().SetInput("late picture", "usage", kgraph::Value(kgraph::Value::List { "eTransferDst", "eSampled" }));
    ASSERT_EQ(Doc().AddNode("Clear", 300, 600, "early clear", "scene"), "early clear");
    ASSERT_TRUE(Doc().Connect("late picture", "out", "early clear", "image"));
    for (int i = 0; i < 6; ++i) Frame();
    const auto flow = Session().Flow();
    EXPECT_FALSE(std::ranges::any_of(flow.links, [](const auto& link) { return link.from == "other" && link.to == "scene"; }))
        << "nothing of the last page's is worked on before the scene page";
    // Every link from a step before to one after, and so an order of them all: each step once all it takes from are.
    std::map<std::string, int> waiting;
    for (const auto& step : flow.steps) waiting[step.id] = 0;
    for (const auto& link : flow.links) ++waiting[link.to];
    std::vector<std::string> ready;
    for (const auto& [id, count] : waiting) if (count == 0) ready.push_back(id);
    std::size_t ordered = 0;
    while (!ready.empty()) {
        const std::string id = ready.back();
        ready.pop_back();
        ++ordered;
        for (const auto& link : flow.links)
            if (link.from == id && --waiting[link.to] == 0) ready.push_back(link.to);
    }
    EXPECT_EQ(ordered, waiting.size()) << "a step is in a cycle";
}

// The pages' order is what they use, and links where two write the same thing: the list's order, until the graph is
// settled, made links of where it matters; after, a link taken away is a tie, and a link drawn the other way turns them.
TEST_F(EditorTest, ThePagesAreOrderedByWhatTheyUseAndByLinks) {
    const auto project = scene::MakeProject("order");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    keditor::AddLitScene(Doc());
    for (int i = 0; i < 6; ++i) Frame();
    using Link = std::pair<std::string, std::string>;
    const auto at = [&](const std::string& page) { return std::ranges::find(Doc().Passes(), page) - Doc().Passes().begin(); };
    ASSERT_FALSE(Doc().Graph().orderSettled) << "made in code: in its list's order";
    EXPECT_TRUE(std::ranges::contains(Doc().OrderLinks(), Link { "sky", "scene" })) << "both draw into the picture: the list's order is a link";
    EXPECT_FALSE(std::ranges::contains(Doc().OrderLinks(), Link { "show", "sky" })) << "show only reads it: what it uses orders it";
    EXPECT_TRUE(Doc().OrderTies().empty());

    Doc().SettleOrder();
    EXPECT_TRUE(Doc().Graph().orderSettled);
    EXPECT_TRUE(std::ranges::contains(Doc().Graph().after, Link { "sky", "scene" }));
    EXPECT_NE(Doc().Graph().ToJson().find("\"after\": [[\"sky\", \"scene\"]"), std::string::npos) << "saved as links";

    // Taken away, the two are a tie, said among the problems; drawn the other way, sky runs first.
    ASSERT_TRUE(Doc().StopRunningAfter("sky", "scene"));
    ASSERT_EQ(Doc().OrderTies().size(), 1u);
    EXPECT_TRUE(std::ranges::contains(Doc().OrderTies().front().shared, std::string("color")));
    EXPECT_TRUE(HasProblem("both write color"));
    ASSERT_TRUE(Doc().RunAfter("scene", "sky"));
    EXPECT_LT(at("sky"), at("scene"));
    EXPECT_TRUE(Doc().OrderTies().empty());
    EXPECT_FALSE(Doc().RunAfter("scene", "scene"));
    // A link the other way turns them back: the one it takes the place of goes.
    ASSERT_TRUE(Doc().RunAfter("sky", "scene"));
    EXPECT_LT(at("scene"), at("sky"));
    EXPECT_FALSE(std::ranges::contains(Doc().Graph().after, Link { "scene", "sky" }));

    // Read again, it is as it was saved.
    kgraph::Graph read;
    ASSERT_TRUE(kgraph::Graph::FromJson(Doc().Graph().ToJson(), read, error)) << error;
    EXPECT_TRUE(read.orderSettled);
    EXPECT_EQ(read.after, Doc().Graph().after);
    EXPECT_EQ(read.passes, Doc().Passes());
}

// The pages as a flow: each wired from the pages whose work it uses — the last before it to write what it reads — and a
// Switch that chooses between two pages' pictures a step between them and what uses its choice.
TEST_F(EditorTest, ThePagesAreAFlowAndASwitchChoosesABranch) {
    const auto project = scene::MakeProject("flow");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    keditor::AddLitScene(Doc());
    for (int i = 0; i < 6; ++i) Frame();
    const auto linked = [](const keditor::Session::PassFlow& flow, const std::string& from, const std::string& to) {
        return std::ranges::any_of(flow.links, [&](const auto& link) { return link.from == from && link.to == to; });
    };
    auto flow = Session().Flow();
    std::vector<std::string> pages;
    for (const auto& step : flow.steps) if (!step.isSwitch) pages.push_back(step.id);
    EXPECT_EQ(pages, Doc().Passes());
    EXPECT_TRUE(linked(flow, "frame", "scene"));
    EXPECT_TRUE(linked(flow, "scene", "sky"));
    EXPECT_TRUE(linked(flow, "sky", "show")) << "the sky drew into the picture last, before it is shown";
    EXPECT_FALSE(linked(flow, "scene", "show")) << "what show uses of the picture is what sky left";
    for (const auto& step : flow.steps) EXPECT_TRUE(step.ran) << step.id;

    // A second picture, made on a page of its own before the show page, and a Switch on the show page between it and
    // the first. (A page after show would be read as the frame before left it: no step of this frame's flow.)
    const auto showAt = std::ranges::find(Doc().Passes(), std::string("show")) - Doc().Passes().begin();
    ASSERT_TRUE(Doc().AddPass("other", static_cast<int>(showAt)));
    ASSERT_EQ(Doc().AddNode("Image", 40, 40, "other picture", "other"), "other picture");
    Doc().SetInput("other picture", "format", "eRGBA8_UNORM");
    Doc().SetInput("other picture", "usage", kgraph::Value(kgraph::Value::List { "eTransferDst", "eTransferSrc", "eSampled" }));
    ASSERT_EQ(Doc().AddNode("Clear", 300, 40, "paint other", "other"), "paint other");
    ASSERT_TRUE(Doc().Connect("other picture", "out", "paint other", "image"));
    ASSERT_EQ(Doc().AddNode("Int", 40, 400, "branch", "show"), "branch");
    ASSERT_EQ(Doc().AddNode("Switch", 200, 400, "which picture", "show"), "which picture");
    ASSERT_TRUE(Doc().Connect("branch", "out", "which picture", "index"));
    ASSERT_TRUE(Doc().Connect("color", "out", "which picture", "option[0]"));
    ASSERT_TRUE(Doc().Connect("other picture", "out", "which picture", "option[1]"));
    ASSERT_TRUE(Doc().Connect("which picture", "out", "show", "source"));
    for (int i = 0; i < 4; ++i) Frame();
    flow = Session().Flow();
    const auto choosing = std::ranges::find(flow.steps, std::string("which picture"), &keditor::Session::PassFlow::Step::id);
    ASSERT_NE(choosing, flow.steps.end());
    EXPECT_TRUE(choosing->isSwitch);
    EXPECT_EQ(choosing->options, (std::vector<std::string> { "sky", "other" }));
    EXPECT_EQ(choosing->chosen, 0);
    EXPECT_TRUE(linked(flow, "which picture", "show"));
    EXPECT_TRUE(linked(flow, "other", "which picture"));
    Session().Choose("branch", 1);
    for (int i = 0; i < 4; ++i) Frame();
    flow = Session().Flow();
    EXPECT_EQ(std::ranges::find(flow.steps, std::string("which picture"), &keditor::Session::PassFlow::Step::id)->chosen, 1);

    // The flow is the Graph panel's, gone up to the whole frame: a lane for each rate, every lane there, empty or not.
    Session().ShowFrame();
    EXPECT_TRUE(Session().ShowsFrame());
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(Shows("No setup page")) << "an empty lane, to drag a page into";
    EXPECT_TRUE(Shows("No fixed-step page"));
    Sheet("flow");
    // A page moved to another rate is in that lane.
    ASSERT_TRUE(Doc().SetPassRate("other", kgraph::PassRate::eSetup));
    for (int i = 0; i < 4; ++i) Frame();
    flow = Session().Flow();
    EXPECT_EQ(std::ranges::find(flow.steps, std::string("other"), &keditor::Session::PassFlow::Step::id)->rate, "setup");
    // Gone into, a page is shown; a node gone to is shown on its page, from the frame too.
    Session().ShowPass("sky");
    EXPECT_FALSE(Session().ShowsFrame());
    Session().ShowFrame();
    Session().GoTo("paint other");
    EXPECT_FALSE(Session().ShowsFrame());
    EXPECT_EQ(Session().Pass(), "other");
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(Shows("No fixed-step page"));
}

// What an editor does with the standard scene: looks at it from a view of its own, outlines and picks, plays and
// stops, and makes things from the project's objects.
TEST_F(EditorTest, TheSceneIsLookedAtPickedPlayedAndMadeFromObjects) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "features";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(std::filesystem::exists(directory / "shaders" / "default.slang")) << "the standard shader is the project's own, to edit";
    EXPECT_FALSE(std::filesystem::exists(directory / "shaders" / "lit.slang")) << "and the lit graph's are not, until they are asked for";
    EXPECT_TRUE(std::filesystem::exists(directory / "objects" / "Cube.json"));
    kecs::World& world = Doc().World();
    const kecs::Entity sphere = world.FindEntity("Sphere");
    ASSERT_FALSE(sphere.IsNull());
    EXPECT_EQ(Session().selectedEntity, sphere);

    // Seen from the editor's own view until it is told to look through the scene's camera: once the scene's graph has
    // run a frame — which, its build quick enough (an optimised one), is not yet a few frames in.
    for (int i = 0; i < 4 || (i < 240 && !(Doc().Runtime().CurrentView().view == Session().camera.View().view)); ++i) Frame();
    EXPECT_FALSE(Session().ThroughSceneCamera());
    EXPECT_EQ(Doc().Runtime().CurrentView().view, Session().camera.View().view);
    Session().SetLookThroughCamera(true);
    for (int i = 0; i < 3; ++i) Frame();
    {   // a View towards where the camera faces: its own matrix, to a float's rounding
        const kor::Mat4 seen = Doc().Runtime().CurrentView().view, own = kgraph::ViewOf(world, world.FindEntity("Camera")).view;
        for (int i = 0; i < 16; ++i) EXPECT_NEAR(seen[i / 4][i % 4], own[i / 4][i % 4], 1e-5f) << i;
    }
    Session().SetLookThroughCamera(false);
    Session().Frame(sphere);
    EXPECT_NEAR(Session().camera.distance, 0.87f * 1.2f / std::sin(30.f * 3.14159265f / 180.f), 1e-3f) << "far enough to see all of it";
    Sheet("looked");

    // A click on the picture picks what is drawn there: the sphere in the middle, nothing in a corner. Nothing of
    // the scene's graph does it: there is no node for it there, and the editor's own passes answer.
    EXPECT_FALSE(Doc().Runtime().Pick(0.5f, 0.5f, [](kecs::Entity) {})) << "the standard graph has no Pick node";
    EXPECT_EQ(Doc().Graph().Find("outline"), nullptr);
    keditor::Overlay* over = Session().Play().Over();
    ASSERT_NE(over, nullptr);
    EXPECT_EQ(over->Problem(), "");
    EXPECT_TRUE(over->Picture().Get() != nullptr) << "the line round what is picked, and the gizmo, over the scene's picture";
    kecs::Entity picked = kecs::NullEntity;
    bool answered = false;
    ASSERT_TRUE(over->Pick(0.5f, 0.5f, [&](const kecs::Entity entity) { picked = entity; answered = true; }));
    for (int i = 0; i < 8; ++i) Frame();
    EXPECT_TRUE(answered);
    EXPECT_EQ(picked, sphere);
    answered = false;
    over->Pick(0.02f, 0.02f, [&](const kecs::Entity entity) { picked = entity; answered = true; });
    for (int i = 0; i < 8; ++i) Frame();
    EXPECT_TRUE(answered);
    EXPECT_TRUE(picked.IsNull()) << "the sky is nobody";

    // What it looks like played is the Game panel's: a tab behind the Scene panel, drawing nothing while it is.
    keditor::GameScene* game = Session().Game();
    ASSERT_NE(game, nullptr);
    EXPECT_FALSE(game->Shown()) << "behind the scene's panel, it is not drawn";

    // A script that makes entities as it starts: not started by the frames an edited scene is drawn in, started once
    // it plays, and what it made gone when it stops.
    {
        kscript::ScriptType spawner;
        spawner.name = "Spawner";
        spawner.size = 1;
        spawner.alignment = 1;
        spawner.callbacks.user = &world;
        spawner.callbacks.started = [](void* user, const kecs::Entity entity, void*) {
            auto& w = *static_cast<kecs::World*>(user);
            for (int i = 0; i < 3; ++i) w.SetParent(w.Create(), entity);
        };
        Doc().Scripts().Register(std::move(spawner));
        const kecs::Entity camera = world.FindEntity("Camera");
        ASSERT_TRUE(Doc().AddComponent(camera, "Spawner"));
        for (int i = 0; i < 4; ++i) Frame();
        EXPECT_TRUE(world.ChildrenOf(camera).empty()) << "an edited scene's scripts are not started";
        Session().StartPlaying();
        for (int i = 0; i < 4; ++i) Frame();
        EXPECT_EQ(world.ChildrenOf(world.FindEntity("Camera")).size(), 3u) << "started once, as it plays";
        Session().Stop();
        for (int i = 0; i < 4; ++i) Frame();
        EXPECT_TRUE(world.ChildrenOf(world.FindEntity("Camera")).empty()) << "stopped, what it made is gone and it is not started again";
        Doc().RemoveComponent(world.FindEntity("Camera"), "Spawner");
        Frame();
    }

    // Nothing plays until it is told to. Played, the Game panel comes forward and the scene is seen there through its
    // own camera — the Scene panel stays the editor's view — and it is not saved; stopped, it is as it was when it
    // began, and what was picked still is.
    EXPECT_FALSE(Session().IsPlaying());
    Session().StartPlaying();
    EXPECT_TRUE(Session().InPlayMode());
    EXPECT_TRUE(Session().IsPlaying());
    EXPECT_FALSE(Session().Save()) << "a moment of it is not what is saved";
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(game->Shown()) << "played, the Game panel is brought forward";
    EXPECT_FALSE(Session().ThroughSceneCamera()) << "the Scene panel is still the editor's own view";
    EXPECT_EQ(Doc().Runtime().CurrentView().view, Session().camera.View().view);
    ASSERT_NE(game->Runtime(), nullptr);
    for (const auto& problem : game->Runtime()->Problems()) ADD_FAILURE() << "game: " << problem;
    {   // the Game panel's is the scene's camera
        const kor::Mat4 seen = game->Runtime()->CurrentView().view, own = kgraph::ViewOf(world, world.FindEntity("Camera")).view;
        for (int i = 0; i < 16; ++i) EXPECT_NEAR(seen[i / 4][i % 4], own[i / 4][i % 4], 1e-5f) << i;
    }
    EXPECT_EQ(game->Runtime()->ResourceOf("$sphere"), Doc().Runtime().ResourceOf("$sphere")) << "the meshes are the scene's, not made again";
    {   // Picked as it plays, the inspector follows what is done to it, a tenth of a second behind at most, built again
        // on its own: the rest of the interface (the scene tree, with thousands of bodies) is not.
        const kecs::Entity sphere = world.FindEntity("Sphere");
        Session().Select(sphere);
        Settle();
        const auto version = Session().Version();
        world.SetPosition(sphere, { 7.25f, 0.5f, 0.f });            // what a script, or physics, would do
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        EXPECT_TRUE(Shows("7.25")) << "the inspector shows where it is now";
        EXPECT_EQ(Session().Version(), version) << "and nothing else was built again for it";
    }
    world.SetRotation(world.FindEntity("Sphere"), { 0, 40, 0 });   // what a script would do as it plays
    Doc().SetField(world.FindEntity("Sphere"), "default", "color", kgraph::Value(kgraph::Value::List { 1.f, 1.f, 1.f }));
    Session().Pause();
    EXPECT_FALSE(Session().IsPlaying());
    EXPECT_TRUE(Session().InPlayMode()) << "paused, it is still a moment of the play";
    Session().Stop();
    EXPECT_FALSE(Session().InPlayMode());
    const kecs::Entity again = world.FindEntity("Sphere");
    EXPECT_EQ(world.RotationOf(again), (kecs::Vec3 {}));
    EXPECT_EQ(*Doc().Field(again, "default", "color").Items(), (kgraph::Value::List { 0.81f, 0.19f, 0.19f }));
    EXPECT_EQ(Session().selectedEntity, again) << "found again where it was";

    // The project's objects are what there is to make.
    std::vector<std::string> names;
    for (const auto& object : Session().Objects()) names.push_back(object.name);
    for (const char* name : { "Empty", "Cube", "Sphere", "Planet", "Camera", "Directional Light", "Point Light", "Spot Light", "Sky" })
        EXPECT_NE(std::ranges::find(names, name), names.end()) << name;
    const auto object = [&](const char* name) { return *std::ranges::find(Session().Objects(), name, &keditor::ObjectTemplate::name); };
    const kecs::Entity planet = Session().Create(object("Planet"));
    EXPECT_EQ(world.NameOf(planet), "Planet");
    ASSERT_EQ(world.ChildrenOf(planet).size(), 1u);
    EXPECT_EQ(world.NameOf(world.ChildrenOf(planet)[0]), "Moon");
    EXPECT_EQ(Doc().Field(planet, "Mesh", "mesh"), kgraph::Value("$sphere"));
    EXPECT_EQ(*Doc().Field(planet, "default", "color").Items(), (kgraph::Value::List { 0.3f, 0.55f, 1 })) << "what the object says of the shader's component";
    EXPECT_EQ(Doc().ComponentsOf(planet).size(), 2u);
    const kecs::Entity second = Session().Create(object("Planet"));
    EXPECT_EQ(world.NameOf(second), "Planet 2") << "a name of its own";
    const kecs::Entity lamp = Session().Create(object("Spot Light"), planet);
    EXPECT_EQ(world.ParentOf(lamp), planet);
    EXPECT_EQ(Doc().Field(lamp, "SpotLight", "outerAngle"), kgraph::Value(30));

    // A copy is beside what it is a copy of, with everything under it.
    const kecs::Entity copy = Session().Duplicate(planet);
    EXPECT_EQ(world.NameOf(copy), "Planet 3");
    EXPECT_EQ(world.ChildrenOf(copy).size(), 2u);
    const auto& roots = world.Roots();
    EXPECT_EQ(std::ranges::find(roots, copy) - std::ranges::find(roots, planet), 1);
    // And an entity saved as an object is one of them from then on.
    world.Rename(copy, "Lit Planet");
    EXPECT_TRUE(Session().SaveAsObject(copy));
    EXPECT_TRUE(std::filesystem::exists(directory / "objects" / "Lit Planet.json"));
    const kecs::Entity made = Session().Create(object("Lit Planet"));
    EXPECT_EQ(world.ChildrenOf(made).size(), 2u);
    EXPECT_EQ(world.PositionOf(made), (kecs::Vec3 {})) << "made where it is made, not where the first one was";
    Session().Delete(planet);
    EXPECT_FALSE(world.Alive(lamp));

    Session().Select(made);
    EXPECT_TRUE(Shows("color"));
    Sheet("features");

    // Saved, opened again: all of it is there.
    ASSERT_TRUE(Session().Save());
    ASSERT_TRUE(Session().OpenProject(directory));
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    const kecs::Entity reopened = Doc().World().FindEntity("Lit Planet 2");
    ASSERT_FALSE(reopened.IsNull());
    EXPECT_EQ(*Doc().Field(reopened, "default", "color").Items(), (kgraph::Value::List { 0.3f, 0.55f, 1 }));
    EXPECT_EQ(Doc().Field(reopened, "Mesh", "mesh"), kgraph::Value("$sphere"));
}

// The graph is in passes, a page each: only one page's nodes are in the way at a time, and what a pass uses of the
// passes before it is brought in.
TEST_F(EditorTest, TheGraphIsAPageAPass) {
    const auto project = scene::MakeProject("passes");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(Doc().Passes(), (std::vector<std::string> { "frame", "scene", "sky", "show" }));

    // A ForEach's — an Entity's — component is one output, a Bundle: taken apart, an Unbundle beside it gives each field.
    {
        const auto given = Doc().Runtime().OutputsOf("sky values");
        const auto sky = std::ranges::find(given, std::string("Sky"), &kgraph::Port::name);
        ASSERT_NE(sky, given.end()) << "the Sky, as one output";
        EXPECT_EQ(sky->type, "bundle");
        std::string why;
        const std::string parts = Doc().TakeApart("sky values", "Sky", &why);
        ASSERT_FALSE(parts.empty()) << why;
        EXPECT_EQ(Doc().Graph().Find(parts)->type, "Unbundle");
        EXPECT_EQ(Doc().Graph().Find(parts)->pass, Doc().Graph().Find("sky values")->pass) << "beside it, on its page";
        const auto members = Doc().Runtime().OutputsOf(parts);
        for (const char* field : { "zenith", "horizon", "ground", "ambient", "sunSize" })
            EXPECT_TRUE(std::ranges::contains(members, std::string(field), &kgraph::Port::name)) << field;
        EXPECT_TRUE(Doc().TakeApart("sky values", "worldPosition", &why).empty()) << "one thing already";
        EXPECT_NE(why.find("not a Bundle"), std::string::npos) << why;
        Doc().RemoveNodes({ parts });
    }

    // The first pass's page, to begin with: its nodes, and none of another's.
    for (const char* pass : { "frame", "scene", "show", "Every node at once" }) EXPECT_TRUE(Shows(pass)) << pass;
    EXPECT_TRUE(Shows("camera"));
    EXPECT_TRUE(Shows("camera data"));
    EXPECT_FALSE(Shows("draw sky"));
    EXPECT_FALSE(Shows("scene target"));
    Session().ShowPass("sky");
    EXPECT_TRUE(Shows("draw sky"));
    EXPECT_TRUE(Shows("sky target"));
    // The scene pass's framebuffer is among what this page can use, in its panel — and not on the page itself.
    EXPECT_EQ(std::ranges::count(kui::debug::Texts(editor->Interface()), std::string("scene target")), 1);
    EXPECT_TRUE(Shows("Image, from scene")) << "what it took from an earlier pass says where from";
    EXPECT_TRUE(Shows("BufferBuilder, from frame"));
    Sheet("pass");
    Session().ShowEveryPass();
    EXPECT_TRUE(Shows("scene target"));
    EXPECT_TRUE(Shows("draw sky"));
    Sheet("every pass");
    // Every node at once is a layout of its own: put in order there, no node moves on its pass's page.
    const double skyX = Doc().Graph().Find("draw sky")->x;
    Session().AskToArrange();
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_FALSE(Doc().Graph().overview.empty());
    EXPECT_EQ(Doc().Graph().Find("draw sky")->x, skyX);
    Sheet("every pass arranged");

    // Every page put in order, one after another, each as it is drawn: a node thrown far off comes back among the rest
    // of its page's, and the page shown before is shown again.
    Session().ShowPass("sky");
    for (int i = 0; i < 3; ++i) Frame();
    Doc().PlaceNodes({ { "draw sky", { 50000.0, 50000.0 } }, { "camera", { -40000.0, 40000.0 } } });
    Session().ArrangeEveryPage();
    EXPECT_EQ(Session().PagesToArrange().size(), Doc().Passes().size());
    for (int i = 0; i < 200 && !Session().PagesToArrange().empty(); ++i) Frame();
    EXPECT_TRUE(Session().PagesToArrange().empty());
    EXPECT_LT(std::abs(Doc().Graph().Find("draw sky")->y - Doc().Graph().Find("sky target")->y), 3000.0) << "put among its page's";
    EXPECT_LT(std::abs(Doc().Graph().Find("camera")->y - Doc().Graph().Find("eye")->y), 3000.0) << "on another page too, next to what it is wired to";
    EXPECT_EQ(Session().Pass(), "sky") << "and the page it was on is shown";
    EXPECT_TRUE(Shows("Every page's nodes are in the order they are wired in"));

    // Going to a node turns to its page.
    Session().ShowPass("frame");
    Session().GoTo("draw sky");
    EXPECT_EQ(Session().Pass(), "sky");
    EXPECT_TRUE(Shows("sky target"));

    // A new pass: something made in it, and something brought in from before it.
    ASSERT_TRUE(Doc().AddPass("bloom", 2));
    EXPECT_FALSE(Doc().AddPass("bloom"));
    EXPECT_EQ(Doc().Passes()[2], "bloom");
    const auto importable = Doc().Importable("bloom");
    EXPECT_NE(std::ranges::find(importable, "color"), importable.end()) << "what the scene pass made";
    EXPECT_NE(std::ranges::find(importable, "camera data"), importable.end());
    EXPECT_EQ(std::ranges::find(importable, "sky target"), importable.end()) << "not what a later pass makes";
    EXPECT_EQ(std::ranges::find(importable, "draw scene"), importable.end()) << "a pass node gives nothing to take";
    const std::string taken = Doc().Import("color", "bloom", 40, 40);
    ASSERT_FALSE(taken.empty());
    EXPECT_EQ(Doc().Graph().Find(taken)->type, "Reference");
    EXPECT_EQ(Doc().Graph().Find(taken)->pass, "bloom");
    EXPECT_EQ(Doc().Import("color", "bloom", 90, 90), taken) << "a thing is in a pass once";
    const std::string blurred = Doc().AddNode("Image", 300, 40, "blurred", "bloom");
    EXPECT_EQ(Doc().Graph().Find(blurred)->pass, "bloom");
    const std::string copy = Doc().AddNode("Blit", 560, 40, "blur", "bloom");
    EXPECT_TRUE(Doc().Connect(taken, "out", copy, "source"));
    EXPECT_TRUE(Doc().Connect(blurred, "out", copy, "destination"));
    EXPECT_TRUE(Doc().Runtime().Status(copy)->ok) << Doc().Runtime().Status(copy)->error;
    Session().ShowPass("bloom");
    EXPECT_TRUE(Shows("Image, from scene"));
    EXPECT_TRUE(Shows("blurred"));
    EXPECT_FALSE(Shows("looking at")) << "a node of the frame page: not on this one";

    // A wire straight from another page's node, with nothing brought in: the node is shown beside what it feeds.
    EXPECT_TRUE(Doc().Connect("screen", "extent", blurred, "extent"));
    EXPECT_TRUE(Shows("Screen, in frame"));

    // Renamed, moved and taken away. Its nodes go with it, and what was wired to them is wired to nothing.
    EXPECT_TRUE(Doc().RenamePass("bloom", "glow"));
    EXPECT_EQ(Doc().Graph().Find(blurred)->pass, "glow");
    EXPECT_TRUE(Doc().MovePass("glow", 3));
    EXPECT_EQ(Doc().Passes().back(), "show") << "moved to where the last one was, which is now after it";
    EXPECT_EQ(Doc().Passes()[3], "glow");
    EXPECT_TRUE(Doc().SetNodePass(blurred, "show"));
    EXPECT_TRUE(Doc().RemovePass("glow"));
    EXPECT_EQ(Doc().Graph().Find(copy), nullptr);
    EXPECT_NE(Doc().Graph().Find(blurred), nullptr) << "moved to another pass before, it stays";
    EXPECT_EQ(Doc().Passes().size(), 4u);
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;

    // Saved and read back, the passes are as they were.
    const std::string saved = Doc().ToJson();
    EXPECT_NE(saved.find("\"passes\": [\"frame\", \"scene\""), std::string::npos);
    EXPECT_NE(saved.find("\"pass\": \"sky\""), std::string::npos);
}

// What is made once is a tab of its own: its passes come first, and stay first. What a page can use is in a drawer
// beside it, by the page it was made on; brought in, it says what it is, what the page does with it and what was done
// to it before, and goes again when the last wire from it does.
TEST_F(EditorTest, SetupIsALaneOfItsOwnAndWhatAPageCanUseIsBesideIt) {
    const auto project = scene::MakeProject("setup");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;

    // A setup pass is first, wherever it is asked to be, and the passes of every frame after it.
    ASSERT_TRUE(Doc().AddPass("noise", -1, kgraph::PassRate::eSetup));
    EXPECT_EQ(Doc().Passes().front(), "noise");
    EXPECT_TRUE(Doc().IsSetup("noise"));
    ASSERT_TRUE(Doc().AddPass("early", 0));
    EXPECT_EQ(Doc().Passes()[1], "early") << "a pass of every frame is after setup's, though asked to be first";
    EXPECT_TRUE(Doc().RemovePass("early"));
    EXPECT_TRUE(Doc().MovePass("noise", 3));
    EXPECT_EQ(Doc().Passes().front(), "noise") << "moved, it stays among setup's";
    EXPECT_TRUE(Doc().MovePass("show", 0));
    EXPECT_EQ(Doc().Passes()[1], "show") << "and one of every frame stays after them";
    EXPECT_TRUE(Doc().MovePass("show", 4));

    const std::string texture = Doc().AddNode("Image", 40, 40, "noise texture", "noise");
    const std::string clear = Doc().AddNode("Clear", 320, 40, "noise clear", "noise");
    ASSERT_TRUE(Doc().Connect(texture, "out", clear, "image"));
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    const auto importable = Doc().Importable("noise");
    EXPECT_EQ(std::ranges::find(importable, "color"), importable.end()) << "setup comes before any frame: what a frame makes is not there yet";
    EXPECT_TRUE(std::ranges::contains(Doc().Importable("show"), texture)) << "and what setup made is there for every frame";

    // A lane each, in the Outline as in the frame.
    Session().ShowPass("noise");
    EXPECT_TRUE(Shows("SETUP  ·  MADE ONCE  ·  1"));
    EXPECT_TRUE(Shows("EVERY FRAME  ·  ")) << "every lane listed at once";
    EXPECT_TRUE(Shows("Frame")) << "where the page is: the whole frame, gone back up to";
    Session().ShowPass("show");

    // In a panel of its own, what the page can use, by where it was made.
    EXPECT_TRUE(Shows("WHAT SHOW CAN USE"));
    EXPECT_TRUE(Shows("NOISE  ·  SETUP")) << "a heading a page, as the passes' list has";
    EXPECT_TRUE(Shows("noise texture"));
    // Each with its picture, as the Assets panel has its things: an image as it was last drawn, a mesh by itself.
    for (int i = 0; i < 600 && !(Session().NodePicture(texture).Get() && Session().NodePicture("shape").Get()); ++i) {
        Frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));     // (the pictures are made a few a second)
    }
    EXPECT_TRUE(Session().NodePicture(texture).Get()) << "an image's picture";
    EXPECT_TRUE(Session().NodePicture("shape").Get()) << "a mesh's picture";
    EXPECT_FALSE(Session().NodePicture("camera data").Get()) << "a buffer has none: what kind of thing it is, instead";
    Sheet("drawer");
    Session().SetDrawerList(true);
    EXPECT_TRUE(Shows("noise texture")) << "or a line each";
    Sheet("drawer list");
    Session().SetDrawerList(false);

    // Brought in, it says what it is, that it is made once, what this page does with it, and what was done to it before.
    const std::string brought = Doc().Import(texture, "show", 40, 400);
    ASSERT_FALSE(brought.empty());
    EXPECT_TRUE(Shows("Image, made once in noise  ·  512×512  ·  RGBA8_UNORM"));
    EXPECT_TRUE(Shows("not used here yet"));
    EXPECT_TRUE(Shows("After noise clear."));
    const std::string shown = Doc().AddNode("Blit", 320, 400, "show noise", "show");
    ASSERT_TRUE(Doc().Connect(brought, "out", shown, "source"));
    ASSERT_TRUE(Doc().Connect("screen", "out", shown, "destination"));
    EXPECT_TRUE(Doc().Runtime().Status(shown)->ok) << Doc().Runtime().Status(shown)->error;
    EXPECT_TRUE(Shows("read here"));
    EXPECT_EQ(Doc().Import(texture, "show", 90, 90), brought) << "once a page";

    // Its inspector is the thing itself, said to be brought in.
    Session().Select(brought);
    Sheet("reference");
    EXPECT_TRUE(Shows("Brought in: noise texture, made once, in setup's noise"));
    EXPECT_TRUE(Shows("Go to where it is made"));
    EXPECT_TRUE(Shows("format")) << "what is set where it is made, set here";

    // The last wire from it gone, it goes.
    EXPECT_TRUE(Doc().ClearInput(shown, "source"));
    EXPECT_EQ(Doc().Graph().Find(brought), nullptr);
    const std::string again = Doc().Import(texture, "show", 40, 400);
    Doc().RemoveNodes({ shown });
    EXPECT_NE(Doc().Graph().Find(again), nullptr) << "one never wired stays";

    // Saved apart from the rest; and back to every frame, the first of them.
    EXPECT_NE(Doc().ToJson().find("\"setup\": [\"noise\"]"), std::string::npos);
    EXPECT_TRUE(Doc().SetPassRate("noise", kgraph::PassRate::eFrame));
    EXPECT_FALSE(Doc().IsSetup("noise"));
    EXPECT_EQ(Doc().Passes().front(), "noise");
    EXPECT_TRUE(Doc().SetPassRate("frame", kgraph::PassRate::eSetup));
    EXPECT_EQ(Doc().Passes().front(), "frame") << "made setup, it is the last of setup's: the only one";
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
}

// A script that makes resources: written from the editor, read from its source for what it makes and its settings,
// offered as a node of its own, and run by it — which says, until something runs the script, what it waits for.
TEST_F(EditorTest, AScriptOfTheProjectsMakesResources) {
    const auto project = scene::MakeProject("filled");
    Open(project.directory);
    ASSERT_TRUE(Session().NewScript("Checker", "C# ResourceScript")) << Session().Status();
    EXPECT_TRUE(std::filesystem::exists(project.directory / "scripts" / "Checker.cs"));
    const auto scripts = Doc().ResourceScripts();
    const auto checker = std::ranges::find(scripts, std::string("Checker"), &kgraph::ResourceScript::name);
    ASSERT_NE(checker, scripts.end()) << "its source is read: a script a Script node can name";
    ASSERT_EQ(checker->resources.size(), 2u) << "what it makes";
    EXPECT_EQ(checker->resources[0].name, "Board");
    EXPECT_EQ(checker->resources[0].kind, "Image");
    EXPECT_EQ(checker->resources[1].kind, "Sampler");
    EXPECT_FALSE(checker->fill) << "nothing runs it in an editor that runs no scripts";
    EXPECT_EQ(Doc().World().Find("Checker"), kecs::NoComponent) << "and it is no component";
    EXPECT_FALSE(Session().NewScript("Checker", "C# ResourceScript")) << "a name once";

    // Offered among the nodes: a Script node of it, which gives what it makes.
    const auto presets = Doc().Runtime().Presets();
    const auto offered = std::ranges::find(presets, std::string("Checker"), &kgraph::Preset::name);
    ASSERT_NE(offered, presets.end());
    EXPECT_EQ(offered->type, "Script");
    const std::string script = Doc().AddNode("Script", 40, 40, "checker");
    Doc().SetInput(script, "script", "Checker");
    ASSERT_TRUE(Doc().Runtime().Status(script)->ok) << Doc().Runtime().Status(script)->error;
    EXPECT_TRUE(Shows("Checker fills what it makes when the project runs with its scripts"));
    EXPECT_TRUE(std::ranges::contains(Doc().Runtime().PortsOf(script), std::string("field.Cells"), &kgraph::Port::name)) << "its field, an input";
    EXPECT_TRUE(std::ranges::contains(Doc().Runtime().PortsOf(script), std::string("Board.format"), &kgraph::Port::name)) << "and what its image is made with";
    EXPECT_TRUE(std::ranges::contains(Doc().Runtime().OutputsOf(script), std::string("Board"), &kgraph::Port::name)) << "its image, an output";
    ASSERT_TRUE(Doc().Runtime().ImageOf(script, "Board").Get());
    EXPECT_EQ(Doc().Runtime().ImageOf(script, "Board")->Extent().x, 256u) << "as its source makes it";
    Session().Select(script);
    EXPECT_TRUE(Shows("Checker makes Board (Image), Nearest (Sampler). Nothing runs it now"));

    // Run, as a host runs it — saying nothing of what it makes: given what the source says its field starts as.
    std::int32_t cells = 0;
    std::vector<std::string> given;
    kscript::ScriptType type;
    type.name = "Checker";
    type.makesResources = true;
    type.size = 4;
    type.fields = checker->fields;
    Doc().RegisterResourceScript(type, [&](const std::span<kgraph::Contents> contents, const std::span<const std::byte> settings) -> std::string {
        std::memcpy(&cells, settings.data(), 4);
        given.clear();
        for (auto& each : contents) { given.push_back(each.name); std::ranges::fill(each.bytes, std::byte { 0xFF }); }
        return {};
    });
    EXPECT_FALSE(Shows("Checker fills what it makes when the project runs with its scripts")) << "it says so no more";
    EXPECT_EQ(cells, 8) << "as its source starts it";
    EXPECT_EQ(given, (std::vector<std::string> { "Board" })) << "what its source says it makes";
    EXPECT_TRUE(Doc().Runtime().Status(script)->ok) << Doc().Runtime().Status(script)->error;
}

// An image looked at closely is looked at a level and a layer at a time, drawn blended or a texel a square; a view of
// it has its swizzle chosen a channel at a time.
TEST_F(EditorTest, AnImageIsLookedAtALevelAtATimeAndAViewIsSwizzled) {
    Open();
    const std::string heights = Doc().AddNode("Image", 40, 40, "heights");
    Doc().SetInput(heights, "format", "eR32_SFLOAT");
    Doc().SetInput(heights, "mipLevels", 4);
    Doc().SetInput(heights, "extent.x", 64);
    Doc().SetInput(heights, "extent.y", 64);
    ASSERT_TRUE(Doc().Runtime().Status(heights)->ok) << Doc().Runtime().Status(heights)->error;
    Session().Select(heights);
    EXPECT_FALSE(Shows("Look at what it holds")) << "what it holds is the viewer's to show: no button for it here";
    EXPECT_FALSE(Shows("eTransferSrc")) << "nor its usage: it is made for what uses it";
    Session().Look(heights);
    Session().lookMip = 2;
    for (int i = 0; i < 12; ++i) Frame();
    const auto peek = Doc().Runtime().Watched(heights);
    ASSERT_TRUE(peek && peek->image) << Doc().Runtime().WatchProblem(heights);
    EXPECT_EQ(peek->mip, 2u);
    EXPECT_EQ(peek->extent.x, 16u) << "its third level";

    const std::string view = Doc().AddNode("ImageView", 300, 40, "view");
    ASSERT_TRUE(Doc().Connect(heights, "out", view, "image"));
    Doc().SetInput(view, "componentMapping", kgraph::Value::List { "eR", "eR", "eR", "eOne" });     // a height, grey
    Session().Select(view);
    EXPECT_TRUE(Shows("One"));
    Sheet("swizzle");
    const auto texts = kui::debug::Texts(editor->Interface());
    EXPECT_EQ(std::ranges::count(texts, std::string("One")), 1) << "a choice for each channel, said as what it gives: not JSON";
    EXPECT_EQ(std::ranges::count(texts, std::string("R")), 4) << "three of red, and the label of the first";
}

// A number is dragged as what it is: a lens's near plane by so much of itself, shown finely enough to read; a typed
// vector's components as its kind.
TEST_F(EditorTest, NumbersAreDraggedAsWhatTheyAre) {
    Open();
    const std::string lens = Doc().AddNode("Perspective", 40, 40, "lens");
    Doc().SetInput(lens, "near", 0.05);
    const std::string place = Doc().AddNode("Vec3", 40, 300, "place");
    const std::string cells = Doc().AddNode("UVec2", 300, 300, "cells");
    Doc().SetInput(cells, "x", 16);
    ASSERT_TRUE(Doc().Runtime().Status(place)->ok) << Doc().Runtime().Status(place)->error;
    EXPECT_EQ(Doc().Runtime().ValueOf(cells), kgraph::Value(kgraph::Value::List { 16, 0 }));
    Session().Select(lens);
    EXPECT_TRUE(Shows("0.050")) << "a near plane with three decimals: dragged by so much of it, it is small";
    Sheet("numbers");
    EXPECT_TRUE(Shows("UVec2"));
}

// A new shader of every kind, with every choice this device can run, is one a pipeline names and is built from: vertices
// and fragments, with tessellation and a geometry shader, or none; mesh shaders, with a task shader or none; compute; and
// ray tracing's three, with an any hit shader and an intersection shader. Without a fragment shader, only depth is drawn.
TEST_F(EditorTest, EveryKindOfShaderIsMadeAndBuilt) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "shader-kinds";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewLitScene());      // what a mesh's shader reads, the camera and the lights, is the lit graph's
    Session().ShowPass("sky");
    int made = 0;
    const auto build = [&](const std::string& name, const std::string& kind, const keditor::Session::ShaderChoice& choice, const char* type,
                           std::vector<std::pair<std::string, std::string>> settings) {
        for (const auto& [option, asked] : { std::pair { "", true }, { "tessellation", choice.tessellation }, { "geometry", choice.geometry } })
            if (asked && !keditor::Session::ShaderMissing(kind, option).empty()) return;     // what this GPU has not
        ASSERT_TRUE(Session().NewShader(name, kind, choice)) << name << ": " << Session().Status();
        const std::string pipeline = Doc().AddNode(type, 0, 0, name, "sky");
        for (const auto& [input, value] : settings) {
            if (input.starts_with("@")) Doc().Connect(value, "out", pipeline, input.substr(1));
            else if (input == "layout") Doc().Connect("shape", "layout", pipeline, "vertex.layout");
            else Doc().SetInput(pipeline, input, value == "~" ? name : value);
        }
        const auto* status = Doc().Runtime().Status(pipeline);
        ASSERT_NE(status, nullptr);
        EXPECT_TRUE(status->ok) << name << ": " << status->error;
        ++made;
    };
    for (const bool whole : { false, true })
        for (const bool tess : { false, true })
            for (const bool geometry : { false, true })
                for (const bool fragment : { false, true }) {
                    std::vector<std::pair<std::string, std::string>> settings { { "vertex.module", "~" }, { "@framebuffer", "sky target" } };
                    if (!whole) { settings.push_back({ "layout", "" }); settings.push_back({ "@param.camera", "camera data" }); }
                    if (tess) { settings.push_back({ "tessellationControl.module", "~" }); settings.push_back({ "tessellationEvaluation.module", "~" }); }
                    if (geometry) settings.push_back({ "geometry.module", "~" });
                    if (fragment) settings.push_back({ "fragment.module", "~" });
                    if (whole && fragment) settings.push_back({ "@param.source", "color" });
                    build(std::format("vertex{}{}{}{}", whole ? "Whole" : "", tess ? "Tess" : "", geometry ? "Geometry" : "", fragment ? "" : "Depth"), "Vertex",
                          { .wholePicture = whole, .tessellation = tess, .geometry = geometry, .fragment = fragment }, "GraphicsPipeline", std::move(settings));
                }
    for (const bool task : { false, true })
        for (const bool fragment : { false, true }) {
            std::vector<std::pair<std::string, std::string>> settings { { "stages", "eMesh" }, { "mesh.module", "~" }, { "@framebuffer", "sky target" } };
            if (task) settings.push_back({ "task.module", "~" });
            if (fragment) settings.push_back({ "fragment.module", "~" });
            build(std::format("meshes{}{}", task ? "Task" : "", fragment ? "" : "Depth"), "MeshShading", { .task = task, .fragment = fragment }, "GraphicsPipeline", std::move(settings));
        }
    build("compute", "Compute", {}, "ComputePipeline", { { "compute.module", "~" } });
    for (const bool anyHit : { false, true })
        for (const bool intersection : { false, true }) {
            std::vector<std::pair<std::string, std::string>> settings { { "raygen.module", "~" }, { "miss[0].module", "~" }, { "hitGroup[0].closestHit.module", "~" } };
            if (anyHit) settings.push_back({ "hitGroup[0].anyHit.module", "~" });
            if (intersection) settings.push_back({ "hitGroup[0].intersection.module", "~" });
            build(std::format("rays{}{}", anyHit ? "AnyHit" : "", intersection ? "Intersection" : ""), "RayTracing", { .anyHit = anyHit, .intersection = intersection },
                  "RayTracingPipeline", std::move(settings));
        }
    std::printf("[ shaders  ] %d of 25 made and built here\n", made);
    EXPECT_GE(made, 9) << "every vertex shader without tessellation or geometry, and compute, wherever there is a GPU";
    EXPECT_FALSE(Session().NewShader("other", "Mesh")) << "no such kind";

    // The dialog offers the kinds by what they are, and what each can have.
    Session().AskForNewShader();
    for (const char* kind : { "Vertex + fragment", "Mesh + fragment", "Compute", "Ray tracing" }) EXPECT_TRUE(Shows(kind)) << kind;
    for (const char* option : { "Over the whole picture", "Tessellation", "Geometry shader", "Fragment shader" }) EXPECT_TRUE(Shows(option)) << option;
    Sheet("new shader");
}

// A shader and a script are made from the editor: a file in the project to start from, which a pipeline can name
// as it is and a script host loads.
TEST_F(EditorTest, ShadersAndScriptsAreMadeFromTheEditor) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "assets";
    std::filesystem::remove_all(directory);
    Open();
    EXPECT_FALSE(Session().NewShader("glow", "Compute")) << "there is no project for it to go in";
    EXPECT_TRUE(Session().Failed());
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewLitScene());      // what a mesh's shader reads, the camera and the lights, is the lit graph's

    for (const auto& [kind, what] : keditor::Session::ShaderKinds()) EXPECT_FALSE(what.empty()) << kind;
    ASSERT_TRUE(Session().NewShader("glow", "Compute")) << Session().Status();
    ASSERT_TRUE(Session().NewShader("paint", "Vertex")) << Session().Status();
    ASSERT_TRUE(Session().NewShader("invert", "Vertex", { .wholePicture = true })) << Session().Status();
    for (const char* name : { "glow", "paint", "invert" }) EXPECT_TRUE(std::filesystem::exists(directory / "shaders" / (std::string(name) + ".slang"))) << name;
    EXPECT_TRUE(Shows("Made shaders/invert.slang"));
    EXPECT_FALSE(Session().NewShader("glow", "Compute")) << "one of that name is there";
    EXPECT_FALSE(Session().NewShader("2 fast", "Compute")) << "not a name a module can have";
    EXPECT_FALSE(Session().NewShader("other", "Tessellated")) << "no such kind";
    const auto modules = Session().ShaderModules();
    EXPECT_NE(std::ranges::find(modules, "paint"), modules.end()) << "there for a pipeline to name";

    // Each is a shader as it is: a pipeline that names it is built.
    Session().ShowPass("scene");
    const std::string glow = Doc().AddNode("ComputePipeline", 0, 0, "glow", "scene");
    Doc().SetInput(glow, "compute.module", "glow");
    Doc().SetInput(glow, "compute.entry", "computeMain");
    EXPECT_TRUE(Doc().Runtime().Status(glow)->ok) << Doc().Runtime().Status(glow)->error;
    const std::string paint = Doc().AddNode("GraphicsPipeline", 0, 0, "paint", "scene");
    Doc().SetInput(paint, "vertex.module", "paint");       // its entry points as a pipeline looks for them: vertexMain, fragmentMain
    Doc().SetInput(paint, "fragment.module", "paint");
    Doc().Connect("shape", "layout", paint, "vertex.layout");
    Doc().Connect("sky target", "out", paint, "framebuffer");
    Doc().Connect("camera data", "out", paint, "param.camera");
    Doc().Connect("ambient light", "out", paint, "param.ambient");
    for (const auto& [output, param] : { std::pair { "directional", "param.directionalLights" }, { "points", "param.pointLights" }, { "spots", "param.spotLights" } })
        Doc().Connect("lights", output, paint, param);     // lit as the template says: by the Lights function's buffers
    ASSERT_TRUE(Doc().Runtime().Status(paint)->ok) << Doc().Runtime().Status(paint)->error;
    const kecs::ComponentId component = Doc().Runtime().ComponentOf(paint);
    ASSERT_NE(component, kecs::NoComponent);
    EXPECT_EQ(Doc().World().Describe(component).fields.size(), 3u) << "with no draw of it yet, every push constant is the component's";
    // A draw of every entity with a Mesh and the component: where it is, from the ForEach; its colour, from the component.
    const std::string each = Doc().AddNode("ForEach", 0, 0, "painted", "scene");
    Doc().SetInput(each, "component", "paint");
    Doc().SetInput(each, "with", kgraph::Value::List { "Mesh" });
    const std::string draw = Doc().AddNode("Draw", 0, 0, "draw paint", "scene");
    Doc().Connect(paint, "out", draw, "pipeline");
    Doc().Connect(each, "Mesh.mesh", draw, "mesh");
    Doc().Connect(each, "matrix", draw, "push.model");
    Doc().Connect(each, "field.color", draw, "push.color");
    Doc().Connect("lights", "counts", draw, "push.lightCounts");      // the same for every draw
    EXPECT_TRUE(Doc().Runtime().Status(draw)->ok) << Doc().Runtime().Status(draw)->error;
    ASSERT_EQ(Doc().World().Describe(component).fields.size(), 1u) << "where it is is wired from elsewhere: the colour is all that is left";
    EXPECT_EQ(Doc().World().Describe(component).fields.front().name, "color");
    const std::string invert = Doc().AddNode("GraphicsPipeline", 0, 0, "invert", "scene");
    Doc().SetInput(invert, "vertex.module", "invert");
    Doc().SetInput(invert, "fragment.module", "invert");
    Doc().Connect("sky target", "out", invert, "framebuffer");
    Doc().Connect("color", "out", invert, "param.source");
    EXPECT_TRUE(Doc().Runtime().Status(invert)->ok) << Doc().Runtime().Status(invert)->error;

    // With no scripts, there is nothing to run them, and the player runs it.
    EXPECT_TRUE(Session().ScriptsNotRunning().empty());
    EXPECT_NE(Session().RunCommand().find("koral-play"), std::string::npos);

    // A script, in either language, the first time: a file with a class of its name. Its language is the project's from
    // then on, kept in project.json, and a script in the other is refused.
    EXPECT_EQ(Session().ScriptKindsHere().size(), 4u) << "either, until the first: a script of entities and a ResourceScript in each";
    ASSERT_TRUE(Session().NewScript("Drifter", "Kotlin")) << Session().Status();
    const auto text = [&](const std::filesystem::path& folder, const char* file) {
        std::ifstream in(folder / file);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    EXPECT_NE(text(directory, "project.json").find(R"("scripts": "kotlin")"), std::string::npos) << text(directory, "project.json");
    EXPECT_EQ(Session().CurrentProject().Language(), 'k');
    ASSERT_EQ(Session().ScriptKindsHere().size(), 2u);
    for (const auto& [kind, what] : Session().ScriptKindsHere()) EXPECT_TRUE(kind.starts_with("Kotlin")) << kind << ": the New script dialog offers the project's only";
    EXPECT_FALSE(Session().NewScript("Roller", "C#")) << "a project's scripts are in one language";
    EXPECT_NE(Session().Status().find("in Kotlin"), std::string::npos) << Session().Status();
    ASSERT_TRUE(Session().NewScript("Roller", "Kotlin")) << Session().Status();
    EXPECT_NE(text(directory / "scripts", "Drifter.kt").find("class Drifter : Setup(), PerFrame"), std::string::npos);
    // Kotlin is compiled by the program that runs it; what is written is only what an editor of code imports to know the
    // names: the scripts, the engine's own, and the engine's libraries.
    for (const char* file : { "build.gradle.kts", "settings.gradle.kts", "gradle.properties", "gradlew", "gradle/wrapper/gradle-wrapper.jar" })
        EXPECT_TRUE(std::filesystem::exists(directory / file)) << file << ": what an editor of code imports the scripts by";
    const std::string build = text(directory, "build.gradle.kts");
    EXPECT_NE(build.find("\"scripts\""), std::string::npos) << build;
    EXPECT_NE(build.find((kscript::PackageDirectory() / "scripts").lexically_normal().generic_string()), std::string::npos) << "the engine's own scripts";
    EXPECT_NE(build.find("/kotlin/lib"), std::string::npos) << "and the engine's libraries";
    EXPECT_NE(build.find("Nothing builds the scripts with it"), std::string::npos);
    EXPECT_NE(Session().RunCommand().find("koral-play"), std::string::npos) << "the player runs the scripts itself: " << Session().RunCommand();
    // One in the other language, put there by hand, runs nowhere: it is said so.
    std::ofstream(directory / "scripts" / "Stray.cs") << "// not the project's language";
    EXPECT_EQ(Session().CurrentProject().ScriptsInAnotherLanguage().size(), 1u);
    EXPECT_TRUE(HasProblem("scripts/Stray.cs: the project's scripts are in Kotlin"));

    // This program has nothing to run them with (koral-editor has: khost): it says so rather than play a scene whose
    // scripts do nothing. Data, and plain classes a script uses, have nothing to run: they are not among them.
    std::ofstream(directory / "scripts" / "Reading.kt") << "import koral.engine.Data\n\nclass Reading : Data() {\n    var value = 1f\n}\n";
    std::ofstream(directory / "scripts" / "Buffers.kt") << "class Buffers {\n    var values = FloatArray(0)\n}\n";
    EXPECT_EQ(Session().ScriptsNotRunning(), (std::vector<std::string> { "Drifter", "Roller" }));
    EXPECT_TRUE(HasProblem("Nothing is running the project's scripts (Drifter, Roller): this program runs no scripts"));
    Session().StartPlaying();
    EXPECT_TRUE(Session().Failed());
    EXPECT_TRUE(Shows("Drifter and 1 more are not running"));
    Session().Stop();
    // With something that runs them, why they do not is what it says — and once it runs them, that they did not compile.
    {
        struct Runner final : keditor::ScriptRuntime {
            bool runs = false;
            bool Runs(const std::filesystem::path&, std::string& why) const override { why = "its scripts are in Kotlin, and no Java was found"; return runs; }
            void Forget(keditor::Document*) override {}
        } runner;
        keditor::scriptRuntime = &runner;
        EXPECT_TRUE(HasProblem("Nothing is running the project's scripts (Drifter, Roller): its scripts are in Kotlin, and no Java was found"));
        std::string why;
        EXPECT_FALSE(Session().HostsScripts(&why));
        runner.runs = true;
        EXPECT_TRUE(Session().HostsScripts());
        EXPECT_TRUE(HasProblem("Not running: Drifter, Roller. They did not compile, or are being compiled"));
        keditor::scriptRuntime = nullptr;
    }
    // A new scene knows them too, as one loaded from its file does: an entity in it is given one.
    ASSERT_TRUE(Session().NewStandardScene()) << Session().Status();
    EXPECT_TRUE(Doc().AddComponent(Doc().World().Roots().front(), "Drifter")) << "a new scene forgot the project's scripts";
    EXPECT_FALSE(Session().NewScript("Roller", "Kotlin")) << "a script of that name is there";
    EXPECT_FALSE(Session().NewScript("default", "Kotlin")) << "a component is called that already";
    EXPECT_FALSE(Session().NewScript("My Script", "Kotlin"));
    EXPECT_FALSE(Session().NewScript("Thing", "Lua"));

    // A program in C# or Kotlin that opened the engine's Editor scene with the project's scripts runs them itself.
    Session().HostScripts(directory);
    EXPECT_TRUE(Session().HostsScripts());
    Session().HostScripts({});

    // A project made with its scripts in C#: so from the start, before any script, and Kotlin refused.
    const auto csharp = directory.parent_path() / "assets-csharp";
    std::filesystem::remove_all(csharp);
    ASSERT_TRUE(Session().NewProject(csharp, 'c')) << Session().Status();
    EXPECT_NE(text(csharp, "project.json").find(R"("scripts": "csharp")"), std::string::npos) << text(csharp, "project.json");
    EXPECT_EQ(Session().ScriptKindsHere().size(), 2u) << "only C#'s, from the start";
    ASSERT_TRUE(Session().NewScript("Roller", "C#")) << Session().Status();
    EXPECT_NE(text(csharp / "scripts", "Roller.cs").find("public sealed class Roller : Setup, PerFrame"), std::string::npos);
    EXPECT_EQ(text(csharp / "scripts", "Roller.cs").find("$NAME"), std::string::npos);
    EXPECT_NE(Session().RunCommand().find("koral-play"), std::string::npos) << "the player runs C# scripts itself: " << Session().RunCommand();
    EXPECT_FALSE(Session().NewScript("Drifter", "Kotlin"));
    EXPECT_FALSE(std::filesystem::exists(csharp / "build.gradle.kts")) << "and no Kotlin build was made for it";
}

// A script's entity is set from the inspector; a script dropped on the inspector is the entity's; and a prefab is made,
// placed and dropped where it is wanted.
TEST_F(EditorTest, ScriptsAreGivenEntitiesAndPrefabsArePlaced) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "prefabs";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    kecs::World& world = Doc().World();
    const kecs::Entity sphere = world.FindEntity("Sphere"), camera = world.FindEntity("Camera");
    ASSERT_FALSE(sphere.IsNull());

    // A script with an entity of its own to follow: it starts following none.
    std::filesystem::create_directories(directory / "scripts");
    std::ofstream(directory / "scripts" / "Follower.kt") << "import koral.engine.*\n\nclass Follower : Script() {\n    var target: Entity = Entity.Null\n    var speed = 1f\n}\n";
    Doc().DeclareScripts(directory / "scripts");
    for (const auto& note : Doc().Notes()) EXPECT_EQ(note.find("Follower"), std::string::npos) << note;
    ASSERT_TRUE(Session().GiveAsset("script:Follower", camera)) << Session().Status();
    EXPECT_TRUE(Doc().Field(camera, "Follower", "target").IsNull()) << "none, not the first entity there is";
    Session().Select(camera);
    EXPECT_TRUE(Shows("(nothing)"));
    ASSERT_TRUE(Doc().SetField(camera, "Follower", "target", keditor::EntityValue(sphere)));
    EXPECT_TRUE(Shows("Sphere"));
    EXPECT_EQ(Doc().Field(camera, "Follower", "target"), keditor::EntityValue(sphere));

    // The Scripts are the components the sources declare, not their files: a file of a kind and two of it is three, each
    // opened in that file; a file of plain classes is none. A kind is not given to an entity.
    std::ofstream(directory / "scripts" / "Readings.kt") << "import koral.engine.Data\n\nabstract class Reading : Data() {\n    var value = 1f\n}\n"
                                                             "class Heat : Reading() {\n    var where = 0f\n}\nclass Light2 : Reading() {\n}\n";
    std::ofstream(directory / "scripts" / "Helpers.kt") << "class Helpers {\n    var values = FloatArray(0)\n}\n";
    // A class of conditions is among them too, with an icon of its own: no component.
    std::ofstream(directory / "scripts" / "Tests.kt") << "import koral.engine.*\n\nclass MeshTests : Conditions() {\n    companion object {\n"
                                                         "        @Condition(1) @JvmStatic\n        fun hasNormals(mesh: EntityMesh): Boolean = mesh.layout.has(\"NORMAL\")\n    }\n}\n";
    std::map<std::string, keditor::Session::Asset> scripts;
    for (const auto& asset : Session().Assets())
        if (asset.kind == keditor::Session::Asset::Kind::eScript) scripts[asset.label] = asset;
    EXPECT_EQ(scripts.size(), 5u) << "Follower, Reading, Heat, Light2 and MeshTests";
    ASSERT_TRUE(scripts.contains("MeshTests"));
    EXPECT_EQ(scripts["MeshTests"].group, "conditions");
    EXPECT_FALSE(Session().GiveAsset("script:MeshTests", sphere)) << "no component an entity has";
    ASSERT_TRUE(scripts.contains("Heat") && scripts.contains("Reading") && scripts.contains("Follower"));
    EXPECT_FALSE(scripts.contains("Helpers") || scripts.contains("Readings"));
    EXPECT_EQ(scripts["Heat"].file.filename(), "Readings.kt");
    EXPECT_EQ(scripts["Heat"].group, "data");
    EXPECT_EQ(scripts["Reading"].group, "kind");
    EXPECT_EQ(scripts["Follower"].group, "script");
    Doc().DeclareScripts(directory / "scripts");
    EXPECT_FALSE(Session().GiveAsset("script:Reading", sphere)) << "nothing has a kind itself";
    EXPECT_TRUE(Session().GiveAsset("script:Heat", sphere)) << Session().Status();
    EXPECT_TRUE(Doc().World().Has(sphere, Doc().World().Find("Reading")));

    // Made a prefab of, the sphere is its first instance; the prefab is among the assets, and placed it is another.
    ASSERT_TRUE(Session().SaveAsPrefab(sphere)) << Session().Status();
    EXPECT_TRUE(std::filesystem::exists(directory / "prefabs" / "Sphere.prefab"));
    EXPECT_FALSE(Session().SaveAsPrefab(sphere)) << "one of that name is there";
    const auto assets = Session().Assets();
    const auto prefab = std::ranges::find(assets, std::string("prefab:Sphere.prefab"), &keditor::Session::Asset::id);
    ASSERT_NE(prefab, assets.end());
    EXPECT_EQ(prefab->kind, keditor::Session::Asset::Kind::ePrefab);
    const kecs::Entity placed = Session().PlacePrefab("prefab:Sphere.prefab");
    ASSERT_FALSE(placed.IsNull()) << Session().Status();
    EXPECT_EQ(Session().selectedEntity, placed);
    EXPECT_EQ(Doc().PrefabOf(placed), "prefabs/Sphere.prefab");
    EXPECT_TRUE(Shows("An instance of Sphere"));
    EXPECT_TRUE(Shows("Apply to the prefab"));
    // Dropped on an entity, it is an instance under it.
    ASSERT_TRUE(Session().GiveAsset("prefab:Sphere.prefab", camera)) << Session().Status();
    ASSERT_FALSE(world.ChildrenOf(camera).empty());
    EXPECT_EQ(Doc().PrefabOf(world.ChildrenOf(camera).back()), "prefabs/Sphere.prefab");
    // What is set on an instance is its own, and said so.
    ASSERT_TRUE(Doc().SetField(placed, "default", "color", kgraph::Value(kgraph::Value::List { 1.f, 1.f, 1.f })));
    Session().Select(placed);
    EXPECT_TRUE(Shows("1 thing is its own"));
}

// Nodes picked are made a function from the editor; a call is opened in place, said so over the passes, and closed.
TEST_F(EditorTest, AFunctionIsMadeOpenedAndClosedFromTheEditor) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "functions";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Doc().AddPass("main"));
    Session().ShowPass("main");
    Doc().AddNode("Double", 0, 0, "n", "main");
    Doc().SetInput("n", "value", 2);
    Doc().AddNode("Multiply", 300, 0, "times", "main");
    Doc().SetInput("times", "b", 3);
    ASSERT_TRUE(Doc().Connect("n", "out", "times", "a"));
    Doc().AddNode("Add", 600, 0, "show", "main");
    Doc().SetInput("show", "b", 1);
    ASSERT_TRUE(Doc().Connect("times", "out", "show", "a"));

    const std::string call = Session().MakeFunction({ "times" }, "Triple");
    ASSERT_FALSE(call.empty()) << Session().Status();
    EXPECT_EQ(Session().selectedNode, call);
    EXPECT_EQ(Doc().Runtime().ValueOf("show"), kgraph::Value(7));
    EXPECT_TRUE(Shows("Triple"));
    // Its `function` is chosen among the project's.
    std::vector<std::string> own;
    for (const auto& name : Doc().Functions()) if (!Doc().IsEngineFunction(name)) own.push_back(name);
    EXPECT_EQ(own, (std::vector<std::string> { "Triple" })) << "the project's, besides the engine's";
    // And it is offered to be made like any node: by its name, among the functions.
    const auto presets = Doc().Presets();
    const auto offered = std::ranges::find(presets, std::string("Triple"), &kgraph::Preset::name);
    ASSERT_NE(offered, presets.end());
    EXPECT_EQ(offered->type, "Function");
    const auto found = keditor::FindNodeTypes("Triple", presets);
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(found.front()->name, "Triple");
    EXPECT_EQ(found.front()->category, "Functions");
    const std::string another = Doc().AddNode("Triple", 0, 300, {}, "main");
    ASSERT_FALSE(another.empty());
    EXPECT_EQ(Doc().Graph().Find(another)->type, "Function");
    Doc().SetInput(another, "a", 10);
    EXPECT_EQ(Doc().Runtime().ValueOf(another, "times"), kgraph::Value(30)) << "a call of it, made from the menu";

    // A `.kfunc` opened from the Assets panel is its page: by a call there is, or by one that goes with the page.
    keditor::Session::Asset file;
    file.kind = keditor::Session::Asset::Kind::eFunction;
    file.file = Doc().FunctionsDirectory() / "Triple.kfunc";
    Session().OpenAsset(file);
    ASSERT_TRUE(Doc().Opened().has_value()) << Session().Status();
    EXPECT_EQ(Doc().Opened()->function, "Triple");
    ASSERT_TRUE(Session().CloseFunction(false));

    ASSERT_TRUE(Session().OpenFunction(call)) << Session().Status();
    EXPECT_TRUE(Shows("Triple  ·  every call")) << "where the graph is: in the function, on the call's page";
    EXPECT_TRUE(Shows("Discard"));
    ASSERT_TRUE(Doc().SetInput(call + "/times", "b", 5));
    ASSERT_TRUE(Session().CloseFunction(true)) << Session().Status();
    EXPECT_FALSE(Shows("Triple  ·  every call"));
    EXPECT_EQ(Doc().Runtime().ValueOf("show"), kgraph::Value(11));
    EXPECT_EQ(Session().selectedNode, call);
    // Saved while open, it is closed first, keeping what was done: its page is no part of the scene.
    ASSERT_TRUE(Session().OpenFunction(call));
    ASSERT_TRUE(Session().Save()) << Session().Status();
    EXPECT_FALSE(Doc().Opened().has_value());

    // Its signature, beside its page: what every call takes and gives, each to rename, retype or move.
    ASSERT_TRUE(Session().OpenFunction(call));
    EXPECT_TRUE(Shows("TRIPLE  ·  SIGNATURE"));
    EXPECT_TRUE(Shows("TAKES  ·  1"));
    EXPECT_TRUE(Shows("GIVES  ·  1"));
    ASSERT_TRUE(Session().CloseFunction(true));

    // A call keeps its function by the file's ID: renamed in the Assets panel, every call finds it by its new name.
    EXPECT_EQ(*Doc().Graph().Find(call)->inputs.at("file").value.String(), "function:Triple");
    ASSERT_TRUE(Session().MoveAsset("function:Triple", "functions/Thrice.kfunc")) << Session().Status();
    ASSERT_NE(Doc().Graph().Find(call), nullptr);
    EXPECT_EQ(*Doc().Graph().Find(call)->inputs.at("function").value.String(), "Thrice") << Doc().Graph().Find(call)->inputs.at("file").value.ToJson();
    EXPECT_EQ(Doc().Runtime().ValueOf("show"), kgraph::Value(11)) << "and works out what it did";
    file.file = Doc().FunctionsDirectory() / "Thrice.kfunc";

    // Dropped on a page from the Assets panel, a function is a call of it.
    const std::string dropped = Session().AssetToNode("function:Thrice", 0, 600);
    ASSERT_FALSE(dropped.empty()) << Session().Status();
    EXPECT_EQ(Doc().Graph().Find(dropped)->type, "Function");
    EXPECT_EQ(*Doc().Graph().Find(dropped)->inputs.at("function").value.String(), "Thrice");

    // With no call of it in the scene, one is made for the page and goes with it.
    std::vector<std::string> calls;
    for (const auto& node : Doc().Graph().nodes) if (node.type == "Function") calls.push_back(node.id);
    Doc().RemoveNodes(calls);
    const std::size_t without = Doc().Graph().nodes.size();
    Session().OpenAsset(file);
    ASSERT_TRUE(Doc().Opened().has_value()) << Session().Status();
    ASSERT_TRUE(Session().CloseFunction(true)) << Session().Status();
    EXPECT_EQ(Doc().Graph().nodes.size(), without) << "the call made to open it is gone";
}

TEST_F(EditorTest, TheProjectToStartWithIsTheUsersChoice) {
    const char* was = std::getenv("XDG_CONFIG_HOME");
    const std::string before = was ? was : "";
    const auto config = std::filesystem::temp_directory_path() / "keditor-startup-config";
    std::filesystem::remove_all(config);
    setenv("XDG_CONFIG_HOME", config.string().c_str(), 1);
    const auto project = std::filesystem::temp_directory_path() / "keditor-startup-project";
    std::filesystem::create_directories(project);
    std::ofstream(project / "project.json") << R"({ "koral-engine-project": 1, "name": "startup", "scenes": ["main"], "start": "main" })";

    EXPECT_TRUE(keditor::Session::StartupProject().empty()) << "none said: the engine's own";
    ASSERT_TRUE(keditor::Session::SetStartupProject(project));
    EXPECT_TRUE(std::filesystem::equivalent(keditor::Session::StartupProject(), project));
    std::filesystem::remove(project / "project.json");
    EXPECT_TRUE(keditor::Session::StartupProject().empty()) << "one that is no project any more is not opened";
    std::ofstream(project / "project.json") << "{}";
    ASSERT_TRUE(keditor::Session::SetStartupProject({}));
    EXPECT_TRUE(keditor::Session::StartupProject().empty()) << "and the engine's own again";

    if (was) setenv("XDG_CONFIG_HOME", before.c_str(), 1); else unsetenv("XDG_CONFIG_HOME");
    std::filesystem::remove_all(config);
    std::filesystem::remove_all(project);
}

// Not a test: sets a project up with the standard scene and the entities a file lists, when told where.
//   KEDITOR_WRITE_PROJECT=<project folder> KEDITOR_ENTITIES=<json list of entities> keditor_tests --gtest_also_run_disabled_tests --gtest_filter='*WritesAProject*'
TEST_F(EditorTest, DISABLED_WritesAProject) {
    const char* directory = std::getenv("KEDITOR_WRITE_PROJECT");
    const char* entities = std::getenv("KEDITOR_ENTITIES");
    ASSERT_NE(directory, nullptr);
    Open();
    ASSERT_TRUE(Session().OpenProject(directory)) << Session().Status();
    ASSERT_TRUE(Session().NewStandardScene()) << Session().Status();
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    if (entities) {
        for (const kecs::Entity root : std::vector(Doc().World().Roots())) Doc().DestroyEntity(root);
        std::ifstream in(entities, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        kecs::json::Value parsed;
        std::string error;
        ASSERT_TRUE(kecs::json::Parse(text, parsed, error)) << error;
        std::vector<std::string> problems;
        for (const auto& entity : *kgraph::Value::FromJson(parsed).Items()) Doc().ReadEntity(entity, kecs::NullEntity, &problems);
        for (const auto& problem : problems) ADD_FAILURE() << problem;
    }
    Session().Select(kecs::NullEntity);
    ASSERT_TRUE(Session().Save()) << Session().Status();
    Session().SetLookThroughCamera(true);
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(NothingIsWrong());
    Sheet("project");
    // Opened again from what was written, it is all there and nothing is wrong with it.
    const std::size_t count = Doc().World().EntityCount();
    ASSERT_TRUE(Session().OpenProject(directory)) << Session().Status();
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    for (const auto& note : Doc().Notes()) ADD_FAILURE() << note;
    EXPECT_EQ(Doc().World().EntityCount(), count);
}

TEST_F(EditorTest, NodesAreFoundByWhatSomeoneWouldCallThem) {
    const auto first = [](const std::string_view typed) {
        const auto found = keditor::FindNodeTypes(typed);
        return found.empty() ? std::string("(nothing)") : found.front()->name;
    };
    const auto finds = [](const std::string_view typed, const std::string_view name) {
        return std::ranges::any_of(keditor::FindNodeTypes(typed), [&](const kgraph::NodeType* type) { return type->name == name; });
    };
    EXPECT_EQ(first("image"), "Image") << "its own name, before ImageView and ImageExtent";
    EXPECT_EQ(first("texture"), "Image") << "another word for it";
    EXPECT_EQ(first("mix"), "Lerp");
    EXPECT_EQ(first("pipe"), "GraphicsPipeline") << "the start of a word inside its name";
    EXPECT_EQ(first("compute pipe"), "ComputePipeline") << "every word typed has to fit";
    EXPECT_EQ(first("run compute"), "ComputePass") << "a pipeline and its dispatch at once";
    EXPECT_TRUE(finds("run compute", "Dispatch"));
    EXPECT_EQ(first("if"), "Select");
    EXPECT_EQ(first("mip"), "GenerateMipmaps");
    EXPECT_EQ(first("quad"), "Primitive");
    EXPECT_EQ(first("<"), "Less");
    EXPECT_TRUE(finds("size", "ImageExtent"));
    EXPECT_TRUE(finds("compare", "Equal") && finds("compare", "Greater"));
    EXPECT_TRUE(finds("radians", "Sin")) << "found by what it says it does";
    EXPECT_TRUE(keditor::FindNodeTypes("no such thing at all").empty());
    EXPECT_FALSE(finds("shader", "Shader")) << "what an editor does not offer is not found";

    // Nothing typed: every kind there is to add, in the catalog's order.
    const auto all = keditor::FindNodeTypes("");
    EXPECT_GT(all.size(), 60u);
    EXPECT_EQ(all.front()->name, "Buffer");

    // And in the editor: Control+Space opens the list, wherever the pointer is.
    Open();
    auto& input = editor->SceneInput();
    input.FeedKey(kor::Key::eLeftControl, true);
    Frame();
    input.FeedKey(kor::Key::eSpace, true);
    Frame();
    input.FeedKey(kor::Key::eSpace, false);
    input.FeedKey(kor::Key::eLeftControl, false);
    EXPECT_TRUE(Shows("Add a node"));
}

// What a node holds, made something to look at: a histogram of an image, a texel of it, a picture of some of its
// channels between two values said; a buffer by a layout typed, as a table.
TEST(Look, ImagesAreCountedAndPicturedAndBuffersAreTables) {
    kgraph::Peek image;
    image.image = true;
    image.format = "eRGBA8_UNORM";
    image.extent = { 2, 2, 1 };
    const std::uint8_t texels[] { 255, 0, 0, 255,   0, 255, 0, 128,   0, 0, 255, 0,   255, 255, 255, 255 };
    for (const auto b : texels) image.bytes.push_back(static_cast<std::byte>(b));

    const auto histogram = keditor::HistogramOf(image, 0, 4);
    ASSERT_EQ(histogram.channels, 4);
    EXPECT_EQ(histogram.texels, 4u);
    EXPECT_DOUBLE_EQ(histogram.min[0], 0.0);
    EXPECT_DOUBLE_EQ(histogram.max[0], 1.0);
    EXPECT_DOUBLE_EQ(histogram.mean[0], 0.5);
    EXPECT_EQ(histogram.bins[0][0], 2u) << "two reds of 0";
    EXPECT_EQ(histogram.bins[0][3], 2u) << "and two of 1";

    const auto texel = keditor::TexelAt(image, 1, 0);
    ASSERT_TRUE(texel);
    EXPECT_EQ(texel->channels, 4);
    EXPECT_NEAR(texel->values[3], 128.0 / 255.0, 1e-9);
    EXPECT_NE(texel->text.find("A 0.50196 (128)"), std::string::npos) << texel->text;
    EXPECT_FALSE(keditor::TexelAt(image, 2, 0)) << "outside it";

    // Green alone is grey; and between 0.5 and 1, what is 0.5 or less is black.
    keditor::LookOptions options;
    options.channels = { false, true, false, false };
    options.black = 0.5;
    options.white = 1.0;
    const auto picture = keditor::ToPicture(image, options);
    ASSERT_EQ(picture.pixels.size(), 4u);
    EXPECT_EQ(picture.pixels[0], kor::U8Vec4(0, 0, 0, 255));
    EXPECT_EQ(picture.pixels[1], kor::U8Vec4(255, 255, 255, 255));

    std::string error;
    const auto layout = keditor::ParseBufferLayout("float3 position; uint id\nmat2 m; int weights[2] // the rest", &error);
    ASSERT_TRUE(layout) << error;
    ASSERT_EQ(layout->fields.size(), 4u);
    EXPECT_EQ(layout->stride, (3 + 1 + 4 + 2) * 4u);
    EXPECT_EQ(layout->fields[2].offset, 16u);
    EXPECT_EQ(layout->fields[2].numbers, 4u);
    EXPECT_EQ(layout->fields[3].scalar, 'i');
    EXPECT_EQ(layout->fields[3].numbers, 2u);
    EXPECT_FALSE(keditor::ParseBufferLayout("float5 x", &error));
    EXPECT_NE(error.find("no type"), std::string::npos) << error;
    EXPECT_FALSE(keditor::ParseBufferLayout("   ", &error));

    kgraph::Peek buffer;
    const auto two = keditor::ParseBufferLayout("float2 at; uint id");
    for (const float f : { 1.5f, -2.f }) { const auto* b = reinterpret_cast<const std::byte*>(&f); buffer.bytes.insert(buffer.bytes.end(), b, b + 4); }
    const std::uint32_t id = 255;
    buffer.bytes.insert(buffer.bytes.end(), reinterpret_cast<const std::byte*>(&id), reinterpret_cast<const std::byte*>(&id) + 4);
    auto table = keditor::ToTable(buffer, &*two, 0, 10);
    EXPECT_EQ(table.columns, (std::vector<std::string> { "#", "at.x", "at.y", "id" }));
    ASSERT_EQ(table.total, 1u);
    EXPECT_EQ(table.rows[0], (std::vector<std::string> { "0", "1.5", "-2", "255" }));
    table = keditor::ToTable(buffer, &*two, 0, 10, true);
    EXPECT_EQ(table.rows[0][3], "0x000000FF");
    table = keditor::ToTable(buffer, nullptr, 0, 10);
    EXPECT_EQ(table.columns.front(), "byte") << "no layout: words, eight a row";
    EXPECT_EQ(table.rows[0][3], "255");

    // An element wider than a row — a camera's matrix — is a line a field, the matrix a row of it a line.
    kgraph::Peek camera;
    for (int i = 0; i < 16; ++i) { const float f = i == 12 ? 0.5f : i % 5 == 0 ? 1.f : 0.f; const auto* b = reinterpret_cast<const std::byte*>(&f); camera.bytes.insert(camera.bytes.end(), b, b + 4); }
    const auto matrix = keditor::ParseBufferLayout("float4x4 viewProjection");
    table = keditor::ToTable(camera, &*matrix, 0, 10);
    EXPECT_TRUE(table.byField);
    EXPECT_EQ(table.columns, (std::vector<std::string> { "#", "field", "x", "y", "z", "w" }));
    EXPECT_EQ(table.total, 1u) << "one element";
    ASSERT_EQ(table.rows.size(), 4u) << "four rows of the matrix";
    EXPECT_EQ(table.rows[0], (std::vector<std::string> { "0", "viewProjection", "1", "0", "0", "0.5" })) << "its first row: the first of each column";
    EXPECT_EQ(table.rows[3], (std::vector<std::string> { "", "", "0", "0", "0", "1" }));
}

// Imported, a model or a picture is the project's — one of its assets, with what it names beside it — and nothing of it is
// in the scene until it is dragged there.
TEST_F(EditorTest, AnImportedAssetIsTheProjectsAndNotYetTheScenes) {
    const auto project = scene::MakeProject("imports");
    Open(project.directory);
    // Outside the project: a model that names its materials, and they a picture; another that names a buffer and a picture.
    const auto outside = std::filesystem::temp_directory_path() / "keditor-imports-outside";
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside / "data");
    std::ofstream(outside / "crate.obj") << "mtllib crate.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvn 0 0 1\nusemtl wood\nf 1/1/1 2/2/1 3/3/1\n";
    std::ofstream(outside / "crate.mtl") << "newmtl wood\nmap_Kd -s 1 1 1 wood.png\n";
    std::ofstream(outside / "wood.png") << "not looked at";
    std::ofstream(outside / "lamp.gltf") << R"({ "asset": { "version": "2.0" }, "buffers": [ { "uri": "data/lamp.bin", "byteLength": 4 } ],
                                               "images": [ { "uri": "lamp%20glow.png" }, { "uri": "data:image/png;base64,AAAA" } ] })";
    std::ofstream(outside / "data" / "lamp.bin") << "abcd";
    std::ofstream(outside / "lamp glow.png") << "not looked at";
    const std::size_t entities = Doc().World().EntityCount();

    const std::string crate = Session().ImportAsset(outside / "crate.obj");
    ASSERT_FALSE(crate.empty()) << Session().Status();
    const auto models = project.directory / "assets" / "models";
    for (const char* file : { "crate/crate.obj", "crate/crate.mtl", "crate/wood.png" }) EXPECT_TRUE(std::filesystem::exists(models / file)) << file;
    ASSERT_FALSE(Session().ImportAsset(outside / "lamp.gltf").empty()) << Session().Status();
    for (const char* file : { "lamp/lamp.gltf", "lamp/data/lamp.bin", "lamp/lamp glow.png" }) EXPECT_TRUE(std::filesystem::exists(models / file)) << file;
    ASSERT_FALSE(Session().ImportAsset(outside / "wood.png").empty());
    EXPECT_TRUE(std::filesystem::exists(project.directory / "assets" / "textures" / "wood.png"));

    // None of it is in the scene: the Assets panel has it, picked, in its folder.
    EXPECT_EQ(Doc().World().EntityCount(), entities) << "importing makes no entities";
    EXPECT_EQ(Session().assetFolder, "assets/textures") << "the folder of the last imported is shown";
    const auto assets = Session().Assets();
    const auto listed = std::ranges::find(assets, crate, &keditor::Session::Asset::id);
    ASSERT_NE(listed, assets.end()) << "the model is among the project's assets";
    EXPECT_EQ(listed->kind, keditor::Session::Asset::Kind::eModel);
    EXPECT_FALSE(listed->loaded) << "and not in the scene";
    EXPECT_EQ(Session().ImportAsset(outside / "crate.obj"), crate) << "imported again, it is the one the project has";
    EXPECT_EQ(Session().ImportAsset(models / "crate" / "crate.obj"), crate) << "one of the project's is only picked";
    EXPECT_TRUE(Session().ImportAsset(outside / "crate.mtl").empty()) << "a material file is no asset of its own";

    // Each is known by an ID, kept beside it in its meta — what it names beside it (a material file, a buffer) is no asset.
    kasset::Database* database = Session().AssetDatabase();
    ASSERT_TRUE(database);
    const kasset::Entry* imported = database->FindFile(models / "crate" / "crate.obj");
    ASSERT_TRUE(imported) << "an imported file is an asset at once";
    EXPECT_EQ(imported->meta.kind, kasset::Kind::eModel);
    EXPECT_TRUE(std::filesystem::exists(project.directory / ".koral/meta/assets/models" / "crate" / "crate.obj.meta"));
    EXPECT_FALSE(std::filesystem::exists(project.directory / ".koral/meta/assets/models" / "crate" / "crate.mtl.meta"));
    EXPECT_FALSE(std::filesystem::exists(project.directory / ".koral/meta/assets/models" / "lamp" / "data" / "lamp.bin.meta"));
    EXPECT_EQ(database->FromLegacy(crate)->asset, imported->meta.id) << "its old name is that asset";
    EXPECT_EQ(database->ToLegacy({ imported->meta.id, {} }), crate);
    EXPECT_EQ(database->FromLegacy("$cube")->asset, kasset::Id::Named("mesh/cube")) << "the engine's own are known too";
    // Opened again, the project's assets are as they were.
    const kasset::Id id = imported->meta.id;
    Open(project.directory);
    database = Session().AssetDatabase();
    ASSERT_TRUE(database && database->FindFile(models / "crate" / "crate.obj"));
    EXPECT_EQ(database->FindFile(models / "crate" / "crate.obj")->meta.id, id);
    EXPECT_TRUE(database->FindFile(project.directory / "scenes" / "main.scene.json")) << "a scene is an asset";

    // Dragged onto the scene, it is: entities of it, now.
    ASSERT_TRUE(Session().ImportModel(models / "crate" / "crate.obj")) << Session().Status();
    EXPECT_GT(Doc().World().EntityCount(), entities);
}

// How a model's meshes are read, how a picture is kept and what a model's materials are imported as are the asset's,
// in its meta: every scene of the project reads it so, and a scene saves none of it.
TEST_F(EditorTest, ImportSettingsAreTheAssetsNotTheScenes) {
    const auto project = scene::MakeProject("settings");
    Open(project.directory);
    const auto models = project.directory / "assets" / "models" / "crate";
    const auto textures = project.directory / "assets" / "textures";
    std::filesystem::create_directories(models);
    std::filesystem::create_directories(textures);
    std::ofstream(models / "crate.obj") << "v 0 0 0\nv 1 0 0\nv 1 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    std::ofstream(textures / "wood.png") << "not looked at";
    const std::string crate = Session().ImportAsset(models / "crate.obj");
    const std::string wood = Session().ImportAsset(textures / "wood.png");
    ASSERT_FALSE(crate.empty() || wood.empty()) << Session().Status();

    kgraph::MeshImport import;
    import.leaveOut = { "NORMAL" };
    Doc().SetMeshImport(crate.substr(1), import);
    Doc().SetTextureSettings({ { wood, { kgraph::TextureFormat::eBC1, kgraph::TextureQuality::eFast } } }, {});
    Session().SetImportKind(kgraph::ModelMaterial::Model::eClassic, "PbrMaterial", models / "crate.obj");
    ASSERT_TRUE(Session().Save()) << Session().Status();

    // In the metas, and not in the scene.
    kasset::Database* assets = Session().AssetDatabase();
    ASSERT_TRUE(assets);
    const kasset::Meta crateMeta = assets->FindFile(models / "crate.obj")->meta;
    EXPECT_EQ(kasset::MeshImportFrom(kasset::ParameterOf(crateMeta, "mesh")), import);
    EXPECT_EQ(kasset::KindsFrom(kasset::ParameterOf(crateMeta, "materialKinds")), (std::map<std::string, std::string> { { "classic", "PbrMaterial" } }));
    EXPECT_EQ(kasset::TextureImportFrom(kasset::ParameterOf(assets->FindFile(textures / "wood.png")->meta, "texture")),
              (kgraph::TextureImport { kgraph::TextureFormat::eBC1, kgraph::TextureQuality::eFast }));
    std::ifstream saved(project.directory / "scenes" / "main.scene.json");
    const std::string scene((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    EXPECT_EQ(scene.find("meshImports"), std::string::npos);
    EXPECT_EQ(scene.find("textureImports"), std::string::npos);
    std::ifstream projectFile(project.directory / "project.json");
    const std::string projectText((std::istreambuf_iterator<char>(projectFile)), std::istreambuf_iterator<char>());
    EXPECT_EQ(projectText.find("crate"), std::string::npos) << "a model's own kinds are not the project's";

    // Opened again — any scene of it — it is read so.
    Open(project.directory);
    EXPECT_EQ(Doc().MeshImportOf(crate.substr(1)), import);
    ASSERT_TRUE(Doc().Graph().textureImports.contains(wood));
    EXPECT_EQ(Doc().Graph().textureImports.at(wood).format, kgraph::TextureFormat::eBC1);

    // Back to how it is by default, its meta says nothing of it.
    Doc().SetMeshImport(crate.substr(1), {});
    ASSERT_TRUE(Session().Save());
    EXPECT_EQ(kasset::ParameterOf(Session().AssetDatabase()->FindFile(models / "crate.obj")->meta, "mesh"), "");
}

// A scene saved when how its files were read was the scene's has it moved into the files' metas when it is saved.
TEST_F(EditorTest, AnOldScenesImportSettingsMoveIntoTheMetas) {
    const auto project = scene::MakeProject("old-settings");
    const auto models = project.directory / "assets" / "models";
    std::filesystem::create_directories(models);
    std::ofstream(models / "old.obj") << "v 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\n";
    Open(project.directory);
    ASSERT_TRUE(Session().Save());
    // As an old scene had it: in the graph, saved with the scene, and no meta saying anything.
    kgraph::Graph graph = Doc().Graph();
    graph.meshImports["models/old.obj"] = { { "TANGENT" }, {} };
    std::ifstream in(project.directory / "scenes" / "main.scene.json");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const auto at = text.find("\"graph\": ") + 9;
    const auto end = text.find("\n\"world\"");
    text = text.substr(0, at) + graph.ToJson() + "," + text.substr(end);
    std::ofstream(project.directory / "scenes" / "main.scene.json", std::ios::trunc) << text;

    Open(project.directory);
    EXPECT_EQ(Doc().MeshImportOf("models/old.obj").leaveOut, std::vector<std::string> { "TANGENT" }) << "read as the scene said";
    ASSERT_TRUE(Session().Save());
    EXPECT_EQ(kasset::MeshImportFrom(kasset::ParameterOf(Session().AssetDatabase()->FindFile(models / "old.obj")->meta, "mesh"))->leaveOut,
              std::vector<std::string> { "TANGENT" }) << "and saved into its meta";
}

// An asset is known by its ID: renamed or moved from the Assets panel, every scene that has it still does — the one
// open, and one that is not, saved before files were named by their IDs.
TEST_F(EditorTest, AnAssetMovedIsStillWhatTheScenesHave) {
    const auto project = scene::MakeProject("moving");
    Open(project.directory);
    const auto textures = project.directory / "assets" / "textures";
    std::filesystem::create_directories(textures);
    std::ofstream(textures / "wall.png") << "not looked at";
    const std::string wall = Session().ImportAsset(textures / "wall.png");
    ASSERT_EQ(wall, "@textures/wall.png");
    const kecs::Entity crate = Doc().CreateEntity("Crate");
    Doc().World().Add(crate, Doc().World().Find("PbrMaterial"));
    ASSERT_TRUE(Doc().SetField(crate, "PbrMaterial", "baseColorMap", kgraph::Value(wall)));
    ASSERT_TRUE(Session().Save());
    // On disk it is named by its ID, not where it is.
    {
        std::ifstream in(project.directory / "scenes" / "main.scene.json");
        const std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(saved.find("@textures/wall.png"), std::string::npos);
        EXPECT_NE(saved.find(Session().AssetDatabase()->FindFile(textures / "wall.png")->meta.id.Text()), std::string::npos);
    }
    // Another scene, saved before IDs, that names it by where it is.
    std::filesystem::copy_file(project.directory / "scenes" / "main.scene.json", project.directory / "scenes" / "old.scene.json");
    {
        std::ifstream in(project.directory / "scenes" / "old.scene.json");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const std::string id = Session().AssetDatabase()->FindFile(textures / "wall.png")->meta.id.Text();
        text.replace(text.find(id), id.size(), "@textures/wall.png");
        std::ofstream(project.directory / "scenes" / "old.scene.json", std::ios::trunc) << text;
    }
    EXPECT_EQ(Session().AssetUsers(wall), (std::vector<std::string> { "scenes/main.scene.json", "scenes/old.scene.json" }));

    ASSERT_TRUE(Session().MoveAsset(wall, "assets/brick/red.png")) << Session().Status();
    EXPECT_TRUE(std::filesystem::exists(project.directory / "assets" / "brick" / "red.png"));
    EXPECT_TRUE(std::filesystem::exists(project.directory / ".koral/meta/assets/brick/red.png.meta"));
    EXPECT_FALSE(std::filesystem::exists(project.directory / ".koral/meta/assets/textures/wall.png.meta"));
    EXPECT_EQ(Session().selectedAsset, "@brick/red.png") << "picked where it is now";
    const kecs::Entity again = Doc().World().FindEntity("Crate");
    ASSERT_FALSE(again.IsNull());
    EXPECT_EQ(Doc().Field(again, "PbrMaterial", "baseColorMap"), kgraph::Value("@brick/red.png")) << "the scene open has it where it is now";
    ASSERT_TRUE(Session().OpenScene("old")) << Session().Status();
    const kecs::Entity old = Doc().World().FindEntity("Crate");
    ASSERT_FALSE(old.IsNull());
    EXPECT_EQ(Doc().Field(old, "PbrMaterial", "baseColorMap"), kgraph::Value("@brick/red.png")) << "and so has one that named it by where it was";
    EXPECT_FALSE(Session().MoveAsset("$cube", "assets/cube.obj")) << "the engine's own are of no file";
}

// A model's meshes are known by their names: written again with them in another order — as an export may — each entity
// still has the mesh it had, now at its new place in the file.
TEST_F(EditorTest, AModelsMeshesAreFoundByNameWhenTheFileReordersThem) {
    const auto project = scene::MakeProject("reordered");
    const auto models = project.directory / "assets" / "models";
    std::filesystem::create_directories(models);
    const auto write = [&](const bool swapped) {
        const std::string tall = "o Tall\nv 0 0 0\nv 1 0 0\nv 0 3 0\nf 1 2 3\n";
        const std::string wide = "o Wide\nv 0 0 0\nv 3 0 0\nv 0 1 0\nf 4 5 6\n";
        std::ofstream(models / "pair.obj", std::ios::trunc) << (swapped ? std::string("o Wide\nv 0 0 0\nv 3 0 0\nv 0 1 0\nf 1 2 3\no Tall\nv 0 0 0\nv 1 0 0\nv 0 3 0\nf 4 5 6\n")
                                                                        : tall + wide);
    };
    write(false);
    Open(project.directory);
    const std::string pair = Session().ImportAsset(models / "pair.obj");
    ASSERT_FALSE(pair.empty()) << Session().Status();
    const auto read = [&] {
        for (int i = 0; i < 400 && (Doc().Runtime().Reading() > 0 || !Doc().Runtime().ModelOf(models / "pair.obj")); ++i) Frame();
        for (int i = 0; i < 3; ++i) Frame();
    };
    read();
    const kgraph::Model* model = Doc().Runtime().ModelOf(models / "pair.obj").get();
    ASSERT_TRUE(model);
    ASSERT_EQ(model->meshNames.size(), 2u);
    const int tall = model->meshNames[0] == "Tall" ? 0 : 1;
    const kecs::Entity holder = Doc().CreateEntity("Holder");
    Doc().AddComponent(holder, "Mesh");
    ASSERT_TRUE(Doc().SetField(holder, "Mesh", "mesh", kgraph::Value(std::format("{}#{}", pair, tall))));
    ASSERT_TRUE(Session().Save());

    write(true);
    Open(project.directory);
    read();
    const kecs::Entity again = Doc().World().FindEntity("Holder");
    ASSERT_FALSE(again.IsNull());
    const kgraph::Model* now = Doc().Runtime().ModelOf(models / "pair.obj").get();
    ASSERT_TRUE(now);
    const int moved = now->meshNames[0] == "Tall" ? 0 : 1;
    EXPECT_NE(moved, tall) << "the file has them the other way round";
    const kgraph::Value mesh = Doc().Field(again, "Mesh", "mesh");
    const std::string* held = mesh.String();
    ASSERT_TRUE(held);
    EXPECT_EQ(*held, std::format("{}#{}", pair, moved)) << "and the entity still has Tall";
}

// A model's material extracted is the project's own: a .kmat its entities follow, which the model's file no longer changes.
TEST_F(EditorTest, AModelsMaterialExtractedIsTheProjectsOwn) {
    const auto project = scene::MakeProject("extracted");
    const auto models = project.directory / "assets" / "models" / "crate";
    std::filesystem::create_directories(models);
    std::ofstream(models / "crate.obj") << "mtllib crate.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvn 0 0 1\nusemtl wood\nf 1//1 2//1 3//1\n";
    std::ofstream(models / "crate.mtl") << "newmtl wood\nKd 0.5 0.25 0.125\n";
    Open(project.directory);
    const std::string crate = Session().ImportAsset(models / "crate.obj");
    ASSERT_TRUE(Session().ImportModel(models / "crate.obj")) << Session().Status();
    for (int i = 0; i < 400 && (Doc().Runtime().Reading() > 0 || !Doc().Runtime().ModelOf(models / "crate.obj")); ++i) Frame();
    for (int i = 0; i < 5; ++i) Frame();
    std::string material;
    for (const auto& id : Doc().MaterialAssetIds())
        if (id.starts_with(crate + "#material:") && Doc().MaterialAssetOf(id)->name == "wood") material = id;
    ASSERT_FALSE(material.empty()) << "the model's wood is one of its materials";
    std::size_t following = 0;
    for (const kecs::Entity entity : Doc().World().All()) following += Doc().MaterialAssetOfEntity(entity, Doc().MaterialAssetOf(material)->kind) == material;
    ASSERT_GT(following, 0u) << "the model's things follow its material";

    const std::string own = Session().ExtractMaterial(material);
    ASSERT_FALSE(own.empty()) << Session().Status();
    EXPECT_TRUE(std::filesystem::exists(project.directory / "assets" / "materials" / "wood.kmat"));
    EXPECT_EQ(Session().selectedAsset, own);
    const std::string kind = Doc().MaterialAssetOf(own)->kind;
    std::size_t now = 0;
    for (const kecs::Entity entity : Doc().World().All()) now += Doc().MaterialAssetOfEntity(entity, kind) == own;
    EXPECT_EQ(now, following) << "who followed the model's follows the project's own";
    ASSERT_TRUE(Session().Save());
    Open(project.directory);
    now = 0;
    for (const kecs::Entity entity : Doc().World().All()) now += Doc().MaterialAssetOfEntity(entity, kind) == own;
    EXPECT_EQ(now, following) << "and does when the scene is opened again";
    EXPECT_TRUE(Session().ExtractMaterial(own).empty()) << "what is the project's own is not a model's to extract";
}

// The game exported is what its scenes have and nothing more — its models read ahead into the cache — and opens on its own.
TEST_F(EditorTest, TheGameExportedIsWhatItsScenesHave) {
    const auto project = scene::MakeProject("exported");
    const auto textures = project.directory / "assets" / "textures";
    const auto models = project.directory / "assets" / "models";
    std::filesystem::create_directories(textures);
    std::filesystem::create_directories(models);
    std::ofstream(textures / "wall.png") << "not looked at";
    std::ofstream(textures / "unused.png") << "not looked at";
    std::ofstream(models / "tri.obj") << "v 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\n";
    Open(project.directory);
    const std::string wall = Session().ImportAsset(textures / "wall.png");
    const std::string tri = Session().ImportAsset(models / "tri.obj");
    (void)Session().ImportAsset(textures / "unused.png");
    const kecs::Entity thing = Doc().CreateEntity("Thing");
    Doc().AddComponent(thing, "Mesh");
    ASSERT_TRUE(Doc().SetField(thing, "Mesh", "mesh", kgraph::Value(tri + "#0")));
    Doc().World().Add(thing, Doc().World().Find("PbrMaterial"));
    ASSERT_TRUE(Doc().SetField(thing, "PbrMaterial", "baseColorMap", kgraph::Value(wall)));

    const auto out = std::filesystem::temp_directory_path() / "keditor-exported-game";
    std::filesystem::remove_all(out);
    ASSERT_TRUE(Session().ExportGame(out)) << Session().Status();
    for (const char* file : { "manifest.json", "project.json", "scenes/main.scene.json", "assets/textures/wall.png", ".koral/meta/assets/textures/wall.png.meta", "assets/models/tri.obj" })
        EXPECT_TRUE(std::filesystem::exists(out / file)) << file;
    EXPECT_FALSE(std::filesystem::exists(out / "assets/textures/unused.png")) << "what no scene has is not the game's";
    bool readAhead = false;
    std::error_code none;
    for (const auto& entry : std::filesystem::directory_iterator(out / ".koral/cache/models", none)) readAhead = readAhead || entry.path().extension() == ".kmodel";
    EXPECT_TRUE(readAhead) << "its model read ahead into the cache";
    EXPECT_FALSE(Session().ExportGame(out)) << "not into a folder with something in it";
    EXPECT_FALSE(Session().ExportGame(project.directory / "assets" / "game")) << "not among the project's assets";

    // Opened by itself, it has what it had.
    Open(out);
    const kecs::Entity again = Doc().World().FindEntity("Thing");
    ASSERT_FALSE(again.IsNull());
    const kgraph::Value given = Doc().Field(again, "PbrMaterial", "baseColorMap");
    const std::string* map = given.String();
    ASSERT_TRUE(map);
    EXPECT_EQ(*map, wall);
    std::filesystem::remove_all(out);
}

// A model put in the scene is an instance of the prefab made of it: written again — another mesh in it — every place it
// was put follows, keeping what each made its own.
TEST_F(EditorTest, AModelPutInTheSceneFollowsItsFile) {
    const auto project = scene::MakeProject("followed");
    const auto models = project.directory / "assets" / "models";
    std::filesystem::create_directories(models);
    std::ofstream(models / "kit.obj") << "o Box\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\n";
    Open(project.directory);
    (void)Session().ImportAsset(models / "kit.obj");
    // Everything under an entity, however deep.
    std::function<std::size_t(kecs::Entity)> under = [&](const kecs::Entity entity) {
        std::size_t count = 0;
        for (const kecs::Entity child : Doc().World().ChildrenOf(entity)) count += 1 + under(child);
        return count;
    };
    const auto settle = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 500 && !done(); ++i) { Frame(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        for (int i = 0; i < 3; ++i) Frame();
    };
    const kecs::Entity first = Doc().ImportModel(models / "kit.obj");
    const kecs::Entity second = Doc().ImportModel(models / "kit.obj");
    settle([&] { return under(first) > 0 && under(second) > 0; });
    const std::string prefab = Doc().PrefabOf(first);
    EXPECT_TRUE(prefab.starts_with(".koral/cache/prefabs/")) << "an instance of the model's prefab: " << prefab;
    EXPECT_EQ(Doc().PrefabOf(second), prefab);
    const std::size_t before = under(first);
    // One of them made its own: moved.
    Doc().SetPosition(first, { 5.f, 0.f, 0.f });

    // The file written again, with another mesh in it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::ofstream(models / "kit.obj", std::ios::trunc) << "o Box\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\no Lid\nv 0 2 0\nv 1 2 0\nv 1 3 0\nf 4 5 6\n";
    settle([&] { return under(first) > before && under(second) > before; });
    EXPECT_GT(under(first), before) << "the place it was put has what the file has now";
    EXPECT_GT(under(second), before) << "and so does the other";
    EXPECT_FLOAT_EQ(Doc().World().PositionOf(first).x, 5.f) << "where it is is still its own";
}

// A model's materials imported as a kind that has no field of a name the file reports: each field is told which of the
// file's values it takes, kept in the model's meta, and what follows the material has it at once.
TEST_F(EditorTest, AMaterialsFieldsAreToldWhatOfTheFileEachTakes) {
    const auto project = scene::MakeProject("mapped");
    const auto models = project.directory / "assets" / "models" / "crate";
    std::filesystem::create_directories(models);
    std::ofstream(models / "crate.obj") << "mtllib crate.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvn 0 0 1\nusemtl wood\nf 1//1 2//1 3//1\n";
    std::ofstream(models / "crate.mtl") << "newmtl wood\nKd 1 0 0\n";
    Open(project.directory);
    const std::string crate = Session().ImportAsset(models / "crate.obj");
    // Its classic materials as physical ones: PbrMaterial has no diffuse.
    Session().SetImportKind(kgraph::ModelMaterial::Model::eClassic, "PbrMaterial", models / "crate.obj");
    ASSERT_TRUE(Session().ImportModel(models / "crate.obj")) << Session().Status();
    for (int i = 0; i < 400 && (Doc().Runtime().Reading() > 0 || !Doc().Runtime().ModelOf(models / "crate.obj")); ++i) Frame();
    for (int i = 0; i < 5; ++i) Frame();
    std::string wood;
    for (const auto& id : Doc().MaterialAssetIds())
        if (id.starts_with(crate + "#material:") && Doc().MaterialAssetOf(id)->name == "wood") wood = id;
    ASSERT_FALSE(wood.empty());
    ASSERT_EQ(Doc().MaterialAssetOf(wood)->kind, "PbrMaterial");
    EXPECT_TRUE(std::ranges::contains(Doc().UnusedReported(wood), std::string("diffuse"))) << "the file's colour is taken by nothing";

    std::string error;
    ASSERT_TRUE(Doc().SetMaterialField(models / "crate.obj", "PbrMaterial", "baseColor", "diffuse", &error)) << error;
    EXPECT_EQ(Doc().MaterialFieldsOf(models / "crate.obj", "PbrMaterial"), (std::map<std::string, std::string> { { "baseColor", "diffuse" } }));
    EXPECT_FALSE(std::ranges::contains(Doc().UnusedReported(wood), std::string("diffuse")));
    const keditor::MaterialAsset* asset = Doc().MaterialAssetOf(wood);
    const auto colour = std::ranges::find(asset->values, std::string("baseColor"), &std::pair<std::string, kgraph::Value>::first);
    ASSERT_NE(colour, asset->values.end());
    EXPECT_NEAR(*colour->second.Items()->at(0).Number(), 1.0, 1e-4) << "red, as the file has it";
    EXPECT_NEAR(*colour->second.Items()->at(1).Number(), 0.0, 1e-4);
    std::size_t red = 0;
    for (const kecs::Entity entity : Doc().World().All())
        if (Doc().MaterialAssetOfEntity(entity, "PbrMaterial") == wood) {
            const kgraph::Value has = Doc().Field(entity, "PbrMaterial", "baseColor");
            red += has.Items() && has.Items()->at(1).Number() && std::abs(*has.Items()->at(1).Number()) < 1e-4;
        }
    EXPECT_GT(red, 0u) << "what follows it has it";
    EXPECT_FALSE(kasset::ParameterOf(Session().AssetDatabase()->FindFile(models / "crate.obj")->meta, "materialFields").empty()) << "kept in the model's meta";
}

// A project's script that makes meshes is known for it, whether or not anything runs it: what an entity's Mesh offers.
TEST_F(EditorTest, AScriptThatMakesMeshesIsOfferedForAMesh) {
    const auto project = scene::MakeProject("generators");
    std::filesystem::create_directories(project.directory / "scripts");
    std::ofstream(project.directory / "scripts" / "Pyramid.cs") << "public sealed class Pyramid : Setup, MeshGenerator\n{\n    public float Height = 1;\n"
                                                                    "    public void Initialize(MeshWriter mesh) { }\n}\n";
    std::ofstream(project.directory / "scripts" / "Spin.cs") << "public sealed class Spin : Setup, PerFrame\n{\n    public float Speed;\n    public void Update(float delta) { }\n}\n";
    Open(project.directory);
    EXPECT_EQ(Doc().ScriptsTaking("MeshGenerator"), std::vector<std::string> { "Pyramid" });
    EXPECT_EQ(Doc().ScriptsTaking("PerFrame"), std::vector<std::string> { "Spin" });
}

// A mesh's small picture is kept in the project's cache once it is drawn — by what the mesh is made of — and a material's
// is a sphere of its colour.
TEST_F(EditorTest, PicturesOfMeshesAreKeptAndMaterialsAreSpheres) {
    const auto project = scene::MakeProject("pictured");
    Open(project.directory);
    (void)Session().Assets();       // what the panel lists, and asks to be drawn
    for (int i = 0; i < 600 && !Session().MeshPicture("$cube").Get(); ++i) Frame();
    ASSERT_TRUE(Session().MeshPicture("$cube").Get()) << "the cube is drawn";
    const auto kept = project.directory / ".koral" / "cache" / "thumbnails";
    std::size_t files = 0;
    std::error_code none;
    for (const auto& entry : std::filesystem::directory_iterator(kept, none)) files += entry.path().extension() == ".kthumb";
    EXPECT_GT(files, 0u) << "kept in the project's cache";
    for (int i = 0; i < 600 && !Session().MaterialPicture("material:Gold").Get(); ++i) Frame();
    const auto gold = Session().MaterialPicture("material:Gold");
    ASSERT_TRUE(gold.Get()) << "a material is a sphere of it";
    EXPECT_NE(gold.Get(), Session().MaterialPicture("material:Red plastic").Get()) << "each its own colour";

    // What is drawn is the material: read back from where it is kept, four bytes a texel, 96 by 96.
    const auto middleOf = [&](std::set<std::filesystem::path>& seen) -> std::optional<std::array<int, 4>> {
        for (const auto& entry : std::filesystem::directory_iterator(kept, none)) {
            if (entry.path().extension() != ".kthumb" || !seen.insert(entry.path()).second) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<unsigned char> texels((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (texels.size() != 96 * 96 * 4) continue;
            EXPECT_EQ(texels[3], 0) << "the corner is nothing: only the sphere is drawn";
            const std::size_t at = (48 * 96 + 48) * 4;
            return std::array<int, 4> { texels[at], texels[at + 1], texels[at + 2], texels[at + 3] };
        }
        return std::nullopt;
    };
    std::set<std::filesystem::path> seen;
    for (const auto& entry : std::filesystem::directory_iterator(kept, none)) seen.insert(entry.path());
    const auto settle = [&](const std::string& id) {
        for (int i = 0; i < 600 && !Session().MaterialPicture(id).Get(); ++i) Frame();
        for (int i = 0; i < 3; ++i) Frame();
        return middleOf(seen);
    };
    const auto red = settle("material:Red plastic");
    if (red) { EXPECT_GT((*red)[0], (*red)[1] + 40) << "red, in its colour"; EXPECT_EQ((*red)[3], 255); }
    else { EXPECT_GT(files, 0u); }

    // A material of a picture: what is drawn is the picture, not its colour.
    const auto assets = project.directory / "assets";
    std::filesystem::create_directories(assets);
    {
        std::ofstream file(assets / "blue.ppm", std::ios::binary);
        file << "P6\n8 8\n255\n";
        for (int i = 0; i < 64; ++i) file.put(static_cast<char>(10)).put(static_cast<char>(30)).put(static_cast<char>(230));
    }
    std::ofstream(assets / "Blue.kmat") << "{\n  \"koral-material\": 1,\n  \"kind\": \"PbrMaterial\",\n  \"values\": { \"baseColor\": [1, 1, 1, 1], \"baseColorMap\": \"@blue.ppm\", \"roughness\": 0.9 }\n}\n";
    const auto blue = settle("@Blue.kmat");
    ASSERT_TRUE(blue) << "drawn, and kept";
    EXPECT_GT((*blue)[2], (*blue)[0] + 40) << "blue, in its picture";
}

// What a material looks like is drawn by its kind's own shading, in the light of a room, whatever the kind: the engine's
// two, and one of the project's with a function of its own. What has no shading says so, and can be given one.
TEST_F(EditorTest, MaterialsAreDrawnByTheirKindsShading) {
    const auto project = scene::MakeProject("shaded");
    Open(project.directory);
    const auto drawn = [&](const std::string& id) {
        (void)Session().MaterialPicture(id);
        for (int i = 0; i < 900 && !Session().MaterialPicture(id).Get(); ++i) Frame();
        return Session().MaterialPicture(id);
    };
    ASSERT_TRUE(drawn("material:Gold").Get()) << "PBR";
    ASSERT_TRUE(drawn("material:Blinn-Phong Ruby").Get()) << "Blinn-Phong";
    EXPECT_NE(drawn("material:Gold").Get(), drawn("material:Blinn-Phong Ruby").Get());

    // A kind of the project's, and its shading.
    kgraph::MaterialKind toon = *Doc().Runtime().MaterialKindOf("PbrMaterial");
    toon.name = "Toon";
    std::string error;
    ASSERT_NE(Doc().Runtime().DeclareMaterialKind(toon, &error), kecs::NoComponent) << error;
    Doc().AddMaterialAsset({ .id = "@Green.kmat", .name = "Green", .kind = "Toon", .values = { { "baseColor", kgraph::Value(kgraph::Value::List { 0.f, 1.f, 0.f, 1.f }) } } });
    EXPECT_FALSE(Session().HasShading("Toon"));
    ASSERT_TRUE(Session().WriteShadingModule("Toon")) << Session().Status();
    EXPECT_TRUE(Session().HasShading("Toon"));
    EXPECT_FALSE(Session().WriteShadingModule("Toon")) << "what is there is left";
    {
        std::ifstream in(Session().ShadingModuleOf("Toon"));
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string from = "lit.colour = float3(0.8);";
        text.replace(text.find(from), from.size(), "lit.colour = KoralLinearFromGamma22(baseColor.rgb);");
        std::ofstream(Session().ShadingModuleOf("Toon"), std::ios::trunc) << text;
    }
    const auto toonPicture = drawn("@Green.kmat");
    ASSERT_TRUE(toonPicture.Get()) << "a kind of the project's is drawn by its own function";

    // Picked, it runs nothing: it is the viewer that draws one, and is opened for it.
    Session().SelectAsset("material:Gold");
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(Session().ViewedMaterial(), nullptr);
    ASSERT_TRUE(Session().View("material:Gold"));
    EXPECT_EQ(Session().Viewed(), keditor::Session::ViewKind::eMaterial);
    for (int i = 0; i < 60 && !(Session().ViewedMaterial() && Session().ViewedMaterial()->Settled()); ++i) Frame();
    ASSERT_NE(Session().ViewedMaterial(), nullptr);
    EXPECT_EQ(Session().ViewedMaterial()->Problem(), "");
    EXPECT_TRUE(Session().ViewedMaterial()->Picture().Get());
    Sheet("material-gold");
    Session().SetPreviewMesh("$torus");
    Session().ViewedMaterial()->Turn(1.f, 0.3f);
    for (int i = 0; i < 20; ++i) Frame();
    EXPECT_EQ(Session().ViewedMaterial()->Problem(), "");
    Sheet("material-torus");
    Session().SetPreviewMesh("$sphere");

    // Tried out, it is drawn so, and the material is as it was: in the document, and in what an entity would be given.
    const auto number = [&](const char* field) { const kgraph::Value held = Session().ViewedMaterial()->FieldOf(field); return held.Number() ? *held.Number() : -1.0; };
    EXPECT_NEAR(number("roughness"), 0.25, 1e-5);
    Session().TryMaterialField("roughness", kgraph::Value(0.9));
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_NEAR(number("roughness"), 0.9, 1e-5) << "drawn with what is tried out";
    EXPECT_TRUE(Session().MaterialTried());
    const auto values = keditor::Session::ValuesOf(*keditor::Session::MaterialOf("material:Gold"));
    EXPECT_FLOAT_EQ(static_cast<float>(*std::ranges::find(values, std::string("roughness"), &std::pair<std::string, kgraph::Value>::first)->second.Number()), 0.25f);
    Sheet("material-tried");
    Session().ForgetMaterialTried();
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_NEAR(number("roughness"), 0.25, 1e-5) << "back to the material";
    EXPECT_FALSE(Session().MaterialTried());

    // Dragged with the mouse, a number goes down, and up no further than its limit: metallic, in the viewer's list.
    {
        auto& input = editor->SceneInput();
        const auto drag = [&](const float by) {
            input.FeedMousePosition({ 1010.f, 587.f });
            Frame();
            Frame();
            input.FeedMouseButton(kor::MouseButton::eLeft, true);
            Frame();
            for (int i = 1; i <= 10; ++i) { input.FeedMousePosition({ 1010.f + by * static_cast<float>(i) / 10.f, 587.f }); Frame(); }
            input.FeedMouseButton(kor::MouseButton::eLeft, false);
            for (int i = 0; i < 4; ++i) Frame();
        };
        drag(-60.f);
        EXPECT_TRUE(Session().MaterialTried()) << "a drag tries it out";
        EXPECT_LT(number("metallic"), 1.0);
        EXPECT_GE(number("metallic"), 0.0);
        drag(400.f);
        EXPECT_NEAR(number("metallic"), 1.0, 1e-5) << "and no further than it goes";
        Session().ForgetMaterialTried();
        for (int i = 0; i < 4; ++i) Frame();
    }

    // Its pictures are tried out as its numbers are, and a light keeps being more than a colour can say.
    Session().TryMaterialField("baseColorMap", kgraph::Value("$black"));
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(*Session().ViewedMaterial()->FieldOf("baseColorMap").String(), "$black");
    Sheet("material-textures");
    Session().ForgetMaterialTried();
    ASSERT_TRUE(Session().View("material:Glowing white"));
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_NEAR(*(*Session().ViewedMaterial()->FieldOf("emission").Items())[0].Number(), 4.0, 1e-5);
    ASSERT_TRUE(Session().View("material:Blinn-Phong Gold"));
    for (int i = 0; i < 6; ++i) Frame();
    const auto fields = Doc().Runtime().MaterialKindOf("BlinnPhongMaterial")->fields;
    EXPECT_EQ(std::ranges::find(fields, std::string("specular"), &kecs::FieldDescription::name)->max, 1.f) << "a classic specular colour is at most 1";

    // A project's material, in its own kind: what its asset says stays, and what is tried goes with the viewer.
    ASSERT_TRUE(Session().View("@Green.kmat"));
    for (int i = 0; i < 20; ++i) Frame();
    EXPECT_EQ(Session().ViewedMaterial()->Problem(), "") << "a project's kind in the viewer too";
    Session().TryMaterialField("metallic", kgraph::Value(1.0));
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(std::ranges::contains(Doc().MaterialAssetOf("@Green.kmat")->values, std::string("metallic"), &std::pair<std::string, kgraph::Value>::first)) << "the asset is as it was";
    Session().StopViewing();
    EXPECT_FALSE(Session().MaterialTried());
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(Session().ViewedMaterial(), nullptr);

    Session().assetKind = static_cast<int>(keditor::Session::Asset::Kind::eMaterial);
    Session().ViewChanged();
    for (int i = 0; i < 240; ++i) Frame();
    EXPECT_TRUE(Shows("PBR  ·  23"));
    EXPECT_TRUE(Shows("Blinn-Phong  ·  22"));
    Sheet("materials-tab");
    Session().assetKind = -1;
}

// The view is looked round smoothly: what the mouse did is spread over a few frames, and none of it is lost.
TEST_F(EditorTest, LookingRoundIsSmoothAndLosesNothing) {
    const auto project = scene::MakeProject("looking");
    Open(project.directory);
    for (int i = 0; i < 6; ++i) Frame();
    auto& input = editor->SceneInput();
    auto& camera = Session().camera;
    const kor::Vec2 over = Session().viewport->Size().x > 0 ? kor::Vec2(1000.f, 300.f) : kor::Vec2(1000.f, 300.f);
    input.FeedMousePosition(over);
    Frame();
    Frame();
    input.FeedMouseButton(kor::MouseButton::eRight, true);
    Frame();
    Frame();
    ASSERT_TRUE(Session().Aiming()) << "the right button on the scene";
    EXPECT_EQ(input.CurrentCursorMode(), kor::Input::CursorMode::eNormal) << "or it is captured the frame after: Update comes first";
    Frame();
    EXPECT_EQ(input.CurrentCursorMode(), kor::Input::CursorMode::eCaptured) << "the mouse's own movement, while looking";
    Frame();
    Frame();
    const float before = camera.yaw;
    input.FeedMousePosition(over + kor::Vec2(40.f, 0.f));   // a jump of 40, in one frame
    Frame();
    const float first = before - camera.yaw;
    EXPECT_GT(first, 0.f) << "it turns at once";
    EXPECT_LT(first, 40.f * 0.15f * 0.99f) << "but not all of it in one frame";
    for (int i = 0; i < 60; ++i) Frame();
    EXPECT_NEAR(before - camera.yaw, 40.f * 0.15f, 0.05f) << "and all of it, in the end";
    input.FeedMouseButton(kor::MouseButton::eRight, false);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(Session().Aiming());
    EXPECT_EQ(input.CurrentCursorMode(), kor::Input::CursorMode::eNormal) << "let go, the pointer is back";
}

// A pipeline's shaders are slots of their stage: what is offered, and what can be dropped, is of that stage alone.
TEST_F(EditorTest, APipelinesShadersAreSlotsOfTheirStage) {
    const auto project = scene::MakeProject("slots");
    std::ofstream(project.ShaderDirectory() / "many.slang") << R"(struct V { float4 p : SV_Position; };
[shader("vertex")] V plainVertex(float3 p : POSITION) { V v; v.p = float4(p, 1.0); return v; }
[shader("vertex")] V otherVertex(float3 p : POSITION) { V v; v.p = float4(p, 1.0); return v; }
[shader("fragment")] float4 plainFragment(V v) : SV_Target { return 1.0; }
[shader("geometry")] [maxvertexcount(3)] void plainGeometry(triangle V c[3], inout TriangleStream<V> o) { o.Append(c[0]); }
)";
    std::ofstream(project.ShaderDirectory() / "traced.slang") << R"(struct P { float3 c; };
[shader("raygeneration")] void makeRays() {}
[shader("miss")] void firstMiss(inout P p) {}
[shader("miss")] void secondMiss(inout P p) {}
[shader("closesthit")] void surface(inout P p, BuiltInTriangleIntersectionAttributes a) {}
[shader("anyhit")] void cutOut(inout P p, BuiltInTriangleIntersectionAttributes a) {}
)";
    Open(project.directory);
    const std::string pipeline = Doc().AddNode("GraphicsPipeline", 0, 0, "drawn");
    ASSERT_FALSE(pipeline.empty());
    Session().Select(pipeline);
    for (int i = 0; i < 4; ++i) Frame();

    // What each stage offers, and only that.
    const auto of = [&](const char* stage, const char* module) { return std::ranges::count(Session().ShadersOf(stage), std::string(module), &keditor::Session::ShaderPick::module); };
    EXPECT_EQ(of("vertex", "many"), 2);
    EXPECT_EQ(of("fragment", "many"), 1);
    EXPECT_EQ(of("geometry", "many"), 1);
    EXPECT_EQ(of("miss", "traced"), 2);
    EXPECT_EQ(of("vertex", "traced"), 0);
    EXPECT_TRUE(keditor::Session::IsStage("pixel", "fragment"));
    EXPECT_TRUE(keditor::Session::IsStage("amplification", "task"));

    // Dropped: of the stage it goes in; another's does not, and what was there stays.
    EXPECT_TRUE(Session().DropShader(pipeline, "vertex", "vertex", "shader:many#otherVertex"));
    const auto text = [&](const char* input) { const auto* value = Doc().Graph().Find(pipeline)->inputs.at(input).value.String(); return value ? *value : std::string(); };
    EXPECT_EQ(text("vertex.module"), "many");
    EXPECT_EQ(text("vertex.entry"), "otherVertex");
    EXPECT_FALSE(Session().DropShader(pipeline, "vertex", "vertex", "shader:many#plainFragment"));
    EXPECT_EQ(text("vertex.entry"), "otherVertex") << "as it was";
    EXPECT_TRUE(Session().DropShader(pipeline, "fragment", "fragment", "shader:many")) << "a module, by its entry of that stage";
    EXPECT_EQ(text("fragment.entry"), "plainFragment");
    EXPECT_FALSE(Session().DropShader(pipeline, "fragment", "fragment", "@wall.png"));
    EXPECT_FALSE(Session().DropShader(pipeline, "fragment", "fragment", "shader:traced")) << "a module with none of that stage";

    // VTG: tessellation and geometry hidden until asked for; fragment can go.
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(Shows("otherVertex"));
    EXPECT_TRUE(Shows("plainFragment"));
    EXPECT_TRUE(Shows("Add tessellation"));
    EXPECT_TRUE(Shows("Add a geometry shader"));
    EXPECT_FALSE(Shows("No geometry shader"));
    Sheet("slots-graphics");
    Session().ClearShader(pipeline, "fragment");
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_FALSE(Doc().Graph().Find(pipeline)->inputs.contains("fragment.module"));
    EXPECT_TRUE(Shows("Depth only"));
    Session().Show(pipeline, "geometry");
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(Shows("No geometry shader"));
    EXPECT_FALSE(Shows("Add a geometry shader"));

    // Ray tracing: the hit groups a list, an item taken out moving the rest up.
    const std::string traced = Doc().AddNode("RayTracingPipeline", 0, 300, "traced pipeline");
    Session().Select(traced);
    Session().SetShader(traced, "raygen", "traced", "makeRays");
    Session().SetShader(traced, "hitGroup[0].closestHit", "traced", "surface");
    Session().SetShader(traced, "hitGroup[1].closestHit", "traced", "surface");
    Session().SetShader(traced, "hitGroup[1].anyHit", "traced", "cutOut");
    Session().SetShader(traced, "miss[0]", "traced", "firstMiss");
    EXPECT_EQ(Session().ShaderListSize(traced, "hitGroup"), 2);
    Session().AddToShaderList(traced, "hitGroup");
    EXPECT_EQ(Session().ShaderListSize(traced, "hitGroup"), 3);
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(Shows("Group 2"));
    Sheet("slots-rays");
    Session().RemoveFromShaderList(traced, "hitGroup", 0);
    EXPECT_EQ(Session().ShaderListSize(traced, "hitGroup"), 2);
    const kgraph::Node& after = *Doc().Graph().Find(traced);
    ASSERT_TRUE(after.inputs.contains("hitGroup[0].anyHit.entry")) << "the second is the first now";
    EXPECT_EQ(*after.inputs.at("hitGroup[0].anyHit.entry").value.String(), "cutOut");
    EXPECT_FALSE(after.inputs.contains("hitGroup[1].closestHit.module"));
}

// A generator saved — its fields as they are — is an asset: dropped on another entity, that one makes the same.
TEST_F(EditorTest, AGeneratorSavedIsGivenToOthers) {
    const auto project = scene::MakeProject("kgen");
    std::filesystem::create_directories(project.directory / "scripts");
    std::ofstream(project.directory / "scripts" / "Pyramid.cs") << "public sealed class Pyramid : Setup, MeshGenerator\n{\n    public float Height = 1;\n"
                                                                    "    public void Initialize(MeshWriter mesh) { }\n}\n";
    Open(project.directory);
    const kecs::Entity tall = Doc().CreateEntity("Tall");
    ASSERT_TRUE(Doc().AddComponent(tall, "Pyramid"));
    ASSERT_TRUE(Doc().SetField(tall, "Pyramid", "Height", kgraph::Value(3.0)));
    const std::string id = Session().SaveGenerator(tall);
    ASSERT_FALSE(id.empty()) << Session().Status();
    EXPECT_TRUE(std::filesystem::exists(project.directory / "assets" / "generators" / "Pyramid.kgen"));
    const auto assets = Session().Assets();
    const auto listed = std::ranges::find(assets, id, &keditor::Session::Asset::id);
    ASSERT_NE(listed, assets.end()) << "listed among the assets";
    EXPECT_EQ(listed->kind, keditor::Session::Asset::Kind::eGenerator);
    EXPECT_TRUE(Session().AssetDatabase()->FindFile(project.directory / "assets" / "generators" / "Pyramid.kgen")) << "an asset, by its ID";

    const kecs::Entity other = Doc().CreateEntity("Other");
    ASSERT_TRUE(Session().GiveAsset(id, other)) << Session().Status();
    const kgraph::Value height = Doc().Field(other, "Pyramid", "Height");
    ASSERT_TRUE(height.Number());
    EXPECT_FLOAT_EQ(static_cast<float>(*height.Number()), 3.f) << "with the fields it was saved with";

    // It follows the file: changed there it changes, but for what an entity made its own — here and in the scene opened again.
    const auto file = project.directory / "assets" / "generators" / "Pyramid.kgen";
    const kecs::Entity mine = Doc().CreateEntity("Mine");
    ASSERT_TRUE(Session().GiveAsset(id, mine));
    ASSERT_TRUE(Doc().SetField(mine, "Pyramid", "Height", kgraph::Value(9.0)));
    std::string error;
    ASSERT_TRUE(Doc().SetGeneratorField(file, "Height", kgraph::Value(5.0), &error)) << error;
    const auto heightOf = [&](const char* name) {
        const kgraph::Value held = Doc().Field(Doc().World().FindEntity(name), "Pyramid", "Height");
        return held.Number() ? static_cast<float>(*held.Number()) : 0.f;
    };
    EXPECT_FLOAT_EQ(heightOf("Other"), 5.f) << "what follows it has it";
    EXPECT_FLOAT_EQ(heightOf("Tall"), 5.f) << "and so has the one it was saved from";
    EXPECT_FLOAT_EQ(heightOf("Mine"), 9.f) << "what made it its own keeps it";
    ASSERT_TRUE(Session().Save());
    ASSERT_TRUE(Doc().SetGeneratorField(file, "Height", kgraph::Value(7.0), &error)) << error;
    Open(project.directory);
    EXPECT_FLOAT_EQ(heightOf("Other"), 7.f) << "the file changed since the scene was saved: opened, it has what the file says";
    EXPECT_FLOAT_EQ(heightOf("Mine"), 9.f);
    EXPECT_TRUE(Session().SaveGenerator(Doc().CreateEntity("Plain")).empty()) << "one with no generator saves none";
}

// The Assets panel has a model as what it is: gone into, it shows what it is made of, by kind; and everything of the
// project is found under Resources and Documents, each with its picture on its tile.
TEST_F(EditorTest, TheAssetsPanelShowsWhatThingsAreMadeOf) {
    const auto project = scene::MakeProject("panel");
    const auto models = project.directory / "assets" / "models" / "kit";
    std::filesystem::create_directories(models);
    std::ofstream(models / "kit.obj") << "mtllib kit.mtl\no Box\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvn 0 0 1\nusemtl wood\nf 1//1 2//1 3//1\n"
                                         "o Lid\nv 0 2 0\nv 1 2 0\nv 1 3 0\nvn 0 0 1\nusemtl metal\nf 4//2 5//2 6//2\n";
    std::ofstream(models / "kit.mtl") << "newmtl wood\nKd 0.7 0.4 0.1\nmap_Kd stripes.bmp\nnewmtl metal\nKd 0.2 0.4 0.9\n";
    {
        // A picture of stripes: sixteen by sixteen, three bytes a texel, as a BMP lays them out.
        std::ofstream file(models / "stripes.bmp", std::ios::binary);
        const auto word = [&](const std::uint32_t value, const int bytes) { for (int i = 0; i < bytes; ++i) file.put(static_cast<char>((value >> (8 * i)) & 0xff)); };
        file << "BM";
        word(54 + 16 * 16 * 3, 4); word(0, 4); word(54, 4); word(40, 4); word(16, 4); word(16, 4); word(1, 2); word(24, 2); word(0, 4); word(16 * 16 * 3, 4); word(2835, 4); word(2835, 4); word(0, 4); word(0, 4);
        for (int i = 0; i < 16 * 16; ++i) { const char c = (i / 2) % 2 ? char(230) : char(20); file.put(c).put(char(120)).put(char(255 - c)); }
    }
    Open(project.directory);
    Session().assetTileSize = 64.f;     // what fits the panel at this window's size
    const auto assets = Session().Assets();
    const auto kit = std::ranges::find(assets, std::string("@models/kit/kit.obj"), &keditor::Session::Asset::id);
    ASSERT_NE(kit, assets.end());
    Session().assetFolder = "assets/models/kit";
    for (int i = 0; i < 600 && !Session().ModelPicture(kit->id).Get(); ++i) Frame();
    EXPECT_TRUE(Session().ModelPicture(kit->id).Get()) << "a model that is in no scene has its picture";
    for (int i = 0; i < 60; ++i) Frame();
    EXPECT_TRUE(Shows("Resources"));
    EXPECT_TRUE(Shows("Documents"));
    Sheet("assets-folder");

    Session().OpenAsset(*kit);
    EXPECT_EQ(Session().assetModel, kit->id);
    const auto partsOf = [&](const keditor::Session::Asset::Kind kind) {
        const auto now = Session().Assets();
        return std::ranges::count_if(now, [&](const keditor::Session::Asset& asset) { return asset.kind == kind && asset.parent == kit->id; });
    };
    for (int i = 0; i < 400 && partsOf(keditor::Session::Asset::Kind::eMaterial) < 2; ++i) Frame();
    EXPECT_TRUE(Shows("Meshes  ·  2"));
    EXPECT_GE(partsOf(keditor::Session::Asset::Kind::eMaterial), 2) << "what it is made of, whether or not the scene has it (the importer may add one of its own)";
    EXPECT_TRUE(Shows("Textures  ·  1"));
    for (int i = 0; i < 60; ++i) Frame();
    Sheet("assets-model");
    Session().assetKind = static_cast<int>(keditor::Session::Asset::Kind::eMaterial);
    for (int i = 0; i < 200; ++i) Frame();
    Sheet("assets-materials");
    Session().assetKind = -1;

    // And each picture kept has something in it: a mesh, a material, a texture, the model.
    std::size_t kept = 0;
    std::error_code none;
    for (const auto& entry : std::filesystem::directory_iterator(project.directory / ".koral" / "cache" / "thumbnails", none)) {
        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<unsigned char> texels((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t shown = 0;
        for (std::size_t at = 3; at < texels.size(); at += 4) shown += texels[at] != 0;
        EXPECT_GT(shown, 100u) << entry.path().filename() << " is blank";
        ++kept;
    }
    EXPECT_GT(kept, 4u);
}

// A prefab is made by dragging an entity from the outliner into the Assets panel — or with the inspector's button, the
// Assets panel's Create, the Edit menu, the outliner's menu. It is then among the assets, picked, and the entity its instance.
TEST_F(EditorTest, AnEntityDraggedIntoTheAssetsIsAPrefab) {
    const auto project = scene::MakeProject("prefabs");
    Open(project.directory);
    const kecs::Entity crate = Doc().CreateEntity("Crate");
    Session().Select(crate);
    EXPECT_TRUE(Shows("Save as prefab")) << "the inspector offers it";
    Sheet("prefab before");

    // Picked up in the outliner, let go over the Assets panel.
    auto& input = editor->SceneInput();
    input.FeedMousePosition({ 120.f, 131.f });
    Frame();
    input.FeedMouseButton(kor::MouseButton::eLeft, true);
    Frame();
    for (int step = 1; step <= 12; ++step) {
        input.FeedMousePosition({ 120.f + 30.f * static_cast<float>(step), 131.f + 52.f * static_cast<float>(step) });
        Frame();
    }
    input.FeedMouseButton(kor::MouseButton::eLeft, false);
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(std::filesystem::exists(project.directory / "prefabs" / "Crate.prefab")) << Session().Status();
    EXPECT_EQ(Doc().PrefabOf(crate), "prefabs/Crate.prefab") << "the entity is its first instance";
    EXPECT_EQ(Session().assetFolder, "prefabs") << "the Assets panel shows it";
    EXPECT_EQ(Session().selectedAsset, "prefab:Crate.prefab") << "picked";
    Session().Select(crate);
    EXPECT_TRUE(Shows("An instance of Crate"));
    EXPECT_FALSE(Shows("Save as prefab")) << "an instance is offered Apply, not another prefab";
    Sheet("prefab after");
}

// A prefab is made in the Assets panel and opened by itself, as Unity's prefab mode does: the scene is put aside, the
// prefab's entities are all the outliner and the scene view have, and back to the scene every instance follows it.
TEST_F(EditorTest, APrefabIsMadeAndEditedByItself) {
    const auto project = scene::MakeProject("prefab mode");
    Open(project.directory);
    const kecs::Entity tree = Doc().CreateEntity("Tree");
    const kecs::Entity crate = Doc().CreateEntity("Crate");
    ASSERT_TRUE(Session().SaveAsPrefab(crate)) << Session().Status();
    for (int i = 0; i < 30; ++i) Frame();       // (what can be taken back, counted)
    const bool couldUndo = Session().CanUndo();

    // Right-clicked, the Assets panel offers a new prefab.
    RightClick({ 500.f, 780.f });
    EXPECT_TRUE(Shows("Create prefab...")) << "in the Assets panel's menu";
    Click({ 1400.f, 120.f });       // (the menu shut)

    // Made, it is open by itself: one entity of its name, and nothing of the scene's.
    ASSERT_TRUE(Session().NewPrefab("Lamp")) << Session().Status();
    EXPECT_EQ(Session().PrefabOpen(), "prefabs/Lamp.prefab");
    EXPECT_TRUE(std::filesystem::exists(project.directory / "prefabs" / "Lamp.prefab"));
    ASSERT_EQ(Doc().World().Roots().size(), 1u);
    const kecs::Entity lamp = Doc().World().Roots().front();
    EXPECT_EQ(Doc().NameOf(lamp), "Lamp");
    EXPECT_TRUE(Doc().PrefabRootOf(lamp).IsNull()) << "an entity like any, not an instance";
    EXPECT_TRUE(Shows("Back to the scene"));
    EXPECT_FALSE(Shows("Tree")) << "the scene's entities are put aside";
    EXPECT_FALSE(Session().CanUndo()) << "what is taken back here is the prefab's";
    Session().StartPlaying();
    EXPECT_FALSE(Session().InPlayMode()) << "a prefab is not played";

    // What is made in it is the prefab's, once saved — Ctrl+S saves the prefab.
    Doc().CreateEntity("Bulb", lamp);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_TRUE(Session().PrefabChanged());
    EXPECT_TRUE(Shows("Lamp  ·  changed")) << "the bar over the outliner says so";
    Sheet("prefab mode");
    ASSERT_TRUE(Session().Save()) << Session().Status();
    EXPECT_FALSE(Session().PrefabChanged());
    ASSERT_TRUE(Session().ClosePrefab()) << Session().Status();

    // Back: the scene as it was, and what could be taken back of it.
    EXPECT_TRUE(Session().PrefabOpen().empty());
    EXPECT_TRUE(Doc().World().Alive(Doc().World().FindEntity("Tree")));
    EXPECT_FALSE(Doc().World().Alive(Doc().World().FindEntity("Lamp"))) << "the prefab is no part of the scene until it is placed";
    EXPECT_EQ(Session().CanUndo(), couldUndo);
    const kecs::Entity placed = Session().PlacePrefab("prefab:Lamp.prefab");
    ASSERT_FALSE(placed.IsNull()) << Session().Status();
    ASSERT_EQ(Doc().World().ChildrenOf(placed).size(), 1u);
    EXPECT_EQ(Doc().NameOf(Doc().World().ChildrenOf(placed).front()), "Bulb") << "placed, it is the prefab as it was saved";

    // An instance in the scene follows its prefab, edited by itself.
    ASSERT_TRUE(Session().OpenPrefab("prefab:Crate.prefab")) << Session().Status();
    Doc().CreateEntity("Lid", Doc().World().Roots().front());
    ASSERT_TRUE(Session().ClosePrefab(true)) << Session().Status();     // saved, as it changed
    const kecs::Entity instance = Doc().World().FindEntity("Crate");
    ASSERT_TRUE(Doc().World().Alive(instance));
    ASSERT_EQ(Doc().World().ChildrenOf(instance).size(), 1u) << "the scene's crate has the lid";
    EXPECT_EQ(Doc().NameOf(Doc().World().ChildrenOf(instance).front()), "Lid");
    EXPECT_TRUE(Doc().Dirty()) << "the scene changed, where its instance did";
    (void)tree;
}

// The scene tree's menus: an entity's, short — what is done to it, then submenus beside it for what is made under it and
// for its prefab — and the tree's own, where no entity is, for what is made at the top.
TEST_F(EditorTest, TheSceneTreesMenusHaveSubmenusBesideThem) {
    const auto project = scene::MakeProject("scene tree");
    Open(project.directory);
    const kecs::Entity parent = Doc().CreateEntity("Parent");
    Doc().CreateEntity("Child", parent);
    EXPECT_EQ(Session().EntityKind(parent), "group");

    // An entity's: no line for each kind of object, but one that opens them beside it.
    RightClick({ 120.f, 131.f });
    EXPECT_TRUE(Shows("Create under it"));
    EXPECT_TRUE(Shows("Prefab"));
    EXPECT_TRUE(Shows("Delete, with what is under it"));
    EXPECT_FALSE(Shows("New entity under it"));
    EXPECT_FALSE(Shows("Empty entity")) << "until it is come onto";
    auto& input = editor->SceneInput();
    // Rename, Duplicate, Delete, a line, then Create under it: 6 + 3 × 36 + 9 + 18 below where it opened.
    input.FeedMousePosition({ 170.f, 131.f + 141.f });
    EXPECT_TRUE(Shows("Empty entity")) << "beside it, as the pointer comes onto it";
    Sheet("scene tree menu");
    // Picked in it: an entity under the one whose menu it is.
    const std::size_t before = Doc().World().ChildrenOf(parent).size();
    input.FeedMousePosition({ 410.f, 131.f + 141.f });     // the submenu's first line, beside the line that opened it
    for (int i = 0; i < 4; ++i) Frame();
    input.FeedMouseButton(kor::MouseButton::eLeft, true);
    Frame();
    input.FeedMouseButton(kor::MouseButton::eLeft, false);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_EQ(Doc().World().ChildrenOf(parent).size(), before + 1) << "an empty entity, made under it";

    // Where no entity is: what is made at the top.
    RightClick({ 140.f, 280.f });
    EXPECT_TRUE(Shows("Empty entity"));
    EXPECT_TRUE(Shows("Mesh"));
}

// A model's meshes are read with every attribute they have. Its inspector lists them, each read or left out and in
// which buffer; a change is an edit of the scene — read again at once, taken back with Undo.
TEST_F(EditorTest, AModelsAttributesAreChosenInItsInspector) {
    scene::MakeProject("attributes");
    Open();
    const auto file = std::filesystem::temp_directory_path() / "keditor-attributes.obj";
    std::ofstream(file) << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";
    ASSERT_TRUE(Session().ImportModel(file));
    auto& runtime = Doc().Runtime();
    const std::string name = runtime.AssetId(file).substr(1), mesh = runtime.AssetId(file, 0);
    const auto read = [&]() -> std::optional<kgraph::VertexData> {
        for (int i = 0; i < 300; ++i) {
            if (const auto held = runtime.ResourceOf(mesh); held && !held->pending)
                if (auto data = runtime.ReadMesh(mesh)) return data;
            Frame();
        }
        return std::nullopt;
    };
    auto data = read();
    ASSERT_TRUE(data);
    EXPECT_NE(data->Find("UV"), nullptr) << "read whole, by default";

    Session().SelectAsset("@" + name);
    EXPECT_TRUE(Shows("How its meshes are read"));
    EXPECT_TRUE(Shows("NORMAL"));
    EXPECT_TRUE(Shows("Each vertex holds 56 bytes, in 1 buffer.")) << "POSITION, NORMAL, TANGENT, BITANGENT and UV: what the importer gives an OBJ";

    // UV left out, POSITION apart: an edit of the scene's, and the file read again so.
    const auto before = runtime.ResourceOf(mesh);
    Doc().SetMeshImport(name, { .leaveOut = { "UV" }, .buffers = { { "POSITION" } } });
    for (int i = 0; i < 300 && !before->replacedBy; ++i) Frame();
    data = read();
    ASSERT_TRUE(data);
    EXPECT_EQ(data->Find("UV"), nullptr);
    EXPECT_EQ(data->buffers.size(), 2u);
    EXPECT_TRUE(Shows("Each vertex holds 48 bytes, in 2 buffers.")) << "the inspector says what is held now";
    EXPECT_TRUE(Doc().Dirty());
    EXPECT_NE(Doc().ToJson().find("meshImports"), std::string::npos) << "saved with the scene";

    // Taken back: read whole again.
    for (int i = 0; i < 40; ++i) Frame();
    ASSERT_TRUE(Session().Undo());
    const auto after = runtime.ResourceOf(mesh);
    for (int i = 0; i < 300 && !after->replacedBy; ++i) Frame();
    data = read();
    ASSERT_TRUE(data);
    EXPECT_NE(data->Find("UV"), nullptr);
    std::filesystem::remove(file);
}

// Imported, a model is asked about first: what its meshes have, each attribute to read or leave out and the buffer
// each read one is in. What is left out then is never read at all.
TEST_F(EditorTest, AModelIsAskedAboutBeforeItIsImported) {
    const keditor::Project project = scene::MakeProject("import options");
    Open(project.directory);
    ASSERT_TRUE(Session().HasProject());
    const auto assets = project.directory / "assets";
    std::filesystem::create_directories(assets);
    const auto file = assets / "quad.obj";
    std::ofstream(file) << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";
    auto& runtime = Doc().Runtime();
    const std::string id = runtime.AssetId(file), mesh = runtime.AssetId(file, 0);

    // Where something of the interface is that shows @p text: its middle, in the window — the last drawn of them, over the
    // rest (a dialog's, over the inspector behind it).
    const auto where = [&](const std::string& text) -> std::optional<kor::Vec2> {
        for (int i = 0; i < 4; ++i) Frame();
        std::optional<kor::Vec2> found;
        const std::function<void(kui::RenderObject&)> visit = [&](kui::RenderObject& o) {
            if (o.DebugText() == text) found = o.ToGlobal(o.Size() * 0.5f);
            o.VisitChildren(visit);
        };
        if (auto* root = editor->Interface().RootRenderObject()) visit(*root);
        return found;
    };
    Session().SelectAsset(id);
    const auto import = where("Import into the scene...");
    ASSERT_TRUE(import) << "the model's inspector offers to import it";
    Click(*import);
    EXPECT_TRUE(Shows("Importing quad.obj")) << "asked about first";
    for (int i = 0; i < 300 && !Shows("TANGENT"); ++i) Frame();
    ASSERT_TRUE(Shows("TANGENT")) << "what its meshes have, once the file is read";
    EXPECT_TRUE(std::ranges::none_of(runtime.Assets(), [&](const std::string& held) { return held.starts_with(id + "#"); })) << "nothing of it read yet";

    // UV left out: its box, on its line under the column where POSITION's says it is always read.
    const auto uv = where("UV"), always = where("always");
    ASSERT_TRUE(uv && always);
    Click({ always->x, uv->y });
    EXPECT_TRUE(Shows("Each vertex holds 48 bytes, in 1 buffer.")) << "UV is not held";
    const auto go = where("Import");
    ASSERT_TRUE(go);
    Click(*go);
    EXPECT_FALSE(Shows("Importing quad.obj")) << "the dialog is gone";
    EXPECT_EQ(Doc().MeshImportOf(id.substr(1)).leaveOut, std::vector<std::string> { "UV" }) << "the choice is the scene's";
    std::optional<kgraph::VertexData> data;
    for (int i = 0; i < 300 && !data; ++i) {
        if (const auto held = runtime.ResourceOf(mesh); held && !held->pending) data = runtime.ReadMesh(mesh);
        Frame();
    }
    ASSERT_TRUE(data);
    EXPECT_EQ(data->Find("UV"), nullptr) << "read without it, the first time";
    EXPECT_NE(data->Find("NORMAL"), nullptr);
    EXPECT_FALSE(Doc().World().FindEntity("quad.obj").IsNull() && Doc().World().FindEntity("quad").IsNull()) << "and imported";
}

// What an entity is made of is offered in groups: meshes, lights and cameras each a submenu, opened in place — a Fly
// Camera among the cameras, in a project made before there was one.
TEST_F(EditorTest, NewEntitiesAreOfferedInGroups) {
    scene::MakeProject("groups");
    Open();
    Click({ 80.f, 91.f });     // the outliner's +
    EXPECT_TRUE(Shows("Empty entity"));
    for (const char* group : { "Mesh", "Light", "Camera" }) EXPECT_TRUE(Shows(group)) << group;
    EXPECT_FALSE(Shows("Torus")) << "a mesh is in its group, until that is opened";
    EXPECT_FALSE(Shows("Spot Light"));
    Click({ 75.f, 137.f + 36.f + 9.f });     // under Empty entity and the line after it: Mesh
    EXPECT_TRUE(Shows("Torus")) << "opened beside it";
    Sheet("groups");
    EXPECT_TRUE(Shows("Cube"));
    EXPECT_FALSE(Shows("Fly Camera"));
    const auto made = std::ranges::find(Session().Objects(), "Fly Camera", &keditor::ObjectTemplate::name);
    ASSERT_NE(made, Session().Objects().end()) << "a standard object, though the project's folder has none";
    const kecs::Entity camera = Session().Create(*made);
    EXPECT_TRUE(Doc().World().Has(camera, Doc().World().Find("Camera")));
    EXPECT_TRUE(Doc().World().Has(camera, Doc().World().Find("FlyCamera")));
}

// Every kind of node has an icon in its title bar, and it is one the interface can draw.
TEST(EditorIcons, EveryNodeTypeHasOneThatIsDrawn) {
    for (const auto& type : kgraph::Catalog()) {
        EXPECT_FALSE(type.icon.empty()) << type.name;
        EXPECT_NE(kui::MaterialIcon(type.icon), nullptr) << type.name << ": no icon '" << type.icon << "'";
    }
}

// How long a frame of the editor takes on a project, standing still and flying through it (the right button held, the
// mouse moving, W held): KEDITOR_BENCH names the project. Not run by default.
TEST_F(EditorTest, DISABLED_FlyingThroughAProjectTakesNoLongerThanStandingStill) {
    const char* named = std::getenv("KEDITOR_BENCH");
    if (!named) GTEST_SKIP() << "KEDITOR_BENCH names the project";
    Open(named);
    const int frames = std::getenv("KEDITOR_BENCH_FRAMES") ? std::atoi(std::getenv("KEDITOR_BENCH_FRAMES")) : 400;
    for (int i = 0; i < 60; ++i) Frame();
    const auto time = [&](const std::function<void(int)>& each) {
        const auto began = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) { each(i); Frame(); }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / frames;
    };
    const double still = time([](int) {});
    auto& input = editor->SceneInput();
    const kor::Vec2 middle { 930.f, 350.f };       // over the scene's picture
    input.FeedMousePosition(middle);
    Frame();
    input.FeedMouseButton(kor::MouseButton::eRight, true);
    const auto turning = [&](const int i) { input.FeedMousePosition(middle + kor::Vec2(static_cast<float>(i % 40), 0.f)); };
    const double looking = time(turning);
    const auto versions = Session().Version();
    input.FeedKey(kor::Key::eW, true);
    const double flying = time(turning);
    const auto bumped = Session().Version() - versions;
    input.FeedKey(kor::Key::eW, false);
    input.FeedMouseButton(kor::MouseButton::eRight, false);
    std::printf("            standing still: %.3f ms a frame; looking round: %.3f; flying: %.3f (the interface made again %llu times in %d frames)\n",
                still, looking, flying, static_cast<unsigned long long>(bumped), frames);
}

// What a page can use is pictured, and its pictures are made anew every few seconds: what shows them finds each again, and
// does not hold on to one that is gone. Seconds of it in sight, and then one of them brought in and picked.
TEST_F(EditorTest, WhatAPageCanUseStaysInSightAsItsPicturesAreMadeAgain) {
    const auto project = scene::MakeProject("in sight");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    Session().ShowPass("show");
    EXPECT_TRUE(Shows("WHAT SHOW CAN USE"));
    EXPECT_TRUE(Session().DrawerShown()) << "its panel is in sight, and says so";
    // Long enough, as the clock goes, for its pictures to be made again, each in the place of the last.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < until) Frame();
    EXPECT_TRUE(Session().NodePicture("color").Get());
    Session().Select(Doc().Import("camera data", "show", 0, 0));
    for (int i = 0; i < 30; ++i) Frame();
    ASSERT_NE(Doc().Graph().Find(Session().selectedNode), nullptr);
    EXPECT_EQ(Doc().Graph().Find(Session().selectedNode)->type, "Reference");
}

// Dragged with the gizmo, an entity is moved every frame of the drag: the inspector follows it by itself, nothing else is
// built again for it, and it is a change all the same — the scene not saved, and the drag one thing to take back.
TEST_F(EditorTest, AnEntityDraggedIsFollowedWithoutTheInterfaceBuiltAgainAndIsTakenBack) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "dragged";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    Doc().SetDirty(false);
    kecs::World& world = Doc().World();
    kecs::Entity sphere = kecs::NullEntity;
    world.EachEntity([&](const kecs::Entity e) { if (sphere.IsNull() && world.Has(e, world.Find("Mesh"))) sphere = e; });
    ASSERT_FALSE(sphere.IsNull());
    const std::string name = world.NameOf(sphere);
    Session().Select(sphere);
    Settle();
    const kecs::Vec3 was = world.PositionOf(sphere);
    const auto versions = Session().Version();
    for (int i = 1; i <= 10; ++i) {
        world.SetPosition(sphere, { 0.5f * static_cast<float>(i), was.y, was.z });       // as the gizmo does
        Doc().Moved(sphere, "position");
        Frame();
    }
    EXPECT_EQ(Session().Version(), versions) << "nothing built again for each frame of the drag";
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_TRUE(Shows("5.00")) << "the inspector shows where it is now";
    EXPECT_TRUE(Doc().Dirty()) << "a change all the same";
    for (int i = 0; i < 40; ++i) Frame();     // let go: the drag is one thing to take back
    ASSERT_TRUE(Session().Undo());
    EXPECT_EQ(world.PositionOf(world.FindEntity(name)).x, was.x) << "taken back to where it was";
}

TEST_F(EditorTest, TheAssetsPanelShowsWhatTheProjectIsMadeOfOrItsFiles) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "files-view";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    std::filesystem::create_directories(directory / "empty" / "deeper");
    std::filesystem::create_directories(directory / "notes");
    std::ofstream(directory / "notes" / "Plain.kt") << "class Plain(val x: Int)\n";     // of no component
    std::ofstream(directory / "notes" / "readme.md") << "notes\n";
    std::ofstream(directory / "notes" / ".hidden") << "\n";
    keditor::Session& session = Session();

    // What it is made of: only the folders something of it is in.
    session.SetAssetFiles(false);
    auto folders = session.AssetFolders(session.Assets());
    EXPECT_FALSE(std::ranges::contains(folders, "empty"));
    EXPECT_FALSE(std::ranges::contains(folders, "notes")) << "a file of plain classes is no asset";
    EXPECT_TRUE(std::ranges::contains(folders, "Materials/PBR"));

    // Its files: every folder, empty ones too, and every file in them.
    session.SetAssetFiles(true);
    folders = session.AssetFolders(session.Assets());
    EXPECT_TRUE(std::ranges::contains(folders, "empty"));
    EXPECT_TRUE(std::ranges::contains(folders, "empty/deeper"));
    EXPECT_TRUE(std::ranges::contains(folders, "notes"));
    const auto files = session.AssetFolderFiles("notes");
    ASSERT_EQ(files.size(), 2u) << "not what starts with a dot";
    EXPECT_EQ(files[0].filename(), "Plain.kt");
    EXPECT_EQ(files[1].filename(), "readme.md");
    session.assetFolder = "notes";
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_TRUE(Shows("Plain.kt"));
    EXPECT_TRUE(Shows("readme.md"));

    // Back to what it is made of: the folder of nothing is not shown, nor gone on being in.
    session.SetAssetFiles(false);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(Shows("Plain.kt"));
    EXPECT_TRUE(session.assetFolder.empty());
}

TEST_F(EditorTest, TheDefaultMeshesAndMaterialsAreAssetsOfEveryProject) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "defaults";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    const auto assets = Session().Assets();
    for (const auto& name : kgraph::DefaultMeshNames()) {
        const auto it = std::ranges::find(assets, kgraph::DefaultMeshId(name), &keditor::Session::Asset::id);
        ASSERT_NE(it, assets.end()) << name;
        EXPECT_EQ(it->kind, keditor::Session::Asset::Kind::eMesh);
        EXPECT_TRUE(it->folder.empty()) << "listed with the other meshes, in no folder of its own";
        EXPECT_TRUE(it->builtIn);
    }
    EXPECT_EQ(std::ranges::count(assets, keditor::Session::Asset::Kind::eModel, &keditor::Session::Asset::kind), 0) << "a default mesh is of no model";
    const auto folders = Session().AssetFolders(assets);
    EXPECT_FALSE(std::ranges::contains(folders, "Meshes/Primitives"));
    EXPECT_TRUE(std::ranges::contains(folders, "Materials/PBR"));
    EXPECT_TRUE(std::ranges::contains(folders, "Materials/Blinn-Phong"));
    const auto gold = std::ranges::find(assets, "material:Gold", &keditor::Session::Asset::id);
    ASSERT_NE(gold, assets.end());
    EXPECT_EQ(gold->folder, "Materials/PBR");
    const auto classic = std::ranges::find(assets, "material:Blinn-Phong Gold", &keditor::Session::Asset::id);
    ASSERT_NE(classic, assets.end());
    EXPECT_EQ(classic->folder, "Materials/Blinn-Phong");
    EXPECT_EQ(classic->label, "Gold");

    // In the same tab as a model's meshes, and in no tab of their own.
    Session().assetKind = static_cast<int>(keditor::Session::Asset::Kind::eMesh);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_TRUE(Shows("torus"));
    EXPECT_FALSE(Shows("Primitives"));
    Sheet("meshes-tab");
    Session().assetKind = -1;

    // Dropped on an entity, a default material is a PbrMaterial asset it follows: its colour the material's. A light's
    // colour is no material's.
    kecs::World& world = Doc().World();
    const kecs::Entity sun = world.FindEntity("Sun");
    ASSERT_TRUE(Session().GiveAsset("material:Gold", sun));
    EXPECT_EQ(Doc().MaterialAssetOfEntity(sun, "PbrMaterial"), "material:Gold");
    EXPECT_EQ(*Doc().Field(sun, "PbrMaterial", "baseColor").Items(), (kgraph::Value::List { 1.f, 0.78f, 0.34f, 1.f }));
    EXPECT_EQ(Doc().Field(sun, "PbrMaterial", "metallic"), kgraph::Value(1.0));
    EXPECT_EQ(*Doc().Field(sun, "DirectionalLight", "color").Items(), (kgraph::Value::List { 1.f, 0.96f, 0.9f }));
    const kecs::Entity bare = Doc().CreateEntity("Bare");
    ASSERT_TRUE(Session().GiveAsset("$torus", bare));
    ASSERT_TRUE(Session().GiveAsset("material:Red plastic", bare));
    EXPECT_EQ(*Doc().Field(bare, "PbrMaterial", "baseColor").Items(), (kgraph::Value::List { 0.8f, 0.1f, 0.08f, 1.f }));
    ASSERT_TRUE(Session().GiveAsset("material:Gold", bare));
    EXPECT_EQ(Doc().MaterialAssetOfEntity(bare, "PbrMaterial"), "material:Gold") << "another dropped on it: it follows that one";
    EXPECT_FALSE(Session().GiveAsset("material:Nothing", bare));
    for (int i = 0; i < 5; ++i) Frame();

    // Their tangents: four numbers, unless the graph says otherwise — a change, taken back as any is.
    const auto semantics = [&] {
        std::string all;
        for (const auto& attribute : Doc().Runtime().ReadMesh("$torus")->attributes) all += attribute.semantic + " ";
        return all;
    };
    EXPECT_EQ(semantics(), "POSITION NORMAL TANGENT UV ");
    Doc().SetDefaultMeshTangents(kgraph::Tangents::eWithBitangent);
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(semantics(), "POSITION NORMAL TANGENT BITANGENT UV ");
    ASSERT_TRUE(Session().Undo());
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(semantics(), "POSITION NORMAL TANGENT UV ");
}

TEST_F(EditorTest, AFieldThatIsOneOfSoManyThingsIsShownByItsName) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "named";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    // A material's alphaMode: `[Choices("Opaque", "Masked", "Blended")]`, kept as a number and shown by name.
    const kecs::Entity glass = Doc().CreateEntity("Glass");
    ASSERT_TRUE(Doc().AddComponent(glass, "PbrMaterial"));
    ASSERT_TRUE(Doc().SetField(glass, "PbrMaterial", "alphaMode", "Blended"));
    EXPECT_EQ(Doc().Field(glass, "PbrMaterial", "alphaMode"), kgraph::Value(2)) << "kept as the number it is";
    Session().Select(glass);
    EXPECT_TRUE(Shows("Blended")) << "its mode, by name";
}

TEST_F(EditorTest, APassRunsEveryFrameOnTheFixedStepOrOnceAndTheFixedStepIsTakenWhileItPlays) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "rates";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    ASSERT_TRUE(Doc().AddPass("simulate", -1, kgraph::PassRate::eFixed));
    ASSERT_TRUE(Doc().AddPass("noise", -1, kgraph::PassRate::eSetup));
    const auto& passes = Doc().Passes();
    EXPECT_EQ(passes.front(), "noise") << "setup first";
    EXPECT_EQ(passes[1], "simulate") << "then the fixed step, before every frame's";
    EXPECT_EQ(Doc().RateOf("simulate"), kgraph::PassRate::eFixed);
    Frame();
    // Every frame now — the first of them, next to where it was — and taken back.
    ASSERT_TRUE(Doc().SetPassRate("simulate", kgraph::PassRate::eFrame));
    EXPECT_EQ(passes[1], "simulate");
    EXPECT_EQ(Doc().RateOf("simulate"), kgraph::PassRate::eFrame);
    EXPECT_FALSE(Doc().SetPassRate("simulate", kgraph::PassRate::eFrame)) << "it is that already";
    Frame();
    ASSERT_TRUE(Session().Undo());
    EXPECT_EQ(Doc().RateOf("simulate"), kgraph::PassRate::eFixed);
    Session().ShowPass("simulate");
    EXPECT_TRUE(Shows("FIXED STEP  ·  1")) << "its lane";

    // Its steps are taken while the scene plays, and not while it is paused.
    kgraph::Graph graph = Doc().Graph();
    graph.Add("step", "FixedStep").pass = "simulate";
    Doc().SetGraph(graph);
    const auto count = [&] { const auto n = Doc().Runtime().ValueOf("step", "count").Number(); return n ? *n : -1.0; };
    for (int i = 0; i < 10; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); Frame(); }
    EXPECT_EQ(count(), 0.0) << "not playing: no step";
    Session().StartPlaying();
    for (int i = 0; i < 10; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); Frame(); }
    const double played = count();
    EXPECT_GT(played, 3.0) << "a step every 1/60 s or so of the 0.2 s it played";
    Session().Pause();
    for (int i = 0; i < 10; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); Frame(); }
    EXPECT_LE(count(), played + 1.0) << "paused: none taken";
    Session().Stop();
}

TEST_F(EditorTest, TheEnginesFunctionsAreThereInEveryProjectAndAProjectsOwnComesFirst) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "engine functions";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    const auto names = Doc().Functions();
    for (const char* name : { "Scan", "RadixSort", "BitonicSort", "OddEvenSort", "Compact", "IndirectArgs" }) {
        EXPECT_TRUE(std::ranges::contains(names, std::string(name))) << name;
        EXPECT_TRUE(Doc().IsEngineFunction(name)) << name;
    }
    kgraph::Graph graph = Doc().Graph();
    graph.Add("numbers", "Buffer").Set("instanceCount", 4096).Set("usage", kgraph::Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.Add("sums", "Function").Set("function", "Scan").Wire("values", "numbers").Set("count", 1024);
    graph.Add("sorted", "Function").Set("function", "BitonicSort").Set("type", "float").Set("passes", 55).Wire("elements", "numbers").Set("count", 1024);
    graph.Add("nearer sorted", "Function").Set("function", "OddEvenSort").Wire("elements", "numbers").Set("count", 1024);
    Doc().SetGraph(graph);
    for (int i = 0; i < 3; ++i) Frame();
    for (const char* call : { "sums", "sorted", "nearer sorted" }) {
        ASSERT_NE(Doc().Runtime().Status(call), nullptr) << call;
        EXPECT_TRUE(Doc().Runtime().Status(call)->ok) << call << ": " << Doc().Runtime().Status(call)->error;
    }
    EXPECT_TRUE(std::filesystem::exists(Doc().Runtime().GeneratedShaderDirectory() / "koral_sort_of_float.slang")) << "with the project's generated shaders";

    // The project's own of the same name is called in its place.
    std::filesystem::create_directories(Doc().FunctionsDirectory());
    std::filesystem::copy_file(keditor::Document::EngineFunctionsDirectory() / "Scan.kfunc", Doc().FunctionsDirectory() / "Scan.kfunc");
    EXPECT_FALSE(Doc().IsEngineFunction("Scan"));
    EXPECT_EQ(std::ranges::count(Doc().Functions(), std::string("Scan")), 1) << "listed once";
}

TEST_F(EditorTest, StepIsOneFixedStepAndPaused) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "step";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    ASSERT_TRUE(Doc().AddPass("simulate", -1, kgraph::PassRate::eFixed));
    kgraph::Graph graph = Doc().Graph();
    graph.Add("step", "FixedStep").pass = "simulate";
    Doc().SetGraph(graph);
    const auto count = [&] { const auto n = Doc().Runtime().ValueOf("step", "count").Number(); return n ? *n : -1.0; };
    for (int i = 0; i < 3; ++i) Frame();
    Session().Step();
    for (int i = 0; i < 5; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); Frame(); }
    EXPECT_TRUE(Session().InPlayMode()) << "started, as Play does";
    EXPECT_FALSE(Session().IsPlaying()) << "and paused";
    EXPECT_EQ(count(), 1.0) << "one step, and no more";
    Session().Step();
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_EQ(count(), 2.0);
    Session().Stop();
}

// The grid's settings, changed in their popup over the Scene panel: it stays open while the editor is built again,
// a plane is picked, a number dragged — and an editor closed with it open goes quietly.
TEST_F(EditorTest, TheGridsSettingsAreChangedInTheirPopup) {
    const auto project = scene::MakeProject("grid settings");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    keditor::AddLitScene(Doc());
    for (int i = 0; i < 6; ++i) Frame();
    keditor::Grid grid = Session().SceneGrid();
    grid.shown = true;
    Session().SetGrid(grid);

    // The grid's settings, beside the grid: after the button that says how the scene is shown — its name, its arrow
    // (4 and 14 wide, and its padding of 4) — the grid's button (24, after a gap of 4), and a gap of 2.
    const auto lit = Where("Lit");
    ASSERT_TRUE(lit) << "the scene view's toolbar";
    Click({ lit->second.x + 4.f + 14.f + 4.f + 4.f + 24.f + 2.f + 12.f, (lit->first.y + lit->second.y) * 0.5f });
    ASSERT_TRUE(Shows("Behind things"));
    for (int i = 0; i < 10; ++i) Frame();
    EXPECT_TRUE(Shows("Behind things")) << "open still, the editor built again under it";
    Sheet("grid settings");

    const auto xy = Where("XY");
    ASSERT_TRUE(xy);
    Click((xy->first + xy->second) * 0.5f);
    EXPECT_EQ(Session().SceneGrid().plane, keditor::Grid::Plane::eXY);
    EXPECT_TRUE(Shows("Behind things")) << "a setting changed leaves it open";
    // The cell's size, dragged: its field is after its label's 96.
    const auto label = Where("Cell (m)");
    ASSERT_TRUE(label);
    const float from = label->first.x + 96.f + 20.f, row = (label->first.y + label->second.y) * 0.5f;
    const float cell = Session().SceneGrid().cell;
    auto& input = editor->SceneInput();
    input.FeedMousePosition({ from, row });
    Frame();
    input.FeedMouseButton(kor::MouseButton::eLeft, true);
    for (int x = 0; x <= 80; x += 10) { input.FeedMousePosition({ from + static_cast<float>(x), row }); Frame(); }
    input.FeedMouseButton(kor::MouseButton::eLeft, false);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_GT(Session().SceneGrid().cell, cell) << "dragged to the right, a bigger cell";
    EXPECT_TRUE(Shows("Behind things"));
    for (const auto& problem : Session().Play().Over()->ViewProblems()) ADD_FAILURE() << problem;
}

// A node says how abstract it is — a value, a resource, a command, the scene's data, a technique — and whether it is
// made of other nodes: a Function's call is a technique, of nodes.
TEST_F(EditorTest, ANodeSaysHowAbstractItIsAndWhetherItIsMadeOfNodes) {
    const auto project = scene::MakeProject("levels");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    const std::string call = Doc().AddNode("Function", 900, 300, "tone it", "scene");
    Doc().SetInput(call, "function", "Tonemap");
    Session().ShowPass("scene");
    EXPECT_TRUE(Shows("Function  ·  technique, of nodes"));
    EXPECT_TRUE(Shows("Draw  ·  command"));
    EXPECT_TRUE(Shows("Image  ·  resource"));
}

// The pages laid out as a flow: each in the column, between what it takes from and what takes from it, where its wires
// are shortest; each branch of a Switch a band of its own, apart from the others; no card over another.
TEST(PassFlowLayout, BranchesAreBandsAndWiresAreShort) {
    using Flow = keditor::Session::PassFlow;
    Flow flow;
    int order = 0;
    const auto page = [&](const char* id) { flow.steps.push_back({ .id = id, .rate = "frame", .ran = true, .order = order++ }); };
    for (const char* id : { "frame", "lights", "a1", "a2", "a3", "a4", "b1", "b2", "c1", "post", "show" }) page(id);
    Flow::Step choose { .id = "choose", .isSwitch = true, .driver = "renderer", .options = { "a4", "b2", "c1" }, .order = 8 };
    flow.steps.push_back(choose);
    const auto link = [&](const char* a, const char* b, const int option = -1) { flow.links.push_back({ a, b, {}, option }); };
    link("frame", "lights");
    for (const char* first : { "a1", "b1", "c1" }) { link("frame", first); link("lights", first); }
    link("a1", "a2"); link("a2", "a3"); link("a3", "a4"); link("b1", "b2");
    link("a4", "choose", 0); link("b2", "choose", 1); link("c1", "choose", 2);
    link("choose", "post"); link("post", "show"); link("frame", "post");
    const auto layout = keditor::Session::LayOut(flow, {});

    ASSERT_EQ(layout.branches.size(), 3u);
    EXPECT_EQ(layout.branches[0].steps.size(), 4u);
    EXPECT_EQ(layout.branches[1].steps.size(), 2u);
    EXPECT_EQ(layout.branches[2].steps, (std::vector<std::string> { "c1" }));
    EXPECT_NE(layout.branches[0].title.find("renderer = 0"), std::string::npos) << layout.branches[0].title;
    // Each branch a band: the heights of one are none of another's.
    std::vector<std::pair<float, float>> spans;
    for (const auto& branch : layout.branches) {
        float low = 1e9f, high = -1e9f;
        for (const auto& id : branch.steps) { low = std::min(low, layout.at.at(id).second); high = std::max(high, layout.at.at(id).second + 90.f); }
        spans.push_back({ low, high });
    }
    for (std::size_t a = 0; a < spans.size(); ++a)
        for (std::size_t b = a + 1; b < spans.size(); ++b)
            EXPECT_TRUE(spans[a].second <= spans[b].first || spans[b].second <= spans[a].first) << "branches " << a << " and " << b << " share heights";
    // The short branch is next to the Switch it ends in, not as far from it as the long one starts.
    EXPECT_GT(layout.at.at("c1").first, layout.at.at("a1").first) << "one page long, it is beside what takes from it";
    EXPECT_LT(layout.at.at("frame").first, layout.at.at("a1").first);
    EXPECT_LT(layout.at.at("choose").first, layout.at.at("post").first);
    // No card over another.
    for (const auto& [a, at] : layout.at)
        for (const auto& [b, other] : layout.at)
            if (a < b && at.first == other.first) EXPECT_GE(std::abs(at.second - other.second), 90.f) << a << " and " << b << " overlap";
}

// A scene whose pictures are not compressed asks, once it is read, whether to compress them: the dialog lists them, by
// kind, with the packs their names suggest. Kept compressed, each is cooked into the project's cache — kept out of git —
// and read from there; the files of a pack read as its channels. Asked again of another scene only.
TEST_F(EditorTest, TheScenesTexturesAreCompressedWhenItAsks) {
    const auto project = scene::MakeProject("textures");
    const auto assets = project.directory / "assets";
    std::filesystem::create_directories(assets);
    std::ofstream(project.directory / ".gitignore") << "assets/";      // no newline at its end: one is put before what is added
    const auto write = [&](const std::string& name, const std::vector<unsigned char>& texel) {
        std::ofstream file(assets / name, std::ios::binary);
        file << (texel.size() == 3 ? "P6" : "P5") << "\n8 8\n255\n";
        for (int i = 0; i < 64; ++i) file.write(reinterpret_cast<const char*>(texel.data()), static_cast<std::streamsize>(texel.size()));
    };
    write("wall_BaseColor.ppm", { 200, 40, 10 });
    write("wall_Normal.ppm", { 128, 128, 255 });
    write("wall_AO.pgm", { 30 });
    write("wall_Roughness.pgm", { 100 });
    write("wall_Metalness.pgm", { 220 });
    Open(project.directory);
    for (const char* id : { "@wall_BaseColor.ppm", "@wall_Normal.ppm", "@wall_AO.pgm", "@wall_Roughness.pgm", "@wall_Metalness.pgm" })
        ASSERT_NE(Doc().Runtime().ResourceOf(id), nullptr) << id;
    for (int i = 0; i < 200 && Doc().Reading() > 0; ++i) Frame();

    for (int i = 0; i < 10; ++i) Frame();
    EXPECT_FALSE(Shows("5 textures in this scene, 0 compressed")) << "nothing is asked on opening a scene: the menu has it";

    const auto textures = Session().SceneTextures();
    ASSERT_EQ(textures.size(), 5u);
    const auto kind = [&](const std::string& id) { return std::ranges::find(textures, id, &keditor::Session::SceneTexture::id)->kind; };
    EXPECT_EQ(kind("@wall_Normal.ppm"), kgraph::TextureKind::eNormal);
    EXPECT_EQ(kind("@wall_Roughness.pgm"), kgraph::TextureKind::eMask);

    // As the dialog keeps them: each by its kind, but the files of the pack, which is the pack's.
    std::map<std::string, kgraph::TextureImport> imports {
        { "@wall_BaseColor.ppm", { kgraph::TextureFormat::eBC7, kgraph::TextureQuality::eFast } },
        { "@wall_Normal.ppm", { kgraph::TextureFormat::eBC5, kgraph::TextureQuality::eFast } } };
    auto packs = kgraph::SuggestTexturePacks({ "@wall_AO.pgm", "@wall_Roughness.pgm", "@wall_Metalness.pgm" });
    ASSERT_EQ(packs.size(), 1u);
    packs.begin()->second.quality = kgraph::TextureQuality::eFast;
    Session().KeepTextures(imports, packs, true);
    for (int i = 0; i < 3000 && Session().Cooking().done < Session().Cooking().total; ++i) Frame();
    for (int i = 0; i < 200 && Doc().Reading() > 0; ++i) Frame();
    EXPECT_EQ(Session().Cooking().total, 3u) << "two files and a pack";
    for (const auto& why : Session().Cooking().failed) ADD_FAILURE() << why;
    EXPECT_TRUE(Shows("3 textures compressed")) << "said when it is done";
    for (const auto& texture : Session().SceneTextures()) EXPECT_TRUE(texture.cooked) << texture.id;
    const auto rough = Doc().Runtime().ResourceOf("@wall_Roughness.pgm");
    ASSERT_TRUE(rough && rough->kind == "ImageView" && rough->source) << "a channel of the pack";
    EXPECT_EQ(kor::capi::ImageOf(rough->source->handle)->PixelFormat(), kor::Image::Format::eBC7_UNORM);
    std::ifstream ignore(project.directory / ".gitignore");
    const std::string ignored((std::istreambuf_iterator<char>(ignore)), std::istreambuf_iterator<char>());
    EXPECT_EQ(ignored, "assets/\ncache/\n") << "the cache is the machine's own";
    EXPECT_EQ(Doc().Graph().textureImports, imports) << "kept in the scene's graph";
    EXPECT_EQ(Doc().Graph().texturePacks.size(), 1u);
}

// The Frame panel: how long a frame takes against the time one has, then a line for each pass node with its GPU and CPU
// time and its share, in the order they run or by what they cost; the editor's own passes one line of their own.
TEST_F(EditorTest, TheFrameIsALineForEachPassWithWhatItCosts) {
    scene::MakeProject("editor");
    Open();
    scene::BuildGraph(Doc());
    scene::BuildEntities(Doc());
    for (int i = 0; i < 60; ++i) Frame();
    Click({ 20.f, 855.f });     // the Frame panel, from the rail down the side
    for (int i = 0; i < 40; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); Frame(); }
    for (const char* shown : { " fps", "GPU ", "CPU ", "60 Hz", "SHARE", "tint backdrop", "The editor's previews" }) EXPECT_TRUE(Shows(shown)) << shown;
    EXPECT_FALSE(Shows("$peek")) << "the editor's passes are not each a line";
    Click({ 170.f, 768.f });    // by what they cost
    EXPECT_TRUE(Shows("tint backdrop"));
    Sheet("frame");
    // On a timeline: the CPU's threads and the GPU's queues, each pass where it was prepared, recorded and run.
    Click({ 240.f, 768.f });
    EXPECT_TRUE(Shows("The newest frame the GPU has measured")) << "or the device times no pass";
    EXPECT_FALSE(Shows("SHARE")) << "no list of lines";
    Sheet("frame-timeline");
}

// An entity's mesh is offered meshes: the built-in ones and the project's, and of the graph only what makes a mesh — by the
// type its field says it is, as the inspector asks, not a type typed here.
TEST_F(EditorTest, AMeshFieldIsOfferedMeshesAndNothingElse) {
    const auto directory = std::filesystem::temp_directory_path() / "keditor-tests" / "mesh-choices";
    std::filesystem::remove_all(directory);
    Open();
    ASSERT_TRUE(Session().NewProject(directory));
    ASSERT_TRUE(Session().NewStandardScene());
    kecs::World& world = Doc().World();
    const auto& described = world.Describe(world.Find("Mesh"));
    const auto field = std::ranges::find(described.fields, std::string("mesh"), &kecs::FieldDescription::name);
    ASSERT_NE(field, described.fields.end());
    EXPECT_EQ(field->type, "Mesh") << "a mesh, not any resource";
    const auto choices = Session().FieldChoices("Mesh", field->type);
    EXPECT_TRUE(std::ranges::contains(choices, kgraph::DefaultMeshId(kgraph::DefaultMeshNames().front()))) << "the built-in meshes";
    for (const auto& choice : choices) {
        const auto held = Doc().Runtime().ResourceOf(choice);
        if (Doc().Graph().Find(choice)) EXPECT_TRUE(held && held->kind == "Mesh") << choice << " is in the graph and makes no mesh";
    }
    std::filesystem::remove_all(directory);
}

// The scene view's tools: what is under the pointer, seen straight on and along an axis, hidden and shown by itself,
// picked in a box and more than one at once, bookmarked, split in two, and its numbers in a corner.
TEST_F(EditorTest, TheSceneViewMeasuresHidesPicksInABoxAndSplits) {
    const auto project = scene::MakeProject("viewtools");
    keditor::WriteLitShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddLitGraph(Doc(), error)) << error;
    const kecs::Entity sphere = keditor::AddLitScene(Doc());
    Session().Frame(sphere);
    for (int i = 0; i < 8; ++i) Frame();
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    keditor::Overlay* over = Session().Play().Over();
    ASSERT_NE(over, nullptr);

    // What is under the middle of the picture: the sphere, on its surface, facing the view.
    const auto probe = [&](const float u, const float v) {
        std::optional<kgraph::Runtime::PickHit> found;
        EXPECT_TRUE(over->PickAt(u, v, [&](const kgraph::Runtime::PickHit& hit) { found = hit; }));
        for (int i = 0; i < 12 && !found; ++i) Frame();
        return found;
    };
    const auto hit = probe(0.5f, 0.5f);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, sphere);
    ASSERT_TRUE(hit->deep) << "the depth round the pixel is read with it";
    // Where on it, worked out of the depth as the editor does: about its radius from its middle, facing out.
    const kor::Mat4 inverse = kor::Inverse(hit->viewProjection);
    const auto at = [&](const int dx, const int dy) {
        const float depth = hit->depth[(dy + 1) * 3 + dx + 1];
        const kor::Vec4 clip((static_cast<float>(hit->pixel.x + dx) + 0.5f) / static_cast<float>(hit->extent.x) * 2.f - 1.f,
                             (static_cast<float>(hit->pixel.y + dy) + 0.5f) / static_cast<float>(hit->extent.y) * 2.f - 1.f, depth, 1.f);
        const kor::Vec4 world = inverse * clip;
        return kor::Vec3(world.x, world.y, world.z) / world.w;
    };
    const kor::Vec3 centre(kgraph::ToKor(Doc().World().WorldMatrix(sphere))[3]), surface = at(0, 0);
    EXPECT_NEAR(kor::Length(surface - centre), 0.5f, 0.05f) << "on the surface of a sphere half a unit round";
    const kor::Vec3 facing = kor::Normalize(kor::Cross(at(0, 1) - surface, at(1, 0) - surface));
    EXPECT_GT(std::abs(kor::Dot(facing, kor::Normalize(surface - centre))), 0.9f) << "the surface faces out from the middle";
    // Something dropped there is put on that surface: the point is found, then what places it runs.
    bool placed = false;
    Session().DropAt(0.5f, 0.5f, [&] { placed = true; });
    for (int i = 0; i < 12 && !placed; ++i) Frame();
    ASSERT_TRUE(placed);
    ASSERT_TRUE(Session().placeAt);
    EXPECT_NEAR(kor::Length(*Session().placeAt - centre), 0.5f, 0.05f) << "on the sphere";
    Session().placeAt.reset();

    // Seen straight on, from above: the view is a box, and the scene is drawn through it as before.
    Session().LookAlong(keditor::Session::Axis::eTop);
    EXPECT_TRUE(Session().camera.orthographic);
    EXPECT_GT(Session().camera.View().orthographic, 0.f);
    for (int i = 0; i < 6; ++i) Frame();
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    const kor::Mat4 straight = Doc().Runtime().ViewProjection();
    EXPECT_NEAR(straight[0][3], 0.f, 1e-6f) << "no perspective: w is 1 wherever a point is";
    EXPECT_NEAR(straight[1][3], 0.f, 1e-6f);
    EXPECT_NEAR(straight[2][3], 0.f, 1e-6f);
    const auto above = probe(0.5f, 0.5f);
    ASSERT_TRUE(above);
    EXPECT_EQ(above->entity, sphere) << "picked from above, straight on";
    Session().SetOrthographic(false);
    Session().Frame(sphere);
    for (int i = 0; i < 6; ++i) Frame();

    // Hidden in the scene view: not drawn, not picked — and the scene is as it was.
    Session().Select(sphere);
    Session().HideSelected();
    EXPECT_TRUE(Session().ViewHidden().contains(sphere));
    for (int i = 0; i < 6; ++i) Frame();
    const auto through = probe(0.5f, 0.5f);
    ASSERT_TRUE(through);
    EXPECT_NE(through->entity, sphere) << "hidden, it is not what is under the pointer";
    EXPECT_FALSE(Doc().Dirty() && !Doc().IsEnabled(sphere, "Mesh")) << "the scene keeps it as it was";
    Session().ShowAll();
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_EQ(probe(0.5f, 0.5f)->entity, sphere);
    // By itself: everything else hidden; and back.
    Session().Isolate();
    EXPECT_TRUE(Session().Isolated());
    EXPECT_FALSE(Session().ViewHidden().contains(sphere));
    EXPECT_TRUE(Session().ViewHidden().contains(Doc().World().FindEntity("Sun")));
    Session().ShowAll();
    EXPECT_TRUE(Session().ViewHidden().empty());

    // Picked in a box round the whole picture: the sphere is among them. With another added, two; one picked, one.
    const kor::UVec2 size = Session().viewport->Size();
    ASSERT_GT(size.x, 0u);
    Session().SelectInBox({ 0.f, 0.f }, { static_cast<float>(size.x), static_cast<float>(size.y) }, false);
    EXPECT_TRUE(std::ranges::contains(Session().Selection(), sphere));
    const kecs::Entity sun = Doc().World().FindEntity("Sun");
    Session().Select(sphere);
    Session().SelectAlso(sun);
    EXPECT_EQ(Session().Selection(), (std::vector<kecs::Entity> { sphere, sun }));
    Session().SelectAlso(sphere);
    EXPECT_EQ(Session().Selection(), std::vector<kecs::Entity> { sun }) << "pressed again, taken away";
    Session().Select(sphere);
    EXPECT_EQ(Session().Selection(), std::vector<kecs::Entity> { sphere });

    // A bookmark of the view, gone back to.
    const keditor::EditorCamera marked = Session().camera;
    Session().SaveBookmark(2);
    EXPECT_TRUE(Session().HasBookmark(2));
    Session().camera.yaw += 90.f;
    Session().camera.distance *= 3.f;
    EXPECT_TRUE(Session().GoToBookmark(2));
    EXPECT_FLOAT_EQ(Session().camera.yaw, marked.yaw);
    EXPECT_FLOAT_EQ(Session().camera.distance, marked.distance);
    EXPECT_FALSE(Session().GoToBookmark(5)) << "none there";

    // Split: a second view of the scene, from its own place, drawn as the first is.
    Session().SetSplit(true);
    Session().secondCamera.yaw += 120.f;
    for (int i = 0; i < 10; ++i) Frame();
    ASSERT_NE(Session().SecondView(), nullptr);
    ASSERT_NE(Session().SecondView()->Runtime(), nullptr);
    EXPECT_EQ(Session().SecondView()->Runtime()->CurrentView().view, Session().secondCamera.View().view);
    EXPECT_TRUE(Session().SecondView()->Runtime()->EditorView());
    for (const auto& problem : Session().SecondView()->Runtime()->Problems()) ADD_FAILURE() << problem;
    EXPECT_TRUE(Shows("Second view"));
    Session().SetSplit(false);
    for (int i = 0; i < 4; ++i) Frame();
    EXPECT_FALSE(Session().SecondView()->Shown());

    // The frame's numbers, in a corner.
    Session().statsShown = true;
    Session().Select(sphere);   // (the interface built again)
    for (int i = 0; i < 6; ++i) Frame();
    EXPECT_TRUE(Shows("passes")) << "the stats corner";
    EXPECT_TRUE(Shows("entities"));
    // For looking at: the tools, a ruler drawn, the icons, a box being dragged, and the view split.
    Session().SetMeasure(keditor::Session::Measure::eRuler);
    Session().ruler = { centre + kor::Vec3(-0.5f, 0.f, 0.f), centre + kor::Vec3(0.5f, 0.3f, 0.f) };
    Session().SelectAlso(sun);
    Session().camera.yaw = 34.f;
    Session().camera.pitch = -22.f;
    Session().camera.distance = 9.f;
    Sheet("view-tools");
    Session().SetSplit(true);
    for (int i = 0; i < 6; ++i) Frame();
    Sheet("view-split");
    Session().SetSplit(false);
}

// A module is a project minus its scenes: what it has — scripts, functions, objects, assets — is the project's that
// imports it, as long as their scripts are in one language. Its one scene is for trying it out.
TEST_F(EditorTest, AModuleIsImportedAndWhatItHasIsTheProjects) {
    const auto root = std::filesystem::temp_directory_path() / "keditor-tests" / "modules";
    std::filesystem::remove_all(root);
    Open();
    const auto write = [](const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    };

    // A project of no use to import, and a module in C#.
    ASSERT_TRUE(Session().NewProject(root / "plain", 'k')) << Session().Status();
    ASSERT_TRUE(Session().NewProject(root / "sharp", 'c', true)) << Session().Status();

    // The module: a kind and what is of it, a function, an object and a picture.
    ASSERT_TRUE(Session().NewProject(root / "physics", 'k', true)) << Session().Status();
    EXPECT_TRUE(Session().CurrentProject().module);
    EXPECT_EQ(Session().CurrentProject().start, "test") << "its one scene is for trying it out";
    EXPECT_TRUE(std::filesystem::exists(root / "physics" / "scenes" / "test.scene.json"));
    write(root / "physics" / "scripts" / "Colliders.kt",
          "import koral.engine.Data\n\nabstract class Collider : Data() {\n    var friction = 0.5f\n}\nclass SphereCollider : Collider() {\n    var radius = 0.5f\n}\n");
    std::filesystem::create_directories(root / "physics" / "functions");
    std::filesystem::copy_file(keditor::Document::EngineFunctionsDirectory() / "RadixSort.kfunc", root / "physics" / "functions" / "Sorted.kfunc",
                               std::filesystem::copy_options::overwrite_existing);
    write(root / "physics" / "objects" / "Ball.json", R"({ "template": "Ball", "name": "Ball", "components": { "SphereCollider": { "radius": 1 } } })");
    write(root / "physics" / "assets" / "textures" / "rubber.png", "x");

    // What is offered to a project made beside them: the modules, not the project.
    const auto offered = keditor::Project::ModulesIn(root);
    ASSERT_EQ(offered.size(), 2u);
    EXPECT_EQ(offered[0].name, "physics");
    EXPECT_EQ(offered[1].name, "sharp");

    // A project that imports it has what it has.
    ASSERT_TRUE(Session().NewProject(root / "game", 'k', false, { root / "physics" })) << Session().Status();
    EXPECT_FALSE(Session().CurrentProject().module);
    EXPECT_EQ(Session().CurrentProject().modules, std::vector<std::string> { "../physics" }) << "kept from its own folder";
    {
        std::ifstream in(root / "game" / "build.gradle.kts");
        const std::string build((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(build.find((root / "physics" / "scripts").lexically_normal().generic_string()), std::string::npos) << "an editor of code reads the module's scripts too: " << build;
    }
    ASSERT_EQ(Doc().Modules().size(), 1u);
    kecs::World& world = Doc().World();
    ASSERT_NE(world.Find("SphereCollider"), kecs::NoComponent) << "the module's components";
    EXPECT_TRUE(world.IsOf(world.Find("SphereCollider"), world.Find("Collider")));
    EXPECT_TRUE(std::ranges::contains(Doc().Functions(), std::string("Sorted"))) << "its functions";
    EXPECT_EQ(Doc().FunctionFile("Sorted").parent_path().parent_path().filename(), "physics");
    EXPECT_TRUE(std::ranges::any_of(Session().Objects(), [](const auto& object) { return object.name == "Ball"; })) << "its objects";
    ASSERT_NE(Doc().Assets(), nullptr);
    EXPECT_NE(Doc().Assets()->FindFile(root / "physics" / "assets" / "textures" / "rubber.png"), nullptr) << "its assets, by their IDs";
    EXPECT_EQ(Doc().Assets()->FindFile(root / "physics" / "scenes" / "test.scene.json"), nullptr) << "but not its scene";
    std::map<std::string, keditor::Session::Asset> listed;
    for (const auto& asset : Session().Assets()) listed[asset.label] = asset;
    ASSERT_TRUE(listed.contains("SphereCollider"));
    EXPECT_EQ(listed["SphereCollider"].folder, "Modules/physics/scripts") << "under the modules, in a folder of its name";
    ASSERT_TRUE(listed.contains("rubber.png"));
    EXPECT_EQ(listed["rubber.png"].folder, "Modules/physics/assets/textures");
    EXPECT_EQ(Session().AssetFolderPath("Modules/physics/assets"), (root / "physics" / "assets").lexically_normal());

    // What is not a module, or is in another language, is not imported; nor is a module twice.
    EXPECT_FALSE(Session().ImportModule(root / "plain")) << "a project";
    EXPECT_FALSE(Session().ImportModule(root / "sharp")) << "scripts in C#";
    EXPECT_TRUE(Session().ImportModule(root / "physics"));
    EXPECT_EQ(Session().CurrentProject().modules.size(), 1u);
    EXPECT_FALSE(Session().NewProject(root / "mixed", 'k', false, { root / "sharp" }));
    EXPECT_FALSE(std::filesystem::exists(root / "mixed" / "project.json"));

    // Opened again, it imports what it did; and forgotten, it imports nothing.
    ASSERT_TRUE(Session().OpenProject(root / "game")) << Session().Status();
    ASSERT_EQ(Doc().Modules().size(), 1u);
    EXPECT_TRUE(Session().ForgetModule("../physics"));
    EXPECT_TRUE(Doc().Modules().empty());
    keditor::Project saved;
    std::string error;
    ASSERT_TRUE(keditor::Project::Open(root / "game", saved, error)) << error;
    EXPECT_TRUE(saved.modules.empty());

    // The form a new one is made with: its name, its language, what it is, where, and the modules there to import.
    Session().AskForNewProject();
    for (int i = 0; i < 3; ++i) Frame();
    EXPECT_TRUE(Shows("A new project"));
    EXPECT_TRUE(Shows("Its scripts are in"));
    EXPECT_TRUE(Shows("Browse..."));
    EXPECT_TRUE(Shows("It imports"));
    EXPECT_TRUE(Shows("Add a module from a folder..."));
}

// Another scene of the project's: made as the standard one, listed in project.json, and opened — the one open saved
// first. Each opens again as it was left; a name the project has is no new scene's.
TEST_F(EditorTest, AProjectHasAsManyScenesAsAreMadeInIt) {
    const auto project = scene::MakeProject("scenes");
    Open(project.directory);
    const std::string first = Session().SceneName();
    const kecs::Entity marked = Doc().CreateEntity("Only in the first");
    ASSERT_FALSE(marked.IsNull());

    ASSERT_TRUE(Session().AddScene("second")) << Session().Status();
    EXPECT_EQ(Session().SceneName(), "second");
    EXPECT_TRUE(std::filesystem::exists(project.ScenePath("second")));
    EXPECT_TRUE(std::ranges::contains(Session().CurrentProject().scenes, "second"));
    keditor::Project read;
    std::string error;
    ASSERT_TRUE(keditor::Project::Open(project.directory, read, error)) << error;
    EXPECT_TRUE(std::ranges::contains(read.scenes, "second")) << "listed in project.json";
    EXPECT_TRUE(Doc().World().FindEntity("Only in the first").IsNull()) << "a scene of its own";

    ASSERT_TRUE(Session().OpenScene(first)) << Session().Status();
    EXPECT_FALSE(Doc().World().FindEntity("Only in the first").IsNull()) << "the first as it was left: saved before the second was made";
    EXPECT_FALSE(Session().AddScene("second")) << "a name the project has";
    EXPECT_FALSE(Session().AddScene("a/b")) << "no file's name";
}

// A module of shaders and functions alone is any project's, whatever its scripts' language; a module says the version
// of the shader library it was made for, and one made for another — or saying none — is said; two modules that have a
// shader of one name are said too. The engine's own modules are imported by their names, wherever the engine is.
TEST_F(EditorTest, ModulesSayWhatTheyAreMadeForAndTheEnginesAreByName) {
    const auto root = std::filesystem::temp_directory_path() / "keditor-tests" / "module-rules";
    std::filesystem::remove_all(root);
    Open();
    const auto write = [](const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    };
    ASSERT_TRUE(Session().NewProject(root / "glow", 0, true)) << Session().Status();
    EXPECT_EQ(Session().CurrentProject().library, keditor::Project::LibraryVersion()) << "made for this engine's library";
    write(root / "glow" / "shaders" / "glow.slang", "module glow;\npublic float Glow(float x) { return x * 2.0; }\n");
    ASSERT_TRUE(Session().NewProject(root / "bright", 0, true)) << Session().Status();
    write(root / "bright" / "shaders" / "glow.slang", "module glow;\npublic float Glow(float x) { return x; }\n");
    // Made before modules said what they were made for.
    write(root / "old" / "project.json", R"({ "koral-engine-project": 1, "name": "old", "kind": "module", "scenes": ["test"], "start": "test" })");

    for (const char language : { 'c', 'k' }) {
        const auto project = root / (language == 'c' ? "sharp" : "kotlin");
        ASSERT_TRUE(Session().NewProject(project, language, false, { root / "glow" })) << "a module of shaders alone is a project's of any language: " << Session().Status();
        EXPECT_EQ(Session().CurrentProject().modules, std::vector<std::string> { "../glow" });
    }
    keditor::Project game;
    std::string error;
    ASSERT_TRUE(keditor::Project::Create(root / "game", "game", game, error, 'k', false, { root / "glow", root / "bright", root / "old" })) << error;
    std::vector<std::string> problems;
    EXPECT_EQ(game.Imported(&problems).size(), 3u) << "each is imported, and what is wrong with it said";
    const auto said = [&](const std::string& what) { return std::ranges::any_of(problems, [&](const std::string& p) { return p.find(what) != std::string::npos; }); };
    EXPECT_TRUE(said("old says no version of the shader library")) << ::testing::PrintToString(problems);
    EXPECT_TRUE(said("shaders/glow.slang")) << "two modules have one shader: " << ::testing::PrintToString(problems);

    // The engine's own: by name.
    const auto filters = keditor::Project::OfficialModules() / "filters";
    ASSERT_TRUE(std::filesystem::exists(filters / "project.json")) << filters.generic_string();
    EXPECT_EQ(game.Reference(filters), "@modules/filters");
    ASSERT_TRUE(Session().OpenProject(root / "kotlin")) << Session().Status();
    ASSERT_TRUE(Session().ImportModule(filters)) << Session().Status();
    EXPECT_TRUE(std::ranges::contains(Session().CurrentProject().modules, std::string("@modules/filters")));
    EXPECT_TRUE(std::ranges::contains(Doc().Functions(), std::string("Sharpen"))) << "its functions are the project's";
    std::vector<std::string> none;
    for (const auto& module : Session().CurrentProject().Imported(&none)) EXPECT_EQ(module.library, keditor::Project::LibraryVersion()) << module.name;
    EXPECT_TRUE(none.empty()) << ::testing::PrintToString(none);
}
