// The editor's interface, offscreen: the panels over a scene, showing what the document holds and following it as it
// changes. With KEDITOR_SHEET set to a file's path, the editor as it looks with the tests' scene in it is written
// there (a PPM), to be looked at.

#include <gtest/gtest.h>

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
    Click({ 530.f, 198.f });
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
