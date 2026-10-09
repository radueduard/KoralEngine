// The editor's interface, offscreen: the panels over a scene, showing what the document holds and following it as it
// changes. With KEDITOR_SHEET set to a file's path, the editor as it looks with the tests' scene in it is written
// there (a PPM), to be looked at.

#include <gtest/gtest.h>

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
    for (const char* input : { "pipeline", "cover", "groupSize", "with" }) EXPECT_TRUE(Shows(input)) << input;
    EXPECT_FALSE(Shows("groupSize.x")) << "the parts of one thing are one line, under its name";
    EXPECT_FALSE(Shows("without")) << "what it can do without, and was not given, is not in the way";
    EXPECT_TRUE(Shows("more it can be given")) << "and is there to be asked for";
    EXPECT_TRUE(Shows("Lit")) << "the components it asks for, to tick";
    EXPECT_TRUE(Shows("from stamp")) << "what an input is wired to";
    Session().Select(std::string("stamp"));
    EXPECT_TRUE(Shows("compute.module")) << "a pipeline names its shader";
    EXPECT_TRUE(Shows("computeMain")) << "and the entry point in it";
    EXPECT_TRUE(Shows("param.backdrop")) << "a pipeline's inputs are its shader's";
    EXPECT_TRUE(Shows("Entities run it by having the component 'stamp'."));
    Session().Select(std::string("canvas"));
    EXPECT_TRUE(Shows("eRGBA8_UNORM")) << "an enumeration, by name";
    EXPECT_TRUE(Shows("eColorAttachment")) << "flags, each to tick";
    EXPECT_TRUE(Shows("extent")) << "its size, on one line";
    EXPECT_FALSE(Shows("mipLevels")) << "an image is made with its format, size and usage: the rest is asked for";
    Session().Show("canvas", "mipLevels");
    EXPECT_TRUE(Shows("mipLevels"));
    EXPECT_TRUE(Doc().Runtime().Ok()) << "asked for, it is shown: nothing is given until it is set";
    EXPECT_FALSE(Doc().Graph().Find("canvas")->inputs.contains("mipLevels"));
    Session().Hide("canvas", "mipLevels");
    EXPECT_FALSE(Shows("mipLevels"));

    // A graphics pipeline is drawn one of two ways, and shows the inputs of the one it is.
    Session().Select(std::string("show"));
    for (const char* input : { "stages", "eVertex", "vertex.module", "vertex.layout", "fragment.entry", "framebuffer", "param.source" }) EXPECT_TRUE(Shows(input)) << input;
    EXPECT_FALSE(Shows("geometry.module") || Shows("rasterizationState") || Shows("mesh.module"));
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

    // The button over the list makes one, and picks it.
    Click({ 85.f, 85.f });
    ASSERT_EQ(Doc().Entities().size(), 5u);
    EXPECT_EQ(Session().selectedEntity, Doc().Entities().back());
    EXPECT_TRUE(Shows("Entity")) << "its name, to change";

    // A node on the graph: picked instead.
    Click({ 741.f, 198.f });   // the graph starts after the list of passes
    EXPECT_EQ(Session().selectedNode, "stamp");
    EXPECT_TRUE(Session().selectedEntity.IsNull());
    EXPECT_TRUE(Shows("Entities run it by having the component 'stamp'."));

    // A right click on the graph where nothing is: every kind of node, to add one there.
    RightClick({ 880.f, 300.f });
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
    // The Entity menu, at the top of the window: its first line makes one, and picks it.
    ASSERT_TRUE(Shows("Entity"));
    const auto before = Doc().Entities().size();
    Session().Select(Doc().CreateEntity("Made by hand"));
    EXPECT_TRUE(Shows("Made by hand"));
    EXPECT_TRUE(Shows("Destroy the entity")) << "picked: the inspector is its";
    EXPECT_EQ(Doc().Entities().size(), before + 1);
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
    Session().Show("draw", "with");
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

// An image's usage is ticked a flag at a time, in a scene that is running, with nothing waiting between frames. Some
// of what that passes through cannot be made, or cannot be used as it is wired: each is said on a node, and none of
// it reaches the GPU. Ticking eTransferSrc alone on a new image once took the program down.
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

    // A new image, with nothing set but one flag ticked, then unticked.
    const auto fresh = Doc().AddNode("Image", 0, 0, "fresh");
    Doc().SetInput(fresh, "usage", Value::List { "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Status(fresh)->ok) << error("fresh");
    Doc().SetInput(fresh, "usage", Value::List {});
    frames(3);
    EXPECT_NE(error("fresh").find("at least one usage"), std::string::npos) << error("fresh");
    Doc().SetInput(fresh, "extent.x", 16);
    Doc().SetInput(fresh, "extent.y", 16);
    Doc().SetInput(fresh, "usage", Value::List { "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Status(fresh)->ok) << "a size across and down is a size: " << error("fresh");

    // The image a framebuffer draws into, without the usage that takes: the framebuffer says so, and nothing draws.
    Doc().SetInput("canvas", "usage", Value::List { "eTransferSrc" });
    frames(3);
    EXPECT_NE(error("target").find("eColorAttachment"), std::string::npos) << error("target");
    EXPECT_FALSE(Doc().Runtime().Status("draw")->ok);
    Doc().SetInput("canvas", "usage", Value::List { "eColorAttachment", "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Ok()) << "ticked again, it all runs";

    // What a shader samples, and what it writes, likewise: on the pipeline, or on the entity that supplies it.
    Doc().SetInput("backdrop", "usage", Value::List { "eTransferDst" });
    frames(3);
    EXPECT_NE(error("stamp").find("eSampled"), std::string::npos) << error("stamp");
    Doc().SetInput("backdrop", "usage", Value::List { "eSampled", "eTransferDst" });
    Doc().SetInput("image 0", "usage", Value::List { "eSampled", "eTransferSrc" });
    frames(3);
    EXPECT_TRUE(Doc().Runtime().Ok());
    const auto* report = Doc().Runtime().ReportOf("run");
    EXPECT_EQ(report->skipped, 1u) << "the entity whose image cannot be written is passed by";
    EXPECT_NE(report->why.find("eStorage"), std::string::npos) << report->why;
    Doc().SetInput("image 0", "usage", Value::List { "eStorage", "eSampled", "eTransferSrc" });
    frames(3);
    EXPECT_EQ(Doc().Runtime().ReportOf("run")->skipped, 0u);

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
    EXPECT_EQ(inputs.at("usage").value, kgraph::Value(kgraph::Value::List { "eSampled", "eTransferSrc", "eTransferDst" }));
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

    // What it gives besides the image: asked for, it is on the node, and the inspector no longer offers it.
    Session().Select(image);
    EXPECT_TRUE(Shows("Add output"));
    EXPECT_FALSE(Shows("width")) << "none of it is in the way until it is wanted";
    Session().Show(image, "out:width");
    EXPECT_TRUE(Shows("width"));
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

    // An output asked for is in the inspector, to be taken off; one wired from says so, and takes its wires with it.
    Session().Select(image);
    EXPECT_FALSE(Shows("an output"));
    Session().Show(image, "out:extent");
    EXPECT_TRUE(Shows("an output: wire from it"));
    Doc().Connect(image, "extent", over, "a");
    Doc().Connect(image, "extent", run, "groupSize");
    EXPECT_TRUE(Shows("an output, wired to 2 inputs"));
    EXPECT_EQ(Doc().DisconnectOutput(image, "extent"), 2u);
    EXPECT_FALSE(Doc().Graph().Find(over)->inputs.contains("a"));
    EXPECT_FALSE(Doc().Graph().Find(run)->inputs.contains("groupSize"));
    EXPECT_TRUE(Shows("an output: wire from it")) << "still asked for";
    Session().Hide(image, "out:extent");
    EXPECT_FALSE(Shows("an output"));
    EXPECT_EQ(Doc().DisconnectOutput(image, "extent"), 0u);

    // The whole of an input in parts is one more it can be given; given, its parts are gone from the node and the panel.
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
    EXPECT_EQ(Doc().Runtime().ReportOf(draw), nullptr) << "it draws once: no entity has anything to give it";

    // The draw says the rest when asked: how many vertices, where, over what.
    Session().Select(draw);
    EXPECT_TRUE(Shows("more it can be given"));
    EXPECT_FALSE(Shows("vertexCount"));
    Session().Show(draw, "vertexCount");
    EXPECT_TRUE(Shows("vertexCount"));
    Doc().SetInput(draw, "scissor", kgraph::Value::List { 0, 0, 256, 512 });
    EXPECT_TRUE(Shows("scissor"));
    Sheet("triangle");
    Doc().SetInput(draw, "cullMode", "eBack");
    EXPECT_TRUE(Doc().Runtime().Status(draw)->ok) << Doc().Runtime().Status(draw)->error;
    EXPECT_TRUE(Shows("eBack"));
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
    for (const char* row : { "Position", "Rotation", "Scale", "Under 'Ship'." }) EXPECT_TRUE(Shows(row)) << row;
    EXPECT_TRUE(Shows("45.00"));
    EXPECT_TRUE(Doc().ComponentsOf(turret).empty()) << "none of it is a component";
    const kecs::Mat4 world = Doc().World().WorldMatrix(turret);
    EXPECT_NEAR(world.At(3, 0), 10.f, 1e-4f);
    EXPECT_NEAR(world.At(3, 1), 2.f, 1e-4f);

    // Moved to the top, it stays where it is in the world; and nothing goes under what is under it.
    EXPECT_FALSE(Doc().SetParent(ship, barrel));
    EXPECT_TRUE(Doc().SetParent(turret, kecs::NullEntity));
    EXPECT_NEAR(Doc().World().PositionOf(turret).x, 10.f, 1e-4f);
    EXPECT_FALSE(Shows("Under 'Ship'."));
    EXPECT_EQ(Doc().World().ParentOf(barrel), turret) << "what was under it went with it";
    EXPECT_TRUE(Doc().SetParent(turret, ship, 0));

    // A Mesh is one of the default meshes, without a node for any.
    Doc().AddComponent(barrel, "Mesh");
    std::string error;
    EXPECT_TRUE(Doc().SetField(barrel, "Mesh", "mesh", kgraph::Value("$cylinder"), &error)) << error;
    Session().Select(barrel);
    EXPECT_TRUE(Shows("$cylinder"));
    Sheet("tree");

    // Deleted, what is under it goes too.
    Doc().DestroyEntity(ship);
    EXPECT_EQ(Doc().Entities(), std::vector<kecs::Entity> { rock });
    EXPECT_FALSE(Shows("Barrel"));
}

// The scene a project starts from: meshes lit under a sky, by nodes alone and the four standard shaders.
TEST_F(EditorTest, TheStandardSceneDrawsLitMeshesUnderASky) {
    const auto project = scene::MakeProject("standard");
    EXPECT_EQ(keditor::WriteStandardShaders(project.ShaderDirectory()).size(), 4u);
    EXPECT_TRUE(keditor::WriteStandardShaders(project.ShaderDirectory()).empty()) << "one that is there is left as it is";
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddStandardGraph(Doc(), error)) << error;
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
    EXPECT_EQ(Doc().Runtime().ComponentOf("outline"), kecs::NoComponent) << "given its colour and thickness on the node, the outline runs once";
    EXPECT_EQ(Doc().Runtime().ComponentOf("sky"), kecs::NoComponent);

    const kecs::Entity sphere = keditor::AddStandardScene(Doc());
    for (const char* name : { "Sky", "Camera", "Sun", "Sphere" }) EXPECT_TRUE(Shows(name)) << name;
    EXPECT_TRUE(Shows("Nothing is wrong."));
    ASSERT_NE(Doc().Runtime().ReportOf("draw scene"), nullptr);
    EXPECT_EQ(Doc().Runtime().ReportOf("draw scene")->ran, 1u) << Doc().Runtime().ReportOf("draw scene")->why;
    Session().Select(sphere);
    Sheet("standard");
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
    EXPECT_TRUE(std::filesystem::exists(directory / "shaders" / "lit.slang")) << "the standard shaders are the project's own, to edit";
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
    EXPECT_EQ(Doc().Runtime().CurrentView().view, kgraph::ViewOf(world, world.FindEntity("Camera")).view);
    Session().SetLookThroughCamera(false);
    Session().Frame(sphere);
    EXPECT_NEAR(Session().camera.distance, 0.87f * 1.2f / std::sin(30.f * 3.14159265f / 180.f), 1e-3f) << "far enough to see all of it";
    Sheet("looked");

    // A click on the picture picks what is drawn there: the sphere in the middle, nothing in a corner.
    kecs::Entity picked = kecs::NullEntity;
    bool answered = false;
    ASSERT_TRUE(Doc().Runtime().Pick(0.5f, 0.5f, [&](const kecs::Entity entity) { picked = entity; answered = true; }));
    for (int i = 0; i < 8; ++i) Frame();
    EXPECT_TRUE(answered);
    EXPECT_EQ(picked, sphere);
    answered = false;
    Doc().Runtime().Pick(0.02f, 0.02f, [&](const kecs::Entity entity) { picked = entity; answered = true; });
    for (int i = 0; i < 8; ++i) Frame();
    EXPECT_TRUE(answered);
    EXPECT_TRUE(picked.IsNull()) << "the sky is nobody";

    // Nothing plays until it is told to. Played, the scene is seen through its own camera and is not saved; stopped,
    // it is as it was when it began, and what was picked still is.
    EXPECT_FALSE(Session().IsPlaying());
    Session().StartPlaying();
    EXPECT_TRUE(Session().InPlayMode());
    EXPECT_TRUE(Session().IsPlaying());
    EXPECT_TRUE(Session().ThroughSceneCamera()) << "played, it is seen through its own camera";
    EXPECT_FALSE(Session().Save()) << "a moment of it is not what is saved";
    for (int i = 0; i < 3; ++i) Frame();
    world.SetRotation(world.FindEntity("Sphere"), { 0, 40, 0 });   // what a script would do as it plays
    Doc().SetField(world.FindEntity("Sphere"), "lit", "metallic", kgraph::Value(1));
    Session().Pause();
    EXPECT_FALSE(Session().IsPlaying());
    EXPECT_TRUE(Session().InPlayMode()) << "paused, it is still a moment of the play";
    Session().Stop();
    EXPECT_FALSE(Session().InPlayMode());
    const kecs::Entity again = world.FindEntity("Sphere");
    EXPECT_EQ(world.RotationOf(again), (kecs::Vec3 {}));
    EXPECT_EQ(Doc().Field(again, "lit", "metallic"), kgraph::Value(0));
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
    EXPECT_EQ(*Doc().Field(planet, "lit", "albedo").Items(), (kgraph::Value::List { 0.3f, 0.55f, 1 })) << "what the object says of the shader's component";
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
    for (const char* field : { "albedo", "metallic", "roughness", "emission" }) EXPECT_TRUE(Shows(field)) << field;
    Sheet("features");

    // Saved, opened again: all of it is there.
    ASSERT_TRUE(Session().Save());
    ASSERT_TRUE(Session().OpenProject(directory));
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    const kecs::Entity reopened = Doc().World().FindEntity("Lit Planet 2");
    ASSERT_FALSE(reopened.IsNull());
    EXPECT_EQ(Doc().Field(reopened, "lit", "roughness"), kgraph::Value(0.5));
    EXPECT_EQ(Doc().Field(reopened, "Mesh", "mesh"), kgraph::Value("$sphere"));
}

// The graph is in passes, a page each: only one page's nodes are in the way at a time, and what a pass uses of the
// passes before it is brought in.
TEST_F(EditorTest, TheGraphIsAPageAPass) {
    const auto project = scene::MakeProject("passes");
    keditor::WriteStandardShaders(project.ShaderDirectory());
    Open();
    std::string error;
    ASSERT_TRUE(keditor::AddStandardGraph(Doc(), error)) << error;
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(Doc().Passes(), (std::vector<std::string> { "frame", "scene", "sky", "outline", "editor", "show" }));

    // The first pass's page, to begin with: its nodes, and none of another's.
    for (const char* pass : { "1. frame", "2. scene", "6. show", "Every node at once" }) EXPECT_TRUE(Shows(pass)) << pass;
    EXPECT_TRUE(Shows("camera"));
    EXPECT_TRUE(Shows("frame data"));
    EXPECT_FALSE(Shows("draw sky"));
    EXPECT_FALSE(Shows("overlay target"));
    Session().ShowPass("sky");
    EXPECT_TRUE(Shows("draw sky"));
    EXPECT_TRUE(Shows("sky target"));
    EXPECT_FALSE(Shows("overlay target"));
    EXPECT_TRUE(Shows("ImageView, from scene")) << "what it took from an earlier pass says where from";
    EXPECT_TRUE(Shows("DataBuffer, from frame"));
    Sheet("pass");
    Session().ShowEveryPass();
    EXPECT_TRUE(Shows("overlay target"));
    EXPECT_TRUE(Shows("draw sky"));

    // Going to a node turns to its page.
    Session().ShowPass("frame");
    Session().GoTo("draw outline");
    EXPECT_EQ(Session().Pass(), "outline");
    EXPECT_TRUE(Shows("overlay target"));

    // A new pass: something made in it, and something brought in from before it.
    ASSERT_TRUE(Doc().AddPass("bloom", 3));
    EXPECT_FALSE(Doc().AddPass("bloom"));
    EXPECT_EQ(Doc().Passes()[3], "bloom");
    const auto importable = Doc().Importable("bloom");
    EXPECT_NE(std::ranges::find(importable, "color"), importable.end()) << "what the scene pass made";
    EXPECT_NE(std::ranges::find(importable, "frame data"), importable.end());
    EXPECT_EQ(std::ranges::find(importable, "overlay target"), importable.end()) << "not what a later pass makes";
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
    EXPECT_TRUE(Doc().MovePass("glow", 5));
    EXPECT_EQ(Doc().Passes().back(), "show") << "moved to where the last one was, which is now after it";
    EXPECT_EQ(Doc().Passes()[5], "glow");
    EXPECT_TRUE(Doc().SetNodePass(blurred, "show"));
    EXPECT_TRUE(Doc().RemovePass("glow"));
    EXPECT_EQ(Doc().Graph().Find(copy), nullptr);
    EXPECT_NE(Doc().Graph().Find(blurred), nullptr) << "moved to another pass before, it stays";
    EXPECT_EQ(Doc().Passes().size(), 6u);
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;

    // Saved and read back, the passes are as they were.
    const std::string saved = Doc().ToJson();
    EXPECT_NE(saved.find("\"passes\": [\"frame\", \"scene\""), std::string::npos);
    EXPECT_NE(saved.find("\"pass\": \"sky\""), std::string::npos);
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
    ASSERT_TRUE(Session().NewStandardScene());

    for (const auto& [kind, what] : keditor::Session::ShaderKinds()) EXPECT_FALSE(what.empty()) << kind;
    ASSERT_TRUE(Session().NewShader("glow", "Compute")) << Session().Status();
    ASSERT_TRUE(Session().NewShader("paint", "Mesh")) << Session().Status();
    ASSERT_TRUE(Session().NewShader("invert", "Fullscreen")) << Session().Status();
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
    Doc().SetInput(paint, "vertex.module", "paint");
    Doc().SetInput(paint, "vertex.entry", "vertex");
    Doc().SetInput(paint, "fragment.module", "paint");
    Doc().SetInput(paint, "fragment.entry", "fragment");
    Doc().Connect("shape", "layout", paint, "vertex.layout");
    Doc().Connect("sky target", "out", paint, "framebuffer");
    Doc().Connect("frame data", "out", paint, "param.frame");
    ASSERT_TRUE(Doc().Runtime().Status(paint)->ok) << Doc().Runtime().Status(paint)->error;
    const kecs::ComponentId component = Doc().Runtime().ComponentOf(paint);
    ASSERT_NE(component, kecs::NoComponent);
    ASSERT_EQ(Doc().World().Describe(component).fields.size(), 1u) << "the shader's own push constant is the component's field";
    EXPECT_EQ(Doc().World().Describe(component).fields.front().name, "color");
    const std::string invert = Doc().AddNode("GraphicsPipeline", 0, 0, "invert", "scene");
    Doc().SetInput(invert, "vertex.module", "invert");
    Doc().SetInput(invert, "vertex.entry", "vertex");
    Doc().SetInput(invert, "fragment.module", "invert");
    Doc().SetInput(invert, "fragment.entry", "fragment");
    Doc().Connect("overlay target", "out", invert, "framebuffer");
    Doc().Connect("ids", "out", invert, "param.source");
    EXPECT_TRUE(Doc().Runtime().Status(invert)->ok) << Doc().Runtime().Status(invert)->error;

    // A script, in either language: a file with a class of its name.
    ASSERT_TRUE(Session().NewScript("Roller", "C#")) << Session().Status();
    ASSERT_TRUE(Session().NewScript("Drifter", "Kotlin")) << Session().Status();
    const auto text = [&](const char* file) {
        std::ifstream in(directory / "scripts" / file);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    EXPECT_NE(text("Roller.cs").find("public sealed class Roller : Script"), std::string::npos);
    EXPECT_NE(text("Drifter.kt").find("class Drifter : Script()"), std::string::npos);
    EXPECT_EQ(text("Roller.cs").find("$NAME"), std::string::npos);
    for (const char* file : { "build.gradle.kts", "settings.gradle.kts", "gradle.properties", "gradlew" })
        EXPECT_TRUE(std::filesystem::exists(directory / file)) << file << ": a Kotlin script comes with the build that compiles it";
    EXPECT_FALSE(Session().NewScript("Roller", "Kotlin")) << "a script of that name is there, in whichever language";
    EXPECT_FALSE(Session().NewScript("lit", "C#")) << "a component is called that already";
    EXPECT_FALSE(Session().NewScript("My Script", "C#"));
    EXPECT_FALSE(Session().NewScript("Thing", "Lua"));
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
