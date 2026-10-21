// Mass properties and the bodies' arrays, as MassPropertiesTest.kt and BodiesTest.kt check them.
#include "support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <numbers>
#include <stdexcept>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    /** Mass and inertia diagonal of what @p inside says is solid in the box ±e, summed over n small cubes along x. */
    std::array<double, 4> Voxels(const double density, const Vec3 e, const int n, const std::function<bool(double, double, double)>& inside)
    {
        const double size = 2.0 * e.x / n;
        const int ny = static_cast<int>(std::ceil(2.0 * e.y / size)), nz = static_cast<int>(std::ceil(2.0 * e.z / size));
        const double dm = density * size * size * size;
        std::array<double, 4> total {};
        for (int a = 0; a < n; ++a)
            for (int b = 0; b < ny; ++b)
                for (int c = 0; c < nz; ++c) {
                    const double x = -e.x + (a + 0.5) * size, y = -e.y + (b + 0.5) * size, z = -e.z + (c + 0.5) * size;
                    if (!inside(x, y, z)) continue;
                    total[0] += dm;
                    total[1] += dm * (y * y + z * z);
                    total[2] += dm * (x * x + z * z);
                    total[3] += dm * (x * x + y * y);
                }
        return total;
    }
}

TEST(Mass, ASphere)
{
    Vec3 inertia;
    const float m = mass::Sphere(1000.f, 0.5f, inertia);
    const double expected = 1000.0 * 4.0 / 3.0 * std::numbers::pi * 0.125;
    EXPECT_NEAR(expected, m, expected * 1e-5);
    for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(0.4 * expected * 0.25, inertia[axis], expected * 1e-5);
    const auto sum = Voxels(1000.0, { 0.5f, 0.5f, 0.5f }, 120, [](double x, double y, double z) { return x * x + y * y + z * z <= 0.25; });
    EXPECT_NEAR(sum[0], m, sum[0] * 0.01);
    for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(sum[axis + 1], inertia[axis], sum[axis + 1] * 0.01);
}

TEST(Mass, ABox)
{
    Vec3 inertia;
    EXPECT_NEAR(96.f, mass::Box(2.f, { 1.f, 2.f, 3.f }, inertia), 1e-3f);
    EXPECT_NEAR(416.f, inertia.x, 1e-2f);
    EXPECT_NEAR(320.f, inertia.y, 1e-2f);
    EXPECT_NEAR(160.f, inertia.z, 1e-2f);
}

TEST(Mass, ACapsuleOfNoLengthIsASphere)
{
    Vec3 sphere, capsule;
    const float ms = mass::Sphere(7.f, 0.3f, sphere);
    EXPECT_NEAR(ms, mass::Capsule(7.f, 0.3f, 0.f, capsule), ms * 1e-5f);
    for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(sphere[axis], capsule[axis], sphere[axis] * 1e-4f);
}

TEST(Mass, TheDefaultCapsuleAgainstVoxels)
{
    Vec3 inertia;
    const double r = 0.25, h = 0.25;
    const float m = mass::Capsule(1000.f, 0.25f, 0.25f, inertia);
    const auto sum = Voxels(1000.0, { 0.25f, 0.5f, 0.25f }, 100, [&](double x, double y, double z) {
        const double along = std::clamp(y, -h, h);
        return x * x + (y - along) * (y - along) + z * z <= r * r;
    });
    EXPECT_NEAR(sum[0], m, sum[0] * 0.01);
    for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(sum[axis + 1], inertia[axis], sum[axis + 1] * 0.01);
}

TEST(Bodies, ABodyIsAddedWhereItIsAskedForAndStoredAsItsInverses)
{
    Bodies bodies(4);
    EXPECT_EQ(0, bodies.Add({}, {}, 4.f, { 1.f, 2.f, 4.f }));
    EXPECT_EQ(1, bodies.Add({ 1.f, 2.f, 3.f }, { 0.f, 2.f, 0.f, 2.f }, 2.f, { 1.f, 2.f, 4.f }));
    EXPECT_EQ(2, bodies.Count());
    EXPECT_EQ((Vec3 { 1.f, 2.f, 3.f }), bodies.position[1]);
    EXPECT_NEAR(0.70710678f, bodies.orientation[1].y, 1e-6f) << "made length 1";
    EXPECT_EQ(Vec3 {}, bodies.velocity[1]);
    EXPECT_NEAR(0.25f, bodies.inverseMass[0], 1e-7f);
    EXPECT_EQ((Vec3 { 1.f, 0.5f, 0.25f }), bodies.inverseInertiaLocal[0]);
}

TEST(Bodies, AMassOfNothingIsStatic)
{
    Bodies bodies(1);
    bodies.Add({}, {}, 0.f, { 1.f, 2.f, 4.f });
    EXPECT_EQ(0.f, bodies.inverseMass[0]);
    EXPECT_EQ(Vec3 {}, bodies.inverseInertiaLocal[0]);
    for (const Vec3 column : bodies.inverseInertiaWorld[0].column) EXPECT_EQ(Vec3 {}, column);
}

TEST(Bodies, TheWorldInertiaFollowsTheTurn)
{
    Bodies bodies(1);
    const float half = 0.70710678f;
    bodies.Add({}, { 0.f, half, 0.f, half }, 1.f, { 1.f, 2.f, 4.f });         // a quarter turn about Y
    const Mat3& world = bodies.inverseInertiaWorld[0];
    EXPECT_NEAR(0.25f, world.column[0].x, 1e-5f);
    EXPECT_NEAR(0.5f, world.column[1].y, 1e-5f);
    EXPECT_NEAR(1.f, world.column[2].z, 1e-5f);
    bodies.orientation[0] = {};
    bodies.UpdateWorldInertia();
    EXPECT_NEAR(1.f, world.column[0].x, 1e-6f);
    EXPECT_NEAR(0.25f, world.column[2].z, 1e-6f);
    bodies.orientation[0] = FromAxisAngle({ 1.f, 1.f, 1.f }, 1.234f);
    bodies.UpdateWorldInertia();
    EXPECT_NEAR(1.75f, world.column[0].x + world.column[1].y + world.column[2].z, 1e-5f) << "the trace is the sum of 1/I";
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row) EXPECT_NEAR(world.column[column][row], world.column[row][column], 1e-6f);
}

TEST(Bodies, AFullSetRefusesMoreAndCanBeCleared)
{
    Bodies bodies(1);
    bodies.Add({}, {}, 1.f, { 1.f, 1.f, 1.f });
    EXPECT_THROW(bodies.Add({}, {}, 1.f, { 1.f, 1.f, 1.f }), std::length_error);
    bodies.Clear();
    EXPECT_EQ(0, bodies.Add({ 5.f, 0.f, 0.f }, {}, 1.f, { 1.f, 1.f, 1.f })) << "the first place again";
}
