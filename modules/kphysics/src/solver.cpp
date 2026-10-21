#include <kphysics/simulation.h>

#include <algorithm>
#include <cmath>

namespace kphysics
{
    namespace
    {
        float Inverse(const float k) { return k > 0.f ? 1.f / k : 0.f; }
    }

    /**
     * Direction @p d of a point with arms @p armA and @p armB: r × d and I⁻¹ (r × d) for each body (zero for one nothing
     * turns), and 1 over how hard the point is to push along d: 1/mA + 1/mB + (rA × d) · I_A⁻¹ (rA × d) + the same for B.
     */
    ContactSolver::Direction ContactSolver::Prepared(const Simulation& sim, const int a, const int b, const Vec3 armA, const Vec3 armB, const Vec3 d)
    {
        Direction j;
        j.d = d;
        j.armA = Cross(armA, d);
        j.armB = Cross(armB, d);
        j.turnA = sim.bodies.inverseMass[a] == 0.f ? Vec3 {} : sim.bodies.inverseInertiaWorld[a] * j.armA;
        j.turnB = sim.bodies.inverseMass[b] == 0.f ? Vec3 {} : sim.bodies.inverseInertiaWorld[b] * j.armB;
        j.mass = Inverse(sim.bodies.inverseMass[a] + sim.bodies.inverseMass[b] + Dot(j.armA, j.turnA) + Dot(j.armB, j.turnB));
        return j;
    }

    /**
     * The point's velocity along the direction: d · (vB − vA) + ωB · (rB × d) − ωA · (rA × d). Summed term by term in
     * one chain, in the Kotlin core's order, so the two round alike and a run of either can be checked against the other's.
     */
    float ContactSolver::Along(const Pair& pair, const Direction& j)
    {
        const Vec3 va = pair.va, vb = pair.vb, wa = pair.wa, wb = pair.wb;
        return j.d.x * (vb.x - va.x) + j.d.y * (vb.y - va.y) + j.d.z * (vb.z - va.z) +
               wb.x * j.armB.x + wb.y * j.armB.y + wb.z * j.armB.z - wa.x * j.armA.x - wa.y * j.armA.y - wa.z * j.armA.z;
    }

    /** An impulse λ along the direction: −λ on A, +λ on B, linear and angular. */
    void ContactSolver::Push(Pair& pair, const Direction& j, const float lambda)
    {
        if (lambda == 0.f) return;
        pair.va -= j.d * (pair.ma * lambda);
        pair.vb += j.d * (pair.mb * lambda);
        pair.wa -= j.turnA * lambda;
        pair.wb += j.turnB * lambda;
    }

    ContactSolver::Pair::Pair(const std::vector<Vec3>& v, const std::vector<Vec3>& w, const std::vector<float>& inverseMass, const int a, const int b)
        : va(v[a]), vb(v[b]), wa(w[a]), wb(w[b]), ma(inverseMass[a]), mb(inverseMass[b])
    {
    }

    void ContactSolver::Pair::Store(std::vector<Vec3>& v, std::vector<Vec3>& w, const int a, const int b) const
    {
        // Only a body that moves: nothing changed a static or kinematic one's, and two threads solving manifolds of one
        // colour may share one of those (never one that moves: that is what a colour is).
        if (ma != 0.f) { v[a] = va; w[a] = wa; }
        if (mb != 0.f) { v[b] = vb; w[b] = wb; }
    }

    template <typename F>
    void ContactSolver::EachManifold(Simulation& sim, F&& solve)
    {
        const int count = sim.contacts.Count();
        if (!sim.ByColour()) {
            for (int m = 0; m < count; ++m) solve(m);
            return;
        }
        // A colour at a time, every manifold of it at once: no two of them share a body that moves.
        const Colouring& colouring = sim.colouring;
        ThreadPool* pool = sim.Pool();
        const int threads = sim.ThreadsFor(count);
        for (int k = 0; k < colouring.ColourCount(); ++k) {
            const int start = colouring.start[k], size = colouring.start[k + 1] - start;
            if (k == Colouring::Overflow || threads == 1) {
                for (int i = 0; i < size; ++i) solve(colouring.order[start + i]);
                continue;
            }
            pool->For(size, std::max(16, size / (threads * 4)), [&](const int begin, const int end) {
                for (int i = begin; i < end; ++i) solve(colouring.order[start + i]);
            }, threads);
        }
    }

    template <typename F>
    void ContactSolver::Each(Simulation& sim, const int count, F&& work)
    {
        const int threads = sim.ThreadsFor(count);
        if (threads == 1) {
            for (int i = 0; i < count; ++i) work(i);
            return;
        }
        sim.Pool()->For(count, std::max(32, count / (threads * 4)), [&](const int begin, const int end) {
            for (int i = begin; i < end; ++i) work(i);
        }, threads);
    }

    void ContactSolver::Prepare(Simulation& sim, const float h)
    {
        const Contacts& c = sim.contacts;
        const std::size_t manifolds = c.manifolds.size();
        if (_rows.size() < manifolds * 4) _rows.resize(std::max(manifolds * 4, _rows.size() * 2));
        if (_friction.size() < manifolds) _friction.resize(std::max(manifolds, _friction.size() * 2));
        if (static_cast<int>(_pseudoVelocity.size()) < sim.bodies.Capacity()) {
            _pseudoVelocity.assign(sim.bodies.Capacity(), {});
            _pseudoSpin.assign(sim.bodies.Capacity(), {});
        }
        const auto prepare = [&](const std::size_t m) {
            const Manifold& manifold = c.manifolds[m];
            const int a = manifold.a, b = manifold.b;
            _friction[m] = std::sqrt(sim.friction[a] * sim.friction[b]);
            const float restitution = std::max(sim.restitution[a], sim.restitution[b]);
            const Vec3 n = manifold.normal;
            // Two directions across the normal: the same ones for the same normal, so friction warm-starts.
            const Vec3 t1 = Perpendicular(n);
            const Vec3 t2 = Cross(n, t1);
            for (int p = 0; p < manifold.count; ++p) {
                Row& row = _rows[m * 4 + p];
                const Vec3 at = manifold.points[p].at;
                const Vec3 armA = at - sim.bodies.position[a], armB = at - sim.bodies.position[b];
                row.direction[0] = Prepared(sim, a, b, armA, armB, n);
                row.direction[1] = Prepared(sim, a, b, armA, armB, t1);
                row.direction[2] = Prepared(sim, a, b, armA, armB, t2);
                // What the normal velocity should end at.
                const float depth = manifold.points[p].depth;
                const float approach = Along(Pair(sim.bodies.velocity, sim.bodies.angularVelocity, sim.bodies.inverseMass, a, b), row.direction[0]);
                if (depth < 0.f) {
                    row.target = depth / h;                                 // speculative: close the gap, no more
                } else {
                    const float bounce = approach < -restitutionThreshold ? -restitution * approach : 0.f;
                    const float push = positionCorrection == PositionCorrection::Baumgarte
                                     ? std::min(baumgarte / h * std::max(depth - slop, 0.f), maxCorrection) : 0.f;
                    row.target = std::max(bounce, push);
                }
                row.pseudoTarget = positionCorrection == PositionCorrection::SplitImpulse && depth > 0.f
                                 ? std::min(baumgarte / h * std::max(depth - slop, 0.f), maxCorrection) : 0.f;
                row.pseudoImpulse = 0.f;
            }
        };
        // Each manifold's rows its own: in any order, on any thread.
        Each(sim, static_cast<int>(manifolds), [&](const int m) { prepare(static_cast<std::size_t>(m)); });
    }

    void ContactSolver::WarmStart(Simulation& sim)
    {
        auto& v = sim.bodies.velocity;
        auto& w = sim.bodies.angularVelocity;
        EachManifold(sim, [&](const int m) {
            const Manifold& manifold = sim.contacts.manifolds[m];
            Pair pair(v, w, sim.bodies.inverseMass, manifold.a, manifold.b);
            for (int p = 0; p < manifold.count; ++p) {
                const Row& row = _rows[m * 4 + p];
                const ContactPoint& point = manifold.points[p];
                Push(pair, row.direction[0], point.normalImpulse);
                Push(pair, row.direction[1], point.tangentImpulse1);
                Push(pair, row.direction[2], point.tangentImpulse2);
            }
            pair.Store(v, w, manifold.a, manifold.b);
        });
    }

    void ContactSolver::Solve(Simulation& sim)
    {
        auto& v = sim.bodies.velocity;
        auto& w = sim.bodies.angularVelocity;
        EachManifold(sim, [&](const int m) {
            Manifold& manifold = sim.contacts.manifolds[m];
            Pair pair(v, w, sim.bodies.inverseMass, manifold.a, manifold.b);
            for (int p = 0; p < manifold.count; ++p) {
                Row& row = _rows[m * 4 + p];
                ContactPoint& point = manifold.points[p];
                // Friction: the impulse across the normal that would stop the sliding, kept within a circle of μ times
                // the normal impulse (a cone, so sliding diagonally meets the same friction).
                const float old1 = point.tangentImpulse1, old2 = point.tangentImpulse2;
                float now1 = old1 - row.direction[1].mass * Along(pair, row.direction[1]);
                float now2 = old2 - row.direction[2].mass * Along(pair, row.direction[2]);
                const float most = _friction[m] * point.normalImpulse;
                const float size2 = now1 * now1 + now2 * now2;
                if (size2 > most * most) {
                    const float scale = most / std::sqrt(size2);
                    now1 *= scale;
                    now2 *= scale;
                }
                point.tangentImpulse1 = now1;
                point.tangentImpulse2 = now2;
                Push(pair, row.direction[1], now1 - old1);
                Push(pair, row.direction[2], now2 - old2);
                // The normal: push apart until the velocity along it reaches the target; never pull together.
                const float old = point.normalImpulse;
                const float now = std::max(0.f, old + row.direction[0].mass * (row.target - Along(pair, row.direction[0])));
                point.normalImpulse = now;
                Push(pair, row.direction[0], now - old);
            }
            pair.Store(v, w, manifold.a, manifold.b);
        });
    }

    void ContactSolver::SolvePositions(Simulation& sim)
    {
        if (positionCorrection != PositionCorrection::SplitImpulse) return;
        EachManifold(sim, [&](const int m) {
            const Manifold& manifold = sim.contacts.manifolds[m];
            Pair pair(_pseudoVelocity, _pseudoSpin, sim.bodies.inverseMass, manifold.a, manifold.b);
            for (int p = 0; p < manifold.count; ++p) {
                Row& row = _rows[m * 4 + p];
                if (row.pseudoTarget == 0.f && row.pseudoImpulse == 0.f) continue;
                const float vn = Along(pair, row.direction[0]);
                const float old = row.pseudoImpulse;
                const float now = std::max(0.f, old + row.direction[0].mass * (row.pseudoTarget - vn));
                row.pseudoImpulse = now;
                Push(pair, row.direction[0], now - old);
            }
            pair.Store(_pseudoVelocity, _pseudoSpin, manifold.a, manifold.b);
        });
    }

    void ContactSolver::MovePseudo(Simulation& sim, const float h)
    {
        if (positionCorrection != PositionCorrection::SplitImpulse) return;
        Bodies& bodies = sim.bodies;
        for (int b = 0; b < bodies.Count(); ++b) {
            const Vec3 v = _pseudoVelocity[b], w = _pseudoSpin[b];
            if (v == Vec3 {} && w == Vec3 {}) continue;
            bodies.position[b] += v * h;
            // q += h/2 (w, 0) q, as IntegratePositions does.
            const Quat q = bodies.orientation[b];
            const float half = h / 2;
            bodies.orientation[b] = Normalized({ q.x + half * (w.x * q.w + w.y * q.z - w.z * q.y),
                                                 q.y + half * (w.y * q.w + w.z * q.x - w.x * q.z),
                                                 q.z + half * (w.z * q.w + w.x * q.y - w.y * q.x),
                                                 q.w + half * (-w.x * q.x - w.y * q.y - w.z * q.z) });
            _pseudoVelocity[b] = {};
            _pseudoSpin[b] = {};
        }
    }

    void ContactSolver::AdvanceDepths(Simulation& sim, const float h)
    {
        Each(sim, sim.contacts.Count(), [&](const int m) {
            Manifold& manifold = sim.contacts.manifolds[m];
            const Pair pair(sim.bodies.velocity, sim.bodies.angularVelocity, sim.bodies.inverseMass, manifold.a, manifold.b);
            for (int p = 0; p < manifold.count; ++p) manifold.points[p].depth -= h * Along(pair, _rows[m * 4 + p].direction[0]);
        });
    }
}
