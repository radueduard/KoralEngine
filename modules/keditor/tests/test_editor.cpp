// The editor's interface, offscreen: the panels over a scene, showing what the document holds and following it as it
// changes. With KEDITOR_SHEET set to a file's path, the editor as it looks with the tests' scene in it is written
// there (a PPM), to be looked at.

#include <gtest/gtest.h>

#include <kui/icons.h>
#include <functional>
#include <optional>

#include <cmath>

#include <keditor/starter.h>
#include <kecs/json.h>
#include <kgraph/scene.h>

#include <cstdlib>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>

#include <keditor/editor.h>
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
            for (int frame = 0, still = 0; frame < 1000 && still < 12; ++frame) {
                const auto before = Session().Version();
                Frame();
                still = Session().Version() == before ? still + 1 : 0;
            }
        }
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
    EXPECT_TRUE(Shows("compute.module")) << "a pipeline names its shader";
    EXPECT_TRUE(Shows("computeMain")) << "and the entry point in it";
    EXPECT_TRUE(Shows("backdrop")) << "a pipeline's inputs are its shader's, by the shader's own names";
    EXPECT_FALSE(Shows("param.backdrop")) << "what kind of input it is is no part of what it is called";
    EXPECT_TRUE(Shows("What its draws take from each entity is the component 'stamp'"));
    Session().Select(std::string("canvas"));
    EXPECT_TRUE(Shows("eRGBA8_UNORM")) << "an enumeration, by name";
    EXPECT_TRUE(Shows("eColorAttachment")) << "flags, each to tick";
    EXPECT_TRUE(Shows("extent")) << "its size, on one line";
    EXPECT_TRUE(Shows("mipLevels")) << "an image is made with its format, size and usage, and the rest is there as it starts";
    EXPECT_TRUE(Doc().Runtime().Ok());
    EXPECT_FALSE(Doc().Graph().Find("canvas")->inputs.contains("mipLevels")) << "nothing is given until it is set";

    // A graphics pipeline is drawn one of two ways, and shows the inputs of the one it is.
    Session().Select(std::string("show"));
    for (const char* input : { "stages", "eVertex", "vertex.module", "vertex.layout", "fragment.entry", "framebuffer", "source" }) EXPECT_TRUE(Shows(input)) << input;
    EXPECT_TRUE(Shows("geometry.module") && Shows("rasterizationState")) << "what it can do without, as it starts";
    EXPECT_FALSE(Shows("mesh.module")) << "and nothing of the other way of drawing";
    const std::string drawn = Doc().AddNode("GraphicsPipeline", 900, 40, "drawn");
    Session().Select(drawn);
    EXPECT_TRUE(Shows("vertex.module"));
    Doc().SetInput(drawn, "stages", "eMesh");
    EXPECT_TRUE(Shows("mesh.module")) << "with mesh shaders, it has a mesh shader";
    EXPECT_FALSE(Shows("vertex.module") || Shows("geometry.module")) << "and no vertex shader";
    Doc().RemoveNodes({ drawn });

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

// A Draw or a Dispatch run once an element says how many it runs a frame, and what it would take to be one: what of it is
// each element's, which its shader would read from a buffer of instances. One whose shader reads them so already is
// offered `batched`, and taken up it is one draw of them all.
TEST_F(EditorTest, ADrawOfEachElementIsOfferedBeingOne) {
    scene::MakeProject("editor");
    Open();
    scene::BuildGraph(Doc());
    scene::BuildEntities(Doc());
    EXPECT_TRUE(Shows("Nothing is wrong."));

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
    EXPECT_TRUE(Shows("Nothing is wrong."));
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

    // The project's shaders, for a pipeline to name: its modules, and the entry points of each.
    EXPECT_EQ(Session().ShaderModules(), (std::vector<std::string> { "show", "stamp" }));
    EXPECT_EQ(Session().EntryPoints("show"), (std::vector<std::string> { "vertexMain", "fragmentMain" }));
    EXPECT_EQ(Session().EntryPoints("stamp"), std::vector<std::string> { "computeMain" });
    EXPECT_TRUE(Session().EntryPoints("no such module").empty());

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
    EXPECT_TRUE(Shows("Dispatch   ·   Passes")) << "found by other words for it";
    EXPECT_FALSE(Shows("GenerateMipmaps") || Shows("Smoothstep")) << "and what does not fit is gone";
    Sheet("find");
    // Enter adds the first: it is on the graph where the click was, and the inspector is its.
    const auto nodes = Doc().Graph().nodes.size();
    editor->SceneInput().FeedKey(kor::Key::eEnter, true);
    Frame();
    editor->SceneInput().FeedKey(kor::Key::eEnter, false);
    for (int i = 0; i < 3; ++i) Frame();
    ASSERT_EQ(Doc().Graph().nodes.size(), nodes + 1);
    EXPECT_EQ(Doc().Graph().nodes.back().type, "Dispatch");
    EXPECT_EQ(Session().selectedNode, "dispatch");
    EXPECT_FALSE(Shows("Add a node")) << "and the dialog is gone";
    EXPECT_TRUE(Shows("dispatch: 'pipeline' needs a ComputePipeline wired in"));
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
    EXPECT_TRUE(Shows("Nothing is wrong."));
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
    for (const char* type : { "Sampler", "Buffer", "Primitive", "Number", "Vector", "Color" }) {
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
    EXPECT_TRUE(Shows("Nothing is wrong."));
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
    for (const char* field : { "intensity", "cone" }) EXPECT_TRUE(Shows(field)) << field << ": a port on its node, with nothing selected";

    Doc().Connect(lamps, "field.color", buffer, "field.color");
    Doc().Connect(lamps, "worldPosition", buffer, "field.position");
    EXPECT_TRUE(Shows("Nothing is wrong."));
    EXPECT_EQ(Doc().Runtime().ValueOf(buffer, "count"), kgraph::Value(1));
    Doc().AddComponent(Doc().CreateEntity("Another"), "PointLight");
    Frame();
    Frame();
    EXPECT_EQ(Doc().Runtime().ValueOf(buffer, "count"), kgraph::Value(2)) << "an element for each entity that has the component";
    Sheet("buffers");
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
    EXPECT_TRUE(Shows("cylinder (built in)"));
    Sheet("tree");

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
    EXPECT_TRUE(Shows("Nothing is wrong."));
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

    // The pipeline's component is what makes an entity drawn, and it is the shader's: what its push constants ask of
    // each entity. Where it is, which it is and which is picked are the engine's to say, and no fields of it.
    const kecs::ComponentId renderer = Doc().Runtime().ComponentOf("lit");
    ASSERT_NE(renderer, kecs::NoComponent);
    std::vector<std::string> fields;
    for (const auto& field : Doc().World().Describe(renderer).fields) fields.push_back(field.name);
    std::ranges::sort(fields);
    EXPECT_EQ(fields, (std::vector<std::string> { "albedo", "emission", "metallic", "roughness" }));
    EXPECT_EQ(Doc().World().Find("MeshRenderer"), kecs::NoComponent) << "there is no renderer but what a shader makes";
    EXPECT_EQ(Doc().World().Find("Spin"), kecs::NoComponent) << "what a thing does is a script's";
    EXPECT_EQ(Doc().Runtime().ComponentOf("sky"), kecs::NoComponent);

    const kecs::Entity sphere = keditor::AddLitScene(Doc());
    for (const char* name : { "Sky", "Camera", "Sun", "Sphere" }) EXPECT_TRUE(Shows(name)) << name;
    EXPECT_TRUE(Shows("Nothing is wrong."));
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

    // A second picture, made on a page of its own, and a Switch on the show page between it and the first.
    ASSERT_TRUE(Doc().AddPass("other"));
    ASSERT_EQ(Doc().AddNode("Image", 40, 40, "other picture", "other"), "other picture");
    Doc().SetInput("other picture", "format", "eRGBA8_UNORM");
    Doc().SetInput("other picture", "usage", kgraph::Value(kgraph::Value::List { "eTransferDst", "eTransferSrc", "eSampled" }));
    ASSERT_EQ(Doc().AddNode("Clear", 300, 40, "paint other", "other"), "paint other");
    ASSERT_TRUE(Doc().Connect("other picture", "out", "paint other", "image"));
    ASSERT_EQ(Doc().AddNode("Number", 40, 400, "branch", "show"), "branch");
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
    Sheet("flow");
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

    // Seen from the editor's own view until it is told to look through the scene's camera.
    for (int i = 0; i < 4; ++i) Frame();
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

    // The first pass's page, to begin with: its nodes, and none of another's.
    for (const char* pass : { "frame", "scene", "show", "Every node at once" }) EXPECT_TRUE(Shows(pass)) << pass;
    EXPECT_TRUE(Shows("camera"));
    EXPECT_TRUE(Shows("frame data"));
    EXPECT_FALSE(Shows("draw sky"));
    EXPECT_FALSE(Shows("scene target"));
    Session().ShowPass("sky");
    EXPECT_TRUE(Shows("draw sky"));
    EXPECT_TRUE(Shows("sky target"));
    // The scene pass's framebuffer is among what this page can use, in its panel — and not on the page itself.
    EXPECT_EQ(std::ranges::count(kui::debug::Texts(editor->Interface()), std::string("scene target")), 1);
    EXPECT_TRUE(Shows("Image, from scene")) << "what it took from an earlier pass says where from";
    EXPECT_TRUE(Shows("DataBuffer, from frame"));
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
    EXPECT_NE(std::ranges::find(importable, "frame data"), importable.end());
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
    EXPECT_FALSE(Shows("camera"));

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
TEST_F(EditorTest, SetupIsATabOfItsOwnAndWhatAPageCanUseIsBesideIt) {
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

    // A tab each.
    Session().ShowPass("noise");
    EXPECT_TRUE(Shows("Setup  1"));
    EXPECT_TRUE(Shows("MADE ONCE, BEFORE ANY FRAME"));
    EXPECT_FALSE(Shows("PASSES, AS THEY ARE DRAWN")) << "the passes of every frame are under the other tab";
    Session().ShowPass("show");
    EXPECT_TRUE(Shows("PASSES, AS THEY ARE DRAWN"));

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
    EXPECT_FALSE(Session().NodePicture("frame data").Get()) << "a buffer has none: what kind of thing it is, instead";
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
    Session().Look(heights);
    EXPECT_TRUE(Shows("Level 0  ·  64 × 64")) << "which level is looked at";
    EXPECT_TRUE(Shows("Filtering"));
    Sheet("look-level");
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
    ASSERT_TRUE(Session().NewLitScene());      // what a mesh's shader reads, the frame data, is the lit graph's
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
                    if (!whole) { settings.push_back({ "layout", "" }); settings.push_back({ "@param.frame", "frame data" }); }
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
    ASSERT_TRUE(Session().NewLitScene());      // what a mesh's shader reads, the frame data, is the lit graph's

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
    Doc().Connect("frame data", "out", paint, "param.frame");
    ASSERT_TRUE(Doc().Runtime().Status(paint)->ok) << Doc().Runtime().Status(paint)->error;
    const kecs::ComponentId component = Doc().Runtime().ComponentOf(paint);
    ASSERT_NE(component, kecs::NoComponent);
    EXPECT_EQ(Doc().World().Describe(component).fields.size(), 2u) << "with no draw of it yet, every push constant is the component's";
    // A draw of every entity with a Mesh and the component: where it is, from the ForEach; its colour, from the component.
    const std::string each = Doc().AddNode("ForEach", 0, 0, "painted", "scene");
    Doc().SetInput(each, "component", "paint");
    Doc().SetInput(each, "with", kgraph::Value::List { "Mesh" });
    const std::string draw = Doc().AddNode("Draw", 0, 0, "draw paint", "scene");
    Doc().Connect(paint, "out", draw, "pipeline");
    Doc().Connect(each, "Mesh.mesh", draw, "mesh");
    Doc().Connect(each, "matrix", draw, "push.model");
    Doc().Connect(each, "field.color", draw, "push.color");
    EXPECT_TRUE(Doc().Runtime().Status(draw)->ok) << Doc().Runtime().Status(draw)->error;
    ASSERT_EQ(Doc().World().Describe(component).fields.size(), 1u) << "where it is is wired from elsewhere: the colour is all that is left";
    EXPECT_EQ(Doc().World().Describe(component).fields.front().name, "color");
    const std::string invert = Doc().AddNode("GraphicsPipeline", 0, 0, "invert", "scene");
    Doc().SetInput(invert, "vertex.module", "invert");
    Doc().SetInput(invert, "fragment.module", "invert");
    Doc().Connect("sky target", "out", invert, "framebuffer");
    Doc().Connect("color", "out", invert, "param.source");
    EXPECT_TRUE(Doc().Runtime().Status(invert)->ok) << Doc().Runtime().Status(invert)->error;

    // With no scripts, this editor is all the project needs, and the player runs it.
    EXPECT_TRUE(Session().EditCommand().empty());
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
    EXPECT_NE(text(directory / "scripts", "Drifter.kt").find("class Drifter : Script()"), std::string::npos);
    for (const char* file : { "build.gradle.kts", "settings.gradle.kts", "gradle.properties", "gradlew" })
        EXPECT_TRUE(std::filesystem::exists(directory / file)) << file << ": a Kotlin script comes with the build that compiles it";
    EXPECT_NE(Session().RunCommand().find("gradlew"), std::string::npos) << "scripts in Kotlin are run by the project's build: " << Session().RunCommand();
    EXPECT_NE(Session().RunCommand().find(" play"), std::string::npos);
    EXPECT_NE(Session().EditCommand().find(" edit"), std::string::npos);
    // One in the other language, put there by hand, runs nowhere: it is said so.
    std::ofstream(directory / "scripts" / "Stray.cs") << "// not the project's language";
    EXPECT_EQ(Session().CurrentProject().ScriptsInAnotherLanguage().size(), 1u);
    EXPECT_TRUE(Shows("scripts/Stray.cs: the project's scripts are in Kotlin"));

    // This editor was not started with them: nothing runs them, and it says so rather than play a scene whose
    // scripts do nothing.
    EXPECT_EQ(Session().ScriptsNotRunning(), (std::vector<std::string> { "Drifter", "Roller" }));
    EXPECT_TRUE(Shows("Nothing is running the project's scripts (Drifter, Roller)"));
    Session().StartPlaying();
    EXPECT_TRUE(Session().Failed());
    EXPECT_TRUE(Shows("nothing runs Drifter and 1 more"));
    EXPECT_TRUE(Shows("Open with its scripts"));
    Session().Stop();
    // A new scene knows them too, as one loaded from its file does: an entity in it is given one.
    ASSERT_TRUE(Session().NewStandardScene()) << Session().Status();
    EXPECT_TRUE(Doc().AddComponent(Doc().World().Roots().front(), "Drifter")) << "a new scene forgot the project's scripts";
    EXPECT_FALSE(Session().NewScript("Roller", "Kotlin")) << "a script of that name is there";
    EXPECT_FALSE(Session().NewScript("default", "Kotlin")) << "a component is called that already";
    EXPECT_FALSE(Session().NewScript("My Script", "Kotlin"));
    EXPECT_FALSE(Session().NewScript("Thing", "Lua"));

    // Before any window, the editor's program goes to the runner of a project with scripts — not of one with none,
    // and not when that runner is what started it.
    {
        keditor::Project none;
        std::string error;
        std::filesystem::remove_all(directory.parent_path() / "assets-none");
        ASSERT_TRUE(keditor::Project::Create(directory.parent_path() / "assets-none", "none", none, error)) << error;
        EXPECT_FALSE(keditor::RunThroughScripts(none).has_value());
        setenv("KORAL_ENGINE_HANDED_OVER", directory.string().c_str(), 1);
        EXPECT_FALSE(keditor::RunThroughScripts(Session().CurrentProject()).has_value()) << "started by the runner it would go to";
        unsetenv("KORAL_ENGINE_HANDED_OVER");
        EXPECT_NE(keditor::ScriptRunner(Session().CurrentProject(), true).find(" edit"), std::string::npos);
    }

    // The editor's program hands a project with scripts over to their runner as it opens it, and closes.
    std::vector<std::string> ran;
    int quits = 0;
    keditor::Session::launch = [&](const std::string& command) { ran.push_back(command); return true; };
    keditor::Session::quit = [&] { ++quits; };
    unsetenv("KORAL_ENGINE_HANDED_OVER");
    Session().handOver = true;
    ASSERT_TRUE(Session().OpenProject(directory)) << Session().Status();
    Frame();
    ASSERT_EQ(ran.size(), 1u) << "handed over, between frames";
    EXPECT_NE(ran[0].find(" edit"), std::string::npos) << ran[0];
    EXPECT_EQ(quits, 1);
    // The editor the runner opens says it runs them, and keeps the project.
    Session().HostScripts(directory);
    ASSERT_TRUE(Session().OpenProject(directory)) << Session().Status();
    Frame();
    EXPECT_TRUE(Session().HostsScripts());
    EXPECT_EQ(ran.size(), 1u) << "it runs them: it is not handed over again";
    // A runner that does not say so is not handed the project again, round and round.
    Session().HostScripts({});
    ASSERT_TRUE(Session().OpenProject(directory)) << Session().Status();
    Frame();
    EXPECT_EQ(ran.size(), 1u) << "this editor was handed it already";
    unsetenv("KORAL_ENGINE_HANDED_OVER");
    Session().handOver = false;
    keditor::Session::launch = {};
    keditor::Session::quit = {};

    // A project whose scripts are in C#: run by the C# runner, and Kotlin refused.
    const auto csharp = directory.parent_path() / "assets-csharp";
    std::filesystem::remove_all(csharp);
    ASSERT_TRUE(Session().NewProject(csharp)) << Session().Status();
    ASSERT_TRUE(Session().NewScript("Roller", "C#")) << Session().Status();
    EXPECT_NE(text(csharp / "scripts", "Roller.cs").find("public sealed class Roller : Script"), std::string::npos);
    EXPECT_EQ(text(csharp / "scripts", "Roller.cs").find("$NAME"), std::string::npos);
    EXPECT_NE(Session().RunCommand().find("koral-engine-dotnet"), std::string::npos) << "scripts in C# are run by their runner: " << Session().RunCommand();
    EXPECT_NE(Session().EditCommand().find("--edit"), std::string::npos);
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
    ASSERT_TRUE(Session().GiveAsset("script:Follower.kt", camera)) << Session().Status();
    EXPECT_TRUE(Doc().Field(camera, "Follower", "target").IsNull()) << "none, not the first entity there is";
    Session().Select(camera);
    EXPECT_TRUE(Shows("(nothing)"));
    ASSERT_TRUE(Doc().SetField(camera, "Follower", "target", keditor::EntityValue(sphere)));
    EXPECT_TRUE(Shows("Sphere"));
    EXPECT_EQ(Doc().Field(camera, "Follower", "target"), keditor::EntityValue(sphere));

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
    Doc().AddNode("Number", 0, 0, "n", "main");
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

    ASSERT_TRUE(Session().OpenFunction(call)) << Session().Status();
    EXPECT_TRUE(Shows("Function Triple is open"));
    EXPECT_TRUE(Shows("Discard"));
    ASSERT_TRUE(Doc().SetInput(call + "/times", "b", 5));
    ASSERT_TRUE(Session().CloseFunction(true)) << Session().Status();
    EXPECT_FALSE(Shows("Function Triple is open"));
    EXPECT_EQ(Doc().Runtime().ValueOf("show"), kgraph::Value(11));
    EXPECT_EQ(Session().selectedNode, call);
    // Saved while open, it is closed first, keeping what was done: its page is no part of the scene.
    ASSERT_TRUE(Session().OpenFunction(call));
    ASSERT_TRUE(Session().Save()) << Session().Status();
    EXPECT_FALSE(Doc().Opened().has_value());
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

// Not a test: gives the project at KEDITOR_WRITE_PROJECT the build its Kotlin scripts need.
TEST_F(EditorTest, DISABLED_WritesAKotlinBuild) {
    const char* directory = std::getenv("KEDITOR_WRITE_PROJECT");
    ASSERT_NE(directory, nullptr);
    keditor::Project project;
    std::string error;
    ASSERT_TRUE(keditor::Project::Open(directory, project, error)) << error;
    for (const auto& file : keditor::WriteKotlinBuild(project.directory, project.name)) std::printf("wrote %s\n", file.c_str());
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
    EXPECT_TRUE(Shows("Nothing is wrong."));
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
    EXPECT_EQ(first("run compute"), "Dispatch");
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

    // Dragged onto the scene, it is: entities of it, now.
    ASSERT_TRUE(Session().ImportModel(models / "crate" / "crate.obj")) << Session().Status();
    EXPECT_GT(Doc().World().EntityCount(), entities);
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
    Session().Select(Doc().Import("frame data", "show", 0, 0));
    for (int i = 0; i < 30; ++i) Frame();
    ASSERT_NE(Doc().Graph().Find(Session().selectedNode), nullptr);
    EXPECT_EQ(Doc().Graph().Find(Session().selectedNode)->type, "Reference");
}

// How long a frame of the editor takes while a scene plays, nothing selected and then a body selected: KEDITOR_BENCH
// names the project, whose start scene is played. Not run by default.
TEST_F(EditorTest, DISABLED_PlayingWithABodySelectedTakesNoLongerThanWithNothing) {
    const char* named = std::getenv("KEDITOR_BENCH");
    if (!named) GTEST_SKIP() << "KEDITOR_BENCH names the project";
    Open(named);
    const int frames = std::getenv("KEDITOR_BENCH_FRAMES") ? std::atoi(std::getenv("KEDITOR_BENCH_FRAMES")) : 300;
    Session().StartPlaying();
    for (int i = 0; i < 120; ++i) Frame();
    const auto time = [&] {
        const auto versions = Session().Version();
        const auto began = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) Frame();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / frames;
        return std::pair { ms, Session().Version() - versions };
    };
    // Nothing, something still (the sun: what having a selection costs, its outline, without its inspector built again),
    // a body: in turn, round after round, so that the scene settling as it plays (cheaper as bodies sleep) falls on each
    // alike.
    kecs::Entity body = kecs::NullEntity;
    Doc().World().EachEntity([&](const kecs::Entity e) { if (body.IsNull() && Doc().World().Has(e, Doc().World().Find("RigidBody"))) body = e; });
    const kecs::Entity picks[3] { kecs::NullEntity, Doc().World().FindEntity("Sun"), body };
    double total[3] {};
    std::uint64_t rebuilt[3] {};
    const int rounds = 6;
    for (int round = 0; round < rounds; ++round)
        for (int k = 0; k < 3; ++k) {
            Session().Select(picks[k]);
            for (int i = 0; i < 5; ++i) Frame();
            const auto [ms, versions] = time();
            total[k] += ms;
            rebuilt[k] += versions;
        }
    std::printf("            playing, nothing selected: %.3f ms a frame; the sun selected: %.3f; a body selected: %.3f (interface made again %llu, %llu, %llu times), %d rounds of %d frames\n",
                total[0] / rounds, total[1] / rounds, total[2] / rounds, static_cast<unsigned long long>(rebuilt[0]),
                static_cast<unsigned long long>(rebuilt[1]), static_cast<unsigned long long>(rebuilt[2]), rounds, frames);

    // Not playing, the body picked and dragged, as the gizmo drags it: moved, and the document told, every frame.
    Session().Stop();
    for (int i = 0; i < 10; ++i) Frame();
    body = kecs::NullEntity;
    Doc().World().EachEntity([&](const kecs::Entity e) { if (body.IsNull() && Doc().World().Has(e, Doc().World().Find("RigidBody"))) body = e; });
    Session().Select(body);
    for (int i = 0; i < 5; ++i) Frame();
    const auto [held, rebuiltHeld] = time();
    const auto versions = Session().Version();
    const auto began = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        Doc().World().SetPosition(body, { 0.01f * static_cast<float>(i), 2.f, 0.f });
        Doc().Moved(body, "position");
        Frame();
    }
    const double dragging = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / frames;
    std::printf("            stopped, a body picked: %.3f ms a frame; dragged: %.3f (interface made again %llu times while dragged)\n",
                held, dragging, static_cast<unsigned long long>(Session().Version() - versions));
    (void)rebuiltHeld;
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
        EXPECT_EQ(it->folder, "Meshes/Primitives");
        EXPECT_TRUE(it->builtIn);
    }
    EXPECT_EQ(std::ranges::count(assets, keditor::Session::Asset::Kind::eModel, &keditor::Session::Asset::kind), 0) << "a default mesh is of no model";
    const auto folders = Session().AssetFolders(assets);
    EXPECT_TRUE(std::ranges::contains(folders, "Meshes/Primitives"));
    EXPECT_TRUE(std::ranges::contains(folders, "Materials/Defaults"));
    EXPECT_TRUE(Session().AssetFolderPath("Meshes/Primitives").empty()) << "of no folder on disk";
    const auto gold = std::ranges::find(assets, "material:Gold", &keditor::Session::Asset::id);
    ASSERT_NE(gold, assets.end());
    EXPECT_EQ(gold->folder, "Materials/Defaults");

    // Dropped on an entity that is drawn, what draws it is given the material — the standard graph's, its colour; on
    // one that is not, it is given that first. A light's colour is no material's.
    kecs::World& world = Doc().World();
    kecs::Entity drawn = kecs::NullEntity;
    world.EachEntity([&](const kecs::Entity e) { if (drawn.IsNull() && world.Has(e, world.Find("default"))) drawn = e; });
    ASSERT_FALSE(drawn.IsNull());
    ASSERT_TRUE(Session().GiveAsset("material:Gold", drawn));
    EXPECT_EQ(*Doc().Field(drawn, "default", "color").Items(), (kgraph::Value::List { 1.f, 0.78f, 0.34f }));
    const kecs::Entity sun = world.FindEntity("Sun");
    ASSERT_TRUE(Session().GiveAsset("material:Gold", sun));
    EXPECT_TRUE(std::ranges::contains(Doc().ComponentsOf(sun), std::string("default"))) << "given what draws, not its light coloured";
    EXPECT_EQ(*Doc().Field(sun, "DirectionalLight", "color").Items(), (kgraph::Value::List { 1.f, 0.96f, 0.9f }));
    const kecs::Entity bare = Doc().CreateEntity("Bare");
    ASSERT_TRUE(Session().GiveAsset("$torus", bare));
    ASSERT_TRUE(Session().GiveAsset("material:Red plastic", bare));
    EXPECT_TRUE(std::ranges::contains(Doc().ComponentsOf(bare), std::string("default")));
    EXPECT_EQ(*Doc().Field(bare, "default", "color").Items(), (kgraph::Value::List { 0.8f, 0.1f, 0.08f }));
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
    const kecs::Entity body = Doc().CreateEntity("Body");
    ASSERT_TRUE(Doc().AddComponent(body, "RigidBody"));
    ASSERT_TRUE(Doc().AddComponent(body, "Collider"));
    ASSERT_TRUE(Doc().SetField(body, "Collider", "shape", "Capsule"));
    Session().Select(body);
    EXPECT_TRUE(Shows("Dynamic")) << "its kind, by name";
    EXPECT_TRUE(Shows("Capsule")) << "its shape, by name";

    // Its collider shown, and being changed, in the scene's view: every shape, a mesh's by its edges.
    Session().ShowCollider(true, true);
    ASSERT_TRUE(Doc().AddComponent(body, "Mesh"));
    ASSERT_TRUE(Doc().SetField(body, "Mesh", "mesh", "$cube"));
    for (const char* shape : { "Sphere", "Box", "Capsule", "Plane", "Cone", "Mesh" }) {
        ASSERT_TRUE(Doc().SetField(body, "Collider", "shape", shape));
        for (int i = 0; i < 3; ++i) Frame();
    }
    EXPECT_TRUE(Session().colliderShown && Session().colliderEditing);
    Session().ShowCollider(false, false);
    EXPECT_FALSE(Session().colliderShown);
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
    EXPECT_TRUE(Shows("Fixed step")) << "its tab";
    EXPECT_TRUE(Shows("ON THE FIXED STEP, BEFORE THE FRAME"));

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
    for (const char* name : { "Scan", "RadixSort", "Compact", "IndirectArgs" }) {
        EXPECT_TRUE(std::ranges::contains(names, std::string(name))) << name;
        EXPECT_TRUE(Doc().IsEngineFunction(name)) << name;
    }
    kgraph::Graph graph = Doc().Graph();
    graph.Add("numbers", "Buffer").Set("instanceCount", 4096).Set("usage", kgraph::Value::List { "eStorage", "eTransferSrc", "eTransferDst" });
    graph.Add("sums", "Function").Set("function", "Scan").Wire("values", "numbers").Set("count", 1024);
    Doc().SetGraph(graph);
    for (int i = 0; i < 3; ++i) Frame();
    ASSERT_NE(Doc().Runtime().Status("sums"), nullptr);
    EXPECT_TRUE(Doc().Runtime().Status("sums")->ok) << Doc().Runtime().Status("sums")->error;

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

    Click({ 1009.f, 90.f });       // the grid's settings, beside the grid
    ASSERT_TRUE(Shows("Behind things"));
    for (int i = 0; i < 10; ++i) Frame();
    EXPECT_TRUE(Shows("Behind things")) << "open still, the editor built again under it";
    Sheet("grid settings");

    Click({ 1164.f, 180.f });      // XY
    EXPECT_EQ(Session().SceneGrid().plane, keditor::Grid::Plane::eXY);
    EXPECT_TRUE(Shows("Behind things")) << "a setting changed leaves it open";
    const float cell = Session().SceneGrid().cell;
    auto& input = editor->SceneInput();
    input.FeedMousePosition({ 1170.f, 215.f });
    Frame();
    input.FeedMouseButton(kor::MouseButton::eLeft, true);
    for (int x = 1170; x <= 1250; x += 10) { input.FeedMousePosition({ static_cast<float>(x), 215.f }); Frame(); }
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
