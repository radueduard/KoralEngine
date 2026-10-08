// E6's gate, on the engine's own side: shadow mapping, with the scripts written as C functions. A script moves the
// light; a graphics script turns it into the camera the shadow pass draws from; and the picture shows the shadow
// where it should fall, and following the light. The project it makes is saved for the C# and Kotlin tests, which
// run the same scene with the scripts written in their languages.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include <app.h>
#include <buffer.h>
#include <commandBuffer.h>
#include <context.h>
#include <frameGraph.h>
#include <scene.h>
#include <scheduler.h>
#include <shader.h>

#include "shadow_scene.h"

extern kor::App* TestApp();
extern const std::string& TestAppReason();
extern void TestFrame();

namespace {
    constexpr kor::u32 Size = 32;

    class ReadScreen final : public kor::RenderPass {
    public:
        explicit ReadScreen(kor::ResourceRef<const kor::Buffer> out) : RenderPass("Read screen"), _out(std::move(out)) {}
        void Setup(kor::PassBuilder& b) override { b.Read(kor::FrameGraph::Screen, kor::Image::Usage::eTransferSrc).SideEffect(); }
        void Initialize(const kor::PassResources& r) override { _screen = r.ImageNamed(kor::FrameGraph::Screen); }
        void Record(kor::CommandBuffer& cb) const override { cb.CopyImageToBuffer(_screen, _out); }
    private:
        kor::ResourceRef<const kor::Buffer> _out;
        kor::ResourceRef<const kor::Image> _screen;
    };

    // The scripts, as a language's layer registers them.
    struct Light { float slantX, slantY, speed; };
    struct LightCamera { float reach; };
    struct Shared { KengineDocument* doc = nullptr; KecsWorld* world = nullptr; uint32_t light = KECS_NO_COMPONENT; };
}

TEST(ShadowGate, AScriptMovesTheLightAndAGraphicsScriptMakesItsCamera) {
    if (!TestApp()) GTEST_SKIP() << "no Vulkan device: " << TestAppReason();
    // The project: its shaders in place before anything asks for them.
    const std::filesystem::path project = KENGINE_SHADOW_PROJECT;
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project / "shaders");
    std::filesystem::create_directories(project / "scenes");
    for (const char* shader : { "shadow.slang", "lit.slang" })
        std::filesystem::copy_file(std::filesystem::path(KENGINE_SHADOW_SHADERS) / shader, project / "shaders" / shader);
    { std::ofstream(project / "project.json") << "{\n  \"koral-engine-project\": 1,\n  \"name\": \"Shadow\",\n  \"scenes\": [\"main\"],\n  \"start\": \"main\"\n}\n"; }
    kor::Shader::AddSearchPath(project / "shaders", true);

    kor::Scene* scene = TestApp()->OpenOffscreen("Play", { .title = "shadow", .extent = { Size, Size } });
    ASSERT_NE(scene, nullptr);
    const auto readback = kor::Buffer::RawBuilder{}.SetRawSize(Size * Size * 4).SetUsage(kor::Buffer::Usage::eTransferDst).SetType(kor::Buffer::Type::eReadback).Build();
    scene->Graph().Add<ReadScreen>(kor::ResourceRef<const kor::Buffer>(readback));
    const auto pixel = [&](const kor::u32 x, const kor::u32 y) { return readback->Read<kor::U8Vec4>(Size * Size)[y * Size + x]; };
    const auto lit = [&](const kor::u32 x, const kor::u32 y) { return pixel(x, y).x > 200; };
    const auto dark = [&](const kor::u32 x, const kor::u32 y) { const auto p = pixel(x, y); return p.x > 30 && p.x < 80; };

    KengineDocument* doc = kengine_document(reinterpret_cast<KoralFrameGraph*>(&scene->Graph()));
    ASSERT_NE(doc, nullptr);
    KecsWorld* world = kengine_document_world(doc);
    ASSERT_TRUE(shadow::BuildGraph(doc));
    const shadow::Entities entities = shadow::BuildEntities(doc);
    ASSERT_TRUE(shadow::s_ok);
    // Saved before any script is on anything: what the other languages' tests open, to put their own on.
    ASSERT_TRUE(kengine_document_save(doc, (project / "scenes" / "main.scene.json").string().c_str())) << kengine_last_error();

    // With no camera written, nothing is where the light looks: the buffer is whatever it was made as.
    Shared shared { doc, world, KECS_NO_COMPONENT };
    KengineScriptCallbacks move {};
    move.user = &shared;
    move.update = [](void*, KecsEntity, void* data, float) { auto& light = *static_cast<Light*>(data); light.slantX += light.speed; };
    const KecsField lightFields[] { { "slantX", "f32", 0, 4 }, { "slantY", "f32", 4, 4 }, { "speed", "f32", 8, 4 } };
    shared.light = kengine_script_register(doc, "Light", sizeof(Light), alignof(Light), lightFields, 3, &move);
    ASSERT_NE(shared.light, KECS_NO_COMPONENT) << kengine_last_error();

    KengineScriptCallbacks camera {};
    camera.user = &shared;
    camera.prepare = [](void* user, const KecsEntity entity, void* data) {
        const auto& shared = *static_cast<Shared*>(user);
        const auto* light = static_cast<const Light*>(kecs_get(shared.world, entity, shared.light));
        if (!light) return;
        const float reach = static_cast<const LightCamera*>(data)->reach;
        float matrix[16];
        shadow::LightCamera(light->slantX * reach, light->slantY * reach, matrix);
        if (KoralResource* buffer = kengine_node_resource(shared.doc, "light camera", nullptr)) {
            koral_buffer_write(buffer, matrix, sizeof(matrix), 0);
            koral_resource_release(buffer);
        }
    };
    const KecsField cameraFields[] { { "reach", "f32", 0, 4 } };
    const uint32_t lightCamera = kengine_script_register(doc, "LightCamera", sizeof(LightCamera), alignof(LightCamera), cameraFields, 1, &camera);
    ASSERT_NE(lightCamera, KECS_NO_COMPONENT) << kengine_last_error();

    const Light straight { 0.f, 0.f, 0.f };
    const LightCamera whole { 1.f };
    ASSERT_TRUE(kecs_add(world, entities.light, shared.light, &straight));
    ASSERT_TRUE(kecs_add(world, entities.light, lightCamera, &whole));
    TestFrame();
    TestFrame();

    // The light straight overhead: the shadow is under the occluder, in the middle. The occluder is a quarter of
    // the picture wide (8 pixels of 32), and is not itself drawn.
    EXPECT_TRUE(dark(16, 16)) << "the middle is in shadow: " << int(pixel(16, 16).x);
    EXPECT_TRUE(dark(13, 18));
    EXPECT_TRUE(lit(4, 4)) << "a corner is lit: " << int(pixel(4, 4).x);
    EXPECT_TRUE(lit(24, 16)) << "and so is the ground beside it";
    EXPECT_TRUE(lit(9, 16) && lit(22, 16)) << "the shadow is as wide as what casts it, and no wider";

    // The light leaning: half a unit up, the occluder's shadow falls half the lean away. A lean of 1 moves it a
    // quarter of the picture to the right: 8 pixels.
    EXPECT_TRUE(kengine_field_set_json(doc, entities.light, "Light", "slantX", "1"));
    TestFrame();
    TestFrame();
    EXPECT_TRUE(dark(24, 16)) << "the shadow moved with the light: " << int(pixel(24, 16).x);
    EXPECT_TRUE(lit(16, 16)) << "and left where it was: " << int(pixel(16, 16).x);

    // The script moving it: a step a frame, back the other way.
    EXPECT_TRUE(kengine_field_set_json(doc, entities.light, "Light", "speed", "-0.25"));
    for (int i = 0; i < 8; ++i) TestFrame();
    const auto* now = static_cast<const Light*>(kecs_get(world, entities.light, shared.light));
    EXPECT_FLOAT_EQ(now->slantX, -1.f);
    TestFrame();    // the picture of the last step
    EXPECT_TRUE(kengine_field_set_json(doc, entities.light, "Light", "speed", "0"));
    EXPECT_TRUE(kengine_field_set_json(doc, entities.light, "Light", "slantX", "-1"));
    TestFrame();
    TestFrame();
    EXPECT_TRUE(dark(8, 16)) << "the shadow followed it to the left: " << int(pixel(8, 16).x);
    EXPECT_TRUE(lit(16, 16) && lit(24, 16));

    // The passes are ordered by what they use: the shadow map is drawn before it is read.
    std::vector<std::string> order;
    for (const auto& pass : scene->Graph().Schedule()) order.push_back(pass.name);
    EXPECT_EQ(order, (std::vector<std::string> { "cast", "draw", "show", "Read screen" }));

    TestApp()->Close(*scene);
    TestFrame();
}
