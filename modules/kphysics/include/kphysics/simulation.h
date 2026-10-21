//
// kphysics: a simulation, the bodies, their colliders and joints, what acts on them, and the step that moves them all.
//
// The step, in order:
//   1. broadphase     each body's box, and the pairs whose boxes overlap
//   2. narrowphase    each pair's manifold, last step's impulses carried over to the points found again
//   then, in each substep of h = dt / substeps:
//   3. velocities     gravity, forces, torques, damping, the gyroscopic term
//   4. constraints    joints and contacts: prepared, warm-started, then solved in passes
//   5. positions      from the new velocities; split impulse pushes overlapping bodies apart; joints' drift closed
//   then, once:
//   6. sleep          islands, and which of them go to sleep or wake
//
// It depends on nothing, as kecs depends on nothing: the engine's PhysicsSimulation script copies the ECS's bodies in and
// their positions and orientations back out.
//

#pragma once

#include <kphysics/bodies.h>
#include <kphysics/broadphase.h>
#include <kphysics/collision.h>
#include <kphysics/islands.h>
#include <kphysics/joints.h>
#include <kphysics/solver.h>
#include <kphysics/thread_pool.h>

#include <memory>

#include <string>
#include <vector>

namespace kphysics
{
    /** @brief What is done about the gyroscopic torque −ω × (I ω). */
    enum class Gyroscopic { None, Explicit, Implicit };

    /** @brief How long each phase of the last step took, in milliseconds. */
    struct Timing {
        float broadphase = 0.f, narrowphase = 0.f, integrate = 0.f, solve = 0.f, sleep = 0.f, step = 0.f;
        [[nodiscard]] std::string ToString() const;
    };

    /** @brief What a body is besides its mass and shape. */
    struct Material {
        float linearDamping = 0.f;      ///< Each step the velocity is divided by 1 + h · damping.
        float angularDamping = 0.f;
        float gravityScale = 1.f;       ///< How much of the gravity pulls it.
        float friction = 0.6f;          ///< A pair takes √(μa μb),
        float restitution = 0.f;        ///< and the larger restitution.
        bool kinematic = false;         ///< Moved from outside by its velocity: pushes, is never pushed.
    };

    class Simulation {
    public:
        explicit Simulation(int capacity);

        [[nodiscard]] int Capacity() const { return _capacity; }

        Bodies bodies;
        Vec3 gravity { 0.f, -9.81f, 0.f };
        int substeps = 1;
        Gyroscopic gyroscopic = Gyroscopic::None;

        // What each body has besides Bodies' arrays, indexed the same way.
        std::vector<float> linearDamping, angularDamping, gravityScale, friction, restitution;
        std::vector<char> kinematic;
        std::vector<char> awake;                ///< Whether the body is awake,
        std::vector<float> stillTime;           ///< and how long it has been nearly still.

        Colliders colliders;
        /** @brief What the last step's narrowphase found, with the impulses the solver ended with. */
        Contacts contacts;
        /** @brief Pairs closer than this (metres) are in contact already, with a negative depth. */
        float contactMargin = 0.02f;

        Broadphase broadphase;
        ContactSolver solver;
        Joints joints;
        Islands islands;
        Colouring colouring;
        Timing timing;
        /** @brief Joint passes after the positions move, without the pull back together: 0 while joints.baumgarte is 0. */
        int relaxIterations = 0;
        /**
         * @brief How many threads find and solve the contacts: 1, the caller alone; n, n of them; 0, as many as the work
         *        is worth, a thread for each WorkPerThread manifolds (or pairs, for the narrowphase), up to as many as the
         *        machine has cores, and 8. Anything but 1 solves by colour (see ContactSolver::byColour), so a run is the
         *        same, to the bit, however many threads there are, and whichever machine it runs on.
         */
        int threads = 1;
        /** @brief With threads 0: how much work is worth a thread. Below it, handing work over costs more than it saves. */
        static constexpr int WorkPerThread = 500;
        /** @brief How many threads to put on @p work manifolds or pairs, as threads says. */
        [[nodiscard]] int ThreadsFor(int work) const;
        /** @brief Whether the contacts are solved a colour at a time. */
        [[nodiscard]] bool ByColour() const { return solver.byColour || threads != 1; }
        /** @brief The threads, while there are more than one. */
        [[nodiscard]] ThreadPool* Pool() const { return _pool.get(); }

        /**
         * @brief Adds a body as Bodies::Add does, with its material. A @p mass of 0 is static, or kinematic when the
         *        material says so. Add its collider next, in the same order.
         */
        int Add(Vec3 at, Quat turn, float mass, Vec3 inertia, const Material& material = {});
        void Clear();
        /** @brief Pushes body @p b through its centre of mass, for the next step only. Wakes it. */
        void AddForce(int b, Vec3 force);
        /** @brief Turns body @p b (a world-space torque), for the next step only. Wakes it. */
        void AddTorque(int b, Vec3 torque);
        /** @brief Wakes body @p b; its island wakes with it at the end of the next step. */
        void Wake(int b) { awake[b] = 1; stillTime[b] = 0.f; }
        /** @brief Whether body @p b takes part: a kinematic body always, a dynamic one while awake, a static one never. */
        [[nodiscard]] bool IsActive(const int b) const { return kinematic[b] || (bodies.inverseMass[b] != 0.f && awake[b]); }

        void Step(float dt);
        /** @brief Every pair that touches, into contacts, in pair order; points also found last step keep their impulses. */
        void FindContacts();
        /** @brief How many dynamic bodies are asleep. */
        [[nodiscard]] int SleepingCount() const;

    private:
        void IntegrateVelocities(float h);
        void IntegratePositions(float h);
        void GyroscopicExplicit(int b, float h);
        void GyroscopicImplicit(int b, float h);

        int _capacity = 0;
        Contacts _previous;
        std::unique_ptr<ThreadPool> _pool;
        std::vector<Contacts> _chunks;          // the narrowphase's, a chunk of pairs each, on threads
        int _poolFor = 1;                       // the threads setting the pool was made for
        double _broadphaseDone = 0.0;
        float _sweep = 0.f;                     // the step's time, as far ahead as contacts with meshes are looked for
    };
}
