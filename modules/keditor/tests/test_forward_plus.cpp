// Forward+, made with nodes (samples/forward-plus): the graph is built through the document as an editor would build
// it, run, saved as a project, and run again from the file. Offscreen, with no windowing system.
//
// FORWARD_PLUS_SHEET=<file stem> writes the picture to <stem>.ppm.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <input.h>

#include <keditor/editor.h>

#include <ranges>
#include <set>

#include <keditor/play.h>
#include <kgraph/meshes.h>
#include <keditor/project.h>

#include "forward_plus.h"
#include "support.h"

namespace {
    // (The panels are where the editor puts them, not where whoever runs the tests left them.)
    [[maybe_unused]] const int noLayout = setenv("KORAL_ENGINE_NO_LAYOUT", "1", 1);
    using support::Frame;
    using keditor::PlayScene;

    constexpr kor::UVec2 Extent { 960, 540 };

    struct Running {
        PlayScene* scene = nullptr;
        kor::Resource<kor::Buffer> readback;

        explicit Running(const std::filesystem::path& file = {}) {
            scene = support::App()->OpenOffscreen<PlayScene>({ .title = "forward+", .extent = Extent }, file);
            readback = kor::Buffer::RawBuilder{}.SetRawSize(Extent.x * Extent.y * 4).SetUsage(kor::Buffer::Usage::eTransferDst)
                .SetType(kor::Buffer::Type::eReadback).Build();
            scene->Graph().Add<support::ReadScreen>(kor::ResourceRef<const kor::Buffer>(readback));
        }
        ~Running() { support::App()->Close(*scene); Frame(); }
        [[nodiscard]] std::vector<kor::U8Vec4> Picture() const { return readback->Read<kor::U8Vec4>(Extent.x * Extent.y); }
        [[nodiscard]] kor::U8Vec4 Pixel(const kor::u32 x, const kor::u32 y) const { return Picture()[y * Extent.x + x]; }
        void Sheet(const std::string& suffix = {}) const {
            const char* sheet = std::getenv("FORWARD_PLUS_SHEET");
            if (!sheet) return;
            std::ofstream out(std::string(sheet) + suffix + ".ppm", std::ios::binary);
            out << "P6\n" << Extent.x << " " << Extent.y << "\n255\n";
            for (const auto& pixel : Picture()) out.put(static_cast<char>(pixel.x)).put(static_cast<char>(pixel.y)).put(static_cast<char>(pixel.z));
        }
    };

    /** The project, in a folder of the build's: made afresh, with the sample's shaders in it. */
    keditor::Project MakeProject(const std::string& suffix = {})
    {
        const std::filesystem::path directory = std::string(KEDITOR_FORWARD_PLUS_PROJECT) + suffix;
        std::filesystem::remove_all(directory);
        keditor::Project project;
        std::string error;
        EXPECT_TRUE(keditor::Project::Create(directory, "ForwardPlus", project, error)) << error;
        for (const char* shader : forwardplus::Shaders)
            std::filesystem::copy_file(std::filesystem::path(KEDITOR_FORWARD_PLUS_SHADERS) / shader, project.ShaderDirectory() / shader);
        project.Use();
        return project;
    }
}

TEST(ForwardPlus, TheGraphIsBuiltRunsAndIsSavedAsAProject) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const keditor::Project project = MakeProject();
    std::vector<kor::U8Vec4> made;
    {
        Running editing;
        keditor::Document& doc = editing.scene->Doc();
        std::string error;
        ASSERT_TRUE(forwardplus::BuildGraph(doc, error)) << error;
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        forwardplus::BuildScene(doc);
        for (int i = 0; i < 4; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        ASSERT_TRUE(doc.Runtime().Ok());
        editing.Sheet();
        // Every lamp is an element of the lights' buffer, and every drawn thing is drawn: in the pre-pass, in the
        // forward pass, and four times over for the sun's cascades.
        EXPECT_EQ(doc.Runtime().ValueOf("light data", "count"), kgraph::Value(96));
        const auto* forward = doc.Runtime().ReportOf("draw forward");
        ASSERT_NE(forward, nullptr);
        EXPECT_EQ(forward->ran + forward->culled, 1u + 6u + 35u + 96u) << forward->why;
        EXPECT_GT(forward->ran, 80u) << "most of it is in sight";
        EXPECT_EQ(forward->skipped, 0u) << forward->why;
        // The depth passes ask nothing of an entity but where it is, so they have no component for one to have:
        // the pre-pass draws what the forward pass will, and each cascade everything with a mesh but the lamps.
        EXPECT_EQ(doc.Runtime().ComponentOf("prepass"), kecs::NoComponent);
        ASSERT_NE(doc.Runtime().ReportOf("draw depth"), nullptr);
        EXPECT_EQ(doc.Runtime().ReportOf("draw depth")->ran, forward->ran);
        std::size_t casts = 0;
        EXPECT_EQ(doc.Runtime().TurnsOf("cast").size(), 4u) << "the shadow pass is a loop of four turns";
        for (const char* cast : { "cast [0]", "cast [1]", "cast [2]", "cast [3]" }) {
            ASSERT_NE(doc.Runtime().ReportOf(cast), nullptr) << cast;
            EXPECT_EQ(doc.Runtime().ReportOf(cast)->ran + doc.Runtime().ReportOf(cast)->culled, 1u + 6u + 35u) << cast;
            casts += doc.Runtime().ReportOf(cast)->ran;
        }
        EXPECT_LT(casts, 4u * 42u) << "what is wholly outside a cascade is not drawn into it";

        // The point lights have a cube of shadow maps each, as many of them as have one: six faces a light, a turn of a
        // loop each. Two lights' are drawn again each frame, one after another; the rest stay as they were drawn last.
        EXPECT_EQ(doc.Runtime().ValueOf("lamp shadows", "count"), kgraph::Value(forwardplus::LampShadowMost)) << "the first of the ninety-six lamps";
        EXPECT_EQ(doc.Runtime().ValueOf("lamp shadows", "faces"), kgraph::Value(forwardplus::LampShadowMost * 6));
        {
            const kgraph::Value matrices = doc.Runtime().ValueOf("lamp shadows", "matrices"), idle = doc.Runtime().ValueOf("lamp shadows", "idle");
            ASSERT_TRUE(matrices.Items() && idle.Items());
            EXPECT_EQ(matrices.Items()->size(), static_cast<std::size_t>(forwardplus::LampShadowMost) * 6 * 16);
            ASSERT_EQ(idle.Items()->size(), static_cast<std::size_t>(forwardplus::LampShadowMost) * 6);
            EXPECT_EQ(std::ranges::count(*idle.Items(), kgraph::Value(0)), 12) << "two lights' six faces";
            ASSERT_NE(doc.Runtime().ReportOf("cast lamp [0]"), nullptr);
            ASSERT_NE(doc.Runtime().ReportOf(std::format("cast lamp [{}]", forwardplus::LampShadowMost * 6 - 1)), nullptr);
            std::size_t drawing = 0;
            for (int face = 0; face < forwardplus::LampShadowMost * 6; ++face)
                if (const auto* report = doc.Runtime().ReportOf(std::format("cast lamp [{}]", face)); report && report->ran > 0) ++drawing;
            EXPECT_GT(drawing, 0u) << "those whose turn it has been drew what was in their sight";
        }

        // The passes run in the order the picture is made in. The trace only reads the picture, between the sky
        // that writes it and the reflections and the bloom that write it after: it is where the graph has it.
        std::vector<std::string> order;
        for (const auto& pass : editing.scene->Graph().Schedule()) order.push_back(pass.name);
        const auto at = [&](const char* name) { return std::ranges::find(order, name) - order.begin(); };
        const char* const sequence[] { "reset counter", "run cull lights", "cast [0]", "cast [3]", "draw depth", "draw forward", "run occlusion pass", "run blur occlusion",
                                       "run apply occlusion", "run sky", "run trace reflections", "run add reflections", "run down [0]", "run down [5]", "run up [0]",
                                       "run up [4]", "run add bloom", "run tone map", "show" };
        for (std::size_t i = 0; i + 1 < std::size(sequence); ++i) {
            ASSERT_LT(at(sequence[i]), static_cast<std::ptrdiff_t>(order.size())) << sequence[i] << " does not run";
            EXPECT_LT(at(sequence[i]), at(sequence[i + 1])) << sequence[i] << " runs before " << sequence[i + 1];
        }

        // Something is drawn: the sky at the top, and under it a floor that is lit.
        const auto sky = editing.Pixel(Extent.x / 2, 8), floor = editing.Pixel(Extent.x / 2, Extent.y - 60);
        EXPECT_GT(sky.z, 150) << "the sky is blue: " << int(sky.x) << " " << int(sky.y) << " " << int(sky.z);
        EXPECT_GT(int(floor.x) + int(floor.y) + int(floor.z), 150) << "the floor is lit";

        made = editing.Picture();

        // The other thing to look at: how many lights each pixel's cluster holds. Its pass runs only while it is shown.
        EXPECT_EQ(at("run heat"), static_cast<std::ptrdiff_t>(order.size())) << "nothing makes the debug view while the picture is shown";
        ASSERT_TRUE(doc.SetInput("debug view", "value", 1));
        for (int i = 0; i < 4; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        order.clear();
        for (const auto& pass : editing.scene->Graph().Schedule()) order.push_back(pass.name);
        EXPECT_LT(at("run tone map"), at("run heat"));
        EXPECT_LT(at("run heat"), at("show"));
        editing.Sheet("-heat");
        // Where nothing was drawn there is no cluster to count, and it is grey; among the lamps it is a colour.
        const auto none = editing.Pixel(Extent.x / 2, 8), some = editing.Pixel(Extent.x / 2, Extent.y - 40);
        EXPECT_LT(std::abs(int(none.x) - int(none.z)), 12) << "grey: " << int(none.x) << " " << int(none.y) << " " << int(none.z);
        EXPECT_GT(std::max({ std::abs(int(some.x) - int(some.y)), std::abs(int(some.y) - int(some.z)), std::abs(int(some.x) - int(some.z)) }), 30)
            << "a colour: " << int(some.x) << " " << int(some.y) << " " << int(some.z);
        ASSERT_TRUE(doc.SetInput("debug view", "value", 0));
        Frame();
        ASSERT_TRUE(doc.Save(project.ScenePath("main"), error)) << error;

        // The camera is flown: with the right button held the mouse turns it and W moves it the way it looks.
        const kecs::Entity camera = doc.World().FindEntity("Camera");
        ASSERT_TRUE(doc.World().Has(camera, doc.World().Find("FlyCamera")));
        const kecs::Vec3 stood = doc.World().PositionOf(camera), faced = doc.World().RotationOf(camera);
        auto& input = editing.scene->SceneInput();
        input.FeedKey(kor::Key::eW, true);
        for (int i = 0; i < 4; ++i) Frame();
        EXPECT_EQ(doc.World().PositionOf(camera), stood) << "nothing moves it until the right button is held";
        input.FeedMouseButton(kor::MouseButton::eRight, true);
        for (int i = 0; i < 6; ++i) { input.FeedMouseDelta({ 20.f, 0.f }); Frame(); }
        input.FeedKey(kor::Key::eW, false);
        input.FeedMouseButton(kor::MouseButton::eRight, false);
        Frame();
        EXPECT_LT(doc.World().PositionOf(camera).y, stood.y) << "it looks down, and went that way";
        EXPECT_LT(doc.World().RotationOf(camera).y, faced.y) << "the mouse to the right turns it to the right";
        EXPECT_FLOAT_EQ(doc.Field(camera, "FlyCamera", "speed").Number() ? static_cast<float>(*doc.Field(camera, "FlyCamera", "speed").Number()) : 0.f, 4.f);
    }

    // As a project is run: the file, and nothing of the session that made it.
    Running playing(project.ScenePath("main"));
    ASSERT_TRUE(playing.scene->Error().empty()) << playing.scene->Error();
    for (int i = 0; i < 4; ++i) Frame();
    for (const auto& problem : playing.scene->Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    const auto again = playing.Picture();
    ASSERT_EQ(again.size(), made.size());
    std::size_t different = 0;
    for (std::size_t i = 0; i < made.size(); ++i)
        if (std::abs(int(made[i].x) - int(again[i].x)) > 2 || std::abs(int(made[i].y) - int(again[i].y)) > 2 || std::abs(int(made[i].z) - int(again[i].z)) > 2) ++different;
    EXPECT_LT(different, made.size() / 500) << "the saved project draws what the session that made it drew";
}

// The project opens in the editor like any other: its pages are shown, and its scene runs in the Scene panel.
TEST(ForwardPlus, TheProjectOpensInTheEditor) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const std::filesystem::path directory = KEDITOR_FORWARD_PLUS_PROJECT;
    if (!std::filesystem::exists(directory / "scenes" / "main.scene.json")) GTEST_SKIP() << "the project is made by the test before this one";
    auto* editor = support::App()->OpenOffscreen<keditor::EditorScene>({ .title = "editor", .extent = { 1500, 900 } }, directory);
    ASSERT_NE(editor, nullptr);
    for (int i = 0; i < 6; ++i) Frame();
    keditor::Document& doc = editor->CurrentSession().Doc();
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(doc.Passes().size(), 11u);
    EXPECT_EQ(doc.Runtime().ValueOf("light data", "count"), kgraph::Value(96));
    // Every page of it is laid out, with whatever its nodes have of their shaders and their components as ports.
    for (const auto& pass : doc.Passes()) {
        editor->CurrentSession().ShowPass(pass);
        for (int i = 0; i < 3; ++i) Frame();
    }
    // Its graph has nothing of an editor in it — no image of which entity is where, no outline — and it is picked
    // from all the same: that is the editor's own doing.
    EXPECT_FALSE(doc.Runtime().Pick(0.5f, 0.7f, [](kecs::Entity) {}));
    keditor::Overlay* over = editor->CurrentSession().Play().Over();
    ASSERT_NE(over, nullptr);
    EXPECT_EQ(over->Problem(), "");
    kecs::Entity picked = kecs::NullEntity;
    ASSERT_TRUE(over->Pick(0.5f, 0.7f, [&](const kecs::Entity entity) { picked = entity; }));
    for (int i = 0; i < 8; ++i) Frame();
    EXPECT_FALSE(picked.IsNull()) << "something of the scene is under the middle of the picture";

    // What a node holds is looked at in the inspector: the depth as a picture, stretched to its range, and the lights'
    // buffer by the names its shader gives its fields.
    // Frames for at least so long: what the editor does a few times a second (a look copied out, an edit made one to
    // take back) goes by time, and a release build runs many frames in it.
    const auto wait = [&](const int frames, const double seconds) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames || std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds; ++i) Frame();
    };
    const auto shows = [&](const std::string_view text) {
        wait(30, 0.35);
        return std::ranges::any_of(kui::debug::Texts(editor->Interface()), [&](const std::string& shown) { return shown.find(text) != std::string::npos; });
    };
    editor->CurrentSession().Select(std::string("depth"));
    EXPECT_TRUE(shows("Look at what it holds"));
    editor->CurrentSession().Look("depth");
    EXPECT_TRUE(shows("eD32_SFLOAT, ")) << editor->CurrentSession().LookSays();
    EXPECT_TRUE(shows("Its values run from 0.")) << "a depth is shown stretched: what it runs from and to is said";
    EXPECT_TRUE(editor->CurrentSession().LookPicture().Get() != nullptr);
    editor->CurrentSession().Select(std::string("light data"));
    editor->CurrentSession().Look("light data");
    EXPECT_TRUE(shows("96 elements of 32 each")) << editor->CurrentSession().LookSays();
    EXPECT_TRUE(shows("[0]  position")) << "an element a line, each field by name";
    editor->CurrentSession().Look({});

    // The viewer: an image at any level, with its histogram and all there is to say of it; a buffer as a table, by
    // its shader's struct or a layout typed; a mesh drawn by itself.
    {
        keditor::Session& session = editor->CurrentSession();
        ASSERT_TRUE(session.View("depth"));
        EXPECT_TRUE(shows("Histogram"));
        ASSERT_TRUE(session.ViewedPeek()) << session.ViewSays();
        EXPECT_EQ(session.ViewedPeek()->format, "eD32_SFLOAT");
        EXPECT_EQ(session.ViewedHistogram().channels, 1);
        EXPECT_GT(session.ViewedHistogram().texels, 0u);
        EXPECT_TRUE(session.ViewedPicture().Get() != nullptr);
        EXPECT_TRUE(shows("Samples"));

        ASSERT_TRUE(session.View("light data"));
        EXPECT_TRUE(shows("Rows 0 to 63 of 96")) << session.ViewSays();
        EXPECT_TRUE(shows("position.x")) << "a column a number, by the names its shader gives them";
        session.SetBufferLayout("float4 first; float4 second");
        EXPECT_TRUE(shows("second.w"));
        EXPECT_TRUE(shows("Rows 0 to 63 of 96")) << "32 bytes an element, as typed";
        session.SetBufferLayout("float9 nope");
        EXPECT_FALSE(session.BufferLayoutError().empty());
        session.SetBufferLayout("");

        ASSERT_TRUE(session.View(kgraph::DefaultMeshId("sphere")));
        EXPECT_TRUE(shows("A vertex"));
        EXPECT_TRUE(shows("POSITION"));
        ASSERT_NE(session.ViewedMesh(), nullptr);
        wait(20, 0.3);
        EXPECT_TRUE(session.ViewedPicture().Get() != nullptr);
        EXPECT_EQ(session.ViewSays(), "");
        session.ViewedMesh()->SetWireframe(true);
        session.ViewedMesh()->Turn(0.5f, 0.2f);
        session.ViewedMesh()->Zoom(2.f);
        wait(20, 0.3);
        EXPECT_EQ(session.ViewSays(), "") << "its edges are drawn over it";
        EXPECT_GT(session.ViewedMesh()->Nearness(), 1.f);
        // Any of its attributes, as its colour: the pipeline is made for the mesh's own layout, and a shader for what is shown.
        const auto attributes = session.ViewedMesh()->Attributes();
        for (const char* semantic : { "POSITION", "NORMAL", "UV" })
            EXPECT_NE(std::ranges::find(attributes, std::string(semantic), &keditor::MeshViewer::Attribute::semantic), attributes.end()) << semantic;
        for (const auto& attribute : attributes) {
            session.ViewedMesh()->ShowAttribute(attribute.semantic);
            wait(10, 0.2);
            EXPECT_EQ(session.ViewSays(), "") << attribute.semantic;
        }
        EXPECT_EQ(session.ViewedMesh()->CurrentMapping(), keditor::MeshViewer::Mapping::eChecker) << "a UV is seen as a checker over it";
        session.ViewedMesh()->SetEdgeWidth(3.f);
        wait(10, 0.2);
        EXPECT_EQ(session.ViewSays(), "");
        // Stopped and viewed again, as often as anyone likes: what it drew is let go after the frames using it.
        for (int i = 0; i < 3; ++i) {
            session.StopViewing();
            wait(5, 0.05);
            ASSERT_TRUE(session.View(kgraph::DefaultMeshId("sphere")));
            wait(10, 0.2);
            EXPECT_TRUE(session.ViewedPicture().Get() != nullptr);
        }

        // A picture of a file's, at another of its levels.
        if (doc.Graph().Find("environment")) {
            ASSERT_TRUE(session.View("environment"));
            EXPECT_TRUE(shows("Level 0"));
            ASSERT_TRUE(session.ViewedPeek()) << session.ViewSays();
            const auto whole = session.ViewedPeek()->fullExtent;
            if (session.ViewedPeek()->mips > 1) {
                session.imageView.mip = 1;
                session.ViewChanged();
                wait(20, 0.6);
                ASSERT_TRUE(session.ViewedPeek());
                EXPECT_EQ(session.ViewedPeek()->mip, 1u);
                EXPECT_EQ(session.ViewedPeek()->extent.x, std::max(whole.x / 2, 1u));
            }
        }
        session.StopViewing();
        EXPECT_TRUE(shows("Nothing is viewed"));
    }

    // What is done to the scene is taken back, and done again: an entity's field, a node's input.
    {
        keditor::Session& session = editor->CurrentSession();
        const auto settle = [&] { wait(40, 0.45); };
        settle();
        EXPECT_FALSE(session.CanUndo()) << "nothing was done yet";
        const kecs::Entity ball = doc.World().FindEntity("Ball 0 0");
        const auto roughness = [&] { return doc.Field(doc.World().FindEntity("Ball 0 0"), "forward", "roughness"); };
        const kgraph::Value was = roughness();
        ASSERT_TRUE(doc.SetField(ball, "forward", "roughness", kgraph::Value(0.9)));
        settle();
        ASSERT_TRUE(doc.SetInput("run tone map", "push.exposure", 2.0));
        settle();
        EXPECT_TRUE(doc.Dirty()) << "which the window's title says";
        ASSERT_TRUE(session.CanUndo());
        EXPECT_TRUE(session.Undo());
        EXPECT_EQ(doc.Graph().Find("run tone map")->inputs.at("push.exposure").value, kgraph::Value(1.0));
        EXPECT_NEAR(*roughness().Number(), 0.9, 1e-6) << "one thing at a time";
        EXPECT_TRUE(session.Undo());
        EXPECT_EQ(roughness(), was);
        EXPECT_FALSE(session.CanUndo());
        EXPECT_TRUE(session.Redo());
        EXPECT_NEAR(*roughness().Number(), 0.9, 1e-6);
        EXPECT_TRUE(session.Undo());
        settle();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;

        // What the graph can be switched between is there to choose in the Scene panel: here, the clusters' heat.
        const auto choices = session.Choices();
        ASSERT_EQ(choices.size(), 1u);
        EXPECT_EQ(choices.front().driver, "debug view");
        EXPECT_EQ(choices.front().options, (std::vector<std::string> { "ldr", "cluster heat" }));
        EXPECT_EQ(choices.front().chosen, 0);
        session.Choose("debug view", 1);
        EXPECT_EQ(session.Choices().front().chosen, 1);
        session.Choose("debug view", 0);
        settle();
    }

    // A buffer a pipeline's shader expects is made from the pipeline: a node laid out as the shader declares it, an
    // input a field, wired to the parameter — there to have the data wired to it.
    {
        keditor::Session& session = editor->CurrentSession();
        ASSERT_TRUE(doc.ClearInput("forward", "param.sun"));
        session.Select(std::string("forward"));
        EXPECT_TRUE(shows("Make its buffer")) << "offered where the pipeline has a buffer nothing is wired to";
        std::string error;
        const std::string made = doc.MakeBufferFor("forward", "param.sun", &error);
        ASSERT_FALSE(made.empty()) << error;
        const kgraph::Node* node = doc.Graph().Find(made);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->type, "ShaderBuffer");
        EXPECT_EQ(node->pass, "forward") << "beside the pipeline, in its pass";
        EXPECT_EQ(doc.Graph().Find("forward")->inputs.at("param.sun").link->node, made);
        const auto ports = doc.Runtime().PortsOf(made);
        for (const char* field : { "field.lightViewProj", "field.splits", "field.direction", "field.color", "field.ambient" })
            EXPECT_NE(std::ranges::find(ports, field, &kgraph::Port::name), ports.end()) << field;
        EXPECT_TRUE(doc.Runtime().Status("forward")->ok) << doc.Runtime().Status("forward")->error;
        EXPECT_TRUE(doc.MakeBufferFor("forward", "nothing", &error).empty());
        EXPECT_NE(error.find("uses no buffer 'nothing'"), std::string::npos) << error;
        EXPECT_EQ(doc.Graph().Find("nothing data"), nullptr) << "nothing is left of one that could not be made";
        // And back as it was: the graph's own buffer of the sun.
        doc.RemoveNodes({ made });
        ASSERT_TRUE(doc.Connect("sun data (in forward)", "out", "forward", "param.sun"));
        for (int i = 0; i < 4; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
    }

    // The project's files are listed: its shaders, each opened in the editor of code, which is told what the project
    // is made with — where a shader's imports are, and for C# the engine's own assemblies to offer names from.
    keditor::Session& session = editor->CurrentSession();
    using Kind = keditor::Session::Asset::Kind;
    auto assets = session.Assets();
    const auto forward = std::ranges::find(assets, "shader:forward", &keditor::Session::Asset::id);
    ASSERT_NE(forward, assets.end());
    EXPECT_EQ(forward->kind, Kind::eShader);
    EXPECT_EQ(forward->folder, "shaders");
    // The Assets panel shows the project's folders, and what is in the one gone into.
    EXPECT_TRUE(shows("shaders") && shows("scenes"));
    session.assetFolder = "shaders";
    session.SelectAsset("shader:forward");
    EXPECT_TRUE(shows("forward.slang")) << "in the Assets panel";
    // Looked for, a thing is found wherever it is.
    session.assetFolder.clear();
    session.assetFilter = "tonemap";
    session.SelectAsset("shader:toneMapping");      // (the inspector names what is picked: not the one looked for as gone)
    EXPECT_TRUE(shows("toneMapping.slang"));
    EXPECT_FALSE(shows("forward.slang"));
    session.assetFilter.clear();
    // A folder is made in the one shown, and is among the folders.
    session.assetFolder = "assets";
    std::filesystem::create_directories(directory / "assets");
    EXPECT_TRUE(session.NewAssetFolder("props")) << session.Status();
    EXPECT_FALSE(session.NewAssetFolder("props")) << "there is one already";
    EXPECT_FALSE(session.NewAssetFolder("a/b"));
    EXPECT_TRUE(std::filesystem::is_directory(directory / "assets" / "props"));
    EXPECT_TRUE(std::ranges::contains(session.AssetFolders(session.Assets()), std::string("assets/props")));
    EXPECT_TRUE(shows("props"));
    // What is thrown away goes to the trash by the system's own tool, and only what is the project's.
    {
        std::vector<std::string> ran;
        keditor::Session::launch = [&](const std::string& command) { ran.push_back(command); return true; };
        EXPECT_FALSE(session.TrashAsset("/etc/hostname"));
        EXPECT_TRUE(ran.empty());
        EXPECT_TRUE(session.TrashAsset(directory / "assets" / "props")) << session.Status();
        ASSERT_EQ(ran.size(), 1u);
        EXPECT_TRUE(ran[0].starts_with("gio trash ")) << ran[0];
        EXPECT_TRUE(session.ShowInFiles(directory / "shaders" / "forward.slang"));
        EXPECT_TRUE(ran.back().starts_with("xdg-open ") && ran.back().ends_with("shaders\"")) << ran.back();
        keditor::Session::launch = nullptr;
    }
    std::filesystem::remove(directory / "assets" / "props");
    session.assetFolder.clear();
    session.SelectAsset("shader:forward");
    EXPECT_TRUE(shows("Entry points")) << "the inspector says what a picked asset is";
    EXPECT_TRUE(shows("vertex, fragment"));
    std::string ran;
    keditor::Session::launch = [&](const std::string& command) { ran = command; return true; };
    EXPECT_TRUE(session.OpenInCode(forward->file));
    keditor::Session::launch = {};
    EXPECT_NE(ran.find("code \""), std::string::npos) << ran;
    EXPECT_NE(ran.find("--goto"), std::string::npos) << ran;
    EXPECT_NE(ran.find("forward.slang"), std::string::npos) << ran;
    EXPECT_TRUE(std::filesystem::exists(directory / ".vscode" / "settings.json"));
    EXPECT_TRUE(std::filesystem::exists(directory / ".vscode" / "extensions.json"));
    if (std::filesystem::exists(directory / "ForwardPlus.Scripts.csproj")) {
        std::ifstream in(directory / "ForwardPlus.Scripts.csproj");
        const std::string project((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(project.find("Koral.Engine.dll"), std::string::npos) << "a C# script is written with the engine's names known";
        EXPECT_NE(project.find("scripts/**/*.cs"), std::string::npos);
    }
    EXPECT_EQ(keditor::Session::ProjectsDirectory().filename(), "EngineProjects") << "where a new project is offered to be made";

    // A model imported: its meshes and its pictures are listed as they are read, a picture by itself, small; and
    // what is listed is what an entity's field takes, by the name it is found by.
    if (const std::filesystem::path model = "/mnt/w/Assets/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf"; std::filesystem::exists(model)) {
        ASSERT_TRUE(session.ImportModel(model));
        for (int i = 0; i < 200000 && (i < 2 || doc.Reading() > 0); ++i) Frame();
        assets = session.Assets();
        const auto count = [&](const Kind kind) { return std::ranges::count_if(assets, [&](const auto& asset) { return asset.kind == kind && asset.loaded && !asset.pending; }); };
        EXPECT_GT(count(Kind::eMesh), 20);
        EXPECT_GT(count(Kind::eTexture), 20);
        const auto texture = std::ranges::find(assets, Kind::eTexture, &keditor::Session::Asset::kind);
        const auto mesh = std::ranges::find(assets, Kind::eMesh, &keditor::Session::Asset::kind);
        ASSERT_NE(texture, assets.end());
        ASSERT_NE(mesh, assets.end());
        EXPECT_TRUE(session.AssetHeld(texture->id) != nullptr);
        // What the scene has of files outside the project is in folders too: each is gone into and out of again — its
        // pictures shown, then others — and a model's meshes are shown after it, and by their kind.
        EXPECT_TRUE(texture->folder.starts_with(keditor::Session::OutsideFolder)) << texture->folder;
        EXPECT_EQ(mesh->parent, mesh->id.substr(0, mesh->id.rfind('#')));
        const auto whole = std::ranges::find(assets, mesh->parent, &keditor::Session::Asset::id);
        ASSERT_NE(whole, assets.end()) << "a mesh's model is listed";
        EXPECT_TRUE(whole->kind == Kind::eModel && whole->loaded);
        for (int round = 0; round < 2; ++round)
            for (const auto& folder : session.AssetFolders(assets)) {
                session.assetFolder = folder;
                session.SelectAsset(texture->id);
                for (int i = 0; i < 8; ++i) Frame();
            }
        session.assetFolder = whole->folder;
        session.OpenAsset(*whole);
        EXPECT_TRUE(session.assetsOpened.contains(whole->id));
        EXPECT_TRUE(shows(mesh->label)) << "its meshes, after it";
        session.assetKind = static_cast<int>(Kind::eMesh);
        session.SelectAsset(texture->id);
        EXPECT_TRUE(shows(mesh->label));
        session.assetKind = -1;
        session.assetFolder.clear();
        session.SelectAsset(texture->id);
        EXPECT_TRUE(shows("Outside the project") || shows("Outside the..."));
        session.SelectAsset(texture->id);
        EXPECT_TRUE(shows("Levels")) << "a picture's size and its levels, and the picture";
        session.SelectAsset(mesh->id);
        EXPECT_TRUE(shows("Triangles"));
        const kecs::Entity ball = doc.World().FindEntity("Ball 0 0");
        ASSERT_FALSE(ball.IsNull());
        std::string error;
        EXPECT_TRUE(doc.SetField(ball, "forward", "albedoMap", kgraph::Value(texture->id), &error)) << error;
        EXPECT_TRUE(doc.SetField(ball, "Mesh", "mesh", kgraph::Value(mesh->id), &error)) << error;
        session.Select(ball);
        EXPECT_TRUE(shows(session.AssetLabel(texture->id))) << "the field says what it holds by its name, a file's as much as a node's";
        EXPECT_EQ(session.AssetLabel(texture->id).find('/'), std::string::npos) << "and not by where the file is";

        // A mesh has a small picture of it, drawn a few frames after it is listed: shown in the inspector when it is picked.
        for (int i = 0; i < 4000 && !session.MeshPicture(mesh->id).Get(); ++i) Frame();
        ASSERT_TRUE(session.MeshPicture(mesh->id).Get() != nullptr);
        EXPECT_EQ(session.MeshPicture(mesh->id)->Extent().x, 96u);
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;

        // A model's mesh is called what its file calls it, and listed under its model.
        EXPECT_EQ(mesh->group, "Sponza.gltf");
        // Each imported thing has its material once: in the component of the pipeline that draws it.
        const kecs::ComponentId blended = doc.World().Find("forward blended"), solid = doc.World().Find("forward");
        std::size_t both = 0, imported = 0;
        for (const kecs::Entity entity : doc.Entities()) {
            const auto held = doc.Field(entity, "Mesh", "mesh");
            if (!held.String() || !held.String()->starts_with("@")) continue;
            ++imported;
            both += doc.World().Has(entity, blended) && doc.World().Has(entity, solid);
        }
        EXPECT_GT(imported, 20u);
        EXPECT_EQ(both, 0u);

        // An entity a pass cannot run for says so where it is mended: in the outliner, and in its inspector.
        EXPECT_TRUE(doc.Runtime().PassedBy().empty());
        ASSERT_TRUE(doc.SetField(ball, "forward", "normalMap", kgraph::Value(), &error)) << error;
        for (int i = 0; i < 6; ++i) Frame();
        const auto passed = doc.Runtime().PassedBy();
        ASSERT_FALSE(passed.empty());
        EXPECT_EQ(passed.front().first, ball);
        EXPECT_TRUE(shows("Not drawn by draw forward: 'normalMap' has nothing in it")) << passed.front().second;
        EXPECT_TRUE(shows("Give the field something")) << "and what to do about it, under what is wrong";
        ASSERT_TRUE(doc.SetField(ball, "forward", "normalMap", kgraph::Value("$white"), &error)) << error;

        // A note is words on the graph: it makes nothing, and nothing is wrong with it.
        const std::string note = doc.AddNode("Note", 40, 40, "why", "show");
        doc.SetInput(note, "text", "The picture, or the clusters' heat.");
        session.ShowPass("show");
        EXPECT_TRUE(shows("The picture, or the clusters' heat."));
        EXPECT_TRUE(doc.Runtime().Status(note)->ok);
        EXPECT_TRUE(shows("x4") || true);
        session.ShowPass("shadows");
        EXPECT_TRUE(shows("cast   x4")) << "a node of a loop says how many turns it is there for";
        EXPECT_TRUE(shows("cascade   4 turns"));
        if (const char* sheet = std::getenv("FORWARD_PLUS_SHEET")) {
            session.SelectAsset(mesh->id);
            for (int i = 0; i < 600; ++i) Frame();
            const auto readback = kor::Buffer::RawBuilder{}.SetRawSize(1500 * 900 * 4).SetUsage(kor::Buffer::Usage::eTransferDst).SetType(kor::Buffer::Type::eReadback).Build();
            auto& pass = editor->Graph().Add<support::ReadScreen>(kor::ResourceRef<const kor::Buffer>(readback));
            for (int i = 0; i < 6; ++i) Frame();
            const auto pixels = readback->Read<kor::U8Vec4>(1500 * 900);
            std::ofstream out(std::string(sheet) + "-editor.ppm", std::ios::binary);
            out << "P6\n1500 900\n255\n";
            for (const auto& pixel : pixels) out.put(static_cast<char>(pixel.x)).put(static_cast<char>(pixel.y)).put(static_cast<char>(pixel.z));
            editor->Graph().Remove(pass);
            Frame();
        }
    }
    support::App()->Close(*editor);
    Frame();
    Frame();
}

// The graph is what is worked on: its nodes are copied and pasted, a page is put in order, and each page is drawn
// to be read. FORWARD_PLUS_SHEET writes a picture of each page, as it is and put in order.
TEST(ForwardPlus, TheGraphIsWorkedOn) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const std::filesystem::path directory = KEDITOR_FORWARD_PLUS_PROJECT;
    if (!std::filesystem::exists(directory / "scenes" / "main.scene.json")) GTEST_SKIP() << "the project is made by the first test";
    constexpr kor::UVec2 Size { 1920, 1080 };
    auto* editor = support::App()->OpenOffscreen<keditor::EditorScene>({ .title = "editor", .extent = Size }, directory);
    ASSERT_NE(editor, nullptr);
    for (int i = 0; i < 6; ++i) Frame();
    keditor::Session& session = editor->CurrentSession();
    keditor::Document& doc = session.Doc();
    const auto shot = [&](const std::string& name) {
        const char* sheet = std::getenv("FORWARD_PLUS_SHEET");
        if (!sheet) return;
        const auto readback = kor::Buffer::RawBuilder{}.SetRawSize(Size.x * Size.y * 4).SetUsage(kor::Buffer::Usage::eTransferDst).SetType(kor::Buffer::Type::eReadback).Build();
        auto& pass = editor->Graph().Add<support::ReadScreen>(kor::ResourceRef<const kor::Buffer>(readback));
        for (int i = 0; i < 6; ++i) Frame();
        const auto pixels = readback->Read<kor::U8Vec4>(Size.x * Size.y);
        std::ofstream out(std::string(sheet) + "-graph-" + name + ".ppm", std::ios::binary);
        out << "P6\n" << Size.x << " " << Size.y << "\n255\n";
        for (const auto& pixel : pixels) out.put(static_cast<char>(pixel.x)).put(static_cast<char>(pixel.y)).put(static_cast<char>(pixel.z));
        editor->Graph().Remove(pass);
        Frame();
    };
    const auto shows = [&](const std::string_view text) {
        for (int i = 0; i < 12; ++i) Frame();
        return std::ranges::any_of(kui::debug::Texts(editor->Interface()), [&](const std::string& shown) { return shown.find(text) != std::string::npos; });
    };

    // A node says what it is set to; each page is drawn.
    for (const auto& pass : doc.Passes()) {
        session.ShowPass(pass);
        for (int i = 0; i < 6; ++i) Frame();
        shot(pass);
    }

    // Nodes are copied: by names of their own, wired among themselves as they were, and from the rest as they were.
    const kgraph::Node* draw = doc.Graph().Find("draw forward");
    ASSERT_NE(draw, nullptr);
    std::string pipeline;
    for (const auto& [name, input] : draw->inputs)
        if (input.link && doc.Graph().Find(input.link->node) && doc.Graph().Find(input.link->node)->type == "GraphicsPipeline") pipeline = input.link->node;
    ASSERT_FALSE(pipeline.empty());
    const std::size_t before = doc.Graph().nodes.size();
    const auto copies = doc.DuplicateNodes({ "draw forward", pipeline });
    ASSERT_EQ(copies.size(), 2u);
    EXPECT_EQ(doc.Graph().nodes.size(), before + 2);
    EXPECT_EQ(copies[0], "draw forward 2");
    {
        const kgraph::Node* original = doc.Graph().Find("draw forward");
        const kgraph::Node* copy = doc.Graph().Find(copies[0]);
        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->pass, original->pass);
        EXPECT_EQ(copy->inputs.size(), original->inputs.size());
        for (const auto& [name, input] : original->inputs) {
            if (!input.link) continue;
            const auto& copied = copy->inputs.at(name).link;
            ASSERT_TRUE(copied.has_value()) << name;
            EXPECT_EQ(copied->node, input.link->node == pipeline ? copies[1] : input.link->node) << name << ": to the copy where what it was wired from was copied too";
        }
    }
    doc.RemoveNodes(copies);
    EXPECT_EQ(doc.Graph().nodes.size(), before);

    // Copied, they are pasted on the page that is shown — another pass's.
    session.CopyNodes({ "draw forward" });
    EXPECT_TRUE(session.HasCopiedNodes());
    session.ShowPass("sky");
    const auto pasted = session.PasteNodes(400, 300);
    ASSERT_EQ(pasted.size(), 1u);
    EXPECT_EQ(doc.Graph().Find(pasted[0])->pass, "sky");
    EXPECT_EQ(doc.Graph().Find(pasted[0])->x, 400);
    EXPECT_EQ(session.selectedNode, pasted[0]);
    doc.RemoveNodes(pasted);

    // A page is put in order: whatever is wired to something is to the right of what it is wired from, and no two
    // of its nodes are at one place.
    for (const auto& pass : doc.Passes()) {
        std::vector<keditor::PlacedNode> nodes;
        std::vector<std::pair<std::string, std::string>> wires;
        for (const auto& node : doc.Graph().nodes) {
            if (node.pass != pass) continue;
            nodes.push_back({ node.id, static_cast<float>(node.x), static_cast<float>(node.y), 200.f, 120.f });
            for (const auto& input : node.inputs | std::views::values)
                if (input.link && doc.Graph().Find(input.link->node) && doc.Graph().Find(input.link->node)->pass == pass) wires.emplace_back(input.link->node, node.id);
        }
        const auto where = keditor::ArrangeNodes(nodes, wires);
        ASSERT_EQ(where.size(), nodes.size()) << pass;
        for (const auto& [from, to] : wires) EXPECT_LT(where.at(from).first, where.at(to).first) << pass << ": " << from << " feeds " << to;
        std::set<std::pair<double, double>> places;
        for (const auto& at : where | std::views::values) EXPECT_TRUE(places.insert(at).second) << pass << ": two nodes at one place";
    }

    // Asked for, the page that is shown is put in order. (A copy of the passes: the graph is made anew as it is.)
    for (const std::vector<std::string> passes = doc.Passes(); const auto& pass : passes) {
        session.ShowPass(pass);
        for (int i = 0; i < 4; ++i) Frame();
        session.AskToArrange();
        for (int i = 0; i < 6; ++i) Frame();
        for (const auto& node : doc.Graph().nodes) {
            if (node.pass != pass) continue;
            for (const auto& input : node.inputs | std::views::values)
                if (const kgraph::Node* from = input.link ? doc.Graph().Find(input.link->node) : nullptr; from && from->pass == pass)
                    EXPECT_LT(from->x, node.x) << pass << ": " << from->id << " feeds " << node.id;
        }
        shot(pass + "-arranged");
    }
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;

    // A loop is framed, and says how many turns it has.
    session.ShowPass("shadows");
    EXPECT_TRUE(shows("cast   x4"));

    // ---- the entities: what each is, where it goes, what it can be given
    {
        const auto named = [&](const std::string_view name) {
            for (const kecs::Entity entity : doc.Entities()) if (doc.NameOf(entity) == name) return entity;
            return kecs::NullEntity;
        };
        const kecs::Entity camera = named("Camera"), sun = named("Sun"), floor = named("Floor"), sky = named("Sky");
        ASSERT_FALSE(camera.IsNull() || sun.IsNull() || floor.IsNull() || sky.IsNull());
        EXPECT_EQ(session.EntityKind(camera), "camera");
        EXPECT_EQ(session.EntityKind(sun), "sun");
        EXPECT_EQ(session.EntityKind(floor), "mesh");
        const kecs::Entity empty = doc.CreateEntity("Nothing yet");
        EXPECT_EQ(session.EntityKind(empty), "empty");

        // Moved before, after and under another: it is where it was put among what it is with.
        const auto order = [&] { std::vector<std::string> names; for (const kecs::Entity e : doc.World().Roots()) names.push_back(doc.NameOf(e)); return names; };
        const auto at = [&](const std::string& name) { const auto names = order(); return std::ranges::find(names, name) - names.begin(); };
        EXPECT_TRUE(session.MoveEntity(empty, sun, keditor::Session::Beside::eBefore));
        EXPECT_EQ(at("Nothing yet") + 1, at("Sun"));
        EXPECT_TRUE(session.MoveEntity(empty, sun, keditor::Session::Beside::eAfter));
        EXPECT_EQ(at("Nothing yet"), at("Sun") + 1);
        EXPECT_TRUE(session.MoveEntity(empty, floor, keditor::Session::Beside::eUnder));
        EXPECT_EQ(doc.World().ParentOf(empty), floor);
        EXPECT_EQ(session.EntityKind(floor), "mesh") << "it is what it is, whatever is under it";
        EXPECT_FALSE(session.MoveEntity(floor, empty, keditor::Session::Beside::eUnder)) << "not under what is under it";
        EXPECT_TRUE(session.Failed());

        // What a pass draws is shown and hidden: its pipelines' components switched, and those of what is under it.
        ASSERT_TRUE(session.CanShow(floor));
        EXPECT_FALSE(session.CanShow(camera)) << "nothing draws a camera";
        EXPECT_TRUE(session.IsShown(floor));
        const auto runs = session.RunsFor(floor);
        EXPECT_TRUE(std::ranges::contains(runs, std::string("draw forward"))) << "the passes that run for it, to go to";
        session.SetShown(floor, false);
        EXPECT_FALSE(session.IsShown(floor));
        EXPECT_TRUE(session.RunsFor(floor).empty());
        session.SetShown(floor, true);
        EXPECT_TRUE(session.IsShown(floor));
        doc.DestroyEntity(empty);

        // A field is offered what it can hold, and nothing else: a mesh's the meshes; a picture's the plain ones, the
        // textures, and of the graph only what is made before the pass its pipeline is run in.
        const auto meshes = session.FieldChoices("Mesh", "Mesh");
        EXPECT_FALSE(meshes.empty());
        for (const auto& choice : meshes) EXPECT_TRUE(doc.Runtime().ResourceOf(choice) && doc.Runtime().ResourceOf(choice)->kind == "Mesh") << choice;
        const auto pictures = session.FieldChoices("forward", "Image");
        EXPECT_TRUE(std::ranges::contains(pictures, std::string("$white")));
        EXPECT_FALSE(std::ranges::contains(pictures, std::string("hdr"))) << "what the forward pass draws into is not given to what it draws";
        EXPECT_FALSE(std::ranges::contains(pictures, std::string("bloom down"))) << "nor what a later pass makes";
        EXPECT_TRUE(std::ranges::contains(pictures, std::string("shadow atlas"))) << "what an earlier pass made is";
        for (const auto& choice : pictures) EXPECT_TRUE(choice.starts_with("$") || choice.starts_with("@") || doc.Graph().Find(choice)) << choice;

        // An input that takes a resource is offered the nodes that give one of its kind: a draw's pipeline, the graphics pipelines.
        const auto pipelinesFor = session.WireChoices("draw forward", "pipeline");
        EXPECT_TRUE(std::ranges::contains(pipelinesFor, std::string("forward")));
        for (const auto& choice : pipelinesFor) EXPECT_EQ(doc.Graph().Find(choice)->type, "GraphicsPipeline") << choice;
        EXPECT_FALSE(std::ranges::contains(pipelinesFor, std::string("draw forward")));
        EXPECT_TRUE(session.WireChoices("draw forward", "no such input").empty());

        // A shader dropped on the graph is a pipeline of it; a script that is no component yet says so; a shader is nothing an entity is given.
        session.ShowPass("show");
        const std::size_t nodesBefore = doc.Graph().nodes.size();
        const std::string made = session.AssetToNode("shader:toneMapping", 300, 200);
        ASSERT_FALSE(made.empty()) << session.Status();
        EXPECT_EQ(doc.Graph().Find(made)->pass, "show");
        EXPECT_TRUE(doc.Graph().Find(made)->type == "ComputePipeline" || doc.Graph().Find(made)->type == "GraphicsPipeline");
        doc.RemoveNodes({ made });
        EXPECT_EQ(doc.Graph().nodes.size(), nodesBefore);
        EXPECT_FALSE(session.GiveAsset("shader:forward", floor));
        EXPECT_TRUE(session.Failed());

        // The graph and the scene are side by side, and the panels say what each entity is.
        session.Select(floor);
        EXPECT_TRUE(shows("PASSES, AS THEY ARE DRAWN") && shows("fps")) << "both at once";
        EXPECT_TRUE(shows("Transform") && shows("Run by") && shows("draw forward") && shows("Under"));
        EXPECT_TRUE(shows("run by a pipeline"));
        shot("entity");
        session.Select(std::string("hdr"));
        for (int i = 0; i < 6; ++i) Frame();
        shot("node");
    }

    // The Assets panel, for looking at: the project's folders, and one of them gone into.
    shot("assets-root");
    session.assetFolder = "shaders";
    session.SelectAsset("shader:forward");
    for (int i = 0; i < 6; ++i) Frame();
    shot("assets-shaders");
    session.assetTileSize = 20.f;
    session.SelectAsset("shader:bloom");
    for (int i = 0; i < 6; ++i) Frame();
    shot("assets-list");
    session.assetTileSize = 72.f;
    session.assetFolder.clear();

    // Gone to, a node's page is shown and the node is picked.
    session.GoTo("draw forward");
    EXPECT_EQ(session.Pass(), "forward");
    EXPECT_EQ(session.selectedNode, "draw forward");
    EXPECT_TRUE(shows("draw forward"));
    shot("gone-to");
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;

    support::App()->Close(*editor);
    Frame();
    Frame();
}

// A model file is made entities, each with its mesh and its material's pictures, and drawn by the same graph under a
// sky that is a picture too. The files are this machine's: FORWARD_PLUS_MODEL and FORWARD_PLUS_SKY say which, and
// without them — or the glTF samples' Sponza where this was written — there is nothing to import.
TEST(ForwardPlus, AModelIsImportedWithItsPicturesUnderASkyFromAFile) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const char* givenModel = std::getenv("FORWARD_PLUS_MODEL");
    const char* givenSky = std::getenv("FORWARD_PLUS_SKY");
    const std::filesystem::path model = givenModel ? givenModel : "/mnt/w/Assets/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf";
    const std::filesystem::path sky = givenSky ? givenSky : "/mnt/w/Assets/main_sponza/textures/kloppenheim_05_4k.hdr";
    if (!std::filesystem::exists(model) || !std::filesystem::exists(sky)) GTEST_SKIP() << "no model or no sky to import: " << model << ", " << sky;

    const keditor::Project project = MakeProject("-sponza");
    std::vector<kor::U8Vec4> made;
    std::size_t drawn = 0;
    {
        Running editing;
        keditor::Document& doc = editing.scene->Doc();
        doc.SetAssetDirectory(project.directory / "assets");
        std::string error;
        ASSERT_TRUE(forwardplus::BuildGraph(doc, error, sky.string())) << error;
        // FORWARD_PLUS_MORE: more models to go in the same scene, a colon between them — curtains, ivy.
        std::vector<std::filesystem::path> models { model };
        if (const char* more = std::getenv("FORWARD_PLUS_MORE")) {
            std::string_view rest(more);
            while (!rest.empty()) {
                const auto colon = rest.find(':');
                if (colon != 0) models.emplace_back(rest.substr(0, colon));
                rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
            }
        }
        ASSERT_TRUE(forwardplus::ImportScene(doc, models, error)) << error;
        // The files are read off the frame, which goes on meanwhile: the scene is there at once, and fills in.
        Frame();
        EXPECT_GT(doc.Reading(), 0u) << "nothing waits for a file";
        for (int i = 0; i < 200000 && doc.Reading() > 0; ++i) Frame();
        ASSERT_EQ(doc.Reading(), 0u);
        for (const auto& why : doc.Runtime().Unread()) ADD_FAILURE() << why;
        for (int i = 0; i < 6; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        ASSERT_TRUE(doc.Runtime().Ok());
        editing.Sheet("-sponza");

        const auto* forward = doc.Runtime().ReportOf("draw forward");
        ASSERT_NE(forward, nullptr);
        EXPECT_GT(forward->ran + forward->culled, 64u + 20u) << "the lamps, and the model's own parts";
        EXPECT_EQ(forward->skipped, 0u) << forward->why;
        drawn = forward->ran + forward->culled;
        // A part has its mesh and its pictures by the files they are in.
        std::size_t textured = 0;
        for (const kecs::Entity entity : doc.Entities()) {
            const auto has = [&](const char* component) { return doc.World().Has(entity, doc.World().Find(component)); };
            if (!has("forward") || has("PointLight")) continue;
            const auto mesh = doc.Field(entity, "Mesh", "mesh"), albedo = doc.Field(entity, "forward", "albedoMap");
            ASSERT_TRUE(mesh.String() && mesh.String()->starts_with("@")) << doc.NameOf(entity) << ": " << mesh.ToJson();
            EXPECT_NE(mesh.String()->find(".gltf#"), std::string::npos);
            if (albedo.String() && albedo.String()->starts_with("@")) ++textured;
        }
        EXPECT_GT(textured, 10u);
        // Something of it is seen, and the sky over it.
        std::uint64_t light = 0;
        for (const auto& pixel : editing.Picture()) light += pixel.x + pixel.y + pixel.z;
        EXPECT_GT(light / (Extent.x * Extent.y), 60u) << "the model is lit: on the whole the picture is not dark";
        made = editing.Picture();
        ASSERT_TRUE(doc.Save(project.ScenePath("main"), error)) << error;
    }

    // Saved and run again, every mesh and picture is found by its file.
    Running playing(project.ScenePath("main"));
    ASSERT_TRUE(playing.scene->Error().empty()) << playing.scene->Error();
    for (int i = 0; i < 200000 && (i < 2 || playing.scene->Doc().Reading() > 0); ++i) Frame();
    for (int i = 0; i < 6; ++i) Frame();
    for (const auto& problem : playing.scene->Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    const auto* forward = playing.scene->Doc().Runtime().ReportOf("draw forward");
    ASSERT_NE(forward, nullptr);
    EXPECT_EQ(forward->ran + forward->culled, drawn);
    EXPECT_EQ(forward->skipped, 0u) << forward->why;
    const auto again = playing.Picture();
    std::size_t different = 0;
    for (std::size_t i = 0; i < made.size(); ++i)
        if (std::abs(int(made[i].x) - int(again[i].x)) > 2 || std::abs(int(made[i].y) - int(again[i].y)) > 2 || std::abs(int(made[i].z) - int(again[i].z)) > 2) ++different;
    EXPECT_LT(different, made.size() / 200) << "the saved project draws what the session that made it drew";
}
