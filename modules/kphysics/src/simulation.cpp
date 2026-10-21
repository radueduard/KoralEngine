#include <kphysics/simulation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace kphysics
{
    namespace
    {
        double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    }

    std::string Timing::ToString() const
    {
        char text[160];
        std::snprintf(text, sizeof text, "step %.3f ms: broadphase %.3f, narrowphase %.3f, integrate %.3f, solve %.3f, sleep %.3f",
                      step, broadphase, narrowphase, integrate, solve, sleep);
        return text;
    }

    Simulation::Simulation(const int capacity)
        : bodies(capacity), linearDamping(capacity), angularDamping(capacity), gravityScale(capacity), friction(capacity),
          restitution(capacity), kinematic(capacity), awake(capacity), stillTime(capacity), colliders(capacity),
          broadphase(capacity), islands(capacity), _capacity(capacity)
    {
    }

    int Simulation::Add(const Vec3 at, const Quat turn, const float mass, const Vec3 inertia, const Material& material)
    {
        const int b = bodies.Add(at, turn, mass, inertia);
        linearDamping[b] = material.linearDamping;
        angularDamping[b] = material.angularDamping;
        gravityScale[b] = material.gravityScale;
        friction[b] = material.friction;
        restitution[b] = material.restitution;
        kinematic[b] = material.kinematic;
        awake[b] = 1;
        stillTime[b] = 0.f;
        return b;
    }

    void Simulation::Clear()
    {
        bodies.Clear();
        colliders.Clear();
        contacts.Clear();
        _previous.Clear();
        joints.Clear();
    }

    void Simulation::AddForce(const int b, const Vec3 force)
    {
        bodies.force[b] += force;
        Wake(b);
    }

    void Simulation::AddTorque(const int b, const Vec3 torque)
    {
        bodies.torque[b] += torque;
        Wake(b);
    }

    void Simulation::Step(const float dt)
    {
        if (threads != _poolFor) {
            _pool = threads == 1 ? nullptr : std::make_unique<ThreadPool>(threads);
            if (_pool && _pool->Size() == 1) _pool.reset();     // a machine of one core
            _poolFor = threads;
        }
        const double start = Now();
        _sweep = dt;
        FindContacts();
        const double found = Now();
        if (ByColour()) colouring.Colour(*this);

        const float h = dt / static_cast<float>(substeps);
        double integrating = 0.0;
        for (int substep = 0; substep < substeps; ++substep) {
            // Prepared before gravity is added, so a bounce answers the speed the bodies met with, not one step more.
            joints.Prepare(*this, h);
            solver.Prepare(*this, h);
            const double t0 = Now();
            IntegrateVelocities(h);
            const double t1 = Now();
            joints.WarmStart(*this);
            solver.WarmStart(*this);
            for (int pass = 0; pass < solver.iterations; ++pass) {
                joints.Solve(*this);
                solver.Solve(*this);
            }
            for (int pass = 0; pass < solver.positionIterations; ++pass) solver.SolvePositions(*this);
            const double t2 = Now();
            IntegratePositions(h);
            // Relax: the joints again without their pull, taking out the velocity it added (Box2D v3's soft step).
            for (int pass = 0; pass < relaxIterations; ++pass) joints.Solve(*this, false);
            // Then their drift closed directly.
            if (joints.Count() > 0) {
                bodies.UpdateWorldInertia();
                joints.SolvePositions(*this);
            }
            solver.MovePseudo(*this, h);
            solver.AdvanceDepths(*this, h);
            bodies.UpdateWorldInertia();
            integrating += (t1 - t0) + (Now() - t2);
        }
        const double solved = Now();
        islands.Update(*this, dt);
        std::fill_n(bodies.force.begin(), bodies.Count(), Vec3 {});
        std::fill_n(bodies.torque.begin(), bodies.Count(), Vec3 {});
        const double end = Now();

        timing.broadphase = static_cast<float>(_broadphaseDone - start);
        timing.narrowphase = static_cast<float>(found - _broadphaseDone);
        timing.integrate = static_cast<float>(integrating);
        timing.solve = static_cast<float>((solved - found) - integrating);
        timing.sleep = static_cast<float>(end - solved);
        timing.step = static_cast<float>(end - start);
    }

    void Simulation::FindContacts()
    {
        broadphase.Update(*this);
        _broadphaseDone = Now();
        std::swap(_previous, contacts);
        contacts.Clear();
        const auto& pairs = broadphase.pairs;
        const int count = static_cast<int>(pairs.size());
        const int workers = ThreadsFor(count);
        if (workers == 1) {
            for (const std::uint64_t key : pairs) Collide(bodies, colliders, LowerOf(key), HigherOf(key), contactMargin, contacts, _sweep);
        } else {
            // The pairs in chunks of a fixed size, each collided into a list of its own by whichever thread takes it, the
            // lists then joined in chunk order: the same manifolds in the same order as one thread makes them.
            const int grain = std::max(64, count / (workers * 8));
            const int chunks = (count + grain - 1) / grain;
            if (static_cast<int>(_chunks.size()) < chunks) _chunks.resize(chunks);
            _pool->For(count, grain, [&](const int begin, const int end) {
                Contacts& out = _chunks[begin / grain];
                out.Clear();
                for (int i = begin; i < end; ++i) Collide(bodies, colliders, LowerOf(pairs[i]), HigherOf(pairs[i]), contactMargin, out, _sweep);
            }, workers);
            for (int c = 0; c < chunks; ++c) contacts.manifolds.insert(contacts.manifolds.end(), _chunks[c].manifolds.begin(), _chunks[c].manifolds.end());
        }
        contacts.CarryImpulsesFrom(_previous);
    }

    int Simulation::ThreadsFor(const int work) const
    {
        if (!_pool) return 1;
        if (threads != 0) return _pool->Size();
        return std::clamp(work / WorkPerThread, 1, _pool->Size());
    }

    int Simulation::SleepingCount() const
    {
        int n = 0;
        for (int b = 0; b < bodies.Count(); ++b)
            if (bodies.inverseMass[b] != 0.f && !awake[b]) ++n;
        return n;
    }

    // ---- integration: semi-implicit Euler --------------------------------------------------------------------------
    //
    //   v ← v + h (g · gravityScale + F / m), then v ← v / (1 + h · linearDamping)
    //   ω ← ω + h I_w⁻¹ τ, the gyroscopic term, then ω ← ω / (1 + h · angularDamping)
    //   x ← x + h v
    //   q ← normalize(q + h/2 · (ω, 0) · q)
    //
    // Velocities first, then positions from the new velocities: what keeps orbits and springs from gaining energy, as
    // explicit Euler does. Static and kinematic bodies are not accelerated; asleep ones are left as they are.

    void Simulation::IntegrateVelocities(const float h)
    {
        for (int b = 0; b < bodies.Count(); ++b) {
            if (bodies.inverseMass[b] == 0.f || !awake[b]) continue;
            const Vec3 acceleration = gravity * gravityScale[b] + bodies.force[b] * bodies.inverseMass[b];
            bodies.velocity[b] = (bodies.velocity[b] + acceleration * h) * (1.f / (1.f + h * linearDamping[b]));
            bodies.angularVelocity[b] += (bodies.inverseInertiaWorld[b] * bodies.torque[b]) * h;
            switch (gyroscopic) {
            case Gyroscopic::None: break;
            case Gyroscopic::Explicit: GyroscopicExplicit(b, h); break;
            case Gyroscopic::Implicit: GyroscopicImplicit(b, h); break;
            }
            bodies.angularVelocity[b] *= 1.f / (1.f + h * angularDamping[b]);
        }
    }

    /**
     * Explicit: ω += h · I_w⁻¹ · (−ω × (I_w ω)). Applied on its own it adds energy: a body spinning about its middle axis
     * spins faster and faster. That is why most engines leave it out, and why the implicit form exists.
     */
    void Simulation::GyroscopicExplicit(const int b, const float h)
    {
        const Vec3 local = bodies.inverseInertiaLocal[b];
        const Mat3 inertia = RotateDiagonal(FromQuat(bodies.orientation[b]), { 1.f / local.x, 1.f / local.y, 1.f / local.z });
        const Vec3 w = bodies.angularVelocity[b];
        bodies.angularVelocity[b] = w + (bodies.inverseInertiaWorld[b] * Cross(w, inertia * w)) * -h;
    }

    /**
     * Implicit (Catto, "Numerical Methods", GDC 2015): one Newton step on the gyroscopic equation, in the body's frame
     * where the inertia is the diagonal I_b:
     *   ω_b = Rᵀ ω, f = h · ω_b × (I_b ω_b), J = I_b + h · (skew(ω_b) · I_b − skew(I_b ω_b)), ω_b ← ω_b − J⁻¹ f, ω = R ω_b.
     * It adds no energy, and a T-handle flips over and back as the real one does.
     */
    void Simulation::GyroscopicImplicit(const int b, const float h)
    {
        const Vec3 local = bodies.inverseInertiaLocal[b];
        const float ix = 1.f / local.x, iy = 1.f / local.y, iz = 1.f / local.z;
        const Quat q = bodies.orientation[b];
        const Vec3 w = Rotate(Conjugate(q), bodies.angularVelocity[b]);
        const Vec3 l { ix * w.x, iy * w.y, iz * w.z };
        const Vec3 f = Cross(w, l) * h;
        // skew(a)'s columns are (0, a.z, −a.y), (−a.z, 0, a.x), (a.y, −a.x, 0); skew(ω_b) · I_b scales them by ix, iy, iz.
        const Mat3 j { { { ix, h * (w.z * ix - l.z), h * (-w.y * ix + l.y) },
                         { h * (-w.z * iy + l.z), iy, h * (w.x * iy - l.x) },
                         { h * (w.y * iz - l.y), h * (-w.x * iz + l.x), iz } } };
        Vec3 change;
        if (!Solve3(j, f, change)) return;
        bodies.angularVelocity[b] = Rotate(q, w - change);
    }

    void Simulation::IntegratePositions(const float h)
    {
        for (int b = 0; b < bodies.Count(); ++b) {
            if (!awake[b]) continue;
            bodies.position[b] += bodies.velocity[b] * h;
            // q += h/2 · (ω, 0) · q, made length 1 again.
            const Vec3 w = bodies.angularVelocity[b];
            const Quat q = bodies.orientation[b];
            const Quat d = Quat { w.x, w.y, w.z, 0.f } * q;
            bodies.orientation[b] = Normalized({ q.x + d.x * h / 2.f, q.y + d.y * h / 2.f, q.z + d.z * h / 2.f, q.w + d.w * h / 2.f });
        }
    }
}
