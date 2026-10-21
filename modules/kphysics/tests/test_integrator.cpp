// Integration and the step, as IntegratorTest.kt checks them.
#include "support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <utility>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    constexpr float Dt = 1.f / 60.f;
    constexpr float HalfPi = std::numbers::pi_v<float> / 2;

    Simulation One(const float mass = 1.f, const Vec3 inertia = { 0.1f, 0.1f, 0.1f }, const bool gravity = false)
    {
        Simulation sim(4);
        if (!gravity) sim.gravity = {};
        sim.Add({}, {}, mass, inertia);
        return sim;
    }

    void ExpectVec(const Vec3 expected, const Vec3 actual, const float tolerance = 1e-5f)
    {
        EXPECT_NEAR(expected.x, actual.x, tolerance);
        EXPECT_NEAR(expected.y, actual.y, tolerance);
        EXPECT_NEAR(expected.z, actual.z, tolerance);
    }

    /** Box half-extents (0.1, 0.5, 1): I_y between I_x and I_z, the T-handle's unstable middle axis. */
    Simulation THandle(const Gyroscopic gyroscopic, const Vec3 spin)
    {
        Vec3 inertia;
        const float mass = mass::Box(1000.f, { 0.1f, 0.5f, 1.f }, inertia);
        Simulation sim(1);
        sim.gravity = {};
        sim.gyroscopic = gyroscopic;
        sim.Add({}, {}, mass, inertia);
        sim.bodies.angularVelocity[0] = spin;
        return sim;
    }

    /** ½ ωᵀ I_w ω, and |I_w ω|. */
    std::pair<float, float> EnergyAndMomentum(const Simulation& sim)
    {
        const Vec3 local = sim.bodies.inverseInertiaLocal[0];
        const Mat3 iw = RotateDiagonal(FromQuat(sim.bodies.orientation[0]), { 1.f / local.x, 1.f / local.y, 1.f / local.z });
        const Vec3 l = iw * sim.bodies.angularVelocity[0];
        return { 0.5f * Dot(sim.bodies.angularVelocity[0], l), Length(l) };
    }
}

TEST(Integrator, AStaticBodyStaysPut)
{
    Simulation sim(1);
    sim.Add({ 1.f, 2.f, 3.f }, {}, 0.f, { 1.f, 1.f, 1.f });
    sim.AddForce(0, { 100.f, 0.f, 0.f });
    for (int i = 0; i < 60; ++i) sim.Step(Dt);
    EXPECT_EQ((Vec3 { 1.f, 2.f, 3.f }), sim.bodies.position[0]);
    EXPECT_EQ(Vec3 {}, sim.bodies.velocity[0]);
}

TEST(Integrator, AThrownBodyFallsAsSemiImplicitEulerSays)
{
    Simulation sim = One(1.f, { 0.1f, 0.1f, 0.1f }, true);
    sim.bodies.velocity[0] = { 2.f, 5.f, 0.f };
    const int n = 60;
    for (int i = 0; i < n; ++i) sim.Step(Dt);
    const float g = -9.81f;
    EXPECT_NEAR(2.f * n * Dt, sim.bodies.position[0].x, 1e-4f);
    EXPECT_NEAR(5.f * n * Dt + g * Dt * Dt * n * (n + 1) / 2, sim.bodies.position[0].y, 1e-4f) << "x_n = x_0 + v_0 n h + g h² n(n+1)/2";
    EXPECT_NEAR(5.f + g * n * Dt, sim.bodies.velocity[0].y, 1e-4f);
}

TEST(Integrator, GravityScaleAndForce)
{
    Simulation sim(2);
    sim.Add({}, {}, 4.f, { 1.f, 1.f, 1.f }, { .gravityScale = 0.f });
    sim.AddForce(0, { 2.f, 0.f, 0.f });
    sim.Step(0.1f);
    ExpectVec({ 0.05f, 0.f, 0.f }, sim.bodies.velocity[0]);
    ExpectVec({ 0.005f, 0.f, 0.f }, sim.bodies.position[0]);
    EXPECT_EQ(Vec3 {}, sim.bodies.force[0]) << "the force lasted one step";
    sim.Step(0.1f);
    ExpectVec({ 0.05f, 0.f, 0.f }, sim.bodies.velocity[0]);
}

TEST(Integrator, DampingSlowsByOneOverOnePlusHC)
{
    Simulation sim(2);
    sim.gravity = {};
    sim.Add({}, {}, 1.f, { 1.f, 1.f, 1.f }, { .linearDamping = 2.f });
    sim.bodies.velocity[0].x = 10.f;
    sim.Step(0.1f);
    EXPECT_NEAR(10.f / 1.2f, sim.bodies.velocity[0].x, 1e-4f);

    Simulation spinning(1);
    spinning.Add({}, {}, 1.f, { 1.f, 1.f, 1.f }, { .angularDamping = 1.f });
    spinning.bodies.angularVelocity[0].z = 6.f;
    spinning.Step(0.5f);
    EXPECT_NEAR(4.f, spinning.bodies.angularVelocity[0].z, 1e-4f);
}

TEST(Integrator, ATorqueTurnsByOverI)
{
    Simulation sim = One(1.f, { 0.5f, 2.f, 4.f });
    sim.AddTorque(0, { 0.f, 3.f, 0.f });
    sim.Step(0.1f);
    ExpectVec({ 0.f, 0.15f, 0.f }, sim.bodies.angularVelocity[0]);
}

TEST(Integrator, ASpinTurnsTheBodyAndItsInertia)
{
    Simulation sim = One(1.f, { 1.f, 2.f, 4.f });
    sim.bodies.angularVelocity[0].y = HalfPi;                      // a quarter turn a second, about Y
    for (int i = 0; i < 60; ++i) sim.Step(Dt);
    const Quat q = sim.bodies.orientation[0];
    EXPECT_NEAR(1.f, std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1e-5f);
    ExpectVec({ 0.f, 0.f, -1.f }, Rotate(q, { 1.f, 0.f, 0.f }), 1e-2f);
    // A quarter turn about Y: the world's X entry is 1 / I_zz.
    EXPECT_NEAR(0.25f, sim.bodies.inverseInertiaWorld[0].column[0].x, 2e-2f);
    EXPECT_NEAR(1.f, sim.bodies.inverseInertiaWorld[0].column[2].z, 2e-2f);
}

TEST(Integrator, SubstepsAreSmallerSteps)
{
    Simulation a = One(1.f, { 0.1f, 0.1f, 0.1f }, true), b = One(1.f, { 0.1f, 0.1f, 0.1f }, true);
    b.substeps = 4;
    for (Simulation* sim : { &a, &b }) {
        sim->bodies.velocity[0].x = 3.f;
        sim->bodies.angularVelocity[0].z = 2.f;
    }
    for (int i = 0; i < 40; ++i) a.Step(Dt / 4);
    for (int i = 0; i < 10; ++i) b.Step(Dt);
    ExpectVec(a.bodies.position[0], b.bodies.position[0]);
    EXPECT_NEAR(a.bodies.orientation[0].w, b.bodies.orientation[0].w, 1e-5f);
    EXPECT_NEAR(a.bodies.orientation[0].z, b.bodies.orientation[0].z, 1e-5f);
}

TEST(Integrator, AKinematicBodyMovesByItsVelocityAlone)
{
    Simulation sim(1);
    sim.Add({}, {}, 0.f, { 1.f, 1.f, 1.f });
    sim.bodies.velocity[0].x = 1.f;
    for (int i = 0; i < 30; ++i) sim.Step(Dt);
    EXPECT_NEAR(0.5f, sim.bodies.position[0].x, 1e-4f);
    EXPECT_EQ(0.f, sim.bodies.position[0].y);
    EXPECT_EQ((Vec3 { 1.f, 0.f, 0.f }), sim.bodies.velocity[0]);
}

TEST(Integrator, ASpinAboutAPrincipalAxisIsSteady)
{
    for (const Gyroscopic gyroscopic : { Gyroscopic::None, Gyroscopic::Explicit, Gyroscopic::Implicit }) {
        Simulation sim = THandle(gyroscopic, { 0.f, 0.f, 5.f });
        for (int i = 0; i < 600; ++i) sim.Step(Dt);
        ExpectVec({ 0.f, 0.f, 5.f }, sim.bodies.angularVelocity[0], 1e-3f);
    }
}

TEST(Integrator, WithNoGyroscopicTermTheSpinNeverChanges)
{
    Simulation sim = THandle(Gyroscopic::None, { 0.05f, 5.f, 0.05f });
    for (int i = 0; i < 600; ++i) sim.Step(Dt);
    ExpectVec({ 0.05f, 5.f, 0.05f }, sim.bodies.angularVelocity[0], 1e-4f);
}

TEST(Integrator, TheImplicitTHandleFlipsAndGainsNoEnergy)
{
    Simulation sim = THandle(Gyroscopic::Implicit, { 0.05f, 5.f, 0.05f });
    const auto [e0, l0] = EnergyAndMomentum(sim);
    float lowest = 1.f;
    for (int i = 0; i < 600; ++i) {
        sim.Step(Dt);
        lowest = std::min(lowest, Rotate(sim.bodies.orientation[0], { 0.f, 1.f, 0.f }).y);
    }
    const auto [e1, l1] = EnergyAndMomentum(sim);
    EXPECT_LT(lowest, -0.9f) << "the body's Y axis turned over: the Dzhanibekov flip";
    EXPECT_LE(e1, e0 * 1.01f) << "no energy gained";
    EXPECT_LE(l1, l0 * 1.01f) << "no angular momentum gained";
    EXPECT_GE(l1, l0 * 0.75f) << "and not much lost";
}

TEST(Integrator, TheExplicitTermTurnsTheSpinToo)
{
    Simulation sim = THandle(Gyroscopic::Explicit, { 0.05f, 5.f, 0.05f });
    for (int i = 0; i < 60; ++i) sim.Step(Dt);
    const Vec3 w = sim.bodies.angularVelocity[0];
    EXPECT_TRUE(std::abs(w.x - 0.05f) > 1e-3f || std::abs(w.z - 0.05f) > 1e-3f);
}
