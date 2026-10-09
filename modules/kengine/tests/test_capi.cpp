// The engine through its C interface alone, as a program in another language has it: the framework, this library
// loaded for its scenes, and nothing of the engine's C++. Offscreen, with no windowing system.

#include <gtest/gtest.h>

#include <array>

#include <cstring>
#include <string>
#include <vector>

#include <app.h>
#include <context.h>
#include <scene.h>
#include <scheduler.h>

#include <koral_engine.h>

namespace {
    std::unique_ptr<kor::App> s_app;
    std::string s_reason = "no device";

    class Environment final : public ::testing::Environment {
    public:
        void SetUp() override {
            try {
                s_app = std::make_unique<kor::App>(kor::AppSettings { .platform = kor::WindowPlatform::eNone });
                s_app->LoadLibrary(KENGINE_LIBRARY);
            } catch (const std::exception& e) {
                s_reason = e.what();
                s_app.reset();
            }
        }
        void TearDown() override { s_app.reset(); }
    };
    const auto* const s_environment = ::testing::AddGlobalTestEnvironment(new Environment);

    void Frame()
    {
        s_app->Frame();
        kor::Context::Scheduler().WaitIdle();
    }
}

// For the other tests of this program: the one application there is.
kor::App* TestApp() { return s_app.get(); }
const std::string& TestAppReason() { return s_reason; }
void TestFrame() { Frame(); }

namespace {

    class CApiTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!s_app) GTEST_SKIP() << "no Vulkan device: " << s_reason;
            scene = s_app->OpenOffscreen("Play", { .title = "play", .extent = { 32, 32 } });
            ASSERT_NE(scene, nullptr) << "the library's Play scene";
            // What koral_scene_graph hands a program that opened the scene through koral_c.h.
            doc = kengine_document(reinterpret_cast<KoralFrameGraph*>(&scene->Graph()));
            ASSERT_NE(doc, nullptr) << kengine_last_error();
            world = kengine_document_world(doc);
            ASSERT_NE(world, nullptr);
        }
        void TearDown() override {
            if (scene) { s_app->Close(*scene); Frame(); }
        }
        kor::Scene* scene = nullptr;
        KengineDocument* doc = nullptr;
        KecsWorld* world = nullptr;
    };

    // A script as another language registers one: its data a plain struct, its behaviour plain functions.
    struct Turner {
        float angle;
        float speed;
        int32_t updates;
        uint32_t started;
    };
    struct Seen {
        KengineDocument* doc = nullptr;
        std::vector<std::string> calls;
        float prepared = -1.f;
    };
    KengineScriptCallbacks SpinCallbacks(Seen& seen)
    {
        KengineScriptCallbacks callbacks {};
        callbacks.user = &seen;
        callbacks.created = [](void* user, KecsEntity, void*) { static_cast<Seen*>(user)->calls.emplace_back("created"); };
        callbacks.started = [](void* user, KecsEntity, void* data) { static_cast<Turner*>(data)->started = 1; static_cast<Seen*>(user)->calls.emplace_back("started"); };
        callbacks.update = [](void*, KecsEntity, void* data, const float delta) {
            auto& spin = *static_cast<Turner*>(data);
            spin.angle += spin.speed * (delta > 0.f ? 1.f : 1.f);   // a turn a frame, whatever the clock says
            ++spin.updates;
        };
        // The graphics half: what a pass takes, made from the entity's data before anything is drawn.
        callbacks.prepare = [](void* user, KecsEntity, void* data) {
            auto& seen = *static_cast<Seen*>(user);
            KoralResource* buffer = kengine_node_resource(seen.doc, "angles", nullptr);
            if (!buffer) return;
            koral_buffer_write(buffer, &static_cast<Turner*>(data)->angle, sizeof(float), 0);
            koral_resource_release(buffer);
            seen.prepared = static_cast<Turner*>(data)->angle;
        };
        callbacks.destroyed = [](void* user, KecsEntity, void*) { static_cast<Seen*>(user)->calls.emplace_back("destroyed"); };
        return callbacks;
    }
    const KecsField SpinFields[] {
        { "angle", "f32", offsetof(Turner, angle), 4 }, { "speed", "f32", offsetof(Turner, speed), 4 },
        { "updates", "i32", offsetof(Turner, updates), 4 }, { "started", "bool32", offsetof(Turner, started), 4 } };
}

TEST_F(CApiTest, EntitiesAndComponentsByHandle) {
    EXPECT_EQ(kecs_entity_count(world), 0u);
    const KecsEntity lamp = kengine_entity_create(doc, "Lamp");
    EXPECT_TRUE(kecs_alive(world, lamp));
    EXPECT_STREQ(kengine_entity_name(doc, lamp), "Lamp");
    EXPECT_TRUE(kengine_entity_set_name(doc, lamp, "Lantern"));
    EXPECT_STREQ(kengine_entity_name(doc, lamp), "Lantern");

    const uint32_t lit = kecs_register_tag(world, "Lit");
    ASSERT_NE(lit, KECS_NO_COMPONENT);
    EXPECT_EQ(kecs_register_tag(world, "Lit"), lit) << "the one there is";
    EXPECT_EQ(kecs_component_find(world, "Lit"), lit);
    EXPECT_EQ(kecs_component_find(world, "Unheard of"), KECS_NO_COMPONENT);
    EXPECT_STREQ(kecs_component_name(world, lit), "Lit");
    EXPECT_EQ(kecs_component_size(world, lit), 0u);

    EXPECT_TRUE(kecs_add(world, lamp, lit, nullptr));
    EXPECT_TRUE(kecs_has(world, lamp, lit));
    const KecsEntity bare = kecs_create(world);
    const KecsEntity child = kecs_create(world);
    EXPECT_TRUE(kecs_set_parent(world, child, lamp));
    EXPECT_EQ(kecs_parent(world, child).index, lamp.index);

    // A name, a place in the hierarchy and a transform: every entity's, and the world's own.
    EXPECT_EQ(kecs_child_count(world, lamp), 1u);
    EXPECT_EQ(kecs_child(world, lamp, 0).index, child.index);
    EXPECT_EQ(kecs_child(world, lamp, 1).index, KECS_NULL_INDEX);
    const KecsEntity none { KECS_NULL_INDEX, 0 };
    EXPECT_EQ(kecs_child_count(world, none), 2u) << "at the top: the lamp, and the bare one";
    kengine_entity_set_name(doc, child, "Shade");
    EXPECT_EQ(kecs_find(world, "Shade").index, child.index);
    EXPECT_EQ(kecs_find(world, "Nobody").index, KECS_NULL_INDEX);
    const float there[3] { 10, 0, 0 }, here[3] { 1, 2, 3 }, twice[3] { 2, 2, 2 };
    EXPECT_TRUE(kecs_set_position(world, lamp, there));
    EXPECT_TRUE(kecs_set_scale(world, lamp, twice));
    EXPECT_TRUE(kecs_set_position(world, child, here));
    float read[3] {}, matrix[16] {};
    EXPECT_TRUE(kecs_position(world, child, read));
    EXPECT_EQ(read[1], 2.f);
    EXPECT_TRUE(kecs_scale(world, child, read));
    EXPECT_EQ(read[0], 1.f) << "its own scale";
    EXPECT_TRUE(kecs_world_matrix(world, child, matrix));
    EXPECT_FLOAT_EQ(matrix[12], 12.f) << "twice as far out as its own place, from where its parent is";
    EXPECT_FLOAT_EQ(matrix[0], 2.f) << "and twice as big";
    EXPECT_TRUE(kecs_local_matrix(world, child, matrix));
    EXPECT_FLOAT_EQ(matrix[12], 1.f);
    EXPECT_TRUE(kecs_set_parent_at(world, child, none, 0, true));
    EXPECT_EQ(kecs_child(world, none, 0).index, child.index) << "first at the top, as asked";
    EXPECT_TRUE(kecs_position(world, child, read));
    EXPECT_FLOAT_EQ(read[0], 12.f) << "where it was in the world";
    EXPECT_TRUE(kecs_set_parent_at(world, child, lamp, -1, false));
    EXPECT_TRUE(kecs_world_matrix(world, child, matrix));
    EXPECT_FLOAT_EQ(matrix[12], 34.f) << "its own numbers kept, under the parent again";
    const float identity[16] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    EXPECT_TRUE(kecs_set_world_matrix(world, child, identity));
    EXPECT_TRUE(kecs_position(world, child, read));
    EXPECT_FLOAT_EQ(read[0], -5.f) << "at the middle of the world: half way back, in its parent's terms";
    EXPECT_FALSE(kecs_position(world, none, read));
    EXPECT_TRUE(kecs_set_position(world, lamp, std::array<float, 3> {}.data()));
    EXPECT_TRUE(kecs_set_scale(world, lamp, std::array<float, 3> { 1, 1, 1 }.data()));

    // A query: how many, then which.
    EXPECT_EQ(kecs_query(world, &lit, 1, nullptr, 0, false, nullptr, 0), 1u);
    KecsEntity found[4] {};
    EXPECT_EQ(kecs_query(world, &lit, 1, nullptr, 0, false, found, 4), 1u);
    EXPECT_EQ(found[0].index, lamp.index);
    EXPECT_EQ(kecs_query(world, nullptr, 0, &lit, 1, false, found, 4), 2u) << "the two without it";
    EXPECT_TRUE(kecs_set_enabled(world, lamp, lit, false));
    EXPECT_FALSE(kecs_is_enabled(world, lamp, lit));
    EXPECT_EQ(kecs_query(world, &lit, 1, nullptr, 0, false, nullptr, 0), 0u) << "switched off, it does not match";
    EXPECT_EQ(kecs_query(world, &lit, 1, nullptr, 0, true, nullptr, 0), 1u) << "unless asked";
    const uint32_t nothing = 9999;
    EXPECT_EQ(kecs_query(world, &nothing, 1, nullptr, 0, false, nullptr, 0), 0u);

    EXPECT_TRUE(kecs_remove(world, lamp, lit));
    EXPECT_FALSE(kecs_has(world, lamp, lit));
    EXPECT_TRUE(kecs_destroy(world, lamp));
    EXPECT_FALSE(kecs_alive(world, child)) << "what was under it went with it";
    EXPECT_TRUE(kecs_alive(world, bare));
    EXPECT_FALSE(kecs_add(world, lamp, lit, nullptr));
    EXPECT_NE(std::string(kengine_last_error()).find("not alive"), std::string::npos) << kengine_last_error();
}

TEST_F(CApiTest, AScriptIsAComponentWithFunctionsCalledOnIt) {
    Seen seen;
    seen.doc = doc;
    const KengineScriptCallbacks callbacks = SpinCallbacks(seen);
    const uint32_t spin = kengine_script_register(doc, "Turner", sizeof(Turner), alignof(Turner), SpinFields, 4, &callbacks);
    ASSERT_NE(spin, KECS_NO_COMPONENT) << kengine_last_error();
    EXPECT_EQ(kecs_component_field_count(world, spin), 4u);
    KecsField field {};
    ASSERT_TRUE(kecs_component_field(world, spin, 1, &field));
    EXPECT_STREQ(field.name, "speed");
    EXPECT_STREQ(field.type, "f32");
    EXPECT_EQ(field.offset, 4u);
    EXPECT_FALSE(kecs_component_field(world, spin, 4, &field));

    // A buffer in the graph, for the script to fill each frame.
    ASSERT_STREQ(kengine_node_add(doc, "Buffer", "angles", 0, 0), "angles");
    ASSERT_TRUE(kengine_node_set_input_json(doc, "angles", "instanceCount", "16"));
    ASSERT_TRUE(kengine_node_set_input_json(doc, "angles", "usage", "[\"eStorage\"]"));
    ASSERT_TRUE(kengine_node_ok(doc, "angles")) << kengine_last_error();

    const KecsEntity wheel = kengine_entity_create(doc, "Wheel");
    const Turner start { 10.f, 2.f, 0, 0 };
    ASSERT_TRUE(kecs_add(world, wheel, spin, &start));
    Frame();
    Frame();
    Frame();
    EXPECT_EQ(seen.calls, (std::vector<std::string> { "created", "started" }));
    const auto* now = static_cast<const Turner*>(kecs_get(world, wheel, spin));
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(now->updates, 3);
    EXPECT_FLOAT_EQ(now->angle, 16.f);
    EXPECT_EQ(now->started, 1u);

    // What prepare wrote is in the node's buffer: read back through the framework's own C interface.
    EXPECT_FLOAT_EQ(seen.prepared, 16.f);
    KoralResource* buffer = kengine_node_resource(doc, "angles", "out");
    ASSERT_NE(buffer, nullptr) << kengine_last_error();
    float written = 0.f;
    EXPECT_EQ(koral_buffer_read(buffer, &written, sizeof(float), 0), 0) << koral_last_error();
    koral_resource_release(buffer);
    EXPECT_FLOAT_EQ(written, 16.f);

    // Its fields are fields like any other: as JSON, for whatever shows or saves them.
    EXPECT_STREQ(kengine_field_json(doc, wheel, "Turner", "speed"), "2");
    EXPECT_TRUE(kengine_field_set_json(doc, wheel, "Turner", "speed", "5"));
    Frame();
    EXPECT_FLOAT_EQ(static_cast<const Turner*>(kecs_get(world, wheel, spin))->angle, 21.f);
    EXPECT_FALSE(kengine_field_set_json(doc, wheel, "Turner", "speed", "[1, 2]"));
    EXPECT_NE(std::string(kengine_last_error()).find("number"), std::string::npos) << kengine_last_error();
    EXPECT_NE(std::string(kengine_document_json(doc)).find("\"Turner\""), std::string::npos) << "and it is in the scene's file";

    // The script loaded again with a field renamed: the data that kept its name is kept.
    kengine_script_unregister(doc, "Turner");
    Frame();
    EXPECT_EQ(static_cast<const Turner*>(kecs_get(world, wheel, spin))->updates, 4) << "nothing ran while its code was gone";
    KecsField edited[4];
    std::memcpy(edited, SpinFields, sizeof(edited));
    edited[1].name = "rate";
    EXPECT_EQ(kengine_script_register(doc, "Turner", sizeof(Turner), alignof(Turner), edited, 4, &callbacks), spin);
    const auto* after = static_cast<const Turner*>(kecs_get(world, wheel, spin));
    EXPECT_FLOAT_EQ(after->angle, 21.f);
    EXPECT_FLOAT_EQ(after->speed, 0.f) << "what was 'speed' is 'rate', and new";
    EXPECT_EQ(after->updates, 4);

    EXPECT_TRUE(kecs_destroy(world, wheel));
    EXPECT_EQ(seen.calls.back(), "destroyed");

    // What cannot be done says why.
    EXPECT_EQ(kengine_node_add(doc, "NoSuchNode", nullptr, 0, 0), nullptr);
    EXPECT_NE(std::string(kengine_last_error()).find("NoSuchNode"), std::string::npos);
    EXPECT_EQ(kengine_node_resource(doc, "nothing", nullptr), nullptr);
    EXPECT_EQ(kengine_document(nullptr), nullptr);
    EXPECT_EQ(kengine_script_register(doc, "", 4, 4, nullptr, 0, nullptr), KECS_NO_COMPONENT);
}
