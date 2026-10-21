//
// kphysics: the contact solver, sequential impulses (Catto, GDC 2005–2006, and Box2D since).
//
// For each contact point, three rows: along the normal the bodies may separate but not approach (an impulse ≥ 0), and
// along two directions across it friction holds them, up to μ times the normal impulse (a cone). One pass fixes each
// point in turn against the others' last impulses; ten converge well enough. Each point keeps the sum of its impulses,
// and it is the sum that is clamped, which lets a later pass take back part of an earlier one.
//
// Each substep: Prepare (each point's arms, effective masses, target velocity), WarmStart (last step's impulses,
// carried by feature id), Solve (the passes: friction, then the normal); after the positions move, position correction
// and the depths advanced for the next substep.
//
// Penetration is corrected by split impulse (pseudo velocities that push apart, then are thrown away: no energy) or by
// Baumgarte (β/h · depth in the target velocity: simple, but it bounces). Points not yet touching are speculative: the
// bodies may approach by exactly the gap this step, and no more.
//

#pragma once

#include <kphysics/math.h>

#include <vector>

namespace kphysics
{
    class Simulation;

    enum class PositionCorrection { Baumgarte, SplitImpulse };

    class ContactSolver {
    public:
        int iterations = 10;
        int positionIterations = 4;
        PositionCorrection positionCorrection = PositionCorrection::SplitImpulse;
        float baumgarte = 0.2f;                 ///< β: the share of the remaining depth corrected each step.
        float slop = 0.005f;                    ///< Depth left alone, so resting contacts stay in touch.
        float restitutionThreshold = 1.f;       ///< Slower approaches than this do not bounce.
        float maxCorrection = 3.f;              ///< The fastest a correction may push bodies apart, m/s.
        /// Solve the manifolds in colour order, as a GPU must; with threads (Simulation::threads), always so, so that
        /// the result does not depend on how many there are.
        bool byColour = false;

        void Prepare(Simulation& sim, float h);
        void WarmStart(Simulation& sim);
        void Solve(Simulation& sim);
        /** @brief One pass over the pseudo velocities: pushing overlapping bodies apart, along the normal only. */
        void SolvePositions(Simulation& sim);
        /** @brief Moves each body by its pseudo velocities for this substep, then forgets them. */
        void MovePseudo(Simulation& sim, float h);
        /** @brief Each point's depth less how far the bodies moved apart at it this substep. */
        void AdvanceDepths(Simulation& sim, float h);

    private:
        /**
         * One direction d of a point, worked out once in Prepare: rA × d, rB × d, and each through its body's inverse
         * inertia. A pass then needs no cross product and no matrix: the point's velocity along d is
         * d · (vB − vA) + ωB · (rB × d) − ωA · (rA × d).
         */
        struct Direction {
            Vec3 d, armA, armB, turnA, turnB;
            float mass = 0.f;                   ///< 1 / (how hard the point is to push along d).
        };
        /** A point's three directions (the normal, then the two across it) and its targets. */
        struct Row {
            Direction direction[3];
            float target = 0.f;                 ///< The normal's target velocity: restitution, speculative or Baumgarte.
            float pseudoTarget = 0.f;
            float pseudoImpulse = 0.f;
        };

        /**
         * A manifold's two bodies as its points are solved: their velocities read once, worked on in registers by every
         * row of every point, and written back once, rather than read and written through the arrays a row at a time.
         */
        struct Pair {
            Vec3 va, vb, wa, wb;
            float ma = 0.f, mb = 0.f;
            Pair(const std::vector<Vec3>& v, const std::vector<Vec3>& w, const std::vector<float>& inverseMass, int a, int b);
            void Store(std::vector<Vec3>& v, std::vector<Vec3>& w, int a, int b) const;
        };

        static Direction Prepared(const Simulation& sim, int a, int b, Vec3 armA, Vec3 armB, Vec3 d);
        [[nodiscard]] static float Along(const Pair& pair, const Direction& j);
        /** Each manifold, in its order; or, solving by colour, a colour at a time, each colour's on every thread at once. */
        template <typename F> static void EachManifold(Simulation& sim, F&& solve);
        /** Work on [0, count) whose parts touch nothing in common: on every thread at once, when there are threads. */
        template <typename F> static void Each(Simulation& sim, int count, F&& work);
        static void Push(Pair& pair, const Direction& j, float lambda);

        std::vector<Row> _rows;                 // manifold m's point p at m · 4 + p
        std::vector<float> _friction;           // each manifold's
        std::vector<Vec3> _pseudoVelocity, _pseudoSpin;
    };
}
