// Islands and sleep, as SleepTest.kt sets the gate: a settled pile costs almost nothing, and wakes when a ball hits it.
#include "support.h"

#include <gtest/gtest.h>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    /** @p n boxes dropped in layers of 5 × 5 onto the ground, a little apart and turned. */
    Simulation Pile(const int n)
    {
        Simulation sim(n + 2);
        sim.substeps = 2;
        Ground(sim);
        KotlinRandom random(3);
        for (int i = 0; i < n; ++i) {
            const int layer = i / 25, row = (i % 25) / 5, column = i % 5;
            const float x = static_cast<float>(column - 2) * 1.2f + random.NextFloat() * 0.2f;
            const float y = 0.6f + static_cast<float>(layer) * 1.3f;
            const float z = static_cast<float>(row - 2) * 1.2f + random.NextFloat() * 0.2f;
            const float ax = random.NextFloat(), ay = random.NextFloat(), az = random.NextFloat(), degrees = random.NextFloat() * 40.f;
            AddBox(sim, { x, y, z }, { .half = { 0.45f, 0.45f, 0.45f }, .turn = Turn({ ax, ay, az }, degrees) });
        }
        return sim;
    }
}

TEST(Sleep, ASettledPileSleepsAndCostsAlmostNothing)
{
    Simulation sim = Pile(500);
    double awakeTime = 0.0;
    int awakeSteps = 0;
    int settledAt = -1;
    RunFor(sim, 20.f, [&](const int k) {
        if (k >= 60 && k <= 240) { awakeTime += sim.timing.step; ++awakeSteps; }
        if (settledAt < 0 && sim.SleepingCount() == 500) settledAt = k;
    });
    ASSERT_GE(settledAt, 0) << "still " << 500 - sim.SleepingCount() << " awake after 20 s";
    double asleepTime = 0.0;
    RunFor(sim, 1.f, [&](int) { asleepTime += sim.timing.step; });
    const double awake = awakeTime / awakeSteps, asleep = asleepTime / 60;
    std::printf("500 boxes: all asleep after %.1f s; a step %.3f ms while settling, %.4f ms asleep\n", settledAt * Step, awake, asleep);
    EXPECT_EQ(0, sim.contacts.Count()) << "asleep, no contact is even looked for";
    EXPECT_LT(asleep, awake * 0.05) << "asleep costs under 5% of awake";
}

TEST(Sleep, ABallWakesThePileItHits)
{
    Simulation sim = Pile(100);
    RunFor(sim, 15.f);
    ASSERT_EQ(100, sim.SleepingCount()) << "settled";
    const int ball = AddBall(sim, { 0.f, 12.f, 0.f }, 0.6f, 5000.f);
    sim.bodies.velocity[ball].y = -15.f;
    int woke = 0;
    RunFor(sim, 1.5f, [&](int) { woke = std::max(woke, 100 - sim.SleepingCount() + (sim.awake[ball] ? 0 : 1)); });
    EXPECT_GE(woke, 5) << "the boxes it hit woke";
}

TEST(Sleep, SeparateIslandsSleepSeparately)
{
    Simulation sim(8);
    Ground(sim);
    const int still = AddBox(sim, { -3.f, 0.5f, 0.f });
    const int spinning = AddBox(sim, { 3.f, 0.5f, 0.f });
    RunFor(sim, 2.f, [&](int) { sim.bodies.angularVelocity[spinning].y = 2.f; });      // kept spinning on its face
    EXPECT_FALSE(sim.awake[still]) << "the still box sleeps";
    EXPECT_TRUE(sim.awake[spinning]) << "while the spinning one, on the same ground, does not";
}
