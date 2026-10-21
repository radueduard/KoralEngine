// The thread pool, and the solver on it: every chunk done once, job after job; and a run by colour the same, to the bit,
// on one thread and on many.
#include "support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <vector>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    std::vector<float> State(const Simulation& sim)
    {
        std::vector<float> state;
        for (int b = 0; b < sim.bodies.Count(); ++b) {
            const Vec3 p = sim.bodies.position[b], v = sim.bodies.velocity[b];
            const Quat q = sim.bodies.orientation[b];
            state.insert(state.end(), { p.x, p.y, p.z, q.x, q.y, q.z, q.w, v.x, v.y, v.z });
        }
        return state;
    }

    /** 600 boxes falling into a pile for 4 s, solved by colour on @p threads. */
    std::vector<float> Pile(const int threads)
    {
        Simulation sim = Falling(600, 3);
        sim.solver.byColour = true;
        sim.threads = threads;
        for (int k = 0; k < 240; ++k) sim.Step(Step);
        return State(sim);
    }
}

TEST(Threads, EveryChunkIsDoneOnceJobAfterJobOnHoweverManyThreadsEachAsksFor)
{
    ThreadPool pool(8);
    ASSERT_EQ(8, pool.Size());
    std::vector<std::atomic<int>> done(5000);
    for (int job = 0; job < 2000; ++job) {
        const int count = 1 + (job * 37) % 5000;
        // Each job on another number of threads: the workers it does not ask for must neither take part nor miss the next.
        pool.For(count, 16, [&](const int begin, const int end) {
            for (int i = begin; i < end; ++i) done[i].fetch_add(1, std::memory_order_relaxed);
        }, 1 + (job * 5) % 9);
        for (int i = 0; i < count; ++i) ASSERT_EQ(1, done[i].exchange(0)) << "job " << job << ", index " << i;
    }
}

TEST(Threads, ARunByColourIsTheSameToTheBitOnAnyNumberOfThreads)
{
    const auto one = Pile(1);
    for (const int threads : { 2, 4, 8, 0 }) {
        const auto many = Pile(threads);
        ASSERT_EQ(one.size(), many.size());
        EXPECT_EQ(0, std::memcmp(one.data(), many.data(), one.size() * sizeof(float))) << threads << " threads";
    }
}

TEST(Threads, AStackStandsSolvedOnThreads)
{
    Simulation sim(16);
    sim.islands.allowSleep = false;
    sim.substeps = 2;
    sim.threads = 4;
    Ground(sim);
    int top = -1;
    for (int i = 0; i < 10; ++i) top = AddBox(sim, { 0.f, 0.5f + static_cast<float>(i), 0.f });
    RunFor(sim, 10.f);
    EXPECT_NEAR(9.5f, sim.bodies.position[top].y, 0.05f);
    EXPECT_LT(std::abs(sim.bodies.position[top].x), 0.01f);
}

TEST(Threads, TheNarrowphaseOnThreadsFindsWhatItFindsAlone)
{
    // Every pair, in pair order, the same manifolds to the bit: on threads the narrowphase only splits the work.
    const auto contacts = [](const int threads) {
        Simulation sim = Falling(2000, 9);
        sim.threads = threads;
        sim.solver.byColour = true;
        for (int k = 0; k < 90; ++k) sim.Step(Step);
        return sim.contacts.manifolds;
    };
    const auto one = contacts(1), many = contacts(8);
    ASSERT_GT(one.size(), 1000u);
    ASSERT_EQ(one.size(), many.size());
    for (std::size_t m = 0; m < one.size(); ++m) {
        ASSERT_EQ(one[m].Key(), many[m].Key()) << m;
        ASSERT_EQ(one[m].count, many[m].count) << m;
        for (int p = 0; p < one[m].count; ++p) {
            ASSERT_EQ(0, std::memcmp(&one[m].points[p], &many[m].points[p], sizeof(ContactPoint))) << m << ", point " << p;
        }
    }
}
