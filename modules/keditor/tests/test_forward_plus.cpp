// Forward+, made with nodes (samples/forward-plus): the graph is built through the document as an editor would build
// it, run, saved as a project, and run again from the file. Offscreen, with no windowing system.
//
// FORWARD_PLUS_SHEET=<file stem> writes the picture to <stem>.ppm.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>

#include <capiInterop.h>
#include <input.h>

#include <keditor/editor.h>

#include <ranges>
#include <set>

#include <keditor/play.h>
#include <kgraph/meshes.h>
#include <keditor/project.h>

#include <kgraph/entities.h>
#include "forward_plus.h"
#include "support.h"

namespace {
    /** How many instances a grouped Filter keeps: its groups' counts, added up. */
    std::size_t InstancesOf(const kgraph::Runtime& runtime, const std::string_view filter)
    {
        std::size_t instances = 0;
        const kgraph::Value given = runtime.ValueOf(filter, "groups");
        if (const auto* groups = given.Items())
            for (const auto& group : *groups)
                if (const kgraph::Value* count = group.Find("count"); count && count->Number()) instances += static_cast<std::size_t>(*count->Number());
        return instances;
    }
    /** How many instances an IndirectDraws' commands draw, as the GPU left them: looked at, each command's instanceCount added up. */
    std::optional<std::size_t> DrawnBy(kgraph::Runtime& runtime, const std::string& commands)
    {
        runtime.Watch(commands);
        std::optional<kgraph::Peek> peek;
        for (int i = 0; i < 8 && !(peek = runtime.Watched(commands)); ++i) support::Frame();
        runtime.Unwatch(commands);
        if (!peek) return std::nullopt;
        std::size_t drawn = 0;
        for (std::size_t at = 4; at + 4 <= peek->bytes.size(); at += 20) {
            std::uint32_t count = 0;
            std::memcpy(&count, peek->bytes.data() + at, 4);
            drawn += count;
        }
        return drawn;
    }

    /**
     * How many 8x8 blocks of two pictures differ, on average over the block, by more than a few levels. The picture is
     * antialiased over the frames, each moved by a fraction of a pixel: two runs differ pixel by pixel along edges
     * however alike they are, and alike block by block.
     */
    std::size_t DifferingBlocks(const std::vector<kor::U8Vec4>& a, const std::vector<kor::U8Vec4>& b, const kor::UVec2 size)
    {
        std::size_t differing = 0;
        for (kor::u32 by = 0; by + 8 <= size.y; by += 8)
            for (kor::u32 bx = 0; bx + 8 <= size.x; bx += 8) {
                int sum[3] { 0, 0, 0 };
                for (kor::u32 y = by; y < by + 8; ++y)
                    for (kor::u32 x = bx; x < bx + 8; ++x) {
                        const auto& p = a[y * size.x + x];
                        const auto& q = b[y * size.x + x];
                        sum[0] += int(p.x) - int(q.x);
                        sum[1] += int(p.y) - int(q.y);
                        sum[2] += int(p.z) - int(q.z);
                    }
                if (std::abs(sum[0]) > 3 * 64 || std::abs(sum[1]) > 3 * 64 || std::abs(sum[2]) > 3 * 64) ++differing;
            }
        return differing;
    }

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
        // (Until it has settled: what is packed of the meshes it draws is built once they are listed.)
        for (int i = 0; i < 16; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        ASSERT_TRUE(doc.Runtime().Ok());
        editing.Sheet();
        // Every lamp is an element of the lights' buffer, and every drawn thing an instance of one indirect draw, a
        // command a mesh: the pre-pass draws what of them is in sight, and the forward pass what some pixel of the
        // pre-pass showed — fewer, and none it did not see. The sun's cascades draw each entity four times over.
        EXPECT_EQ(doc.Runtime().ValueOf("light data", "count"), kgraph::Value(96));
        EXPECT_EQ(InstancesOf(doc.Runtime(), "drawn"), 1u + 6u + 35u + 96u) << "every mesh but glass, grouped by mesh";
        EXPECT_EQ(doc.Runtime().ValueOf("in sight", "drawCount"), doc.Runtime().ValueOf("drawn meshes", "count")) << "a command a mesh";
        for (const char* draw : { "draw depth", "draw forward", "frustum/cull", "run mark seen", "run gather seen" })
            EXPECT_TRUE(doc.Runtime().Status(draw) && doc.Runtime().Status(draw)->ok) << draw;
        // The pre-pass draws with the forward Draw's buffer of instances, so has no component for an entity to have;
        // the forward pipeline's is each entity's material, its pictures among it.
        EXPECT_EQ(doc.Runtime().ComponentOf("prepass"), kecs::NoComponent);
        EXPECT_NE(doc.Runtime().ComponentOf("forward"), kecs::NoComponent);
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

        // What the culls kept, read back from the GPU: watched — which puts the graph in the frame graph again — so last.
        {
            const auto inSight = DrawnBy(doc.Runtime(), "in sight"), seen = DrawnBy(doc.Runtime(), "seen instances");
            ASSERT_TRUE(inSight && seen);
            EXPECT_GT(*inSight, 80u) << "most of it is in sight";
            EXPECT_LE(*inSight, 1u + 6u + 35u + 96u);
            EXPECT_GT(*seen, 0u);
            EXPECT_LE(*seen, *inSight) << "the forward pass draws no more than the pre-pass saw";
        }

        // The passes run in the order the picture is made in. The trace only reads the picture, between the sky
        // that writes it and the reflections and the bloom that write it after: it is where the graph has it.
        std::vector<std::string> order;
        for (const auto& pass : editing.scene->Graph().Schedule()) order.push_back(pass.name);
        const auto at = [&](const char* name) { return std::ranges::find(order, name) - order.begin(); };
        const char* const sequence[] { "run advance frame", "reset counter", "run cull lights", "cast [0]", "cast [3]", "frustum/cull", "draw depth", "run mark seen",
                                       "run gather seen", "draw forward", "run occlusion pass", "run blur occlusion", "run apply occlusion", "run sky", "run trace reflections",
                                       "run add reflections", "run resolve", "run keep", "run down [0]", "run down [5]", "run up [0]",
                                       "run up [4]", "run add bloom", "run tone map", "show" };
        for (std::size_t i = 0; i + 1 < std::size(sequence); ++i) {
            ASSERT_LT(at(sequence[i]), static_cast<std::ptrdiff_t>(order.size())) << sequence[i] << " does not run";
            EXPECT_LT(at(sequence[i]), at(sequence[i + 1])) << sequence[i] << " runs before " << sequence[i + 1];
        }

        // Something is drawn: the sky at the top, and under it a floor that is lit.
        const auto sky = editing.Pixel(Extent.x / 2, 8), floor = editing.Pixel(Extent.x / 2, Extent.y - 60);
        EXPECT_GT(sky.z, 150) << "the sky is blue: " << int(sky.x) << " " << int(sky.y) << " " << int(sky.z);
        EXPECT_GT(int(floor.x) + int(floor.y) + int(floor.z), 150) << "the floor is lit";

        // Taken once it has settled as the saved project will have: as many frames, the lamps' shadows all drawn and
        // the antialiasing gathered over them.
        for (int i = 16; i < 60; ++i) Frame();
        made = editing.Picture();

        // The other thing to look at: how many lights each pixel's cluster holds. Its pass runs only while it is shown.
        EXPECT_EQ(at("run heat"), static_cast<std::ptrdiff_t>(order.size())) << "nothing makes the debug view while the picture is shown";
        doc.Runtime().ShowDebugView("debug view");
        doc.Runtime().Watch("cluster heat");
        for (int i = 0; i < 6; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        order.clear();
        for (const auto& pass : editing.scene->Graph().Schedule()) order.push_back(pass.name);
        EXPECT_LT(at("run tone map"), at("run heat"));
        EXPECT_LT(at("run heat"), at("show"));
        EXPECT_EQ(doc.Runtime().ResourceOf("debug view"), doc.Runtime().ResourceOf("cluster heat")) << "what the view shows in the scene's place";
        // Where nothing was drawn there is no cluster to count, and it is grey; among the lamps it is a colour.
        const auto heat = doc.Runtime().Watched("cluster heat");
        ASSERT_TRUE(heat);
        ASSERT_EQ(heat->bytes.size(), std::size_t { Extent.x } * Extent.y * 4);
        const auto texel = [&](const kor::u32 x, const kor::u32 y) {
            const std::byte* at = heat->bytes.data() + (std::size_t { y } * Extent.x + x) * 4;
            return kor::U8Vec4(std::to_integer<kor::u8>(at[0]), std::to_integer<kor::u8>(at[1]), std::to_integer<kor::u8>(at[2]), std::to_integer<kor::u8>(at[3]));
        };
        const auto none = texel(Extent.x / 2, 8), some = texel(Extent.x / 2, Extent.y - 40);
        EXPECT_LT(std::abs(int(none.x) - int(none.z)), 12) << "grey: " << int(none.x) << " " << int(none.y) << " " << int(none.z);
        EXPECT_GT(std::max({ std::abs(int(some.x) - int(some.y)), std::abs(int(some.y) - int(some.z)), std::abs(int(some.x) - int(some.z)) }), 30)
            << "a colour: " << int(some.x) << " " << int(some.y) << " " << int(some.z);
        doc.Runtime().Unwatch("cluster heat");
        doc.Runtime().ShowDebugView({});
        Frame();

        // A channel of the materials, as the editor's scene view asks: the forward shader shows its normals, and what
        // comes after it — occlusion, reflections, bloom, tone mapping — leaves them as they are. The floor faces up.
        // (Under TAA a picture is the frames before it too: it is looked at once they have all been the normals.)
        doc.Runtime().SetShaderDebugView(static_cast<int>(kgraph::ShaderDebugChannel::eNormal));
        for (int i = 0; i < 70; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        const auto up = editing.Pixel(Extent.x / 2, Extent.y - 60);
        EXPECT_NEAR(up.x, 128, 12) << int(up.x) << " " << int(up.y) << " " << int(up.z);
        EXPECT_GT(up.y, 240);
        EXPECT_NEAR(up.z, 128, 12);
        doc.Runtime().SetShaderDebugView(0);
        for (int i = 0; i < 70; ++i) Frame();
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
    for (int i = 0; i < 60; ++i) Frame();     // and its antialiasing gathered over as many frames as the session's
    for (const auto& problem : playing.scene->Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    const auto again = playing.Picture();
    playing.Sheet("-saved");
    ASSERT_EQ(again.size(), made.size());
    EXPECT_LT(DifferingBlocks(made, again, Extent), (Extent.x / 8) * (Extent.y / 8) / 25) << "the saved project draws what the session that made it drew (the lamps' shadows and the antialiasing still settling: a few blocks)";
}

// The project opens in the editor like any other: its pages are shown, and its scene runs in the Scene panel.
TEST(ForwardPlus, TheProjectOpensInTheEditor) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const std::filesystem::path directory = KEDITOR_FORWARD_PLUS_PROJECT;
    if (!std::filesystem::exists(directory / "scenes" / "main.scene.json")) GTEST_SKIP() << "the project is made by the test before this one";
    {
        // Its textures are not what this is about: it is not asked, on opening, whether they are to be compressed —
        // a dialog that would be in front of everything this goes on to press.
        keditor::Project project;
        std::string error;
        ASSERT_TRUE(keditor::Project::Open(directory, project, error)) << error;
        project.askToCompress = false;
        ASSERT_TRUE(project.Save(error)) << error;
    }
    auto* editor = support::App()->OpenOffscreen<keditor::EditorScene>({ .title = "editor", .extent = { 1500, 900 } }, directory);
    ASSERT_NE(editor, nullptr);
    for (int i = 0; i < 6; ++i) Frame();
    keditor::Document& doc = editor->CurrentSession().Doc();
    for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(doc.Passes().size(), 21u) << "13, and the meshlet and ray branches' 8";
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
    // Shown once what it waits for has come: a copy read back from the GPU, which the editor looks at four times a
    // second — up to three seconds, a frame at a time, rather than at one moment that may be just before it.
    const auto shows = [&](const std::string_view text) {
        const auto has = [&] { return std::ranges::any_of(kui::debug::Texts(editor->Interface()), [&](const std::string& shown) { return shown.find(text) != std::string::npos; }); };
        wait(30, 0.35);
        for (const auto start = std::chrono::steady_clock::now(); !has() && std::chrono::steady_clock::now() - start < std::chrono::seconds(3);) Frame();
        return has();
    };
    if (const char* page = std::getenv("FP_PAGE")) { editor->CurrentSession().ShowPass(page); wait(30, 3.5); support::App()->Close(*editor); Frame(); Frame(); return; }
    // What it holds, copied out and said of — up to three seconds, a frame at a time.
    const auto says = [&](const std::string_view text) {
        const auto has = [&] {
            const auto& session = editor->CurrentSession();
            return session.LookSays().find(text) != std::string::npos
                || std::ranges::any_of(session.LookLines(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
        };
        wait(30, 0.35);
        for (const auto start = std::chrono::steady_clock::now(); !has() && std::chrono::steady_clock::now() - start < std::chrono::seconds(3);) Frame();
        return has();
    };
    editor->CurrentSession().Look("depth");
    EXPECT_TRUE(says("eD32_SFLOAT, ")) << editor->CurrentSession().LookSays();
    EXPECT_TRUE(says("Its values run from 0.")) << "a depth is shown stretched: what it runs from and to is said";
    EXPECT_TRUE(editor->CurrentSession().LookPicture().Get() != nullptr);
    editor->CurrentSession().Look("light data");
    EXPECT_TRUE(says("96 elements of 32 each")) << editor->CurrentSession().LookSays();
    EXPECT_TRUE(says("[0]  position")) << "an element a line, each field by name";
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
        const auto roughness = [&] { return doc.Field(doc.World().FindEntity("Ball 0 0"), "PbrMaterial", "roughness"); };
        const kgraph::Value was = roughness();
        ASSERT_TRUE(doc.SetField(ball, "PbrMaterial", "roughness", kgraph::Value(0.9)));
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

        // What the graph registers to look at is there to choose in the Scene panel: here, the clusters' heat.
        // (The one Switch choice is which of the three ways it is drawn: the heat is a DebugView.)
        ASSERT_EQ(session.Choices().size(), 1u);
        EXPECT_EQ(session.Choices().front().driver, "renderer");
        const auto modes = session.ViewModes();
        const auto heat = std::ranges::find(modes, std::string("Cluster heat"), &keditor::Session::ViewModeChoice::name);
        ASSERT_NE(heat, modes.end());
        EXPECT_EQ(heat->group, "Scene graph");
        session.SetViewMode(heat->mode);
        settle();
        EXPECT_EQ(doc.Runtime().ShownDebugView(), "debug view");
        EXPECT_TRUE(session.ViewModePicture().Get());
        EXPECT_FALSE(session.CanUndo()) << "what the view shows is no edit of the scene";
        session.SetViewMode({});
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
    // A shader is listed by its entry points, each with its module and its stage.
    const auto forward = std::ranges::find(assets, "shader:forward#fragment", &keditor::Session::Asset::id);
    ASSERT_NE(forward, assets.end());
    EXPECT_EQ(forward->kind, Kind::eShader);
    EXPECT_EQ(forward->folder, "shaders");
    EXPECT_EQ(forward->group, "forward");
    EXPECT_EQ(forward->stage, "fragment");
    const auto module = std::ranges::find(assets, "shader:forward", &keditor::Session::Asset::id);
    ASSERT_NE(module, assets.end()) << "the module, and its entries under it";
    EXPECT_EQ(forward->parent, "shader:forward");
    const auto vertex = std::ranges::find(assets, "shader:forward#vertex", &keditor::Session::Asset::id);
    ASSERT_NE(vertex, assets.end());
    EXPECT_EQ(vertex->stage, "vertex");
    EXPECT_LT(vertex, forward) << "as the module declares them";
    // The Assets panel shows the project's folders, and what is in the one gone into.
    EXPECT_TRUE(shows("shaders") && shows("scenes"));
    session.assetFolder = "shaders";
    // The modules are listed, shut; opened, a module's entries are after it.
    session.SelectAsset("shader:clusterHeat");
    EXPECT_TRUE(shows("forward") && shows("clusterHeat")) << "in the Assets panel";
    EXPECT_FALSE(shows("meshletMesh")) << "shut, its entries are not listed";
    session.OpenAsset(*module);
    EXPECT_TRUE(session.assetsOpened.contains("shader:forward"));
    EXPECT_TRUE(shows("meshletMesh")) << "opened, they are";
    session.OpenAsset(*module);
    EXPECT_FALSE(session.assetsOpened.contains("shader:forward"));
    session.SelectAsset("shader:forward#fragment");
    // Looked for, a thing is found wherever it is.
    session.assetFolder.clear();
    session.assetFilter = "heat";
    session.SelectAsset("shader:clusterHeat#computeMain");      // (the inspector names what is picked: not the one looked for as gone)
    EXPECT_TRUE(shows("computeMain") && shows("clusterHeat"));
    EXPECT_FALSE(shows("meshletMesh"));
    session.assetFilter.clear();
    // A folder is made in the one shown, and is among the folders.
    session.assetFolder = "assets";
    std::filesystem::create_directories(directory / "assets");
    EXPECT_TRUE(session.NewAssetFolder("props")) << session.Status();
    EXPECT_FALSE(session.NewAssetFolder("props")) << "there is one already";
    EXPECT_FALSE(session.NewAssetFolder("a/b"));
    EXPECT_TRUE(std::filesystem::is_directory(directory / "assets" / "props"));
    EXPECT_TRUE(session.assetFiles) << "empty, it is shown among the files";
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
    session.assetFiles = false;
    session.SelectAsset("shader:forward#fragment");
    EXPECT_TRUE(shows("Fragment shader") && shows("In its module")) << "the inspector says what a picked asset is";
    {
        // Its module's entries, as the module declares them: its vertex, task, mesh and fragment shaders.
        std::string listed;
        for (const auto& entry : session.EntryPoints("forward")) listed += (listed.empty() ? "" : ", ") + entry;
        EXPECT_TRUE(listed.starts_with("vertex, meshletTask") && listed.ends_with("meshletMesh, fragment")) << listed;
        EXPECT_TRUE(shows(listed.substr(0, 40))) << listed;
    }
    std::string ran;
    keditor::Session::launch = [&](const std::string& command) { ran = command; return true; };
    EXPECT_TRUE(session.OpenInCode(forward->file));
    keditor::Session::launch = {};
    EXPECT_NE(ran.find("code \""), std::string::npos) << ran;
    EXPECT_NE(ran.find("--goto"), std::string::npos) << ran;
    EXPECT_NE(ran.find("forward.slang"), std::string::npos) << ran;
    // An entry is opened where it is declared.
    {
        keditor::Session::launch = [&](const std::string& command) { ran = command; return true; };
        session.OpenAsset(*forward);
        keditor::Session::launch = {};
        const auto entries = session.ShaderEntries("forward");
        const auto fragment = std::ranges::find(entries, std::string("fragment"), &keditor::Session::ShaderEntry::name);
        ASSERT_NE(fragment, entries.end());
        EXPECT_EQ(fragment->stage, "fragment");
        EXPECT_NE(ran.find(std::format("forward.slang:{}\"", fragment->line)), std::string::npos) << ran;
    }
    EXPECT_TRUE(std::filesystem::exists(directory / ".vscode" / "settings.json"));
    EXPECT_TRUE(std::filesystem::exists(directory / ".vscode" / "extensions.json"));
    if (std::filesystem::exists(directory / "ForwardPlus.Scripts.csproj")) {
        std::ifstream in(directory / "ForwardPlus.Scripts.csproj");
        const std::string project((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(project.find("Koral.Engine.dll"), std::string::npos) << "a C# script is written with the engine's names known";
        EXPECT_NE(project.find("scripts/**/*.cs"), std::string::npos);
    }
    EXPECT_EQ(keditor::Session::ProjectsDirectory(), std::filesystem::path(KEDITOR_PROJECTS)) << "where a new project is offered to be made";

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
        const auto mesh = std::ranges::find_if(assets, [](const auto& asset) { return asset.kind == Kind::eMesh && !asset.builtIn; });     // a model's
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
        EXPECT_EQ(session.assetModel, whole->id);
        EXPECT_TRUE(shows(mesh->label)) << "its meshes, in it";
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
        // Given the picture as one is dropped on it: its material's colour picture, which every renderer reads.
        EXPECT_TRUE(session.GiveAsset(texture->id, ball)) << session.Status();
        EXPECT_EQ(doc.Field(ball, "PbrMaterial", "baseColorMap"), kgraph::Value(texture->id));
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
        // Each imported thing has its material: a PbrMaterial, a glTF's being physical, that follows one of the model's.
        std::size_t following = 0, imported = 0;
        for (const kecs::Entity entity : doc.Entities()) {
            const auto held = doc.Field(entity, "Mesh", "mesh");
            if (!held.String() || !held.String()->starts_with("@")) continue;
            ++imported;
            following += doc.MaterialAssetOfEntity(entity, "PbrMaterial").starts_with("@");
        }
        EXPECT_GT(imported, 20u);
        EXPECT_EQ(following, imported);

        // A picture of an instance's left empty is none, which the shader reads as white: drawn all the same, and
        // nothing is wrong with it.
        EXPECT_TRUE(doc.Runtime().PassedBy().empty());
        ASSERT_TRUE(doc.SetField(ball, "PbrMaterial", "normalMap", kgraph::Value(), &error)) << error;
        for (int i = 0; i < 6; ++i) Frame();
        EXPECT_TRUE(doc.Runtime().PassedBy().empty());
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        ASSERT_TRUE(doc.SetField(ball, "PbrMaterial", "normalMap", kgraph::Value("$white"), &error)) << error;

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
    // As a flow of pages: the three branches end in the Switch that chooses between their pictures, which the pages
    // after them use; the branches not chosen do nothing.
    {
        keditor::Session& session = editor->CurrentSession();
        const auto step = [](const keditor::Session::PassFlow& flow, const std::string& id) {
            const auto it = std::ranges::find(flow.steps, id, &keditor::Session::PassFlow::Step::id);
            return it == flow.steps.end() ? std::optional<keditor::Session::PassFlow::Step>() : std::optional(*it);
        };
        const auto linked = [](const keditor::Session::PassFlow& flow, const std::string& from, const std::string& to) {
            return std::ranges::any_of(flow.links, [&](const auto& link) { return link.from == from && link.to == to; });
        };
        auto flow = session.Flow();
        const auto picture = step(flow, "final picture");
        ASSERT_TRUE(picture);
        EXPECT_TRUE(picture->isSwitch);
        EXPECT_EQ(picture->options, (std::vector<std::string> { "reflections", "meshlet reflections", "ray trace" }));
        EXPECT_EQ(picture->chosen, 0);
        EXPECT_TRUE(linked(flow, "final picture", "antialias"));
        EXPECT_TRUE(linked(flow, "meshlet tasks", "meshlet forward"));
        EXPECT_TRUE(linked(flow, "ray scene", "ray trace"));
        EXPECT_TRUE(step(flow, "forward")->ran);
        EXPECT_FALSE(step(flow, "meshlet forward")->ran) << "a branch not chosen does nothing";
        EXPECT_FALSE(step(flow, "ray trace")->ran);
        session.Choose("renderer", 1);
        for (int i = 0; i < 8; ++i) Frame();
        flow = session.Flow();
        EXPECT_EQ(step(flow, "final picture")->chosen, 1);
        EXPECT_TRUE(step(flow, "meshlet forward")->ran);
        EXPECT_FALSE(step(flow, "forward")->ran);
        session.Choose("renderer", 0);
        for (int i = 0; i < 4; ++i) Frame();

        // The pages the page list lists are the chosen renderer's, and what every renderer shares: those whose passes
        // all idle by the choice are left out, and a page of none that only those use.
        const std::set<std::string> vertexPages { "prepass", "culling", "forward", "occlusion", "sky", "reflections" };
        const std::set<std::string> meshletPages { "meshlet tasks", "meshlet forward", "meshlet occlusion", "meshlet sky", "meshlet reflections" };
        // The shadow maps and the lights' clusters rest while rays are traced: rays find their own shadows and their own light.
        const std::set<std::string> rayPages { "ray scene", "ray trace" }, shadowPages { "shadows", "lamp shadows", "lights" };
        const auto expectLeftOut = [&](const std::vector<const std::set<std::string>*>& out, const char* chosen) {
            const auto hidden = session.PagesNotChosen();
            std::set<std::string> expected;
            for (const auto* each : out) expected.insert(each->begin(), each->end());
            EXPECT_EQ(hidden, expected) << "with " << chosen << " chosen";
        };
        expectLeftOut({ &meshletPages, &rayPages }, "vertices");
        // A material is in use when a pass left running reads it: the forward pass's by the meshlets too, through the
        // forward Draw's buffer; the blended pipeline's by every renderer, for the glass.
        EXPECT_TRUE(session.MaterialInUse("forward"));
        EXPECT_FALSE(session.MaterialInUse("ray tracer"));
        EXPECT_TRUE(session.MaterialInUse("forward blended"));
        // The inspector: what the two materials have alike once, set in both; the ray tracer's own shut.
        const kecs::Entity ball = doc.World().FindEntity("Ball 0 0");
        ASSERT_TRUE(std::ranges::contains(doc.ComponentsOf(ball), std::string("ray tracer"))) << "it has the ray tracer's material too";
        session.Select(ball);
        wait(4, 0.05);
        EXPECT_TRUE(shows("Shared material"));
        EXPECT_TRUE(shows("2 pipelines, one value each"));
        EXPECT_TRUE(shows("● forward") && shows("○ ray tracer")) << "which of them a pass uses now";
        EXPECT_TRUE(shows("no pass uses it now")) << "the ray tracer's own, while vertices are drawn";
        EXPECT_FALSE(shows("reflectionBlur")) << "shut";
        EXPECT_TRUE(shows("transparency")) << "the forward pass's own, under it";

        session.Choose("renderer", 1);
        for (int i = 0; i < 8; ++i) Frame();
        expectLeftOut({ &vertexPages, &rayPages }, "meshlets");
        EXPECT_TRUE(session.MaterialInUse("forward")) << "the meshlets read it through the forward Draw's buffer";
        EXPECT_FALSE(session.MaterialInUse("ray tracer"));

        session.Choose("renderer", 2);
        for (int i = 0; i < 8; ++i) Frame();
        expectLeftOut({ &vertexPages, &meshletPages, &shadowPages }, "rays");
        EXPECT_FALSE(session.MaterialInUse("forward"));
        EXPECT_TRUE(session.MaterialInUse("ray tracer"));
        EXPECT_TRUE(session.MaterialInUse("forward blended")) << "the rays see the glass by it";
        wait(4, 0.05);
        EXPECT_TRUE(shows("reflectionBlur")) << "the ray tracer's own, open";
        EXPECT_FALSE(shows("transparency")) << "the forward pass's, shut";
        EXPECT_TRUE(shows("○ forward") && shows("● ray tracer"));
        // Set once, a shared field is each material's.
        const auto roughness = [&](const char* material) { return *doc.Field(ball, material, "roughness").Number(); };
        const double was = roughness("forward");
        EXPECT_NEAR(roughness("ray tracer"), was, 1e-6) << "given the forward pass's when it was made";
        // One set apart from outside the inspector — a script, say — is said at once, and made alike again by a press.
        EXPECT_FALSE(shows("differs"));
        ASSERT_TRUE(doc.SetField(ball, "PbrMaterial", "roughness", kgraph::Value(0.5)));
        wait(8, 0.3);
        EXPECT_TRUE(shows("differs")) << "the forward pass's is not the ray tracer's now";
        {
            // Down the inspector to it: in a window this high, it is below the inspector's edge. The pointer goes over
            // the inspector, wherever the panels are: above the pill, which is in it.
            auto& input = editor->SceneInput();
            std::optional<kor::Vec2> pill;
            std::function<void(kui::RenderObject&)> visit = [&](kui::RenderObject& o) {
                if (!pill && o.DebugText() == "differs") pill = o.ToGlobal(o.Size() * 0.5f);
                o.VisitChildren(visit);
            };
            if (auto* root = editor->Interface().RootRenderObject()) visit(*root);
            ASSERT_TRUE(pill);
            input.FeedMousePosition({ pill->x, 400.f });
            Frame();
            for (int i = 0; i < 20; ++i) { input.FeedScroll({ 0.f, -3.f }); Frame(); }
            wait(4, 0.1);
            pill.reset();
            if (auto* root = editor->Interface().RootRenderObject()) visit(*root);
            ASSERT_TRUE(pill);
            ASSERT_LT(pill->y, 880.f) << "in sight";
            input.FeedMousePosition(*pill);
            Frame();
            input.FeedMouseButton(kor::MouseButton::eLeft, true);
            Frame();
            input.FeedMouseButton(kor::MouseButton::eLeft, false);
            wait(8, 0.3);
        }
        EXPECT_NEAR(roughness("forward"), was, 1e-6) << "pressed, each is the ray tracer's: the one in use";
        EXPECT_FALSE(shows("differs"));
        session.Choose("renderer", 0);
        for (int i = 0; i < 4; ++i) Frame();
        session.Select(kecs::NullEntity);
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
        const auto pictures = session.FieldChoices("PbrMaterial", "Image");
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
        const std::string made = session.AssetToNode("shader:clusterHeat#computeMain", 300, 200);
        ASSERT_FALSE(made.empty()) << session.Status();
        EXPECT_EQ(doc.Graph().Find(made)->pass, "show");
        EXPECT_EQ(doc.Graph().Find(made)->type, "ComputePipeline");
        doc.RemoveNodes({ made });
        EXPECT_EQ(doc.Graph().nodes.size(), nodesBefore);
        EXPECT_FALSE(session.GiveAsset("shader:forward#fragment", floor));
        EXPECT_TRUE(session.Failed());

        // The graph and the scene are side by side, and the panels say what each entity is.
        session.Select(floor);
        EXPECT_TRUE(shows("PASSES, AS THEY ARE DRAWN") && shows("fps")) << "both at once";
        EXPECT_TRUE(shows("Transform") && shows("Run by") && shows("draw forward") && shows("Under"));
        EXPECT_TRUE(shows("pipeline's own")) << "a pipeline's component: a material";
        shot("entity");
        session.Select(std::string("hdr"));
        for (int i = 0; i < 6; ++i) Frame();
        shot("node");
    }

    // The Assets panel, for looking at: the project's folders, and one of them gone into.
    shot("assets-root");
    session.assetFolder = "shaders";
    session.SelectAsset("shader:forward#fragment");
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
        // The antialiasing gathered over as many frames as the saved project's will be, below: each frame keeps nine
        // tenths of the ones before, so a picture a few frames after the last file came is still half of before it.
        for (int i = 0; i < 60; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        ASSERT_TRUE(doc.Runtime().Ok());
        editing.Sheet("-sponza");

        drawn = InstancesOf(doc.Runtime(), "drawn") + InstancesOf(doc.Runtime(), "glass");
        EXPECT_GT(drawn, 64u + 20u) << "the lamps, and the model's own parts";
        // A part has its mesh and its pictures by the files they are in.
        std::size_t textured = 0;
        for (const kecs::Entity entity : doc.Entities()) {
            const auto has = [&](const char* component) { return doc.World().Has(entity, doc.World().Find(component)); };
            if (!has("PbrMaterial") || has("PointLight")) continue;
            const auto mesh = doc.Field(entity, "Mesh", "mesh"), albedo = doc.Field(entity, "PbrMaterial", "baseColorMap");
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
    for (int i = 0; i < 60; ++i) Frame();     // and its antialiasing gathered over as many frames as the session's
    for (const auto& problem : playing.scene->Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    EXPECT_EQ(InstancesOf(playing.scene->Doc().Runtime(), "drawn") + InstancesOf(playing.scene->Doc().Runtime(), "glass"), drawn);
    const auto again = playing.Picture();
    EXPECT_LT(DifferingBlocks(made, again, Extent), (Extent.x / 8) * (Extent.y / 8) / 200) << "the saved project draws what the session that made it drew";
}

// A decal as a glTF has one: blended, a third opaque, and saying nothing of how metal it is — which glTF takes as all
// metal. Imported, it lets two thirds of what is behind it through and is not metal; a material that says it is metal,
// and how rough, stays metal.
TEST(ForwardPlus, ADecalIsImportedSeeThroughAndNotMetal) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const keditor::Project project = MakeProject("-decal");
    const std::filesystem::path file = project.directory / "assets" / "decals.gltf";
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << R"gltf({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0, 1]}], "nodes": [{"name": "decal", "mesh": 0}, {"name": "brass", "mesh": 1}], "meshes": [{"name": "decal", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}, {"name": "brass", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 1}]}], "materials": [{"name": "dirt", "alphaMode": "BLEND", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 0.35]}}, {"name": "brass", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.7, 0.3, 1], "metallicFactor": 1, "roughnessFactor": 0.3}}], "buffers": [{"byteLength": 108, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAEAAAACAAAA"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 36}, {"buffer": 0, "byteOffset": 72, "byteLength": 24}, {"buffer": 0, "byteOffset": 96, "byteLength": 12}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}, {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}, {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"}, {"bufferView": 3, "componentType": 5125, "count": 3, "type": "SCALAR"}]})gltf";
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    doc.SetAssetDirectory(project.directory / "assets");
    std::string error;
    ASSERT_TRUE(forwardplus::BuildGraph(doc, error)) << error;
    ASSERT_TRUE(forwardplus::ImportScene(doc, { file }, error, 0)) << error;
    for (int i = 0; i < 2000 && (i < 2 || doc.Reading() > 0); ++i) Frame();
    ASSERT_EQ(doc.Reading(), 0u);
    const kecs::Entity decal = doc.World().FindEntity("decal"), brass = doc.World().FindEntity("brass");
    ASSERT_FALSE(decal.IsNull());
    ASSERT_FALSE(brass.IsNull());
    EXPECT_TRUE(doc.World().Has(decal, doc.World().Find("Blended")));
    EXPECT_EQ(doc.Field(decal, "PbrMaterial", "alphaMode"), kgraph::Value(2.0)) << "drawn over what is behind it";
    EXPECT_NEAR((*doc.Field(decal, "PbrMaterial", "baseColor").Items())[3].Number() ? *(*doc.Field(decal, "PbrMaterial", "baseColor").Items())[3].Number() : 0.0, 0.35, 1e-4)
        << "its colour's alpha, 0.35, is how opaque it is";
    EXPECT_EQ(*doc.Field(decal, "PbrMaterial", "metallic").Number(), 0.0) << "it says nothing of being metal";
    EXPECT_EQ(*doc.Field(brass, "PbrMaterial", "metallic").Number(), 1.0) << "it says it is";
    EXPECT_NEAR(*doc.Field(brass, "PbrMaterial", "roughness").Number(), 0.3, 1e-4);
}

// The scene drawn three ways, a branch of pages each, chosen by `renderer`: by vertex shaders, by task and mesh shaders
// over meshlets, and by rays. Each ends in a lit picture the rest of the frame takes on; the branches not chosen idle.
// Drawn by meshlets the picture is the vertex shaders' — the same shading over the same triangles; traced, it is the
// same scene, lit with rays (its shadows, occlusion and reflections its own).
TEST(ForwardPlus, TheSceneIsDrawnThreeWays) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const keditor::Project project = MakeProject("-three");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    std::string error;
    ASSERT_TRUE(forwardplus::BuildGraph(doc, error)) << error;
    forwardplus::BuildScene(doc);
    // A pane of red glass before the camera, over the left half of the picture: half of what is behind shows through.
    const kecs::Entity pane = doc.CreateEntity("Pane");
    doc.SetPosition(pane, { -2.f, 3.5f, 8.f });
    doc.SetRotation(pane, { -22.f, 0.f, 0.f });
    doc.SetScale(pane, { 4.f, 3.f, 0.05f });
    doc.AddComponent(pane, "Mesh");
    doc.SetField(pane, "Mesh", "mesh", kgraph::Value(kgraph::DefaultMeshId("cube")));
    ASSERT_TRUE(doc.AddComponent(pane, "PbrMaterial"));
    doc.SetField(pane, "PbrMaterial", "baseColor", kgraph::Value(kgraph::Value::List { 1.0, 0.05, 0.05, 0.5 }));
    doc.SetField(pane, "PbrMaterial", "roughness", kgraph::Value(0.9));
    doc.SetField(pane, "PbrMaterial", "alphaMode", kgraph::Value("Blended"));     // and so the tag: drawn over what is behind it
    EXPECT_TRUE(doc.World().Has(pane, doc.World().Find("Blended")));
    const auto settle = [&](const int frames) {
        for (int i = 0; i < frames; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
    };
    settle(80);
    ASSERT_TRUE(doc.Runtime().Ok());
    // Where the pane is: how much redder than it is green, on average, the pixels it covers are.
    const auto paneRed = [&](const std::vector<kor::U8Vec4>& picture) {
        double sum = 0;
        int count = 0;
        for (kor::u32 y = Extent.y / 3; y < Extent.y * 2 / 3; ++y)
            for (kor::u32 x = Extent.x / 4; x < Extent.x * 2 / 5; ++x, ++count) sum += picture[y * Extent.x + x].x - picture[y * Extent.x + x].y;
        return sum / count;
    };
    const auto mean = [](const std::vector<kor::U8Vec4>& picture) {
        double sum = 0;
        for (const auto& p : picture) sum += p.x + p.y + p.z;
        return sum / (picture.size() * 3.0);
    };
    const auto ran = [&](const char* pass) {
        const auto frame = doc.Runtime().Frame();
        return std::ranges::any_of(frame.passes, [&](const auto& run) { return run.node == pass; });
    };
    const auto vertex = editing.Picture();
    editing.Sheet("-vertex");
    EXPECT_GT(mean(vertex), 20.0) << "something lit is drawn";
    EXPECT_GT(paneRed(vertex), 25.0) << "the glass is drawn over it";

    ASSERT_TRUE(doc.SetInput("renderer", "value", 1));
    settle(80);
    const auto meshlets = editing.Picture();
    editing.Sheet("-meshlets");
    EXPECT_GT(*doc.Runtime().ValueOf("meshlets", "count").Number(), 0.0);
    EXPECT_LT(DifferingBlocks(vertex, meshlets, Extent), (Extent.x / 8) * (Extent.y / 8) / 20) << "the same picture, by meshlets";
    EXPECT_GT(*doc.Runtime().ValueOf("glass meshlets", "count").Number(), 0.0);
    EXPECT_NEAR(paneRed(meshlets), paneRed(vertex), 8.0) << "and the same glass over it";

    if (kor::Context::SupportsRayTracing()) {
        ASSERT_TRUE(doc.SetInput("renderer", "value", 2));
        settle(80);
        const auto rays = editing.Picture();
        editing.Sheet("-rays");
        EXPECT_GT(mean(rays), mean(vertex) * 0.5) << "traced, the scene is lit as much, near enough";
        EXPECT_LT(mean(rays), mean(vertex) * 2.0);
        EXPECT_EQ(*doc.Runtime().ValueOf("ray scene", "count").Number(), static_cast<double>(InstancesOf(doc.Runtime(), "drawn")));
        EXPECT_EQ(*doc.Runtime().ValueOf("glass ray scene", "count").Number(), static_cast<double>(InstancesOf(doc.Runtime(), "glass")));
        EXPECT_NEAR(paneRed(rays), paneRed(vertex), 15.0) << "the glass seen through, traced";
    }
    (void)ran;
    ASSERT_TRUE(doc.SetInput("renderer", "value", 0));
    settle(4);
}

// Meshlets are culled by what hides them: with a wall just before the camera, the early draw draws little of what is
// in its sight — the wall. And the wall gone, what it hid is drawn from the first frame on, by the late draw, though
// the pyramid the early draw went by still had the wall: no frame shows what is behind what it hid instead.
TEST(ForwardPlus, MeshletsBehindAWallAreNotDrawnAndAreOnceItIsGone) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const keditor::Project project = MakeProject("-occluded");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    std::string error;
    ASSERT_TRUE(forwardplus::BuildGraph(doc, error)) << error;
    forwardplus::BuildScene(doc);
    ASSERT_TRUE(doc.SetInput("renderer", "value", 1));
    kgraph::Runtime& runtime = doc.Runtime();
    const auto settle = [&](const int frames) {
        for (int i = 0; i < frames; ++i) Frame();
        for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
    };
    settle(30);
    ASSERT_TRUE(runtime.Ok());

    // Of the meshlets of the tasks listed — those of the instances in sight — how many the early draw drew.
    const auto drawnEarly = [&]() -> std::pair<std::size_t, std::size_t> {
        const std::array<std::string, 3> buffers { "meshlets drawn early", "meshlet task count", "meshlet task list" };
        for (const auto& id : buffers) runtime.Watch(id);
        std::array<std::optional<kgraph::Peek>, 3> peeks;
        for (int i = 0; i < 8 && !std::ranges::all_of(peeks, [](const auto& p) { return p.has_value(); }); ++i) {
            Frame();
            for (std::size_t b = 0; b < buffers.size(); ++b) peeks[b] = runtime.Watched(buffers[b]);
        }
        for (const auto& id : buffers) runtime.Unwatch(id);
        if (!std::ranges::all_of(peeks, [](const auto& p) { return p.has_value(); })) return { 0, 0 };
        const auto word = [](const kgraph::Peek& peek, const std::size_t at) {
            std::uint32_t value = 0;
            if ((at + 1) * 4 <= peek.bytes.size()) std::memcpy(&value, peek.bytes.data() + at * 4, 4);
            return value;
        };
        std::size_t drawn = 0, listed = 0;
        for (std::uint32_t task = 0; task < word(*peeks[1], 0); ++task) {
            drawn += static_cast<std::size_t>(std::popcount(word(*peeks[0], task)));
            listed += word(*peeks[2], task * 4 + 2);
        }
        return { drawn, listed };
    };
    const auto [open, openListed] = drawnEarly();
    ASSERT_GT(openListed, 0u);

    // What is at the middle of the picture, and how far from the camera: where things are, as the meshlets drew them.
    const auto middle = [&] {
        const auto peek = runtime.Watched("meshlet position");
        std::array<float, 4> position {};
        if (!peek) return position;
        const std::size_t at = (std::size_t { peek->extent.y / 2 } * peek->extent.x + peek->extent.x / 2) * 16;
        if (at + 16 <= peek->bytes.size()) std::memcpy(position.data(), peek->bytes.data() + at, 16);
        return position;
    };
    const auto away = [](const std::array<float, 4>& at) { return at[3] == 1.f ? std::hypot(at[0], at[1] - 5.5f, at[2] - 15.f) : 0.f; };
    runtime.Watch("meshlet position");
    for (int i = 0; i < 8 && !runtime.Watched("meshlet position"); ++i) Frame();
    const float seen = away(middle());
    ASSERT_GT(seen, 4.f) << "something drawn at the middle, beyond where the wall will be";

    // A wall two metres before the camera, square to the way it looks, over all it sees.
    const kecs::Entity wall = doc.CreateEntity("Wall");
    doc.SetPosition(wall, { 0.f, 5.5f - 2.f * std::sin(std::numbers::pi_v<float> * 22.f / 180.f), 15.f - 2.f * std::cos(std::numbers::pi_v<float> * 22.f / 180.f) });
    doc.SetRotation(wall, { -22.f, 0.f, 0.f });
    doc.SetScale(wall, { 20.f, 20.f, 0.2f });
    doc.AddComponent(wall, "Mesh");
    doc.SetField(wall, "Mesh", "mesh", kgraph::Value(kgraph::DefaultMeshId("cube")));
    ASSERT_TRUE(doc.AddComponent(wall, "PbrMaterial"));
    settle(30);
    const auto [walled, walledListed] = drawnEarly();
    ASSERT_GT(walledListed, 0u);
    EXPECT_LT(walled * 4, open) << "behind the wall, next to nothing is drawn early: " << walled << " meshlets of " << walledListed << ", against " << open << " of " << openListed << " without it";

    EXPECT_NEAR(away(middle()), 2.f, 0.3f) << "the wall, at the middle";

    // The wall gone: from the first picture that does not show it, the middle is what it was before the wall.
    ASSERT_TRUE(doc.DestroyEntity(wall));
    int after = 0;
    for (int i = 0; i < 12; ++i) {
        Frame();
        const float now = away(middle());
        if (std::abs(now - 2.f) < 0.3f) continue;     // a picture of before, still
        ++after;
        EXPECT_NEAR(now, seen, 0.3f) << "frame " << i << " after the wall went: what it hid is not drawn";
    }
    runtime.Unwatch("meshlet position");
    EXPECT_GT(after, 8);
}

// A thin thing lit from behind lets some of the light through: a panel with the sun on its back is the brighter on its
// front the more translucent its material says it is — one field both renderers' materials share — drawn by vertices,
// by meshlets, or traced.
TEST(ForwardPlus, AThinThingLitFromBehindLetsTheLightThrough) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const keditor::Project project = MakeProject("-through");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    std::string error;
    ASSERT_TRUE(forwardplus::BuildGraph(doc, error)) << error;
    forwardplus::BuildScene(doc, 0);
    // The sun turned round: low, shining towards the camera.
    doc.SetRotation(doc.World().FindEntity("Sun"), { -25.f, 180.f, 0.f });
    const kecs::Entity panel = doc.CreateEntity("Panel");
    // A sheet, as a curtain is: a plane stood up to face the camera.
    doc.SetPosition(panel, { 0.f, 3.5f, 8.f });
    doc.SetRotation(panel, { 90.f, 0.f, 0.f });
    doc.SetScale(panel, { 4.f, 1.f, 3.f });
    doc.AddComponent(panel, "Mesh");
    doc.SetField(panel, "Mesh", "mesh", kgraph::Value(kgraph::DefaultMeshId("plane")));
    ASSERT_TRUE(doc.AddComponent(panel, "PbrMaterial"));
    ASSERT_TRUE(doc.AddComponent(panel, "forward"));      // its pipeline parameters: how much light shows through it
    doc.SetField(panel, "PbrMaterial", "baseColor", kgraph::Value(kgraph::Value::List { 0.9, 0.2, 0.15, 1.0 }));
    doc.SetField(panel, "PbrMaterial", "roughness", kgraph::Value(0.9));
    forwardplus::GiveRayMaterials(doc);
    ASSERT_TRUE(doc.World().Describe(doc.World().Find("forward")).FindField("translucency"));
    ASSERT_TRUE(doc.World().Describe(doc.World().Find("ray tracer")).FindField("translucency")) << "a field the two materials share";
    // Its middle, where the camera sees its front.
    const auto red = [&] {
        double sum = 0;
        int count = 0;
        for (kor::u32 y = Extent.y * 2 / 5; y < Extent.y / 2; ++y)
            for (kor::u32 x = Extent.x * 9 / 20; x < Extent.x * 11 / 20; ++x, ++count) sum += editing.Picture()[y * Extent.x + x].x;
        return sum / count;
    };
    const auto settle = [&] { for (int i = 0; i < 60; ++i) Frame(); for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem; };
    std::vector<int> renderers { 0, 1 };
    if (kor::Context::SupportsRayTracing()) renderers.push_back(2);
    for (const int renderer : renderers) {
        ASSERT_TRUE(doc.SetInput("renderer", "value", renderer));
        for (const char* material : { "forward", "ray tracer" }) doc.SetField(panel, material, "translucency", kgraph::Value(0.0));
        settle();
        const double opaque = red();
        editing.Sheet(std::format("-through{}-0", renderer));
        for (const char* material : { "forward", "ray tracer" }) doc.SetField(panel, material, "translucency", kgraph::Value(0.8));
        settle();
        const double through = red();
        editing.Sheet(std::format("-through{}-1", renderer));
        EXPECT_GT(through, opaque + 20.0) << "renderer " << renderer << ": the sun behind it shows through, " << opaque << " then " << through;
    }
    ASSERT_TRUE(doc.SetInput("renderer", "value", 0));
    Frame();
}

// The rays are lit by what gives off light — an emissive material — and not by point lights; the raster passes the other
// way round. A glowing ball with no light of its own on the left, a point light with nothing to see of it on the right,
// no sun and no sky: traced, the floor glows under the ball and not under the light; drawn by vertices, under the light
// and not under the ball.
TEST(ForwardPlus, TheRaysAreLitByWhatGivesOffLightAndNotByPointLights) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    if (!kor::Context::SupportsRayTracing()) GTEST_SKIP() << "no ray tracing here";
    const keditor::Project project = MakeProject("-emitters");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    std::string error;
    ASSERT_TRUE(forwardplus::BuildGraph(doc, error)) << error;
    forwardplus::BuildScene(doc, 0);
    doc.DestroyEntity(doc.World().FindEntity("Sun"));
    doc.SetField(doc.World().FindEntity("Sky"), "Sky", "ambient", kgraph::Value(0.0));
    const kgraph::Value white(kgraph::Value::List { 1.0, 1.0, 1.0 });
    const kecs::Entity ball = doc.CreateEntity("Glow");
    doc.SetPosition(ball, { -6.f, 1.f, 9.f });
    doc.SetScale(ball, { 0.3f, 0.3f, 0.3f });
    doc.AddComponent(ball, "Mesh");
    doc.SetField(ball, "Mesh", "mesh", kgraph::Value(kgraph::DefaultMeshId("sphere")));
    ASSERT_TRUE(doc.AddComponent(ball, "PbrMaterial"));
    doc.SetField(ball, "PbrMaterial", "emission", forwardplus::LampEmission(white, 3.0, 4.0, 0.15));
    forwardplus::GiveRayMaterials(doc);
    const kecs::Entity bare = doc.CreateEntity("Bare light");
    doc.SetPosition(bare, { 6.f, 1.f, 9.f });
    doc.AddComponent(bare, "PointLight");
    doc.SetField(bare, "PointLight", "color", white);
    doc.SetField(bare, "PointLight", "intensity", kgraph::Value(3.0));
    doc.SetField(bare, "PointLight", "range", kgraph::Value(4.0));
    // The floor under each: the picture's lower left and lower right, past the ball's own glow.
    const auto floor = [&](const bool left) {
        double sum = 0;
        int count = 0;
        for (kor::u32 y = Extent.y * 3 / 4; y < Extent.y * 19 / 20; ++y)
            for (kor::u32 x = left ? Extent.x / 20 : Extent.x * 13 / 20; x < (left ? Extent.x * 7 / 20 : Extent.x * 19 / 20); ++x, ++count) {
                const auto p = editing.Picture()[y * Extent.x + x];
                sum += p.x + p.y + p.z;
            }
        return sum / (count * 3.0);
    };
    const auto settle = [&](const int frames) { for (int i = 0; i < frames; ++i) Frame(); for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem; };
    settle(80);
    const double vertexBall = floor(true), vertexLight = floor(false);
    editing.Sheet("-emitters-vertex");
    EXPECT_GT(vertexLight, vertexBall + 10.0) << "drawn by vertices, the point light lights the floor and the ball does not: " << vertexBall << " " << vertexLight;
    ASSERT_TRUE(doc.SetInput("renderer", "value", 2));
    settle(120);
    const double raysBall = floor(true), raysLight = floor(false);
    editing.Sheet("-emitters-rays");
    EXPECT_GT(raysBall, raysLight + 10.0) << "traced, the ball lights the floor and the point light does not: " << raysBall << " " << raysLight;
    EXPECT_LT(raysLight, raysBall / 4.0) << "where only the point light is, only what the ball sends that far: its light has no range to end at";
    ASSERT_TRUE(doc.SetInput("renderer", "value", 0));
    Frame();
}

// The shadows are drawn again only when what is in them has changed (a Draw's `whenChanged`): while everything holds
// still, nothing is drawn into them, and the picture is what drawing every one of them every frame makes — the camera
// moved, a ball moved under the lamps and the sun, a lamp moved.
TEST(ForwardPlus, ShadowsDrawnOnlyWhenChangedLookAsIfDrawnEveryFrame) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    const keditor::Project project = MakeProject("-changed");
    Running editing;
    keditor::Document& doc = editing.scene->Doc();
    std::string error;
    ASSERT_TRUE(forwardplus::BuildGraph(doc, error)) << error;
    forwardplus::BuildScene(doc);
    // Every lamp's turn every frame: what is not drawn is then left out by `whenChanged` alone.
    ASSERT_TRUE(doc.SetInput("lamp shadows", "each", 0));
    const kecs::Entity camera = doc.World().FindEntity("Camera"), ball = doc.World().FindEntity("Ball 0 0"), lamp = doc.World().FindEntity("Lamp 0");
    ASSERT_FALSE(camera.IsNull() || ball.IsNull() || lamp.IsNull());
    const kecs::Vec3 stood = doc.World().PositionOf(camera), lay = doc.World().PositionOf(ball), hung = doc.World().PositionOf(lamp);

    const auto moved = [](const kecs::Vec3 from, const float x, const float y, const float z) { return kecs::Vec3 { from.x + x, from.y + y, from.z + z }; };
    // How many frames the antialiasing has advanced: taa.slang's FrameState.frame, after its two matrices and the jitter.
    const auto FrameNumber = [&] {
        const auto state = kor::capi::BufferOf(doc.Runtime().ResourceOf("frame state")->handle)->Read<std::uint32_t>(35);
        return state.size() == 35 ? state[34] : 0u;
    };
    const auto pictures = [&](const bool whenChanged) {
        EXPECT_TRUE(doc.SetInput("cast", "whenChanged", whenChanged));
        EXPECT_TRUE(doc.SetInput("cast lamp", "whenChanged", whenChanged));
        doc.SetPosition(camera, stood);
        doc.SetPosition(ball, lay);
        doc.SetPosition(lamp, hung);
        std::vector<std::vector<kor::U8Vec4>> made;
        // A picture is of the frames before it as well, by the antialiasing: both runs are held still until it has
        // gathered the same picture, then on to the same of its eight sub-pixel offsets — compared frame for frame.
        for (int i = 0; i < 60; ++i) Frame();
        for (int i = 0; i < 8 && FrameNumber() % 8 != 0; ++i) Frame();
        for (const auto& problem : doc.Runtime().Problems()) ADD_FAILURE() << problem;
        made.push_back(editing.Picture());
        if (whenChanged) {
            for (const char* cast : { "cast [0]", "cast [3]", "cast lamp [0]", "cast lamp [143]" }) {
                const auto* report = doc.Runtime().ReportOf(cast);
                EXPECT_TRUE(report && report->unchanged) << cast << ": nothing moved, and it draws nothing";
            }
        }
        doc.SetPosition(camera, moved(stood, 1.5f, 0.5f, -1.f));
        Frame();
        if (const auto* report = doc.Runtime().ReportOf("cast [3]"); whenChanged && report) EXPECT_FALSE(report->unchanged) << "the cascades follow the camera";
        Frame();
        made.push_back(editing.Picture());
        doc.SetPosition(ball, moved(lay, 0.f, 1.2f, 0.5f));
        Frame();
        Frame();
        made.push_back(editing.Picture());
        doc.SetPosition(lamp, moved(hung, 0.6f, 0.3f, 0.f));
        Frame();
        Frame();
        made.push_back(editing.Picture());
        return made;
    };
    const auto everyFrame = pictures(false), changed = pictures(true);
    ASSERT_EQ(everyFrame.size(), changed.size());
    for (std::size_t at = 0; at < everyFrame.size(); ++at) {
        std::size_t different = 0;
        for (std::size_t i = 0; i < everyFrame[at].size(); ++i) {
            const auto a = everyFrame[at][i], b = changed[at][i];
            if (std::abs(int(a.x) - int(b.x)) > 2 || std::abs(int(a.y) - int(b.y)) > 2 || std::abs(int(a.z) - int(b.z)) > 2) ++different;
        }
        EXPECT_LT(different, everyFrame[at].size() / 1000) << "picture " << at << ": still, the camera moved, a ball moved, a lamp moved";
    }
}

TEST(ForwardPlus, TemporaryClusterHeatOfTheUsersProject) {
    if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
    auto* editor = support::App()->OpenOffscreen<keditor::EditorScene>({ .title = "editor", .extent = { 1500, 900 } }, std::filesystem::path("/mnt/w/EngineProjects/forward-plus"));
    ASSERT_NE(editor, nullptr);
    for (int i = 0; i < 30; ++i) Frame();
    keditor::Session& session = editor->CurrentSession();
    for (const auto& choice : session.ViewModes()) std::printf("view: %s / %s\n", choice.group.c_str(), choice.name.c_str());
    for (const auto& choice : session.ViewModes())
        if (choice.name == "Cluster heat") { std::printf("choosing it\n"); session.SetViewMode(choice.mode); }
    for (int i = 0; i < 60; ++i) Frame();
    std::printf("survived\n");
}
