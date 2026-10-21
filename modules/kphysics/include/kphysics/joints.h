//
// kphysics: joints, constraints between two bodies (or a body and the world), solved by sequential impulses in the
// same passes as contacts.
//
//   Ball      a point of A and a point of B stay together (3 rows)
//   Hinge     a ball, plus A's axis and B's stay aligned (2 more): a door, a wheel; with limits and a motor
//   Distance  a point of A and a point of B stay a length apart (1 row): a rod, or a rope, which only pulls
//
// A row at a time: the velocity the constraint allows is worked out, an impulse that makes it right applied, and the
// impulses summed over the passes and carried to the next step. After the positions move, position passes close what
// the joints have drifted directly (non-linear Gauss–Seidel), which a velocity pull cannot keep up with when a chain
// whips. A body of −1 is the world: fixed at the origin, with no mass and no rotation.
//

#pragma once

#include <kphysics/math.h>

#include <cstdint>
#include <vector>

namespace kphysics
{
    class Simulation;

    enum class JointKind { Ball, Hinge, Distance };

    struct Joint {
        JointKind kind = JointKind::Ball;
        int a = -1, b = -1;
        Vec3 anchorA, anchorB;                  ///< Where it is, in each body's own frame (the world's, for the world).
        Vec3 axisA, axisB, zeroA, zeroB;        ///< Hinge: the axis in each frame, and a direction across it marking angle 0.
        float length = 0.f;                     ///< Distance.
        bool rope = false;
        bool limited = false;                   ///< Hinge limits, radians: B's angle about the axis, relative to A.
        float lowerAngle = 0.f, upperAngle = 0.f;
        bool motor = false;
        float motorSpeed = 0.f, maxMotorTorque = 0.f;
        bool collideConnected = false;          ///< Usually not: a joint's bodies overlap where they meet.

        // The impulses so far, carried from step to step.
        Vec3 pointImpulse;
        float alignImpulse[2] {};
        float lowerImpulse = 0.f, upperImpulse = 0.f, motorImpulse = 0.f, lengthImpulse = 0.f;

        // Worked out each substep for the passes.
        Vec3 armA, armB;                        ///< From each body's centre to its anchor, in the world.
        Mat3 pointMass;                         ///< Ball: K, how the gap between the anchors moves for an impulse.
        Vec3 pointBias;
        Vec3 axis, across[2];                   ///< Hinge: A's axis in the world, and two directions across it.
        float alignBias[2] {}, alignMass[2] {};
        float axialMass = 0.f;
        float angle = 0.f;
        Vec3 direction;                         ///< Distance: from A's anchor to B's.
        float lengthMass = 0.f, lengthError = 0.f;
    };

    class Joints {
    public:
        std::vector<Joint> joints;
        /** @brief How hard a drifted joint is pulled back through its velocity: 0, since the position passes close it. */
        float baumgarte = 0.f;
        int positionIterations = 4;

        [[nodiscard]] int Count() const { return static_cast<int>(joints.size()); }
        /** @brief A ball joint between @p a and @p b (−1: the world) at the world point @p at, as the bodies are now. */
        int AddBall(const Simulation& sim, int a, int b, Vec3 at);
        /** @brief A hinge at @p at turning about the world direction @p axis; the bodies' pose now is angle 0. */
        int AddHinge(const Simulation& sim, int a, int b, Vec3 at, Vec3 axis);
        /** @brief A distance joint between @p onA on a and @p onB on b, as far apart as they are now. */
        int AddDistance(const Simulation& sim, int a, int b, Vec3 onA, Vec3 onB, bool rope = false);
        void SetLimits(int j, float lower, float upper);
        void SetMotor(int j, float speed, float maxTorque);
        void SetCollideConnected(int j, bool collide);
        void Clear() { joints.clear(); _excluded.clear(); }
        /** @brief Whether a and b are joined by a joint that keeps them from colliding. */
        [[nodiscard]] bool Excludes(int a, int b) const;
        /** @brief Hinge @p j's angle as the last step left it, in radians. */
        [[nodiscard]] float AngleOf(int j) const { return joints[j].angle; }
        /** @brief Where joint @p j holds each body, in the world, as the bodies are now. */
        void Anchors(const Simulation& sim, int j, Vec3& onA, Vec3& onB) const;
        /** @brief How far apart joint @p j's anchors are now: 0 for a ball or hinge holding exactly. */
        [[nodiscard]] float Gap(const Simulation& sim, int j) const;

        void Prepare(Simulation& sim, float h);
        void WarmStart(Simulation& sim);
        /** @brief One pass over every joint; without @p useBias, each only stops the drift (the relax passes). */
        void Solve(Simulation& sim, bool useBias = true);
        /** @brief Each joint's actual gap, closed by moving and turning its bodies directly. */
        void SolvePositions(Simulation& sim);

    private:
        int Next(JointKind kind, int a, int b);
        void RebuildExcluded();
        void SolvePoint(Simulation& sim, Joint& j) const;
        void SolveAlign(Simulation& sim, Joint& j) const;
        void SolveHingeAxial(Simulation& sim, Joint& j) const;
        void SolveDistance(Simulation& sim, Joint& j) const;
        void ClosePoint(Simulation& sim, Joint& j) const;
        void CloseAlignment(Simulation& sim, const Joint& j) const;
        void CloseLimit(Simulation& sim, const Joint& j) const;
        void CloseLength(Simulation& sim, Joint& j) const;

        std::vector<std::uint64_t> _excluded;   // the pairs that do not collide, in order
        float _h = 1.f / 60.f;
        float _bias = 1.f;
    };
}
