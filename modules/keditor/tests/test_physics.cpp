// The engine's physics scripts on a scene that plays: PhysicsSimulation, RigidBody, Collider and Joint, registered on every
// play scene as FlyCamera is, stepping kphysics on the fixed step. The scripts' steps are called here as PlayScene calls
// them, a fixed step and an update a frame, so the test does not depend on how fast frames come.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <random>

#include <keditor/document.h>
#include <keditor/play.h>

#include "support.h"
#include "../src/physics.h"

#include <kmath/transform.h>
#include <optional>

namespace {
    using keditor::PlayScene;

    class PhysicsTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            if (!support::App()) GTEST_SKIP() << "no Vulkan device: " << support::Reason();
            scene = support::App()->OpenOffscreen<PlayScene>({ .title = "physics", .extent = { support::Size, support::Size } });
            scene->SetPlaying(false);           // its steps are taken by the test
        }
        void TearDown() override
        {
            if (scene) support::App()->Close(*scene);
            support::Frame();
        }
        keditor::Document& Doc() { return scene->Doc(); }

        kecs::Entity Body(const char* name, const kgraph::Value::List& at, const int kind, const int shape)
        {
            const kecs::Entity e = Doc().CreateEntity(name);
            Doc().World().SetPosition(e, { static_cast<float>(*at[0].Number()), static_cast<float>(*at[1].Number()), static_cast<float>(*at[2].Number()) });
            EXPECT_TRUE(Doc().AddComponent(e, "RigidBody"));
            EXPECT_TRUE(Doc().AddComponent(e, "Collider"));
            EXPECT_TRUE(Doc().SetField(e, "RigidBody", "kind", kind));
            EXPECT_TRUE(Doc().SetField(e, "Collider", "shape", shape));
            return e;
        }
        /** @p seconds of play: a fixed step and an update for each frame at 60 Hz. */
        void Play(const float seconds)
        {
            for (int k = 0; k < static_cast<int>(seconds * 60.f); ++k) {
                Doc().Scripts().FixedUpdate(1.f / 60.f);
                Doc().Scripts().Update(1.f / 60.f);
            }
        }
        float Number(const kecs::Entity e, const char* component, const char* field)
        {
            const auto n = Doc().Field(e, component, field).Number();
            return n ? static_cast<float>(*n) : NAN;
        }

        PlayScene* scene = nullptr;
    };
}

TEST_F(PhysicsTest, TheFourScriptsAreTheEnginesOwnWithTheKotlinScriptsFields)
{
    for (const char* name : { "PhysicsSimulation", "RigidBody", "Collider", "Joint" }) EXPECT_TRUE(Doc().Scripts().Registered(name)) << name;
    const kecs::Entity e = Doc().CreateEntity("Box");
    ASSERT_TRUE(Doc().AddComponent(e, "RigidBody"));
    ASSERT_TRUE(Doc().AddComponent(e, "Collider"));
    ASSERT_TRUE(Doc().AddComponent(e, "Joint"));
    // What a new one starts as: the Kotlin scripts' initial values, so a scene saved with them means the same.
    EXPECT_EQ(1.f, Number(e, "RigidBody", "kind"));
    EXPECT_EQ(1000.f, Number(e, "RigidBody", "density"));
    EXPECT_FLOAT_EQ(0.6f, Number(e, "RigidBody", "friction"));
    EXPECT_FLOAT_EQ(0.05f, Number(e, "RigidBody", "angularDamping"));
    EXPECT_EQ(1.f, Number(e, "Collider", "shape"));
    EXPECT_EQ(0.5f, Number(e, "Collider", "radius"));
    EXPECT_EQ(-45.f, Number(e, "Joint", "lowerDegrees"));
    const kgraph::Value axis = Doc().Field(e, "Joint", "axis");
    ASSERT_TRUE(axis.Items() && axis.Items()->size() == 3);
    EXPECT_EQ(1.0, *(*axis.Items())[1].Number()) << "a hinge turns about its Y to begin with";
}

TEST_F(PhysicsTest, WhatKindOfThingABodyIsIsNamedAndSetByItsName)
{
    const auto choices = [&](const char* component, const char* field) { return Doc().World().Describe(Doc().World().Find(component)).FindField(field)->choices; };
    EXPECT_EQ(choices("RigidBody", "kind"), (std::vector<std::string> { "Static", "Dynamic", "Kinematic" }));
    EXPECT_EQ(choices("Collider", "shape"), (std::vector<std::string> { "Sphere", "Box", "Capsule", "Plane", "Cone", "Mesh" }));
    EXPECT_EQ(choices("Joint", "kind"), (std::vector<std::string> { "Ball", "Hinge", "Distance", "Rope" }));
    EXPECT_EQ(choices("PhysicsSimulation", "broadphase").size(), 4u);
    EXPECT_EQ(choices("PhysicsSimulation", "gyroscopic").size(), 3u);
    EXPECT_TRUE(choices("RigidBody", "density").empty()) << "a number that is only a number";

    const kecs::Entity e = Doc().CreateEntity("Box");
    ASSERT_TRUE(Doc().AddComponent(e, "RigidBody"));
    ASSERT_TRUE(Doc().AddComponent(e, "Collider"));
    // Set by its name, in any case — as a script's world.set(e, "RigidBody", "kind", "Kinematic") does — and kept as the number.
    ASSERT_TRUE(Doc().SetField(e, "RigidBody", "kind", "Kinematic"));
    EXPECT_EQ(2.f, Number(e, "RigidBody", "kind"));
    ASSERT_TRUE(Doc().SetField(e, "Collider", "shape", "cone"));
    EXPECT_EQ(4.f, Number(e, "Collider", "shape"));
    ASSERT_TRUE(Doc().SetField(e, "RigidBody", "kind", 0)) << "and by the number still";
    std::string error;
    EXPECT_FALSE(Doc().SetField(e, "RigidBody", "kind", "Floating", &error));
    EXPECT_NE(error.find("Static, Dynamic, Kinematic"), std::string::npos) << error;
}

TEST_F(PhysicsTest, ABoxFallsOnTheFixedStepAndComesToRestOnTheGround)
{
    Body("Ground", { 0, 0, 0 }, 0, 3);
    const kecs::Entity box = Body("Box", { 0, 3, 0 }, 1, 1);
    const kecs::Entity world = Doc().CreateEntity("Physics");
    ASSERT_TRUE(Doc().AddComponent(world, "PhysicsSimulation"));
    Play(0.5f);
    const float falling = Doc().World().PositionOf(box).y;
    EXPECT_LT(falling, 3.f - 0.5f * 9.81f * 0.25f * 0.9f) << "it fell as gravity says, on the fixed step";
    const kgraph::Value velocity = Doc().Field(box, "RigidBody", "velocity");
    ASSERT_TRUE(velocity.Items());
    EXPECT_LT(*(*velocity.Items())[1].Number(), -4.f) << "its velocity is written back to it";
    Play(2.5f);
    EXPECT_NEAR(0.5f, Doc().World().PositionOf(box).y, 0.01f) << "resting on its face on the ground";
    EXPECT_EQ(2.f, Number(world, "PhysicsSimulation", "bodyCount"));
    EXPECT_GE(Number(world, "PhysicsSimulation", "contactCount"), 0.f);
    EXPECT_EQ(1.f, Number(world, "PhysicsSimulation", "sleepingCount")) << "and asleep";
}

TEST_F(PhysicsTest, ABodyMovedByHandIsPutThereAndABodyAddedJoinsTheSimulation)
{
    Body("Ground", { 0, 0, 0 }, 0, 3);
    const kecs::Entity box = Body("Box", { 0, 0.5, 0 }, 1, 1);
    const kecs::Entity world = Doc().CreateEntity("Physics");
    ASSERT_TRUE(Doc().AddComponent(world, "PhysicsSimulation"));
    Play(0.5f);
    // Moved, as the gizmo moves it: it is put there and falls from there.
    Doc().World().SetPosition(box, { 4.f, 2.f, 0.f });
    Play(1.f / 60.f);
    EXPECT_NEAR(4.f, Doc().World().PositionOf(box).x, 1e-4f);
    EXPECT_LT(Doc().World().PositionOf(box).y, 2.f);
    Play(1.5f);
    EXPECT_NEAR(0.5f, Doc().World().PositionOf(box).y, 0.01f);
    // A body added while it plays: the simulation is built again with it.
    const kecs::Entity ball = Body("Ball", { 4, 3, 0 }, 1, 0);
    Play(2.f);
    EXPECT_EQ(3.f, Number(world, "PhysicsSimulation", "bodyCount"));
    EXPECT_NEAR(1.5f, Doc().World().PositionOf(ball).y, 0.02f) << "a ball of radius 0.5 resting on the box";
}

TEST_F(PhysicsTest, AHingedDoorSwingsToItsLimit)
{
    const kecs::Entity door = Body("Door", { 0.5, 1, 0 }, 1, 1);
    ASSERT_TRUE(Doc().SetField(door, "Collider", "halfExtents", kgraph::Value::List { 0.5, 1, 0.05 }));
    ASSERT_TRUE(Doc().AddComponent(door, "Joint"));
    ASSERT_TRUE(Doc().SetField(door, "Joint", "kind", 1));
    ASSERT_TRUE(Doc().SetField(door, "Joint", "anchor", kgraph::Value::List { -0.5, 0, 0 }));
    ASSERT_TRUE(Doc().SetField(door, "Joint", "limited", true));
    ASSERT_TRUE(Doc().SetField(door, "Joint", "lowerDegrees", 0));
    ASSERT_TRUE(Doc().SetField(door, "Joint", "upperDegrees", 90));
    ASSERT_TRUE(Doc().SetField(door, "RigidBody", "angularVelocity", kgraph::Value::List { 0, 3, 0 }));
    const kecs::Entity world = Doc().CreateEntity("Physics");
    ASSERT_TRUE(Doc().AddComponent(world, "PhysicsSimulation"));
    ASSERT_TRUE(Doc().SetField(world, "PhysicsSimulation", "gravity", kgraph::Value::List { 0, 0, 0 }));
    Play(3.f);
    // Swung a quarter turn about its hinge at the origin: its centre from (0.5, 1, 0) to (0, 1, −0.5).
    const kecs::Vec3 at = Doc().World().PositionOf(door);
    EXPECT_NEAR(0.f, at.x, 0.03f);
    EXPECT_NEAR(-0.5f, at.z, 0.03f);
    EXPECT_NEAR(1.f, at.y, 1e-3f);
}

TEST_F(PhysicsTest, AConeStandsOnAMeshPlatform)
{
    // A $cube, scaled to a platform 4 × 1 × 4, collided as its triangles; a cone (as $cone is: about its middle) dropped on
    // it comes to rest with its middle half its height above the platform's top.
    const kecs::Entity platform = Body("Platform", { 0, 0.5, 0 }, 0, 5);
    Doc().World().SetScale(platform, { 4.f, 1.f, 4.f });
    ASSERT_TRUE(Doc().AddComponent(platform, "Mesh"));
    std::string error;
    ASSERT_TRUE(Doc().SetField(platform, "Mesh", "mesh", "$cube", &error)) << error;
    const kecs::Entity cone = Body("Cone", { 0.3, 3, -0.2 }, 1, 4);
    const kecs::Entity world = Doc().CreateEntity("Physics");
    ASSERT_TRUE(Doc().AddComponent(world, "PhysicsSimulation"));
    Play(3.f);
    EXPECT_NEAR(1.5f, Doc().World().PositionOf(cone).y, 0.01f) << "its base on the platform's top, at y = 1";
    EXPECT_NEAR(0.3f, Doc().World().PositionOf(cone).x, 0.01f);
    EXPECT_EQ(1.f, Number(world, "PhysicsSimulation", "sleepingCount"));
}

TEST_F(PhysicsTest, ACollidersHandleDraggedChangesTheSizeItIsDraggedAlong)
{
    const kecs::Entity box = Body("Box", { 0, 0, 0 }, 1, 1);
    Doc().World().SetScale(box, { 2.f, 1.f, 1.f });
    ASSERT_TRUE(keditor::HasCollider(Doc(), box));
    ASSERT_TRUE(keditor::ColliderResizable(Doc(), box));
    // Looked at from the front: +X is to the right on the picture.
    const kor::Mat4 viewProjection = kor::Perspective(0.8f, 1.f, 0.1f, 100.f) * kor::LookAt(kor::Vec3(0.f, 0.f, 6.f), kor::Vec3(0.f), kor::Vec3(0.f, 1.f, 0.f));
    const kor::Vec2 middle(400.f, 400.f);
    kor::DebugDraw debug;
    const auto frame = [&](const kor::Vec2 at, const bool down, const bool pressed) {
        kor::GizmoPointer pointer { .position = at, .viewport = kor::Vec2(800.f), .down = down, .pressed = pressed };
        const bool dragged = keditor::EditCollider(debug, Doc(), box, viewProjection, pointer);
        debug.Clear();
        return dragged;
    };
    // Its X handle: somewhere right of the middle, along the axis — found by where pressing grabs it.
    std::optional<float> handle;
    for (float x = 30.f; x <= 200.f && !handle; x += 2.f) {
        frame(middle + kor::Vec2(x, 0.f), false, false);
        frame(middle + kor::Vec2(x, 0.f), false, false);
        frame(middle + kor::Vec2(x, 0.f), true, true);
        if (debug.GizmoActive()) handle = x;
        frame(middle + kor::Vec2(x, 0.f), false, false);
    }
    ASSERT_TRUE(handle.has_value()) << "no handle along +X";
    frame(middle + kor::Vec2(*handle, 0.f), false, false);
    frame(middle + kor::Vec2(*handle, 0.f), true, true);
    bool dragged = false;
    for (int step = 1; step <= 10; ++step) dragged |= frame(middle + kor::Vec2(*handle + 4.f * static_cast<float>(step), 0.f), true, false);
    frame(middle + kor::Vec2(*handle + 40.f, 0.f), false, false);
    EXPECT_TRUE(dragged);
    const kgraph::Value half = Doc().Field(box, "Collider", "halfExtents");
    EXPECT_GT(*(*half.Items())[0].Number(), 0.55) << "longer along X, the way it was dragged";
    EXPECT_NEAR(*(*half.Items())[1].Number(), 0.5, 1e-4);
    EXPECT_NEAR(*(*half.Items())[2].Number(), 0.5, 1e-4);
    const kecs::Vec3 scale = Doc().World().ScaleOf(box);
    EXPECT_EQ(scale.x, 2.f) << "the entity's own scale is left as it was";

    // A sphere's radius, whichever way it is dragged; a mesh has no size to drag.
    ASSERT_TRUE(Doc().SetField(box, "Collider", "shape", "Mesh"));
    EXPECT_FALSE(keditor::ColliderResizable(Doc(), box));
    EXPECT_FALSE(frame(middle + kor::Vec2(*handle, 0.f), true, true));
}

// The graph alone runs them, on the GPU: RigidBody and Collider read by the engine's XpbdRigidBodies on setup, XpbdStep on
// the fixed step, what it worked out written back — no PhysicsSimulation, nothing of kphysics. A ball comes to rest on a static
// platform, a crate twice as long as it is wide on the ground, a capsule lying on its side.
TEST_F(PhysicsTest, TheGraphAloneRunsTheBodiesOnTheGpu)
{
    const kecs::Entity platform = Body("Platform", { 0, 1, 0 }, 0, 1);
    ASSERT_TRUE(Doc().SetField(platform, "Collider", "halfExtents", kgraph::Value::List { 1, 0.5, 1 }));
    const kecs::Entity ball = Body("Ball", { 0, 4, 0 }, 1, 0);
    const kecs::Entity crate = Body("Crate", { 4, 3, 0 }, 1, 1);
    Doc().World().SetScale(crate, { 2.f, 1.f, 1.f });
    const kecs::Entity pill = Body("Pill", { -4, 2, 0 }, 1, 2);
    ASSERT_TRUE(Doc().SetField(pill, "Collider", "radius", 0.25));
    Doc().World().SetRotation(pill, { 0.f, 0.f, 90.f });

    using kgraph::Value;
    kgraph::Graph graph = Doc().Graph();
    for (const char* pass : { "made", "simulate", "write" }) graph.passes.push_back(pass);
    graph.setup = { "made" };
    graph.fixed = { "simulate" };
    graph.Add("rigid", "Function").Set("function", "XpbdRigidBodies").pass = "made";
    graph.Add("step", "FixedStep").pass = "simulate";
    graph.Add("physics", "Function").Set("function", "XpbdStep").Wire("bodies", "rigid", "bodies").Wire("materials", "rigid", "materials")
        .Wire("count", "rigid", "count").Wire("shared", "rigid", "shared").Wire("dt", "step", "dt").Set("gravity", Value::List { 0, -9.81, 0 }).Set("ground", 0).pass = "simulate";
    graph.Add("back", "WriteBack").Wire("buffer", "physics", "bodies").Set("forEach", "rigid/bodies of").Set("stride", 112).Set("position", 96).Set("rotation", 48)
        .Set("fields", Value::Members { { "RigidBody.velocity", 16 }, { "RigidBody.angularVelocity", 32 } }).pass = "write";
    Doc().SetGraph(graph);
    for (int i = 0; i < 4; ++i) support::Frame();
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;

    for (int i = 0; i < 180; ++i) { scene->StepOnce(); support::Frame(); }
    for (int i = 0; i < 6; ++i) support::Frame();
    const auto& world = Doc().World();
    EXPECT_NEAR(world.PositionOf(ball).y, 2.f, 0.03f) << "on the platform: its top at 1.5, the ball's radius 0.5";
    EXPECT_NEAR(world.PositionOf(ball).x, 0.f, 0.05f);
    EXPECT_NEAR(world.PositionOf(crate).y, 0.5f, 0.03f) << "on the ground";
    EXPECT_NEAR(world.PositionOf(crate).x, 4.f, 0.05f) << "where it fell";
    EXPECT_NEAR(world.PositionOf(pill).y, 0.25f, 0.03f) << "lying: its radius above the ground";
    EXPECT_NEAR(world.PositionOf(platform).y, 1.f, 1e-4f) << "static: where it was put";
    const auto velocity = Doc().Field(ball, "RigidBody", "velocity");
    ASSERT_TRUE(velocity.Items() && velocity.Items()->size() == 3);
    float speed = 0.f;
    for (const auto& v : *velocity.Items()) speed += static_cast<float>(*v.Number() * *v.Number());
    EXPECT_LT(std::sqrt(speed), 0.1f) << "what it is doing — resting — written back too";
}


// A PhysicsSimulation's mode: on the GPU, the graph is given the pages that run the bodies — XpbdRigidBodies, XpbdStep,
// a WriteBack — and kphysics runs nothing; a plane's Collider is a plane there too. Switched to the CPU as it plays,
// kphysics goes on from where the GPU left them, and the pages idle.
TEST_F(PhysicsTest, ThePhysicsSimulationRunsOnTheCpuOrTheGpuAsItsModeSays)
{
    const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
    ASSERT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
    ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", "GPU"));
    const kecs::Entity ground = Body("Ground", { 0, 1, 0 }, 0, 3);
    Doc().World().SetScale(ground, { 40.f, 1.f, 40.f });
    const kecs::Entity ball = Body("Ball", { 0, 4, 0 }, 1, 0);
    const kecs::Entity crate = Body("Crate", { 3, 3, 0 }, 1, 1);

    for (int i = 0; i < 120; ++i) { scene->StepOnce(); support::Frame(); }
    for (int i = 0; i < 6; ++i) support::Frame();
    ASSERT_NE(Doc().Graph().Find("gpu physics"), nullptr) << "the graph was given what runs them";
    for (const auto& problem : Doc().Runtime().Problems())
        if (problem.find("gpu physics") != std::string::npos) ADD_FAILURE() << problem;
    const auto& world = Doc().World();
    EXPECT_NEAR(world.PositionOf(ball).y, 1.5f, 0.03f) << "on the plane at 1: the GPU ran it";
    EXPECT_NEAR(world.PositionOf(crate).y, 1.5f, 0.03f);
    EXPECT_EQ(Number(simulation, "PhysicsSimulation", "bodyCount"), 0.f) << "kphysics ran nothing";

    // To the CPU, as it plays: kphysics from where they are.
    ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", "CPU"));
    Doc().World().SetPosition(ball, { 0.f, 3.f, 0.f });
    for (int i = 0; i < 120; ++i) { scene->StepOnce(); support::Frame(); }
    for (int i = 0; i < 6; ++i) support::Frame();
    EXPECT_GT(Number(simulation, "PhysicsSimulation", "bodyCount"), 0.f) << "kphysics runs them now";
    EXPECT_NEAR(world.PositionOf(ball).y, 1.5f, 0.03f) << "and the ball fell again, onto the plane, by kphysics";
    EXPECT_NEAR(world.PositionOf(crate).y, 1.5f, 0.03f) << "the crate where the GPU left it";
}


TEST_F(PhysicsTest, JointsHoldOnTheGpuAsTheyDoOnTheCpu)
{
    const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
    ASSERT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
    ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", "GPU"));
    const auto joint = [&](const kecs::Entity e, const int kind, const kgraph::Value::List& anchor) {
        EXPECT_TRUE(Doc().AddComponent(e, "Joint"));
        EXPECT_TRUE(Doc().SetField(e, "Joint", "kind", kind));
        EXPECT_TRUE(Doc().SetField(e, "Joint", "anchor", anchor));
    };
    const auto to = [&](const kecs::Entity e, const kecs::Entity other) {
        EXPECT_TRUE(Doc().SetField(e, "Joint", "other", kgraph::Value::Members { { "entity", static_cast<double>(other.index) }, { "generation", static_cast<double>(other.generation) } }));
    };
    // A door hinged to the world at its left edge, about Y, swung open to its limit at a quarter turn: gravity along its
    // axis does not turn it.
    const kecs::Entity door = Body("Door", { 0.5, 1, 0 }, 1, 1);
    ASSERT_TRUE(Doc().SetField(door, "Collider", "halfExtents", kgraph::Value::List { 0.5, 1, 0.05 }));
    joint(door, 1, { -0.5, 0, 0 });
    ASSERT_TRUE(Doc().SetField(door, "Joint", "limited", true));
    ASSERT_TRUE(Doc().SetField(door, "Joint", "lowerDegrees", 0));
    ASSERT_TRUE(Doc().SetField(door, "Joint", "upperDegrees", 90));
    ASSERT_TRUE(Doc().SetField(door, "RigidBody", "angularVelocity", kgraph::Value::List { 0, 3, 0 }));
    // A pendulum: a ball on a ball joint a metre below where it hangs from, pushed sideways.
    const kecs::Entity bob = Body("Bob", { 5, 3, 0 }, 1, 0);
    ASSERT_TRUE(Doc().SetField(bob, "Collider", "radius", 0.2));
    joint(bob, 0, { 0, 1, 0 });
    ASSERT_TRUE(Doc().SetField(bob, "RigidBody", "velocity", kgraph::Value::List { 0, 0, 3 }));
    // A ball on a rope two metres long from a point in the world, dropped from a metre below it: it falls taut.
    const kecs::Entity roped = Body("Roped", { -5, 4, 0 }, 1, 0);
    ASSERT_TRUE(Doc().SetField(roped, "Collider", "radius", 0.2));
    joint(roped, 3, { 0, 0, 0 });
    ASSERT_TRUE(Doc().SetField(roped, "Joint", "otherAnchor", kgraph::Value::List { -5, 5, 0 }));
    // A chain of three, the first hung from the world, each of the others from the one above.
    std::vector<kecs::Entity> links;
    for (int k = 0; k < 3; ++k) {
        links.push_back(Body("Link", { 10, 4.0 - k, 0 }, 1, 0));
        ASSERT_TRUE(Doc().SetField(links.back(), "Collider", "radius", 0.3));
        joint(links.back(), 0, { 0, 0.5, 0 });
        if (k > 0) to(links.back(), links[k - 1]);
    }
    ASSERT_TRUE(Doc().SetField(links.back(), "RigidBody", "velocity", kgraph::Value::List { 4, 0, 0 }));

    for (int i = 0; i < 180; ++i) { scene->StepOnce(); support::Frame(); }
    for (int i = 0; i < 6; ++i) support::Frame();
    for (const auto& problem : Doc().Runtime().Problems())
        if (problem.find("gpu physics") != std::string::npos) ADD_FAILURE() << problem;
    EXPECT_EQ(Number(simulation, "PhysicsSimulation", "bodyCount"), 0.f) << "kphysics ran nothing";
    const auto& world = Doc().World();
    const auto distance = [](const kecs::Vec3 a, const kecs::Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };

    // Swung a quarter turn about its hinge at the origin: its centre from (0.5, 1, 0) to (0, 1, −0.5).
    EXPECT_NEAR(world.PositionOf(door).x, 0.f, 0.05f);
    EXPECT_NEAR(world.PositionOf(door).z, -0.5f, 0.05f);
    EXPECT_NEAR(world.PositionOf(door).y, 1.f, 0.02f) << "held up by its hinge";
    EXPECT_NEAR(distance(world.PositionOf(bob), { 5.f, 4.f, 0.f }), 1.f, 0.02f) << "a metre from where it hangs";
    EXPECT_GT(std::abs(world.PositionOf(bob).z), 0.05f) << "and swinging";
    EXPECT_NEAR(distance(world.PositionOf(roped), { -5.f, 5.f, 0.f }), 1.f, 0.03f) << "a rope's length is what it is at Play";
    EXPECT_NEAR(distance(world.PositionOf(links[0]), { 10.f, 4.5f, 0.f }), 0.5f, 0.03f);
    for (int k = 1; k < 3; ++k) EXPECT_LT(distance(world.PositionOf(links[k]), world.PositionOf(links[k - 1])), 1.03f) << "link " << k;
    EXPECT_LT(world.PositionOf(links[2]).y, 3.f) << "the chain hangs";
}

TEST_F(PhysicsTest, AHingesMotorTurnsItTheSameWayOnTheCpuAndTheGpu)
{
    const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
    ASSERT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
    // A wheel pinned at its middle to the world, about Y, its motor at 2 rad/s and torque enough to reach it.
    const kecs::Entity wheel = Body("Wheel", { 0, 2, 0 }, 1, 1);
    ASSERT_TRUE(Doc().SetField(wheel, "Collider", "halfExtents", kgraph::Value::List { 1, 0.1, 1 }));
    ASSERT_TRUE(Doc().AddComponent(wheel, "Joint"));
    ASSERT_TRUE(Doc().SetField(wheel, "Joint", "kind", 1));
    ASSERT_TRUE(Doc().SetField(wheel, "Joint", "motorSpeed", 2));
    ASSERT_TRUE(Doc().SetField(wheel, "Joint", "maxMotorTorque", 5000));
    std::vector<float> angles;
    for (const char* mode : { "CPU", "GPU" }) {
        ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", mode));
        Doc().World().SetPosition(wheel, { 0.f, 2.f, 0.f });
        Doc().World().SetRotation(wheel, { 0.f, 0.f, 0.f });
        ASSERT_TRUE(Doc().SetField(wheel, "RigidBody", "angularVelocity", kgraph::Value::List { 0, 0, 0 }));
        for (int i = 0; i < 90; ++i) { scene->StepOnce(); support::Frame(); }
        for (int i = 0; i < 6; ++i) support::Frame();
        // Near a second and a half at 2 rad/s, about +Y: as far one way as the other.
        const kecs::Quat turned = Doc().World().OrientationOf(wheel);
        angles.push_back(2.f * std::atan2(turned.y, turned.w));
        EXPECT_GT(angles.back(), 2.5f) << mode << ": turned at its motor's speed, about +Y";
        EXPECT_NEAR(Doc().World().PositionOf(wheel).y, 2.f, 0.02f) << mode << ": held at its pin";
    }
    ASSERT_EQ(angles.size(), 2u);
    EXPECT_NEAR(angles[1], angles[0], 0.1f) << "the GPU's wheel as far round as kphysics'";
    for (const auto& problem : Doc().Runtime().Problems())
        if (problem.find("gpu physics") != std::string::npos) ADD_FAILURE() << problem;
}

TEST_F(PhysicsTest, BodiesStillLongEnoughSleepOnTheGpuAndWhatStirsAgainstOneWakesIt)
{
    const kgraph::Graph original = Doc().Graph();
    for (const bool allowSleep : { true, false }) {
        const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
        ASSERT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
        ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", "GPU"));
        ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "allowSleep", allowSleep));
        const kecs::Entity ground = Body("Ground", { 0, 1, 0 }, 0, 3);
        // A crate at rest on the plane, and a ball rolling at it from far enough that the crate sleeps first.
        const kecs::Entity crate = Body("Crate", { 0, 1.5, 0 }, 1, 1);
        const kecs::Entity resting = Body("Resting", { 0, 1.5, 6 }, 1, 1);
        const kecs::Entity ball = Body("Ball", { -8, 1.5, 0 }, 1, 0);
        ASSERT_TRUE(Doc().SetField(ball, "RigidBody", "velocity", kgraph::Value::List { 5, 0, 0 }));
        ASSERT_TRUE(Doc().SetField(ball, "RigidBody", "mass", 20));
        ASSERT_TRUE(Doc().SetField(crate, "RigidBody", "mass", 1));

        for (int i = 0; i < 20; ++i) { scene->StepOnce(); support::Frame(); }
        Doc().Runtime().Watch("gpu physics bodies/made/bodies");
        for (int i = 0; i < 220; ++i) { scene->StepOnce(); support::Frame(); }
        for (int i = 0; i < 6; ++i) support::Frame();
        for (const auto& problem : Doc().Runtime().Problems())
            if (problem.find("gpu physics") != std::string::npos) ADD_FAILURE() << problem;

        EXPECT_GT(Doc().World().PositionOf(crate).x, 0.3f) << "the ball pushed the crate: it woke; the ball at " << Doc().World().PositionOf(ball).x << ", " << Doc().World().PositionOf(ball).y;
        EXPECT_NEAR(Doc().World().PositionOf(resting).y, 1.5f, 0.02f);
        // How long each has been still, as the GPU has it: in each body's 112 bytes, its inverse inertia's w.
        const auto peek = Doc().Runtime().Watched("gpu physics bodies/made/bodies");
        ASSERT_TRUE(peek && peek->bytes.size() >= 4 * 112);
        const auto stillFor = [&](const int body) { float t; std::memcpy(&t, peek->bytes.data() + body * 112 + 92, sizeof t); return t; };
        // In the order the ForEach has them: Ground, Crate, Resting, Ball.
        if (allowSleep) EXPECT_GE(stillFor(2), 0.5f) << "the crate left alone sleeps";
        else EXPECT_EQ(stillFor(2), 0.f) << "nothing sleeps when the simulation allows none";

        for (const kecs::Entity e : { simulation, ground, crate, resting, ball }) Doc().DestroyEntity(e);
        Doc().SetGraph(original);      // the GPU's pages given again, as the next one's settings say
        for (int i = 0; i < 4; ++i) support::Frame();
    }
}

TEST_F(PhysicsTest, ConesAndMeshCollidersOnTheGpu)
{
    const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
    ASSERT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
    ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", "GPU"));
    const kecs::Entity ground = Body("Ground", { 0, 0, 0 }, 0, 3);
    // A $cube scaled to a platform 4 × 1 × 4, collided as its triangles, its top at y = 2.
    const kecs::Entity platform = Body("Platform", { 0, 1.5, 0 }, 0, 5);
    Doc().World().SetScale(platform, { 4.f, 1.f, 4.f });
    ASSERT_TRUE(Doc().AddComponent(platform, "Mesh"));
    std::string error;
    ASSERT_TRUE(Doc().SetField(platform, "Mesh", "mesh", "$cube", &error)) << error;
    // On it, a cone (as $cone is: about its middle, a unit high) and a ball; beside it, a cone on the ground plane, and one
    // on a static box.
    const kecs::Entity cone = Body("Cone", { 0.3, 4, -0.2 }, 1, 4);
    const kecs::Entity ball = Body("Ball", { -1.2, 4, 1 }, 1, 0);
    const kecs::Entity standing = Body("Standing", { -6, 2, 0 }, 1, 4);
    const kecs::Entity table = Body("Table", { 6, 0.5, 0 }, 0, 1);
    const kecs::Entity onTable = Body("On the table", { 6.1, 3, 0 }, 1, 4);
    ASSERT_TRUE(Doc().AddComponent(ball, "Mesh"));
    ASSERT_TRUE(Doc().SetField(ball, "Mesh", "mesh", "$sphere", &error)) << error;

    for (int i = 0; i < 240; ++i) { scene->StepOnce(); support::Frame(); }
    for (int i = 0; i < 6; ++i) support::Frame();
    for (const auto& problem : Doc().Runtime().Problems())
        if (problem.find("gpu physics") != std::string::npos) ADD_FAILURE() << problem;
    const auto& world = Doc().World();
    EXPECT_NEAR(world.PositionOf(cone).y, 2.5f, 0.03f) << "the cone's base on the platform's top: its middle half its height above";
    EXPECT_NEAR(world.PositionOf(cone).x, 0.3f, 0.03f);
    EXPECT_NEAR(world.PositionOf(ball).y, 2.5f, 0.03f) << "the ball on the platform's triangles";
    EXPECT_NEAR(world.PositionOf(standing).y, 0.5f, 0.03f) << "a cone on the plane";
    EXPECT_NEAR(world.PositionOf(onTable).y, 1.5f, 0.03f) << "a cone on a box";
    EXPECT_NEAR(world.PositionOf(table).y, 0.5f, 1e-5f);
    EXPECT_NEAR(world.PositionOf(platform).y, 1.5f, 1e-5f) << "a static mesh stays";
}

TEST_F(PhysicsTest, AHeapOfConesBallsAndBoxesStaysOnAMeshAndAKinematicMeshPushes)
{
    const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
    ASSERT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
    ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", "GPU"));
    std::string error;
    const auto meshOf = [&](const kecs::Entity e, const char* mesh) {
        EXPECT_TRUE(Doc().AddComponent(e, "Mesh"));
        EXPECT_TRUE(Doc().SetField(e, "Mesh", "mesh", mesh, &error)) << error;
    };
    Body("Ground", { 0, 0, 0 }, 0, 3);
    // A platform 6 × 1 × 6 of a $cube's triangles, its top at y = 1, and a heap dropped on it: cones, balls and boxes.
    const kecs::Entity platform = Body("Platform", { 0, 0.5, 0 }, 0, 5);
    Doc().World().SetScale(platform, { 6.f, 1.f, 6.f });
    meshOf(platform, "$cube");
    std::vector<kecs::Entity> heap;
    for (int k = 0; k < 27; ++k) {
        const kgraph::Value::List at { -1.0 + (k % 3), 2.0 + 1.1 * (k / 9), -1.0 + ((k / 3) % 3) + 0.1 * (k % 2) };
        heap.push_back(Body("Heaped", at, 1, k % 3 == 0 ? 4 : k % 3 == 1 ? 0 : 1));
        Doc().World().SetScale(heap.back(), { 0.8f, 0.8f, 0.8f });
    }
    // Beside it, a ball on the ground, and a kinematic mesh moved at it a step at a time, as a script moves one.
    const kecs::Entity ball = Body("Ball", { 8, 0.5, 0 }, 1, 0);
    const kecs::Entity pusher = Body("Pusher", { 5.5, 0.6, 0 }, 2, 5);
    meshOf(pusher, "$cube");

    for (int i = 0; i < 240; ++i) {
        Doc().World().SetPosition(pusher, { 5.5f + (i + 1) / 60.f, 0.6f, 0.f });
        scene->StepOnce();
        support::Frame();
    }
    for (int i = 0; i < 6; ++i) support::Frame();
    for (const auto& problem : Doc().Runtime().Problems())
        if (problem.find("gpu physics") != std::string::npos) ADD_FAILURE() << problem;
    const auto& world = Doc().World();
    // What is over the platform is on it, not through it; what rolled off lies on the ground beside it; nothing is thrown.
    int off = 0;
    for (const kecs::Entity e : heap) {
        const kecs::Vec3 at = world.PositionOf(e);
        const bool over = std::abs(at.x) < 2.9f && std::abs(at.z) < 2.9f;
        if (over) EXPECT_GT(at.y, 1.f) << "on the platform, not through it: " << at.x << ", " << at.y << ", " << at.z;
        else ++off;
        EXPECT_LT(at.y, 4.5f) << "not thrown up";
        EXPECT_LT(std::sqrt(at.x * at.x + at.z * at.z), 25.f) << "nor away: " << at.x << ", " << at.y << ", " << at.z;
    }
    EXPECT_LT(off, 20) << "balls and cones roll off its edges, as kphysics' do (12 of 27 there); boxes stay";
    EXPECT_GT(world.PositionOf(ball).x, 9.f) << "pushed along by the kinematic mesh: its face at " << world.PositionOf(pusher).x - 0.5f;
}

// As the Mixer turns its bar: a kinematic body a script turns each step, on the GPU — taken there by the step, not built
// again with all the rest each time, and not written over; what it sweeps is pushed, and what else there is goes on.
TEST_F(PhysicsTest, AKinematicBarAScriptTurnsSweepsOnTheGpu)
{
    const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
    ASSERT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
    ASSERT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", "GPU"));
    Body("Ground", { 0, 0, 0 }, 0, 3);
    const kecs::Entity bar = Body("Bar", { 0, 0.5, 0 }, 2, 1);
    Doc().World().SetScale(bar, { 4.f, 1.f, 0.3f });
    const kecs::Entity swept = Body("Swept", { 1.5, 0.3, 0.6 }, 1, 0);
    ASSERT_TRUE(Doc().SetField(swept, "Collider", "radius", 0.3));
    const kecs::Entity falling = Body("Falling", { 6, 5, 0 }, 1, 0);

    float angle = 0.f;
    for (int i = 0; i < 120; ++i) {
        angle += 90.f / 60.f;                   // a quarter turn a second
        Doc().World().SetRotation(bar, { 0.f, angle, 0.f });
        scene->StepOnce();
        support::Frame();
    }
    for (int i = 0; i < 6; ++i) support::Frame();
    for (const auto& problem : Doc().Runtime().Problems())
        if (problem.find("gpu physics") != std::string::npos) ADD_FAILURE() << problem;
    const auto& world = Doc().World();
    EXPECT_NEAR(world.RotationOf(bar).y, angle, 0.01f) << "the bar where the script turned it: not written over";
    EXPECT_NEAR(world.PositionOf(bar).x, 0.f, 1e-4f);
    EXPECT_NEAR(world.PositionOf(falling).y, 0.5f, 0.03f) << "the rest went on: fell, and was written back";
    const kecs::Vec3 at = world.PositionOf(swept);
    EXPECT_GT(std::abs(at.z - 0.6f) + std::abs(at.x - 1.5f), 0.5f) << "the bar swept the ball: " << at.x << ", " << at.z;
    EXPECT_GT(std::sqrt(at.x * at.x + at.z * at.z), 0.3f) << "out of its way, not through it";
}

// The physics project's Mixer, as its script builds it: a drum of 64 sides a metre thick, a bar a script turns each step,
// and bodies of every shape — what a frame takes, and how often something goes through the blade (changes side of it, along
// it, not round its ends) or the floor. MIXER_SHAPE makes them all of one shape (0 sphere … 4 cone), MIXER_NODRUM puts a
// plane for the drum, MIXER_NOBAR leaves the bar out; MIXER_RADIUS, MIXER_HEIGHT, MIXER_SIDES and MIXER_GAP size the drum and
// the bar as a Mixer's settings do, MIXER_COUNT and MIXER_FRAMES how many bodies and frames the benchmark has, MIXER_SEED
// how the load is turned.
class MixerTest : public PhysicsTest {
protected:
    struct Result { int through = 0, below = 0, out = 0, inTheBlade = 0; float deepest = 0; double msAFrame = 0; };
    Result Run(const int count, const int frameCount, const bool cpu)
    {
    const auto knob = [](const char* name, const float otherwise) { const char* v = std::getenv(name); return v ? static_cast<float>(std::atof(v)) : otherwise; };
    const float radius = knob("MIXER_RADIUS", 5.f), height = knob("MIXER_HEIGHT", 6.f), thickness = 1.f, barGap = knob("MIXER_GAP", 0.05f), barThickness = 0.3f;
    const int sides = static_cast<int>(knob("MIXER_SIDES", 64));
    // The drum: floor, wall and lid, every triangle facing in.
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    const auto vertex = [&](const float x, const float y, const float z) { positions.insert(positions.end(), { x, y, z }); return static_cast<std::uint32_t>(positions.size() / 3 - 1); };
    const auto triangle = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c, const kecs::Vec3 n) {
        const auto p = [&](const std::uint32_t i, const int k) { return positions[i * 3 + k]; };
        const float ux = p(b, 0) - p(a, 0), uy = p(b, 1) - p(a, 1), uz = p(b, 2) - p(a, 2), vx = p(c, 0) - p(a, 0), vy = p(c, 1) - p(a, 1), vz = p(c, 2) - p(a, 2);
        const float facing = (uy * vz - uz * vy) * n.x + (uz * vx - ux * vz) * n.y + (ux * vy - uy * vx) * n.z;
        if (facing >= 0) indices.insert(indices.end(), { a, b, c }); else indices.insert(indices.end(), { a, c, b });
    };
    const std::uint32_t floor = vertex(0, 0, 0), lid = vertex(0, height, 0);
    for (int i = 0; i < sides; ++i) {
        const float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
        const std::uint32_t b0 = vertex(radius * std::cos(a0), 0, radius * std::sin(a0)), b1 = vertex(radius * std::cos(a1), 0, radius * std::sin(a1));
        const std::uint32_t t0 = vertex(radius * std::cos(a0), height, radius * std::sin(a0)), t1 = vertex(radius * std::cos(a1), height, radius * std::sin(a1));
        triangle(floor, b0, b1, { 0, 1, 0 });
        triangle(lid, t0, t1, { 0, -1, 0 });
        const kecs::Vec3 in { -std::cos((a0 + a1) / 2), 0, -std::sin((a0 + a1) / 2) };
        triangle(b0, b1, t1, in);
        triangle(b0, t1, t0, in);
    }
    kgraph::VertexData drum;
    drum.vertexCount = static_cast<std::uint32_t>(positions.size() / 3);
    kgraph::VertexAttribute at { "POSITION", kgraph::AttributeType::eFloat, 3, {} };
    for (const float f : positions) at.words.push_back(std::bit_cast<std::uint32_t>(f));
    drum.attributes = { at, { "NORMAL", kgraph::AttributeType::eFloat, 3, std::vector<std::uint32_t>(drum.vertexCount * 3, 0) },
                        { "UV", kgraph::AttributeType::eFloat, 2, std::vector<std::uint32_t>(drum.vertexCount * 2, 0) } };
    drum.indices = indices;
    std::string error;
    Result result;
    EXPECT_TRUE(Doc().Runtime().MakeMesh("$script/mixer-drum", drum, &error)) << error;
    const kecs::Entity drumEntity = Body("Mixer drum", { 0, 0, 0 }, 0, std::getenv("MIXER_NODRUM") ? 3 : 5);
    EXPECT_TRUE(Doc().AddComponent(drumEntity, "Mesh"));
    EXPECT_TRUE(Doc().SetField(drumEntity, "Mesh", "mesh", "$script/mixer-drum", &error)) << error;
    EXPECT_TRUE(Doc().SetField(drumEntity, "Collider", "thickness", thickness));
    const kecs::Entity bar = Body("Mixer bar", { 0, height / 2, std::getenv("MIXER_NOBAR") ? 100.f : 0.f }, 2, 1);
    Doc().World().SetScale(bar, { 2.f * (radius - barGap), height - 0.02f, barThickness });
    EXPECT_TRUE(Doc().AddComponent(bar, "Mesh"));
    EXPECT_TRUE(Doc().SetField(bar, "Mesh", "mesh", "$cube", &error)) << error;
    // The load, on a grid in the two halves.
    const float big = 0.35f, small = 0.15f, cell = 2.6f * big, reach = radius - cell / 2 - 0.05f;
    std::mt19937 random(static_cast<unsigned>(knob("MIXER_SEED", 1)));
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    int made = 0;
    for (float y = cell / 2; y <= height - cell / 2 && made < count; y += cell)
        for (const float side : { 1.f, -1.f })
            for (float x = -reach; x <= reach && made < count; x += cell)
                for (float z = barThickness / 2 + cell / 2 + 0.02f; z <= reach && made < count; z += cell) {
                    if (x * x + z * z > reach * reach) continue;
                    const float size = small + unit(random) * (big - small);
                    const int shape = std::getenv("MIXER_SHAPE") ? std::atoi(std::getenv("MIXER_SHAPE")) : made % 4 == 3 ? 4 : made % 4;
                    const kecs::Entity e = Body("Load", { x, y, side * z }, 1, shape);
                    const char* mesh = shape == 0 ? "$sphere" : shape == 1 ? "$cube" : shape == 2 ? "$capsule" : "$cone";
                    if (shape == 2) Doc().World().SetScale(e, { size * 2.f, size * 2.5f, size * 2.f });
                    else if (shape == 4) Doc().World().SetScale(e, { size * 1.6f, size * 2.f, size * 1.6f });
                    else Doc().World().SetScale(e, { 2 * size, 2 * size, 2 * size });
                    if (shape == 2) EXPECT_TRUE(Doc().SetField(e, "Collider", "radius", 0.25));
                    Doc().World().SetRotation(e, { unit(random) * 360.f, unit(random) * 360.f, unit(random) * 360.f });
                    EXPECT_TRUE(Doc().AddComponent(e, "Mesh"));
                    EXPECT_TRUE(Doc().SetField(e, "Mesh", "mesh", mesh, &error)) << error;
                    ++made;
                }
    const kecs::Entity simulation = Doc().CreateEntity("Physics Simulation");
    EXPECT_TRUE(Doc().AddComponent(simulation, "PhysicsSimulation"));
    EXPECT_TRUE(Doc().SetField(simulation, "PhysicsSimulation", "mode", cpu ? "CPU" : "GPU"));

    float angle = 0.f;
    const auto frames = [&](const int n) {
        for (int i = 0; i < n; ++i) {
            angle += 45.f / 60.f;
            Doc().World().SetRotation(bar, { 0.f, angle, 0.f });
            scene->StepOnce();
            support::Frame();
        }
    };
    frames(60);
    const auto began = std::chrono::steady_clock::now();
    frames(300);
    const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / 300.0;
    // How often something goes through the blade (changes side of it, along it, not round its ends) or the floor, over
    // as many more frames as asked (MIXER_FRAMES, else frameCount).
    {
        const int more = static_cast<int>(knob("MIXER_FRAMES", static_cast<float>(frameCount)));
        const kecs::ComponentId rigid = Doc().World().Find("RigidBody");
        std::vector<kecs::Entity> load;
        for (const kecs::Entity e : Doc().World().All())
            if (e != drumEntity && e != bar && Doc().World().Get(e, rigid)) load.push_back(e);
        const auto sideOf = [&](const kecs::Vec3 p) {
            const float a = angle * 3.14159265f / 180.f;
            // The bar along its +X turned by `angle` about Y — (cos, 0, −sin) — and across it its +Z turned so: (sin, 0, cos).
            const float across = std::sin(a) * p.x + std::cos(a) * p.z, along = std::cos(a) * p.x - std::sin(a) * p.z;
            return std::pair { across, along };
        };
        std::vector<float> was(load.size());
        for (std::size_t k = 0; k < load.size(); ++k) was[k] = sideOf(Doc().World().PositionOf(load[k])).first;
        int through = 0, below = 0, mostBelow = 0;
        // How far into the blade spheres look pressed, in front of it as it turns (its -Z side along +X, its +Z side along
        // -X), drawn where it is now: the deepest, each stretch of 300 frames, and of all.
        float deepest = 0.f;
        std::vector<bool> sphere(load.size());
        for (std::size_t k = 0; k < load.size(); ++k) sphere[k] = Number(load[k], "Collider", "shape") == 0.f;
        for (int f = 0; f < more; ++f) {
            frames(1);
            int now = 0;
            float deepestNow = 0.f;
            for (std::size_t k = 0; k < load.size(); ++k) {
                const kecs::Vec3 p = Doc().World().PositionOf(load[k]);
                const auto [across, along] = sideOf(p);
                const float ahead = along > 0 ? -across : across;
                if (sphere[k] && std::abs(along) < radius - barGap && ahead > 0)
                    deepestNow = std::max(deepestNow, Doc().World().ScaleOf(load[k]).x / 2 - (ahead - barThickness / 2));
                if ((across > 0) != (was[k] > 0) && std::abs(along) < radius - 0.6f && std::abs(across - was[k]) < 1.f) ++through;
                was[k] = across;
                if (p.y < -0.02f) ++now;
            }
            below += now;
            mostBelow = std::max(mostBelow, now);
            deepest = std::max(deepest, deepestNow);
            result.deepest = std::max(result.deepest, deepestNow);
            if (deepestNow > 0.03f) ++result.inTheBlade;
            if ((f + 1) % 300 == 0) {
                std::printf("MIXER frame %d: spheres at most %.3f m into the blade\n", 60 + 300 + f + 1, deepest);
                deepest = 0.f;
            }
        }
        std::printf("MIXER over %d frames: %d through the blade, at most %d below the floor at once (%d body-frames); spheres at most %.3f m "
                    "into the blade, more than 0.03 m in %d frames\n", more, through, mostBelow, below, result.deepest, result.inTheBlade);
        result.through = through;
        result.below = below;
    }
    for (const auto& problem : Doc().Runtime().Problems()) ADD_FAILURE() << problem;
    std::printf("MIXER %d bodies: %.2f ms a frame; the graph's passes take the GPU %.2f ms\n", made, each, scene->Graph().Timing().gpuMs);
    std::vector<std::pair<double, std::string>> passes;
    for (const auto& pass : scene->Graph().PassTimings()) passes.emplace_back(pass.gpuMs + pass.recordMs + pass.prepareMs, std::format("{} GPU {:.3f} record {:.3f} prepare {:.3f}", pass.name, pass.gpuMs, pass.recordMs, pass.prepareMs));
    std::ranges::sort(passes, std::greater {});
    for (std::size_t i = 0; i < std::min<std::size_t>(passes.size(), 12); ++i) std::printf("MIXER   %s\n", passes[i].second.c_str());
    // Nothing through the drum: every body inside it, below its lid.
    int out = 0;
    for (const kecs::Entity e : Doc().World().All()) {
        const auto* body = Doc().World().Get(e, Doc().World().Find("RigidBody"));
        if (!body || e == drumEntity || e == bar) continue;
        const kecs::Vec3 p = Doc().World().PositionOf(e);
        if (p.y < -0.5f || p.y > height + 0.5f || p.x * p.x + p.z * p.z > (radius + 0.5f) * (radius + 0.5f)) ++out;
    }
    std::printf("MIXER %d out of the drum\n", out);
    result.out = out;
    result.msAFrame = each;
    return result;
}
};

// On the GPU as on the CPU: nothing through the blade, nothing through the floor (the GPU's once let a crowd press bodies
// through both: its pushes out of a wall averaged with the crowd's, a mesh's edges pushing back through its face, a cone's
// slant crossed by the blade with none of its points in it). Nor drawn in the blade: what is read back is a step behind
// the blade the script turned this frame, and is sent on to it — when it was not, a sphere was drawn 0.03 m or more in
// it every frame here (0.25 m at the tip of a drum of 21 m); sent on, only when the blade meets it in this frame's step.
TEST_F(MixerTest, NothingGoesThroughTheBladeOrTheFloorOnTheGpu)
{
    const Result gpu = Run(120, 240, false);
    EXPECT_EQ(gpu.through, 0);
    EXPECT_EQ(gpu.below, 0);
    EXPECT_EQ(gpu.out, 0);
    EXPECT_LT(gpu.inTheBlade, 48) << "frames a sphere is drawn 0.03 m or more into the blade, of 240";
}

// Run by hand (--gtest_also_run_disabled_tests): 400 bodies, MIXER_CPU for kphysics. Measured on an RTX 5090 (2026-10-20):
// GPU 7 ms a frame, kphysics 3.3; through the blade and the floor 0 each, as kphysics'.
TEST_F(MixerTest, DISABLED_TheMixerOnTheGpu)
{
    const Result result = Run(std::getenv("MIXER_COUNT") ? std::atoi(std::getenv("MIXER_COUNT")) : 400, 600, std::getenv("MIXER_CPU") != nullptr);
    std::printf("MIXER %.2f ms a frame\n", result.msAFrame);
}
