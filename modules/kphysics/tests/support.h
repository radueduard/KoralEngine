// What the kphysics tests share: scenes of real bodies, as the Kotlin core's tests make them (Worlds.kt, Scenes.kt).
#pragma once

#include <kphysics/kphysics.h>

#include <cstdint>
#include <functional>

namespace kphysics::test
{
    inline constexpr float Step = 1.f / 60.f;

    /** @brief The turn of @p degrees about @p axis. */
    Quat Turn(Vec3 axis, float degrees);

    /**
     * @brief Kotlin's kotlin.random.Random(seed): XorWow, as the Kotlin core's tests draw their scenes with, so a scene
     *        made here is the same scene, to the bit, as the one its reference run was recorded from.
     */
    class KotlinRandom {
    public:
        explicit KotlinRandom(std::int32_t seed);
        std::int32_t NextInt();
        /** @brief [0, 1): the top 24 bits. */
        float NextFloat();

    private:
        std::uint32_t _x, _y, _z, _w, _v, _addend;
    };

    /** @brief A static ground: the plane y = 0 facing up, or turned by @p turn. */
    int Ground(Simulation& sim, Quat turn = {}, float friction = 0.6f, float restitution = 0.f);
    struct BoxOptions {
        Vec3 half { 0.5f, 0.5f, 0.5f };
        Quat turn {};
        float density = 1000.f;
        float friction = 0.6f;
        float restitution = 0.f;
        bool fixed = false;
    };
    int AddBox(Simulation& sim, Vec3 at, const BoxOptions& options = {});
    int AddBall(Simulation& sim, Vec3 at, float radius = 0.5f, float density = 1000.f, float friction = 0.6f, float restitution = 0.f);
    int AddCapsule(Simulation& sim, Vec3 at, float radius = 0.25f, float half = 0.25f, Quat turn = {}, float density = 1000.f);
    /** @brief A pyramid of @p rows rows of unit boxes on the ground, in the XY plane. Returns its top box. */
    int Pyramid(Simulation& sim, int rows, float gap = 0.02f);
    /** @brief Steps @p sim for @p seconds at Step, calling @p each with the step's number after each. */
    void RunFor(Simulation& sim, float seconds, const std::function<void(int)>& each = {});
    float Speed(const Simulation& sim, int b);
    float Spin(const Simulation& sim, int b);
    /** @brief @p n boxes in a loose block over the ground, falling into a pile: MeasureTest's scene. */
    Simulation Falling(int n, std::int32_t seed = 1);

    /** @brief A few bodies of unit mass with colliders, for the narrowphase tests: made in a line, collided in one. */
    class Scene {
    public:
        Simulation sim { 8 };
        int AddSphere(Vec3 at, float radius, Quat turn = {});
        int AddBox(Vec3 at, Vec3 half, Quat turn = {});
        int AddCapsule(Vec3 at, float radius, float half, Quat turn = {});
        int AddPlane(Vec3 at, Quat turn = {});
        /** @brief The manifold between @p a and @p b through Collide, or none. */
        Contacts Collide(int a, int b, float margin = 0.02f) const;
    };
}
