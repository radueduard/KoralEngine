//
// kphysics: the bodies' state, one array per quantity, indexed by body: never one object per body. That is what the GPU
// version uploads into storage buffers, and what lets a loop over thousands of bodies allocate nothing. And how much
// a shape weighs and how hard it is to turn, from its size and density.
//

#pragma once

#include <kphysics/math.h>

#include <vector>

namespace kphysics
{
    /**
     * @brief Mass properties: each returns the mass and writes the inertia tensor's diagonal (Ixx, Iyy, Izz) about the
     *        centre of mass, in kg·m² for a density in kg/m³ and sizes in metres. The shapes are symmetric about their
     *        own axes, so in the body's frame the tensor is diagonal.
     */
    namespace mass
    {
        /** @brief A solid sphere: m = ρ · 4/3 π r³, I = 2/5 m r² about every axis. */
        float Sphere(float density, float radius, Vec3& inertia);
        /** @brief A solid box of half-extents h: m = ρ · 8 hx hy hz, Ixx = m/3 (hy² + hz²), and so on. */
        float Box(float density, Vec3 half, Vec3& inertia);
        /**
         * @brief A solid capsule along Y: a cylinder of @p radius from −halfSegment to +halfSegment, capped by two half
         *        spheres. The engine's capsule mesh is given an overall height: halfSegment = height/2 − radius.
         */
        float Capsule(float density, float radius, float halfSegment, Vec3& inertia);
        /**
         * @brief A solid cone along Y of @p radius and @p height, about its centre of mass (a quarter of its height above
         *        its base): m = ρ π r² h / 3, Iyy = 3/10 m r², Ixx = Izz = 3/20 m r² + 3/80 m h².
         */
        float Cone(float density, float radius, float height, Vec3& inertia);
    }

    class Bodies {
    public:
        explicit Bodies(int capacity);

        [[nodiscard]] int Capacity() const { return _capacity; }
        /** @brief How many bodies there are; they are 0 until Count. */
        [[nodiscard]] int Count() const { return _count; }

        std::vector<Vec3> position;
        std::vector<Quat> orientation;
        std::vector<Vec3> velocity;              ///< m/s.
        std::vector<Vec3> angularVelocity;       ///< In world space, rad/s.
        std::vector<Vec3> force, torque;         ///< Applied this step, cleared after it.
        std::vector<float> inverseMass;          ///< 0 for a body nothing moves.
        std::vector<Vec3> inverseInertiaLocal;   ///< The inverse of the inertia's diagonal, in the body's own frame.
        std::vector<Mat3> inverseInertiaWorld;   ///< R · diag(inverseInertiaLocal) · Rᵀ, kept by UpdateWorldInertia.

        /**
         * @brief Adds a body at @p at, turned by @p turn (made length 1), still, and returns its index. A @p mass of 0 is
         *        static: its inverse mass and inertia are 0, whatever @p inertia says. Throws std::length_error when full.
         */
        int Add(Vec3 at, Quat turn, float mass, Vec3 inertia);
        /** @brief Takes every body away, keeping the arrays. */
        void Clear() { _count = 0; }
        /** @brief Every body's inverseInertiaWorld from its orientation: once a step, after the orientations change. */
        void UpdateWorldInertia();

    private:
        int _capacity = 0;
        int _count = 0;
    };
}
