// The broadphase's gate: every way finds the same pairs as every pair does; the tree kept up as bodies move, built
// again when most of it moves, and reaching ahead of a body moving steadily. As BroadphaseTest.kt checks it.
#include "support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    /** @p n bodies of every shape, scattered and turned, about one a 1.6 m cube, over a ground. */
    Simulation Scattered(const int n, const int seed)
    {
        Simulation sim(n + 1);
        KotlinRandom random(seed);
        const float side = std::cbrt(static_cast<float>(n)) * 1.6f;
        Ground(sim);
        for (int i = 0; i < n; ++i) {
            const float ax = random.NextFloat() - 0.5f, ay = random.NextFloat() - 0.5f, az = random.NextFloat() - 0.5f;
            const Quat q = Turn({ ax, ay, az }, random.NextFloat() * 360.f);
            const float x = random.NextFloat() * side, y = random.NextFloat() * side - 1.f, z = random.NextFloat() * side;
            if (i % 3 == 0) {
                const float hx = 0.2f + random.NextFloat() * 0.6f, hy = 0.2f + random.NextFloat() * 0.6f, hz = 0.2f + random.NextFloat() * 0.6f;
                AddBox(sim, { x, y, z }, { .half = { hx, hy, hz }, .turn = q });
            } else if (i % 3 == 1) {
                AddBall(sim, { x, y, z }, 0.2f + random.NextFloat() * 0.5f);
            } else {
                const float radius = 0.15f + random.NextFloat() * 0.3f;
                AddCapsule(sim, { x, y, z }, radius, random.NextFloat() * 0.8f, q);
            }
        }
        return sim;
    }

    std::vector<std::uint64_t> PairsBy(Simulation& sim, const BroadphaseKind kind)
    {
        const BroadphaseKind was = sim.broadphase.kind;
        sim.broadphase.kind = kind;
        sim.broadphase.Update(sim);
        sim.broadphase.kind = was;
        return sim.broadphase.pairs;
    }
}

TEST(Broadphase, EachShapesBox)
{
    Simulation sim(4);
    sim.contactMargin = 0.f;
    const float r2 = std::sqrt(0.5f);
    const int box = AddBox(sim, { 1.f, 2.f, 3.f }, { .half = { 1.f, 0.5f, 0.25f }, .turn = Turn({ 0.f, 1.f, 0.f }, 45.f) });
    const int capsule = AddCapsule(sim, {}, 0.25f, 0.5f, Turn({ 0.f, 0.f, 1.f }, 90.f));       // lying along X
    const int ball = AddBall(sim, { -3.f, 0.f, 0.f }, 0.4f);
    const int ground = Ground(sim);
    Broadphase& bp = sim.broadphase;
    for (int b = 0; b < 4; ++b) bp.Bounds(sim, b, 0.f);
    EXPECT_NEAR(1.f - 1.25f * r2, bp.lower[box].x, 1e-5f);
    EXPECT_NEAR(2.5f, bp.upper[box].y, 1e-5f);
    EXPECT_NEAR(3.f + 1.25f * r2, bp.upper[box].z, 1e-5f);
    EXPECT_NEAR(-0.75f, bp.lower[capsule].x, 1e-5f);
    EXPECT_NEAR(0.25f, bp.upper[capsule].y, 1e-5f);
    EXPECT_NEAR(-3.4f, bp.lower[ball].x, 1e-5f);
    EXPECT_TRUE(bp.lower[ground].x < -1e20f && bp.upper[ground].y > 1e20f) << "a plane reaches everywhere";
    bp.Bounds(sim, ball, 0.02f);
    EXPECT_NEAR(-3.42f, bp.lower[ball].x, 1e-5f) << "widened by the margin";
}

TEST(Broadphase, EveryWayFindsTheSamePairs)
{
    for (const int n : { 100, 1000, 10000 }) {
        Simulation sim = Scattered(n, n);
        const auto reference = PairsBy(sim, BroadphaseKind::AllPairs);
        EXPECT_TRUE(std::ranges::is_sorted(reference)) << "in pair order";
        EXPECT_EQ(reference, PairsBy(sim, BroadphaseKind::SweepAndPrune)) << n << " bodies";
        EXPECT_EQ(reference, PairsBy(sim, BroadphaseKind::Grid)) << n << " bodies";
        EXPECT_EQ(reference, PairsBy(sim, BroadphaseKind::Tree)) << n << " bodies";
        const DynamicTree& tree = sim.broadphase.Tree();
        EXPECT_TRUE(tree.IsValid());
        EXPECT_LE(tree.Height(), std::log2(n) + 2) << "split at medians, the tree is balanced";
    }
}

TEST(Broadphase, SweepAndPruneAndTheTreeKeepUpAsBodiesMove)
{
    for (const BroadphaseKind kind : { BroadphaseKind::SweepAndPrune, BroadphaseKind::Tree }) {
        Simulation sim = Scattered(500, 7);
        PairsBy(sim, kind);
        KotlinRandom random(11);
        for (int round = 0; round < 20; ++round) {
            // Everything moves a little, some a lot: what is kept from last time must still give every pair.
            for (int b = 1; b < sim.bodies.Count(); ++b)
                for (int c = 0; c < 3; ++c) sim.bodies.position[b][c] += (random.NextFloat() - 0.5f) * (b % 17 == 0 ? 4.f : 0.2f);
            const auto kept = PairsBy(sim, kind);
            EXPECT_EQ(PairsBy(sim, BroadphaseKind::AllPairs), kept) << "after moving, round " << round;
        }
        if (kind == BroadphaseKind::Tree) EXPECT_TRUE(sim.broadphase.Tree().IsValid());
    }
}

TEST(Broadphase, TheTreeIsBuiltAgainWhenMostOfItMoves)
{
    Simulation sim = Scattered(2000, 3);
    PairsBy(sim, BroadphaseKind::Tree);
    const DynamicTree& tree = sim.broadphase.Tree();
    KotlinRandom random(5);
    for (int round = 0; round < 3; ++round) {
        // Everything thrown up to 3 m: far more than a quarter of the leaves leave their fat boxes.
        for (int b = 1; b < sim.bodies.Count(); ++b)
            for (int c = 0; c < 3; ++c) sim.bodies.position[b][c] += (random.NextFloat() - 0.5f) * 6.f;
        const auto kept = PairsBy(sim, BroadphaseKind::Tree);
        EXPECT_EQ(PairsBy(sim, BroadphaseKind::AllPairs), kept) << "after a rebuild";
    }
    EXPECT_EQ(3, tree.Rebuilds()) << "built again each time, not leaf by leaf";
    EXPECT_TRUE(tree.IsValid());
    EXPECT_LE(tree.Height(), std::log2(2000.0) + 2);
    // Then they stop: every fat box still reaches metres ahead, far bigger than a still body needs, so the tree is built
    // once more with small ones; after that a few moving are put back leaf by leaf.
    EXPECT_EQ(PairsBy(sim, BroadphaseKind::AllPairs), PairsBy(sim, BroadphaseKind::Tree));
    EXPECT_EQ(4, tree.Rebuilds());
    for (int b = 1; b < 20; ++b) sim.bodies.position[b].x += 2.f;
    const auto kept = PairsBy(sim, BroadphaseKind::Tree);
    EXPECT_EQ(PairsBy(sim, BroadphaseKind::AllPairs), kept);
    EXPECT_EQ(4, tree.Rebuilds());
    EXPECT_TRUE(tree.IsValid());
}

TEST(Broadphase, ALeafReachesAheadOfABodyMovingSteadily)
{
    // A body sliding at 20 cm a step among still ones: without reaching ahead its leaf would be put back every step.
    // Reaching 4 steps ahead (0.8 m), about one step in five.
    Simulation sim(64);
    for (int i = 0; i < 50; ++i) AddBox(sim, { static_cast<float>(i % 10) * 3.f, 0.f, static_cast<float>(i / 10) * 3.f }, { .half = { 0.3f, 0.3f, 0.3f } });
    const int mover = AddBox(sim, { -2.f, 0.f, 1.5f }, { .half = { 0.3f, 0.3f, 0.3f } });
    sim.broadphase.kind = BroadphaseKind::Tree;
    sim.broadphase.Update(sim);
    const DynamicTree& tree = sim.broadphase.Tree();
    for (int step = 0; step < 200; ++step) {
        sim.bodies.position[mover].x += 0.2f;
        sim.broadphase.Update(sim);
        if (step % 20 == 0) {
            const auto kept = sim.broadphase.pairs;
            EXPECT_EQ(PairsBy(sim, BroadphaseKind::AllPairs), kept) << "step " << step;
        }
    }
    EXPECT_GE(tree.Reinsertions(), 30);
    EXPECT_LE(tree.Reinsertions(), 70);
    // Then it stops: its fat box, reaching 0.8 m ahead, is more than 4 × Extension too long: put back once, small.
    const long before = tree.Reinsertions();
    for (int i = 0; i < 10; ++i) sim.broadphase.Update(sim);
    EXPECT_EQ(before + 1, tree.Reinsertions());
    EXPECT_TRUE(tree.IsValid());
}

TEST(Broadphase, TheTreeAndTheGridFindBodiesOfVeryDifferentSizes)
{
    Simulation sim(2100);
    KotlinRandom random(4);
    for (int i = 0; i < 2000; ++i) {
        const float x = random.NextFloat() * 60.f, y = random.NextFloat() * 60.f, z = random.NextFloat() * 60.f;
        AddBox(sim, { x, y, z }, { .half = { 0.1f, 0.1f, 0.1f } });
    }
    for (int i = 0; i < 20; ++i) {
        const float x = random.NextFloat() * 60.f, y = random.NextFloat() * 60.f, z = random.NextFloat() * 60.f;
        AddBox(sim, { x, y, z }, { .half = { 6.f, 0.5f, 6.f } });
    }
    const auto reference = PairsBy(sim, BroadphaseKind::AllPairs);
    EXPECT_EQ(reference, PairsBy(sim, BroadphaseKind::Tree));
    EXPECT_EQ(reference, PairsBy(sim, BroadphaseKind::Grid));
}

TEST(Broadphase, StaticPairsAndSleepingPairsAreLeftOut)
{
    Simulation sim(8);
    Ground(sim);
    AddBox(sim, { 0.f, 0.4f, 0.f }, { .fixed = true });                 // static on static: never a pair
    const int a = AddBox(sim, { 3.f, 0.49f, 0.f });
    const int b = AddBox(sim, { 3.f, 1.48f, 0.f });
    sim.broadphase.Update(sim);
    EXPECT_EQ((std::vector { PairKey(0, a), PairKey(0, b), PairKey(a, b) }), sim.broadphase.pairs);
    sim.awake[a] = 0;
    sim.awake[b] = 0;
    sim.broadphase.Update(sim);
    EXPECT_EQ(0, sim.broadphase.PairCount()) << "asleep on the ground: nothing to test";
    sim.awake[b] = 1;
    sim.broadphase.Update(sim);
    EXPECT_EQ(2, sim.broadphase.PairCount()) << "one awake: its pairs again";
}
