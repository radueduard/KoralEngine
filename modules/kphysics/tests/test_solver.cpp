// The solver's gate, as SolverTest.kt sets it: stacks and pyramids stand, bounces and friction are right, a run repeats.
#include "support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <numbers>

using namespace kphysics;
using namespace kphysics::test;

TEST(Solver, AStackOfTenStandsForAMinute)
{
    Simulation sim(16);
    sim.islands.allowSleep = false;                                     // the solver alone, never resting on sleep
    sim.substeps = 2;
    Ground(sim);
    int top = -1;
    for (int i = 0; i < 10; ++i) top = AddBox(sim, { 0.f, 0.5f + static_cast<float>(i), 0.f });
    RunFor(sim, 1.f);
    const Vec3 settled = sim.bodies.position[top];
    RunFor(sim, 59.f);
    const Vec3 drift = sim.bodies.position[top] - settled;
    EXPECT_LT(std::max({ std::abs(drift.x), std::abs(drift.y), std::abs(drift.z) }), 0.01f) << "the top box drifted";
    EXPECT_NEAR(9.5f, sim.bodies.position[top].y, 0.05f);
}

TEST(Solver, APyramidOfTwentyRowsStands)
{
    Simulation sim(256);
    sim.islands.allowSleep = false;
    Ground(sim);
    const int top = Pyramid(sim, 20);
    const float startY = sim.bodies.position[top].y;
    RunFor(sim, 10.f);
    EXPECT_LT(std::abs(sim.bodies.position[top].x), 0.05f);
    EXPECT_LT(startY - sim.bodies.position[top].y, 0.1f);
}

TEST(Solver, ABallBouncesToESquaredOfItsHeight)
{
    Simulation sim(4);
    Ground(sim);
    const float e = 0.8f;
    const int ball = AddBall(sim, { 0.f, 2.5f, 0.f }, 0.5f, 1000.f, 0.6f, e);       // its bottom 2 m up
    bool bounced = false;
    float peak = 0.f;
    RunFor(sim, 3.f, [&](int) {
        if (sim.bodies.velocity[ball].y > 0.f) bounced = true;
        if (bounced) peak = std::max(peak, sim.bodies.position[ball].y - 0.5f);
    });
    EXPECT_NEAR(e * e * 2.f, peak, 0.05f * e * e * 2.f);
}

namespace
{
    /** A box on a slope of @p degrees, both with friction 0.5 (tan θ = 0.5 at 26.57°): how far it slides in 2 s. */
    float Slide(const float degrees)
    {
        Simulation sim(4);
        sim.islands.allowSleep = false;
        const Quat q = Turn({ 0.f, 0.f, 1.f }, degrees);
        Ground(sim, q, 0.5f);
        const float radians = degrees * std::numbers::pi_v<float> / 180.f;
        const int b = AddBox(sim, { -std::sin(radians) * 0.5f, std::cos(radians) * 0.5f, 0.f }, { .turn = q, .friction = 0.5f });
        RunFor(sim, 0.2f);
        const Vec3 start = sim.bodies.position[b];
        RunFor(sim, 2.f);
        return Length(sim.bodies.position[b] - start);
    }
}

TEST(Solver, ABoxHoldsBelowTheFrictionAngleAndSlidesAbove)
{
    EXPECT_LT(Slide(25.6f), 0.01f) << "held a degree below atan 0.5";
    EXPECT_GT(Slide(27.6f), 0.1f) << "slides a degree above";
}

TEST(Solver, ASpinningBoxComesToRest)
{
    Simulation sim(4);
    Ground(sim);
    const int b = AddBox(sim, { 0.f, 1.f, 0.f }, { .turn = Turn({ 1.f, 1.f, 0.f }, 30.f) });
    sim.bodies.velocity[b].x = 3.f;
    sim.bodies.angularVelocity[b] = { 0.f, 6.f, 4.f };
    int restedAt = -1;
    RunFor(sim, 8.f, [&](const int k) { if (restedAt < 0 && !sim.awake[b]) restedAt = k; });
    EXPECT_GE(restedAt, 0) << "it never came to rest";
    EXPECT_NEAR(0.5f, sim.bodies.position[b].y, 0.02f) << "resting on a face";
}

TEST(Solver, TheSameRunTwiceIsTheSameToTheBit)
{
    const auto record = [] {
        Simulation sim(64);
        Ground(sim);
        Pyramid(sim, 5);
        AddBall(sim, { 0.3f, 8.f, 0.2f });
        RunFor(sim, 3.f);
        std::vector<float> state;
        for (int b = 0; b < sim.bodies.Count(); ++b) {
            const Vec3 p = sim.bodies.position[b];
            const Quat q = sim.bodies.orientation[b];
            state.insert(state.end(), { p.x, p.y, p.z, q.x, q.y, q.z, q.w });
        }
        return state;
    };
    const auto first = record(), second = record();
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(0, std::memcmp(first.data(), second.data(), first.size() * sizeof(float)));
}

TEST(Solver, TheStackStandsSolvedByColour)
{
    Simulation sim(16);
    sim.islands.allowSleep = false;
    sim.solver.byColour = true;
    sim.substeps = 2;
    Ground(sim);
    int top = -1;
    for (int i = 0; i < 10; ++i) top = AddBox(sim, { 0.f, 0.5f + static_cast<float>(i), 0.f });
    RunFor(sim, 10.f);
    EXPECT_TRUE(sim.colouring.IsValid(sim));
    EXPECT_NEAR(9.5f, sim.bodies.position[top].y, 0.05f);
    EXPECT_LT(std::abs(sim.bodies.position[top].x), 0.01f);
    EXPECT_EQ(2, sim.colouring.ColourCount()) << "a stack needs two colours: every other contact";
}
