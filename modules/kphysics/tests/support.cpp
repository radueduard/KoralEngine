#include "support.h"

#include <cmath>
#include <numbers>

namespace kphysics::test
{
    Quat Turn(const Vec3 axis, const float degrees)
    {
        // As the Kotlin tests: the angle worked out in double, then made a float.
        return FromAxisAngle(axis, static_cast<float>(degrees * std::numbers::pi / 180));
    }

    KotlinRandom::KotlinRandom(const std::int32_t seed)
    {
        // Random(seed) is XorWowRandom(seed, seed shr 31): x, y, z, w, v = seed1, seed2, 0, 0, ¬seed1, and the addend
        // (seed1 shl 10) xor (seed2 ushr 4); then 64 numbers thrown away.
        const auto seed1 = static_cast<std::uint32_t>(seed);
        const auto seed2 = static_cast<std::uint32_t>(seed >> 31);
        _x = seed1;
        _y = seed2;
        _z = 0;
        _w = 0;
        _v = ~seed1;
        _addend = (seed1 << 10) ^ (seed2 >> 4);
        for (int i = 0; i < 64; ++i) NextInt();
    }

    std::int32_t KotlinRandom::NextInt()
    {
        std::uint32_t t = _x;
        t ^= t >> 2;
        _x = _y;
        _y = _z;
        _z = _w;
        const std::uint32_t v0 = _v;
        _w = v0;
        t = (t ^ (t << 1)) ^ v0 ^ (v0 << 4);
        _v = t;
        _addend += 362437u;
        return static_cast<std::int32_t>(t + _addend);
    }

    float KotlinRandom::NextFloat()
    {
        const std::uint32_t bits = static_cast<std::uint32_t>(NextInt()) >> 8;      // the upper 24 bits
        return static_cast<float>(bits) / static_cast<float>(1 << 24);
    }

    int Ground(Simulation& sim, const Quat turn, const float friction, const float restitution)
    {
        const int b = sim.Add({}, turn, 0.f, { 1.f, 1.f, 1.f }, { .friction = friction, .restitution = restitution });
        sim.colliders.Add(Plane, {});
        return b;
    }

    int AddBox(Simulation& sim, const Vec3 at, const BoxOptions& options)
    {
        Vec3 inertia;
        const float mass = options.fixed ? 0.f : mass::Box(options.density, options.half, inertia);
        const int b = sim.Add(at, options.turn, mass, inertia, { .friction = options.friction, .restitution = options.restitution });
        sim.colliders.Add(Box, options.half);
        return b;
    }

    int AddBall(Simulation& sim, const Vec3 at, const float radius, const float density, const float friction, const float restitution)
    {
        Vec3 inertia;
        const float mass = mass::Sphere(density, radius, inertia);
        const int b = sim.Add(at, {}, mass, inertia, { .friction = friction, .restitution = restitution });
        sim.colliders.Add(Sphere, { radius, 0.f, 0.f });
        return b;
    }

    int AddCapsule(Simulation& sim, const Vec3 at, const float radius, const float half, const Quat turn, const float density)
    {
        Vec3 inertia;
        const float mass = mass::Capsule(density, radius, half, inertia);
        const int b = sim.Add(at, turn, mass, inertia);
        sim.colliders.Add(Capsule, { radius, half, 0.f });
        return b;
    }

    int Pyramid(Simulation& sim, const int rows, const float gap)
    {
        int top = -1;
        for (int row = 0; row < rows; ++row) {
            const int count = rows - row;
            for (int i = 0; i < count; ++i)
                top = AddBox(sim, { (static_cast<float>(i) - static_cast<float>(count - 1) / 2.f) * (1.f + gap), 0.5f + static_cast<float>(row) * 1.f, 0.f });
        }
        return top;
    }

    void RunFor(Simulation& sim, const float seconds, const std::function<void(int)>& each)
    {
        const int steps = static_cast<int>(seconds / Step);
        for (int k = 0; k < steps; ++k) {
            sim.Step(Step);
            if (each) each(k);
        }
    }

    float Speed(const Simulation& sim, const int b) { return Length(sim.bodies.velocity[b]); }
    float Spin(const Simulation& sim, const int b) { return Length(sim.bodies.angularVelocity[b]); }

    Simulation Falling(const int n, const std::int32_t seed)
    {
        Simulation sim(n + 1);
        sim.substeps = 2;
        sim.broadphase.kind = BroadphaseKind::Grid;
        Ground(sim);
        const int side = static_cast<int>(std::ceil(std::sqrt(n / 10.0)));          // ten layers
        KotlinRandom random(seed);
        for (int i = 0; i < n; ++i) {
            const int layer = i / (side * side), row = (i % (side * side)) / side, column = i % side;
            // In the Kotlin call's order: the axis and angle are drawn as its arguments are, left to right.
            const float ax = random.NextFloat(), ay = random.NextFloat(), az = random.NextFloat(), degrees = random.NextFloat() * 20.f;
            AddBox(sim, { (static_cast<float>(column) - static_cast<float>(side) / 2.f) * 1.1f, 0.6f + static_cast<float>(layer) * 1.15f,
                          (static_cast<float>(row) - static_cast<float>(side) / 2.f) * 1.1f },
                   { .turn = Turn({ ax, ay, az }, degrees) });
        }
        return sim;
    }

    int Scene::AddSphere(const Vec3 at, const float radius, const Quat turn)
    {
        const int b = sim.Add(at, turn, 1.f, { 1.f, 1.f, 1.f });
        sim.colliders.Add(Sphere, { radius, 0.f, 0.f });
        return b;
    }

    int Scene::AddBox(const Vec3 at, const Vec3 half, const Quat turn)
    {
        const int b = sim.Add(at, turn, 1.f, { 1.f, 1.f, 1.f });
        sim.colliders.Add(Box, half);
        return b;
    }

    int Scene::AddCapsule(const Vec3 at, const float radius, const float half, const Quat turn)
    {
        const int b = sim.Add(at, turn, 1.f, { 1.f, 1.f, 1.f });
        sim.colliders.Add(Capsule, { radius, half, 0.f });
        return b;
    }

    int Scene::AddPlane(const Vec3 at, const Quat turn)
    {
        const int b = sim.Add(at, turn, 0.f, { 1.f, 1.f, 1.f });
        sim.colliders.Add(Plane, {});
        return b;
    }

    Contacts Scene::Collide(const int a, const int b, const float margin) const
    {
        Contacts out;
        kphysics::Collide(sim.bodies, sim.colliders, a, b, margin, out);
        return out;
    }
}
