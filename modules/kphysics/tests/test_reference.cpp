// The port checked against the Kotlin core: the reference runs it recorded (MeasureTest.kt), made again here from the
// same scenes, and compared step by step. A stack and a pyramid must follow the Kotlin runs to within a millimetre for all
// 300 steps; 200 boxes raining onto a pile are chaotic, so a difference in the last bit grows, and they are held to the
// first second.
#include "support.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    Recording Reference(const std::string& name)
    {
        Recording run(0, 0);
        std::string error;
        EXPECT_TRUE(Recording::Read(std::filesystem::path(KPHYSICS_REFERENCE_RUNS) / (name + ".kprr"), run, error)) << error;
        return run;
    }

    void Compare(const std::string& name, Simulation sim, const float tolerance, const int steps)
    {
        const Recording kotlin = Reference(name);
        ASSERT_EQ(kotlin.BodyCount(), sim.bodies.Count()) << name;
        Recording cpp(sim.bodies.Count(), kotlin.StepCount());
        cpp.Record(sim, Step);
        const int apart = cpp.FirstStepApart(kotlin, tolerance);
        std::printf("reference run '%s': %d bodies, %d steps; the largest difference %.2e m, first beyond %.0e m at step %d\n",
                    name.c_str(), kotlin.BodyCount(), kotlin.StepCount(), cpp.LargestPositionDifference(kotlin), tolerance, apart);
        EXPECT_TRUE(apart < 0 || apart >= steps) << name << ": apart from the Kotlin run at step " << apart;
    }
}

TEST(Reference, TheStackFollowsTheKotlinRun)
{
    Simulation sim(16);
    sim.substeps = 2;
    Ground(sim);
    for (int i = 0; i < 10; ++i) AddBox(sim, { 0.f, 0.5f + static_cast<float>(i), 0.f });
    Compare("stack", std::move(sim), 1e-3f, 300);
}

TEST(Reference, ThePyramidFollowsTheKotlinRun)
{
    Simulation sim(64);
    sim.substeps = 2;
    Ground(sim);
    Pyramid(sim, 8);
    Compare("pyramid", std::move(sim), 1e-3f, 300);
}

TEST(Reference, TheRainFollowsTheKotlinRunForASecond)
{
    Compare("rain", Falling(200, 5), 1e-3f, 60);
}

TEST(Reference, ARunRepeatsAndIsReadBackAsWritten)
{
    const auto make = [] { return Falling(200, 5); };
    Simulation first = make(), again = make();
    Recording a(first.bodies.Count(), 120), b(again.bodies.Count(), 120);
    a.Record(first, Step);
    b.Record(again, Step);
    EXPECT_EQ(0.f, a.LargestPositionDifference(b)) << "the same run twice, to the bit";
    const auto file = std::filesystem::temp_directory_path() / "kphysics_rain.kprr";
    ASSERT_TRUE(a.Write(file));
    Recording back(0, 0);
    std::string error;
    ASSERT_TRUE(Recording::Read(file, back, error)) << error;
    EXPECT_EQ(0.f, a.LargestPositionDifference(back));
    std::filesystem::remove(file);
}
