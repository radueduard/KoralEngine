// The engine's physics functions on the GPU — SpheresStep, kphysics' sequential impulses a colour of contacts at a time,
// for spheres; and XpbdStep, small substeps each body solves by itself, for spheres, boxes and capsules — against
// kphysics on the CPU, set up alike: one substep, ten iterations, nothing asleep. Not the same numbers, but the same
// behaviour: a sphere falls as it does there, rests on the ground, rolls at five sevenths of the speed it slid at,
// bounces as high, and a pile of them settles as theirs does; a box rests flat, slides to a stop as friction says, and
// five stand in a stack; a capsule lying down stays lying down.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <random>
#include <ranges>
#include <sstream>

#include <kecs/json.h>
#include <kgraph/runtime.h>
#include <kphysics/kphysics.h>

#include "support.h"

namespace {
    using support::Frame;
    using support::GraphScene;
    using support::Size;
    using kgraph::Value;
    constexpr float Dt = 1.f / 60.f;
    enum Shape : int { eSphere = 0, eBox = 1, eCapsule = 2 };

    std::shared_ptr<const kgraph::Graph> EngineFunction(const std::string& name)
    {
        const std::filesystem::path file = std::filesystem::path(KGRAPH_PACKAGE_SHADERS).parent_path() / "functions" / (name + ".kfunc");
        std::ifstream in(file, std::ios::binary);
        std::stringstream read;
        read << in.rdbuf();
        const std::string text = read.str();
        kecs::json::Value parsed;
        std::string error;
        auto graph = std::make_shared<kgraph::Graph>();
        if (!kecs::json::Parse(text, parsed, error) || !parsed.Find("graph") || !kgraph::Graph::FromJson(parsed.Find("graph")->raw, *graph, error))
            ADD_FAILURE() << name << ": " << error;
        return graph;
    }

    /** A body as the GPU has it: what both solvers keep where (SpheresStep's a body of 64 bytes, XpbdStep's 112). */
    struct Gpu {
        float position[3], invMass, velocity[3], w1, spin[3], w2, orientation[4];
    };

    class GpuPhysicsTest : public ::testing::TestWithParam<std::string> {
    protected:
        [[nodiscard]] bool Xpbd() const { return GetParam() == "XpbdStep"; }
        [[nodiscard]] std::size_t Stride() const { return Xpbd() ? 112 : 64; }

        void SetUp() override {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            GraphScene::withWorld = true;
            scene = support::App()->OpenOffscreen<GraphScene>({ .title = "physics", .extent = { Size, Size } });
            ASSERT_NE(scene, nullptr);
            // A body: its shape (0 a sphere, 1 a box, 2 a capsule), what it is made of, its size, and how it moves to begin with.
            kecs::ComponentDescription body;
            body.name = "Body";
            body.size = 48;
            body.alignment = 4;
            const auto field = [](const char* name, const char* type, std::size_t offset, std::size_t size) {
                kecs::FieldDescription f;
                f.name = name; f.type = type; f.offset = offset; f.size = size;
                return f;
            };
            body.fields = { field("shape", "f32", 0, 4), field("mass", "f32", 4, 4), field("friction", "f32", 8, 4), field("restitution", "f32", 12, 4),
                            field("size", "vec3", 16, 12), field("velocity", "vec3", 32, 12) };
            scene->world.Register(std::move(body));
            cpu = std::make_unique<kphysics::Simulation>(1 << 18);
            cpu->substeps = 1;
            cpu->solver.iterations = 10;
            cpu->islands.allowSleep = false;
            (void)cpu->Add({ 0, 0, 0 }, {}, 0.f, { 1, 1, 1 });
            cpu->colliders.Add(kphysics::Plane, {});
        }
        void TearDown() override {
            GraphScene::withWorld = false;
            if (scene) { support::App()->Close(*scene); Frame(); }
        }

        /** A body, here and in kphysics alike: a sphere of radius size.x; a box of half extents size; a capsule of radius size.x, its ends size.y from its middle. */
        int Body(const Shape shape, const kecs::Vec3 at, const kecs::Vec3 size, const float mass, const float friction, const float restitution,
                 const kecs::Vec3 velocity = {}, const kecs::Vec3 degrees = {}) {
            auto& world = scene->world;
            const kecs::Entity e = world.Create();
            world.SetPosition(e, at);
            world.SetRotation(e, degrees);
            support::Give(world, e, "Body", { { "shape", { static_cast<float>(shape) } }, { "mass", { mass } }, { "friction", { friction } },
                                             { "restitution", { restitution } }, { "size", { size.x, size.y, size.z } },
                                             { "velocity", { velocity.x, velocity.y, velocity.z } } });
            kphysics::Vec3 inertia;
            if (shape == eSphere) inertia = kphysics::Vec3 { 1, 1, 1 } * (0.4f * mass * size.x * size.x);
            else if (shape == eBox) inertia = { mass / 3.f * (size.y * size.y + size.z * size.z), mass / 3.f * (size.x * size.x + size.z * size.z), mass / 3.f * (size.x * size.x + size.y * size.y) };
            else { const float unit = kphysics::mass::Capsule(1.f, size.x, size.y, inertia); inertia = inertia * (mass / unit); }
            kphysics::Material material;
            material.friction = friction;
            material.restitution = restitution;
            const kecs::Quat q = world.OrientationOf(e);
            const int b = cpu->Add({ at.x, at.y, at.z }, { q.x, q.y, q.z, q.w }, mass, inertia, material);
            cpu->colliders.Add(shape == eSphere ? kphysics::Sphere : shape == eBox ? kphysics::Box : kphysics::Capsule,
                               shape == eSphere ? kphysics::Vec3 { size.x, 0, 0 } : kphysics::Vec3 { size.x, size.y, size.z });
            cpu->bodies.velocity[b] = { velocity.x, velocity.y, velocity.z };
            return b;
        }

        /**
         * The graph that runs them: setup copies each body's state from the world into the GPU's buffers (and, for
         * XpbdStep, works out each one's inverse mass and inertia); the fixed step runs the solver; every frame writes
         * where each is back.
         */
        void Build(const float cell) {
            kgraph::Graph graph;
            for (const std::string& name : { std::string("Scan"), std::string("RadixSort"), std::string("IndirectArgs"), std::string("XpbdPrepare"), GetParam() })
                graph.functions[name] = EngineFunction(name);
            graph.passes = { "made", "simulate", "frame" };
            graph.setup = { "made" };
            graph.fixed = { "simulate" };
            graph.Add("bodies of", "ForEach").Set("component", "Body");
            const auto made = [&](kgraph::Node& node) -> kgraph::Node& { node.pass = "made"; return node; };
            made(graph.Add("at", "Split")).Wire("vector", "bodies of", "worldPosition");
            made(graph.Add("v", "Split")).Wire("vector", "bodies of", "field.velocity");
            made(graph.Add("size", "Split")).Wire("vector", "bodies of", "field.size");
            const auto field = [](const char* name) { return Value::Members { { "name", name }, { "type", "eVec4" } }; };
            if (Xpbd()) {
                made(graph.Add("p4", "Vec4")).Wire("x", "at", "x").Wire("y", "at", "y").Wire("z", "at", "z");
                made(graph.Add("v4", "Vec4")).Wire("x", "v", "x").Wire("y", "v", "y").Wire("z", "v", "z");
                made(graph.Add("s4", "Vec4")).Wire("w", "bodies of", "field.shape");
                made(graph.Add("z4", "Vec4")).Wire("x", "size", "x").Wire("y", "size", "y").Wire("z", "size", "z");
                made(graph.Add("i4", "Vec4")).Wire("w", "bodies of", "field.mass");
                made(graph.Add("initial", "BufferBuilder")).Set("layout", Value::List { field("positionInvMass"), field("velocityReach"), field("spinShape"),
                                                                                     field("orientation"), field("size"), field("invInertia"), field("placed") })
                    .Wire("field.positionInvMass", "p4").Wire("field.placed", "p4").Wire("field.velocityReach", "v4").Wire("field.spinShape", "s4").Wire("field.orientation", "bodies of", "orientation")
                    .Wire("field.size", "z4").Wire("field.invInertia", "i4");
            } else {
                made(graph.Add("massive", "Step")).Set("edge", 1e-9).Wire("x", "bodies of", "field.mass");
                made(graph.Add("some mass", "Max")).Wire("a", "bodies of", "field.mass").Set("b", 1e-9);
                made(graph.Add("inv mass", "Divide")).Wire("a", "massive").Wire("b", "some mass");
                made(graph.Add("r2", "Multiply")).Wire("a", "size", "x").Wire("b", "size", "x");
                made(graph.Add("i0", "Multiply")).Wire("a", "r2").Set("b", 0.4);
                made(graph.Add("inv inertia", "Divide")).Wire("a", "inv mass").Wire("b", "i0");
                made(graph.Add("p4", "Vec4")).Wire("x", "at", "x").Wire("y", "at", "y").Wire("z", "at", "z").Wire("w", "inv mass");
                made(graph.Add("v4", "Vec4")).Wire("x", "v", "x").Wire("y", "v", "y").Wire("z", "v", "z").Wire("w", "size", "x");
                made(graph.Add("s4", "Vec4")).Wire("w", "inv inertia");
                made(graph.Add("initial", "BufferBuilder")).Set("layout", Value::List { field("positionInvMass"), field("velocityRadius"), field("spinInvInertia"), field("orientation") })
                    .Wire("field.positionInvMass", "p4").Wire("field.velocityRadius", "v4").Wire("field.spinInvInertia", "s4").Wire("field.orientation", "bodies of", "orientation");
            }
            made(graph.Add("m4", "Vec4")).Wire("x", "bodies of", "field.friction").Wire("y", "bodies of", "field.restitution");
            made(graph.Add("initial materials", "BufferBuilder")).Set("layout", Value::List { field("material") }).Wire("field.material", "m4");
            // How many there are, from the buffer of them: one number. (A ForEach's own `count` is given with each of its
            // elements, and what is wired from it is worked out once for each.)
            graph.Add("body bytes", "Multiply").Wire("a", "initial", "count").Set("b", static_cast<double>(Stride()));
            graph.Add("material bytes", "Multiply").Wire("a", "initial", "count").Set("b", 16);
            const Value storage = Value::List { "eStorage", "eTransferSrc", "eTransferDst" };
            made(graph.Add("bodies", "Buffer")).Wire("instanceCount", "body bytes").Set("usage", storage);
            made(graph.Add("materials", "Buffer")).Wire("instanceCount", "material bytes").Set("usage", storage);
            made(graph.Add("copy bodies", "CopyBuffer")).Wire("source", "initial").Wire("destination", "bodies");
            made(graph.Add("copy materials", "CopyBuffer")).Wire("source", "initial materials").Wire("destination", "materials");
            if (Xpbd()) made(graph.Add("prepared", "Function")).Set("function", "XpbdPrepare").Wire("bodies", "bodies").Wire("count", "initial", "count");
            graph.Add("step", "FixedStep").pass = "simulate";
            graph.Add("physics", "Function").Set("function", GetParam()).Wire("bodies", "bodies").Wire("materials", "materials").Wire("count", "initial", "count")
                .Wire("dt", "step", "dt").Set("gravity", Value::List { 0, -9.81, 0 }).Set("cell", cell).Set("ground", 0).pass = "simulate";
            if (Xpbd()) graph.Find("physics")->Wire("shared", "prepared", "shared");
            graph.Add("back", "WriteBack").Wire("buffer", "physics", "bodies").Set("forEach", "bodies of").Set("stride", static_cast<double>(Stride()))
                .Set("position", Xpbd() ? 96 : 0).Set("rotation", 48).Set("fields", Value::Members { { "Body.velocity", 16 } }).pass = "frame";
            auto& runtime = *scene->runtime;
            runtime.Set(graph);
            for (const auto& problem : runtime.Problems()) ADD_FAILURE() << problem;
            runtime.Watch("bodies");
            for (int i = 0; i < 4; ++i) Frame();
        }

        /** So many steps, here and there; and frames after, with none, for what the GPU did to be read back. */
        void Steps(const int count, const bool alsoCpu = true) {
            for (int i = 0; i < count; ++i) {
                scene->runtime->FixedStep(Dt);
                Frame();
                if (alsoCpu) cpu->Step(Dt);
            }
            for (int i = 0; i < 4; ++i) Frame();
        }
        [[nodiscard]] std::vector<Gpu> OnTheGpu() const {
            const auto peek = scene->runtime->Watched("bodies");
            std::vector<Gpu> out(peek ? peek->bytes.size() / Stride() : 0);
            for (std::size_t i = 0; i < out.size(); ++i) std::memcpy(&out[i], peek->bytes.data() + i * Stride(), sizeof(Gpu));
            return out;
        }
        /** XPBD's substeps move a falling body a little otherwise than kphysics' one step does: a few centimetres in half a second. */
        [[nodiscard]] float InTheAir() const { return Xpbd() ? 0.05f : 1e-3f; }

        GraphScene* scene = nullptr;
        std::unique_ptr<kphysics::Simulation> cpu;
    };

    float Speed(const Gpu& b) { return std::hypot(b.velocity[0], b.velocity[1], b.velocity[2]); }
    /** How far a body's orientation is from none: 0 unturned, up to 1. */
    float Turned(const Gpu& b) { return std::hypot(b.orientation[0], b.orientation[1], b.orientation[2]); }
}

INSTANTIATE_TEST_SUITE_P(Solvers, GpuPhysicsTest, ::testing::Values("SpheresStep", "XpbdStep"), [](const auto& info) { return info.param; });

// ---- spheres --------------------------------------------------------------------------------------------------------

TEST_P(GpuPhysicsTest, AFallingSphereFallsAsInKphysicsAndRestsOnTheGround) {
    const int b = Body(eSphere, { 0, 5, 0 }, { 0.5f, 0, 0 }, 1.f, 0.5f, 0.f);
    Build(1.f);
    Steps(30);
    auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), 1u);
    EXPECT_NEAR(gpu[0].position[1], cpu->bodies.position[b].y, InTheAir()) << "in the air: the same steps";
    EXPECT_NEAR(gpu[0].velocity[1], cpu->bodies.velocity[b].y, InTheAir() * 0.4f) << "a velocity from how far it went: rounded so in XPBD's small steps";
    Steps(150);
    gpu = OnTheGpu();
    EXPECT_NEAR(gpu[0].position[1], 0.5f, 0.01f) << "resting on the ground";
    EXPECT_NEAR(gpu[0].position[1], cpu->bodies.position[b].y, 0.01f);
    EXPECT_NEAR(gpu[0].velocity[1], 0.f, 0.05f);
    EXPECT_NEAR(scene->world.PositionOf(scene->world.All().front()).y, gpu[0].position[1], 0.01f) << "written back into its entity";
}

TEST_P(GpuPhysicsTest, ASlidingSphereRollsAtFiveSeventhsOfItsSpeed) {
    const int b = Body(eSphere, { 0, 0.5f, 0 }, { 0.5f, 0, 0 }, 1.f, 0.5f, 0.f, { 5, 0, 0 });
    Build(1.f);
    Steps(120);
    const auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), 1u);
    const float rolling = 5.f * 5.f / 7.f;
    EXPECT_NEAR(gpu[0].velocity[0], rolling, 0.05f * rolling) << "a solid sphere slides until it rolls, at 5/7 of its speed";
    EXPECT_NEAR(gpu[0].velocity[0], cpu->bodies.velocity[b].x, 0.03f * rolling) << "as in kphysics";
    EXPECT_NEAR(gpu[0].spin[2], -gpu[0].velocity[0] / 0.5f, 0.05f * rolling / 0.5f) << "turning as fast as it rolls";
}

TEST_P(GpuPhysicsTest, ABouncingSphereComesUpAsHighAsInKphysics) {
    const int b = Body(eSphere, { 0, 3, 0 }, { 0.5f, 0, 0 }, 1.f, 0.f, 0.6f);
    Build(1.f);
    float gpuPeak = 0.f, cpuPeak = 0.f;
    bool bounced = false;
    for (int step = 0; step < 120; ++step) {
        Steps(1);
        const auto gpu = OnTheGpu();
        if (gpu[0].velocity[1] > 0.f) bounced = true;
        if (bounced) { gpuPeak = std::max(gpuPeak, gpu[0].position[1]); cpuPeak = std::max(cpuPeak, cpu->bodies.position[b].y); }
    }
    ASSERT_TRUE(bounced);
    EXPECT_GT(gpuPeak, 1.f) << "it came back up";
    EXPECT_NEAR(gpuPeak, cpuPeak, 0.1f * (cpuPeak - 0.5f)) << "as high as in kphysics";
}

TEST_P(GpuPhysicsTest, APileSettlesAsKphysicsSaysWithoutOneSinkingIntoAnother) {
    constexpr float r = 0.25f;
    int made = 0;
    for (int y = 0; y < 6; ++y)
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x) {
                const float jitter = 0.05f * static_cast<float>((x * 7 + y * 13 + z * 3) % 5) / 5.f;
                Body(eSphere, { (x - 3.5f) * 0.55f + jitter, 0.4f + y * 0.6f, (z - 3.5f) * 0.55f - jitter }, { r, 0, 0 }, 1.f, 0.5f, 0.f);
                ++made;
            }
    Build(2.f * r);
    Steps(300);
    const auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), static_cast<std::size_t>(made));
    float deepest = 0.f, fastest = 0.f, lowest = 1e9f;
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        lowest = std::min(lowest, gpu[i].position[1]);
        fastest = std::max(fastest, Speed(gpu[i]));
        for (std::size_t j = i + 1; j < gpu.size(); ++j) {
            const float d = std::hypot(gpu[i].position[0] - gpu[j].position[0], gpu[i].position[1] - gpu[j].position[1], gpu[i].position[2] - gpu[j].position[2]);
            deepest = std::max(deepest, 2.f * r - d);
        }
    }
    // Spread out, and rolling — nothing stops a sphere rolling on a flat ground — as in kphysics: as high, as fast.
    float gpuHeight = 0.f, cpuHeight = 0.f, gpuSpeed = 0.f, cpuSpeed = 0.f, cpuFastest = 0.f;
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        const auto& v = cpu->bodies.velocity[i + 1];      // (body 0 is the ground)
        gpuHeight += gpu[i].position[1];
        cpuHeight += cpu->bodies.position[i + 1].y;
        gpuSpeed += Speed(gpu[i]);
        cpuSpeed += std::hypot(v.x, v.y, v.z);
        cpuFastest = std::max(cpuFastest, std::hypot(v.x, v.y, v.z));
    }
    EXPECT_GT(lowest, r * 0.9f) << "none sank into the ground";
    EXPECT_LT(deepest, 0.1f * r) << "none sank into another";
    const float n = static_cast<float>(gpu.size());
    EXPECT_NEAR(gpuHeight / n, cpuHeight / n, 0.25f * r) << "as high, on average, as in kphysics";
    EXPECT_NEAR(gpuSpeed / n, cpuSpeed / n, 0.3f * cpuSpeed / n + 0.05f) << "as fast, on average";
    EXPECT_LT(fastest, 1.5f * cpuFastest + 0.5f) << "nothing thrown out faster than there";
}

// ---- boxes and capsules ---------------------------------------------------------------------------------------------

TEST_P(GpuPhysicsTest, ABoxFallsFlatAndRestsWithoutTurning) {
    if (!Xpbd()) GTEST_SKIP() << "SpheresStep is for spheres";
    Body(eBox, { 0, 2, 0 }, { 0.5f, 0.5f, 0.5f }, 1.f, 0.5f, 0.f);
    Build(2.f);
    Steps(120);
    const auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), 1u);
    EXPECT_NEAR(gpu[0].position[1], 0.5f, 0.01f) << "resting on its face";
    EXPECT_LT(Turned(gpu[0]), 0.01f) << "not turned";
    EXPECT_LT(Speed(gpu[0]), 0.05f);
}

TEST_P(GpuPhysicsTest, ASlidingBoxStopsWhereFrictionSays) {
    if (!Xpbd()) GTEST_SKIP() << "SpheresStep is for spheres";
    const int b = Body(eBox, { 0, 0.5f, 0 }, { 0.5f, 0.5f, 0.5f }, 1.f, 0.5f, 0.f, { 4, 0, 0 });
    Build(2.f);
    Steps(90);
    const auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), 1u);
    const float stops = 4.f * 4.f / (2.f * 0.5f * 9.81f);       // v² / 2μg
    EXPECT_NEAR(gpu[0].position[0], stops, 0.05f * stops) << "as far as friction lets it slide";
    // kphysics stops it a little short of that (its four points' friction held a little harder): near it, all the same.
    EXPECT_NEAR(gpu[0].position[0], cpu->bodies.position[b].x, 0.15f * stops) << "near where kphysics stops it";
    EXPECT_LT(Speed(gpu[0]), 0.05f) << "and stopped";
    EXPECT_LT(Turned(gpu[0]), 0.02f) << "without tipping";
}

TEST_P(GpuPhysicsTest, FiveBoxesStandInAStack) {
    if (!Xpbd()) GTEST_SKIP() << "SpheresStep is for spheres";
    for (int i = 0; i < 5; ++i) Body(eBox, { 0, 0.5f + i * 1.f, 0 }, { 0.5f, 0.5f, 0.5f }, 1.f, 0.6f, 0.f);
    Build(2.f);
    Steps(180);
    const auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), 5u);
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        EXPECT_NEAR(gpu[i].position[1], 0.5f + static_cast<float>(i), 0.05f) << "box " << i << " where it was put";
        EXPECT_LT(std::hypot(gpu[i].position[0], gpu[i].position[2]), 0.02f) << "box " << i << " not slid";
        EXPECT_LT(Turned(gpu[i]), 0.02f) << "box " << i << " not turned";
    }
}

TEST_P(GpuPhysicsTest, ACapsuleLyingDownStaysLying) {
    if (!Xpbd()) GTEST_SKIP() << "SpheresStep is for spheres";
    Body(eCapsule, { 0, 0.4f, 0 }, { 0.3f, 0.5f, 0 }, 1.f, 0.5f, 0.f, {}, { 0, 0, 90 });
    Build(2.f);
    Steps(120);
    const auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), 1u);
    EXPECT_NEAR(gpu[0].position[1], 0.3f, 0.01f) << "on its side, its radius above the ground";
    const float qx = gpu[0].orientation[0], qy = gpu[0].orientation[1], qz = gpu[0].orientation[2], qw = gpu[0].orientation[3];
    const float upY = 1.f - 2.f * (qx * qx + qz * qz);          // its own Y, in the world's: still across, not up
    EXPECT_LT(std::abs(upY), 0.05f) << "still lying";
    EXPECT_LT(Speed(gpu[0]), 0.05f);
    (void)qy; (void)qw;
}

TEST_P(GpuPhysicsTest, AHeapOfEveryShapeSettlesAboveTheGround) {
    if (!Xpbd()) GTEST_SKIP() << "SpheresStep is for spheres";
    int made = 0;
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 5; ++x) {
                const int kind = (x + y + z) % 3;
                const kecs::Vec3 at { (x - 2.f) * 1.1f, 0.6f + y * 1.2f, (z - 2.f) * 1.1f };
                if (kind == 0) Body(eSphere, at, { 0.4f, 0, 0 }, 1.f, 0.5f, 0.f);
                else if (kind == 1) Body(eBox, at, { 0.4f, 0.3f, 0.35f }, 1.f, 0.5f, 0.f, {}, { 10.f * x, 20.f * z, 0 });
                else Body(eCapsule, at, { 0.25f, 0.3f, 0 }, 1.f, 0.5f, 0.f, {}, { 0, 0, 45.f * y });
                ++made;
            }
    Build(2.f * 0.6f);
    Steps(300);
    const auto gpu = OnTheGpu();
    ASSERT_EQ(gpu.size(), static_cast<std::size_t>(made));
    float lowest = 1e9f, fastest = 0.f, gpuHeight = 0.f, cpuHeight = 0.f;
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        lowest = std::min(lowest, gpu[i].position[1]);
        fastest = std::max(fastest, Speed(gpu[i]));
        gpuHeight += gpu[i].position[1];
        cpuHeight += cpu->bodies.position[i + 1].y;
    }
    EXPECT_GT(lowest, 0.2f) << "every middle above the ground: none fell through";
    EXPECT_LT(fastest, 3.f) << "none thrown out";
    EXPECT_NEAR(gpuHeight / static_cast<float>(made), cpuHeight / static_cast<float>(made), 0.15f) << "as high, on average, as in kphysics";
}

// The same heap, every box and capsule turned at random: nothing thrown up. Pushes out of what moves turned into speed
// once threw a capsule landing on a box ten metres up (a box's face, then another's, each pushing it out further than it
// was in); as kphysics, which pushes out with no speed, they are at most 5 m/s now.
TEST_P(GpuPhysicsTest, AHeapTurnedAtRandomThrowsNothingUp) {
    if (!Xpbd()) GTEST_SKIP() << "SpheresStep is for spheres";
    std::mt19937 random(static_cast<unsigned>(std::getenv("HEAP_SEED") ? std::atoi(std::getenv("HEAP_SEED")) : 4));
    std::uniform_real_distribution<float> degrees(0.f, 360.f);
    const auto turn = [&] { return kecs::Vec3 { degrees(random), degrees(random), degrees(random) }; };
    float top = 0.f;
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 5; ++x) {
                const int kind = (x + y + z) % 3;
                const kecs::Vec3 at { (x - 2.f) * 1.5f, 0.7f + y * 1.5f, (z - 2.f) * 1.5f };
                top = std::max(top, at.y);
                if (kind == 0) Body(eSphere, at, { 0.4f, 0, 0 }, 1.f, 0.5f, 0.f);
                else if (kind == 1) Body(eBox, at, { 0.4f, 0.3f, 0.35f }, 1.f, 0.5f, 0.f, {}, turn());
                else Body(eCapsule, at, { 0.25f, 0.3f, 0 }, 1.f, 0.5f, 0.f, {}, turn());
            }
    Build(2.f * 0.6f);
    float highest = 0.f;
    for (int k = 0; k < 60; ++k) {
        Steps(5, false);
        for (const auto& b : OnTheGpu()) highest = std::max(highest, b.position[1]);
    }
    float fastest = 0.f;
    for (const auto& b : OnTheGpu()) fastest = std::max(fastest, Speed(b));
    EXPECT_LT(highest, top + 0.3f) << "nothing thrown above where the heap began";
    EXPECT_LT(fastest, 6.f) << "nor along the ground: kphysics' fastest is 2.4 to 3.4 m/s";
}

// How long a step takes, on the GPU and in kphysics, for piles of thousands: run with --gtest_also_run_disabled_tests.
TEST_P(GpuPhysicsTest, DISABLED_AStepOfThousandsTakesSoLongHereAndInKphysics) {
    const int side = std::getenv("KGRAPH_SPHERES_SIDE") ? std::atoi(std::getenv("KGRAPH_SPHERES_SIDE")) : 22;   // side² × 20 layers
    constexpr float r = 0.25f;
    for (int y = 0; y < 20; ++y)
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x)
                Body(eSphere, { (x - side / 2.f) * 0.55f, 0.4f + y * 0.6f, (z - side / 2.f) * 0.55f }, { r, 0, 0 }, 1.f, 0.5f, 0.f);
    const int count = side * side * 20;
    Build(2.f * r);
    for (int i = 0; i < 30; ++i) { scene->runtime->FixedStep(Dt); Frame(); }
    const auto gpuStart = std::chrono::steady_clock::now();
    constexpr int Frames = 60;
    for (int i = 0; i < Frames; ++i) { scene->runtime->FixedStep(Dt); Frame(); }
    const double gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gpuStart).count() / Frames;
    for (int i = 0; i < 30; ++i) cpu->Step(Dt);
    const auto cpuStart = std::chrono::steady_clock::now();
    constexpr int CpuSteps = 20;
    for (int i = 0; i < CpuSteps; ++i) cpu->Step(Dt);
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count() / CpuSteps;
    double gpuPasses = 0.0;
    for (const auto& timing : scene->runtime->Timings() | std::views::values) gpuPasses += timing.gpuMs;
    std::fprintf(stderr, "%d spheres: a frame with a GPU step %.2f ms (its passes' GPU time %.2f ms); a kphysics step %.2f ms\n", count, gpuMs, gpuPasses, cpuMs);
}
