// How fast the port is, on the Kotlin core's own measurements (MeasureTest.kt and BroadphaseBenchmark.kt), so the two
// can be set side by side: each phase on 1,000, 5,000 and 20,000 falling boxes; a 1,000-box pile, awake, against the
// plan's 4 ms budget; and 2,000 boxes falling into a pile with each broadphase. Build in Release to mean anything.
//
//   kphysics_bench [csv folder]
#include "support.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    Timing Measure(Simulation& sim, const int warmUp, const int steps)
    {
        for (int i = 0; i < warmUp; ++i) sim.Step(Step);
        Timing sum;
        for (int i = 0; i < steps; ++i) {
            sim.Step(Step);
            sum.broadphase += sim.timing.broadphase;
            sum.narrowphase += sim.timing.narrowphase;
            sum.integrate += sim.timing.integrate;
            sum.solve += sim.timing.solve;
            sum.sleep += sim.timing.sleep;
            sum.step += sim.timing.step;
        }
        const float n = static_cast<float>(steps);
        return { sum.broadphase / n, sum.narrowphase / n, sum.integrate / n, sum.solve / n, sum.sleep / n, sum.step / n };
    }

    const char* Name(const BroadphaseKind kind)
    {
        switch (kind) {
        case BroadphaseKind::AllPairs: return "AllPairs";
        case BroadphaseKind::SweepAndPrune: return "SweepAndPrune";
        case BroadphaseKind::Grid: return "Grid";
        case BroadphaseKind::Tree: return "Tree";
        }
        return "?";
    }
}

int main(const int argc, char** argv)
{
    const std::filesystem::path folder = argc > 1 ? argv[1] : "kphysics-bench";
    std::filesystem::create_directories(folder);

    std::printf("Each phase, falling boxes (grid broadphase, 2 substeps), averaged over 60 steps:\n");
    for (const int n : { 1000, 5000, 20000 }) {
        Simulation sim = Falling(n);
        const Timing t = Measure(sim, n > 5000 ? 60 : 120, 60);
        std::printf("%6d boxes, %d contacts: %s\n", n, sim.contacts.Count(), t.ToString().c_str());
    }

    {
        Simulation sim = Falling(1000);
        for (int i = 0; i < 300; ++i) sim.Step(Step);                  // fallen, piled, not yet asleep
        sim.islands.allowSleep = false;                                 // measured awake: sleeping would make it free
        const Timing t = Measure(sim, 60, 120);
        std::printf("\n1,000-box pile, awake: %s\n%s\n", t.ToString().c_str(),
                    t.step < 4.f ? "within the 4 ms budget" : "OVER the 4 ms budget");
    }

    std::printf("\nThe solver on threads (by colour; 1* is the caller alone, in manifold order, as the Kotlin core):\n");
    for (const int threads : { -1, 1, 2, 4, 8, 16, 0 }) {
        Simulation sim = Falling(1000);
        for (int i = 0; i < 300; ++i) sim.Step(Step);
        sim.islands.allowSleep = false;
        sim.solver.byColour = threads != -1;
        sim.threads = threads == -1 ? 1 : threads;
        const Timing pile = Measure(sim, 60, 120);
        Simulation big = Falling(5000);
        big.solver.byColour = threads != -1;
        big.threads = threads == -1 ? 1 : threads;
        big.broadphase.kind = BroadphaseKind::Tree;
        const Timing falling = Measure(big, 120, 60);
        if (threads == 0) std::printf("  auto (a thread per %d manifolds, %d and %d here):\n", Simulation::WorkPerThread, sim.ThreadsFor(sim.contacts.Count()), big.ThreadsFor(big.contacts.Count()));
        std::printf("  %2d%s thread%s  1,000-box pile awake: step %.3f ms (solve %.3f)   5,000 falling: step %.3f ms (solve %.3f), %d colours\n",
                    threads == -1 ? 1 : threads, threads == -1 ? "*" : " ", threads == 1 || threads == -1 ? " " : "s", pile.step, pile.solve,
                    falling.step, falling.solve, big.colouring.ColourCount());
    }

    std::printf("\n2,000 boxes falling into a pile, 8 s, averages after the first second:\n");
    std::ofstream csv(folder / "falling.csv");
    csv << "method,step,time_s,broadphase_ms,step_ms,pairs,contacts,asleep\n";
    for (const BroadphaseKind kind : { BroadphaseKind::AllPairs, BroadphaseKind::SweepAndPrune, BroadphaseKind::Grid, BroadphaseKind::Tree }) {
        Simulation sim(2001);
        sim.substeps = 2;
        sim.broadphase.kind = kind;
        Ground(sim);
        KotlinRandom random(7);
        for (int i = 0; i < 2000; ++i) {
            const int layer = i / 100, row = (i % 100) / 10, column = i % 10;
            const float ax = random.NextFloat(), ay = random.NextFloat(), az = random.NextFloat(), degrees = random.NextFloat() * 30.f;
            AddBox(sim, { static_cast<float>(column - 5) * 1.15f, 0.7f + static_cast<float>(layer) * 1.2f, static_cast<float>(row - 5) * 1.15f },
                   { .turn = Turn({ ax, ay, az }, degrees) });
        }
        double broad = 0.0, whole = 0.0, narrow = 0.0, solve = 0.0;
        for (int k = 0; k < 480; ++k) {
            sim.Step(Step);
            char line[200];
            std::snprintf(line, sizeof line, "%s,%d,%.4f,%.4f,%.4f,%d,%d,%d\n", Name(kind), k, (k + 1) * Step, sim.timing.broadphase,
                          sim.timing.step, sim.broadphase.PairCount(), sim.contacts.Count(), sim.SleepingCount());
            csv << line;
            if (k >= 60) {
                broad += sim.timing.broadphase;
                narrow += sim.timing.narrowphase;
                solve += sim.timing.solve;
                whole += sim.timing.step;
            }
        }
        std::printf("  %-13s broadphase %.3f ms, narrowphase %.3f, solve %.3f, whole step %.3f ms; %d asleep at the end\n", Name(kind),
                    broad / 420, narrow / 420, solve / 420, whole / 420, sim.SleepingCount());
    }
    std::printf("wrote %s\n", (folder / "falling.csv").string().c_str());
}
