// Joints, as JointTest.kt sets the gate: a chain of 20 hangs without stretching, a hinged door stops at its limit.
#include "support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    constexpr float Degrees = 180.f / std::numbers::pi_v<float>;
}

TEST(Joints, AChainOfTwentySwingsDownWithoutStretching)
{
    Simulation sim(32);
    sim.substeps = 2;
    const int links = 20;
    const float length = 0.25f;
    // Capsule links lying along X, end to end from a point in the world at the origin: let go, it swings down.
    const Quat along = Turn({ 0.f, 0.f, 1.f }, 90.f);
    std::vector<int> capsules, joints;
    for (int i = 0; i < links; ++i) capsules.push_back(AddCapsule(sim, { (static_cast<float>(i) + 0.5f) * length, 0.f, 0.f }, 0.04f, length / 2, along));
    for (int i = 0; i < links; ++i) joints.push_back(sim.joints.AddBall(sim, i == 0 ? -1 : capsules[i - 1], capsules[i], { static_cast<float>(i) * length, 0.f, 0.f }));
    float worst = 0.f, lowest = 0.f;
    RunFor(sim, 8.f, [&](int) {
        for (const int j : joints) worst = std::max(worst, sim.joints.Gap(sim, j));
        lowest = std::min(lowest, sim.bodies.position[capsules.back()].y);
    });
    std::printf("chain of 20: joints opened by at most %.4f m; its last link swung down to y = %.3f\n", worst, lowest);
    EXPECT_LT(worst, 0.01f) << "a joint opened";
    const float straight = (links - 0.5f) * length;
    EXPECT_LT(lowest, -0.97f * straight);
    EXPECT_GT(lowest, -1.01f * straight);
}

TEST(Joints, ADoorSwingsAndStopsAtItsLimit)
{
    Simulation sim(4);
    sim.gravity = {};
    const int door = AddBox(sim, { 0.5f, 1.f, 0.f }, { .half = { 0.5f, 1.f, 0.05f } });
    const int hinge = sim.joints.AddHinge(sim, -1, door, { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f });
    sim.joints.SetLimits(hinge, 0.f, std::numbers::pi_v<float> / 2);
    sim.bodies.angularVelocity[door].y = 3.f;                           // pushed open
    float widest = 0.f;
    RunFor(sim, 3.f, [&](int) { widest = std::max(widest, sim.joints.AngleOf(hinge)); });
    EXPECT_LT(widest * Degrees, 92.f) << "past its limit";
    EXPECT_NEAR(90.f, sim.joints.AngleOf(hinge) * Degrees, 2.f) << "it stops at it";
    EXPECT_LT(Spin(sim, door), 0.1f) << "and stays";
    const Vec3 p = sim.bodies.position[door];
    EXPECT_NEAR(0.5f, std::sqrt(p.x * p.x + p.z * p.z), 0.01f) << "its centre stays 0.5 m from its hinge";
}

TEST(Joints, AHeavyDoorHangsOnItsHingeAndSwingsToItsLimit)
{
    // 200 kg, with gravity, 5 cm above a ground: held up by its hinge alone. (Its joint matrix has entries near 1/m: a
    // solver that called that singular let it fall.)
    Simulation sim(4);
    sim.substeps = 2;
    Ground(sim);
    const int door = AddBox(sim, { 2.f, 1.05f, 0.f }, { .half = { 0.5f, 1.f, 0.05f } });
    const int hinge = sim.joints.AddHinge(sim, -1, door, { 1.5f, 1.05f, 0.f }, { 0.f, 1.f, 0.f });
    sim.joints.SetLimits(hinge, 0.f, std::numbers::pi_v<float> / 2);
    sim.bodies.angularVelocity[door].y = 3.f;
    RunFor(sim, 3.f);
    EXPECT_NEAR(1.05f, sim.bodies.position[door].y, 0.01f) << "it dropped";
    EXPECT_NEAR(90.f, sim.joints.AngleOf(hinge) * Degrees, 2.f) << "it swung to its limit";
}

TEST(Joints, AMotorDrivesAWheel)
{
    Simulation sim(4);
    sim.gravity = {};
    const int wheel = AddCapsule(sim, {}, 0.5f, 0.f);
    const int hinge = sim.joints.AddHinge(sim, -1, wheel, {}, { 0.f, 0.f, 1.f });
    sim.joints.SetMotor(hinge, 2.f, 1000.f);
    RunFor(sim, 1.f);
    EXPECT_NEAR(2.f, sim.bodies.angularVelocity[wheel].z, 0.01f);
    EXPECT_LT(std::abs(sim.bodies.angularVelocity[wheel].x), 1e-3f);
    EXPECT_LT(std::abs(sim.bodies.angularVelocity[wheel].y), 1e-3f);
}

TEST(Joints, ARopeOnlyPulls)
{
    Simulation sim(4);
    const int bob = AddBall(sim, { 1.f, 0.f, 0.f }, 0.1f);
    sim.joints.AddDistance(sim, -1, bob, {}, { 1.f, 0.f, 0.f }, true);
    sim.bodies.velocity[bob].x = -2.f;                                  // thrown back towards the anchor: slack
    float shortest = 1.f, longest = 0.f;
    RunFor(sim, 3.f, [&](int) {
        const float l = Length(sim.bodies.position[bob]);
        shortest = std::min(shortest, l);
        longest = std::max(longest, l);
    });
    EXPECT_LT(shortest, 0.9f) << "it went slack";
    EXPECT_LT(longest, 1.01f) << "and never stretched";
}

TEST(Joints, JointedBodiesDoNotCollide)
{
    Simulation sim(4);
    sim.gravity = {};
    const int a = AddBox(sim, {});
    const int b = AddBox(sim, { 0.9f, 0.f, 0.f });                     // overlapping by 10 cm where they meet
    sim.joints.AddBall(sim, a, b, { 0.45f, 0.f, 0.f });
    RunFor(sim, 0.5f);
    EXPECT_EQ(0, sim.contacts.Count());
    EXPECT_NEAR(0.9f, sim.bodies.position[b].x, 1e-3f);
}
