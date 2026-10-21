#include <kphysics/simulation.h>

#include <algorithm>
#include <cmath>

namespace kphysics
{
    namespace
    {
        float Inverse(const float k) { return k > 0.f ? 1.f / k : 0.f; }

        // Bodies, with −1 for the world: at the origin, unturned, still, with no mass.
        Vec3 PositionOf(const Simulation& sim, const int b) { return b < 0 ? Vec3 {} : sim.bodies.position[b]; }
        Vec3 VelocityOf(const Simulation& sim, const int b) { return b < 0 ? Vec3 {} : sim.bodies.velocity[b]; }
        Vec3 SpinOf(const Simulation& sim, const int b) { return b < 0 ? Vec3 {} : sim.bodies.angularVelocity[b]; }
        bool Moves(const Simulation& sim, const int b) { return b >= 0 && sim.bodies.inverseMass[b] != 0.f; }

        /** A world point into body b's frame: Rᵀ (p − x). The world's frame is the world. */
        Vec3 ToLocal(const Simulation& sim, const int b, const Vec3 p) { return b < 0 ? p : Rotate(Conjugate(sim.bodies.orientation[b]), p - sim.bodies.position[b]); }
        Vec3 DirectionToLocal(const Simulation& sim, const int b, const Vec3 d) { return b < 0 ? d : Rotate(Conjugate(sim.bodies.orientation[b]), d); }
        /** A direction, or an anchor's arm, from body b's frame into the world's. */
        Vec3 ToWorld(const Simulation& sim, const int b, const Vec3 local) { return b < 0 ? local : Rotate(sim.bodies.orientation[b], local); }

        /** d · I⁻¹ d: how much an angular impulse along d turns body b along d. */
        float AngularResponse(const Simulation& sim, const int b, const Vec3 d) { return b < 0 ? 0.f : Dot(d, sim.bodies.inverseInertiaWorld[b] * d); }
        /** How much an impulse along d at the arm moves body b along d: 1/m + (r × d) · I⁻¹ (r × d). */
        float Response(const Simulation& sim, const int b, const Vec3 arm, const Vec3 d)
        {
            return b < 0 ? 0.f : sim.bodies.inverseMass[b] + AngularResponse(sim, b, Cross(arm, d));
        }
        /** How the anchor's velocity changes for a unit impulse d there: d / m + (I⁻¹ (r × d)) × r. */
        Vec3 PointResponse(const Simulation& sim, const int b, const Vec3 arm, const Vec3 d)
        {
            if (b < 0) return {};
            return d * sim.bodies.inverseMass[b] + Cross(sim.bodies.inverseInertiaWorld[b] * Cross(arm, d), arm);
        }
        /** K, column by column: how the gap between the anchors moves for an impulse along each world axis. */
        Mat3 PointMass(const Simulation& sim, const Joint& j)
        {
            Mat3 k;
            for (int c = 0; c < 3; ++c) {
                Vec3 d;
                d[c] = 1.f;
                k.column[c] = PointResponse(sim, j.a, j.armA, d) + PointResponse(sim, j.b, j.armB, d);
            }
            return k;
        }
        /** B's anchor's velocity minus A's: v + ω × r for each. */
        Vec3 RelativePointVelocity(const Simulation& sim, const Joint& j)
        {
            return (VelocityOf(sim, j.b) + Cross(SpinOf(sim, j.b), j.armB)) - (VelocityOf(sim, j.a) + Cross(SpinOf(sim, j.a), j.armA));
        }
        /** (ωB − ωA) · u. */
        float RelativeSpin(const Simulation& sim, const Joint& j, const Vec3 u) { return Dot(SpinOf(sim, j.b) - SpinOf(sim, j.a), u); }

        /** ω += I⁻¹ l. */
        void Turn(Simulation& sim, const int b, const Vec3 l) { sim.bodies.angularVelocity[b] += sim.bodies.inverseInertiaWorld[b] * l; }
        void ApplyToBody(Simulation& sim, const int b, const Vec3 arm, const Vec3 p)
        {
            if (!Moves(sim, b)) return;
            sim.bodies.velocity[b] += p * sim.bodies.inverseMass[b];
            Turn(sim, b, Cross(arm, p));
        }
        /** Impulse p at the anchor: −p on A, +p on B, each through its arm. */
        void ApplyAt(Simulation& sim, const Joint& j, const Vec3 p)
        {
            ApplyToBody(sim, j.a, j.armA, -p);
            ApplyToBody(sim, j.b, j.armB, p);
        }
        /** Angular impulse l: −l on A, +l on B. */
        void ApplyAngular(Simulation& sim, const Joint& j, const Vec3 l)
        {
            if (Moves(sim, j.a)) Turn(sim, j.a, -l);
            if (Moves(sim, j.b)) Turn(sim, j.b, l);
        }

        /** Turns body b by I⁻¹ l, a small rotation, as a position pass does. */
        void Rotate(Simulation& sim, const int b, const Vec3 l)
        {
            if (!Moves(sim, b)) return;
            sim.bodies.orientation[b] = Turned(sim.bodies.orientation[b], sim.bodies.inverseInertiaWorld[b] * l);
        }
        /** A positional "impulse" p at the arm: x += p / m, and the body turned by I⁻¹ (r × p). */
        void DisplaceAt(Simulation& sim, const int b, const Vec3 arm, const Vec3 p)
        {
            if (!Moves(sim, b)) return;
            sim.bodies.position[b] += p * sim.bodies.inverseMass[b];
            sim.bodies.orientation[b] = Turned(sim.bodies.orientation[b], sim.bodies.inverseInertiaWorld[b] * Cross(arm, p));
        }

        /** B's zero direction against A's, about @p axis: the hinge's angle. */
        float HingeAngle(const Simulation& sim, const Joint& j, const Vec3 axis)
        {
            const Vec3 zeroA = ToWorld(sim, j.a, j.zeroA), zeroB = ToWorld(sim, j.b, j.zeroB);
            return static_cast<float>(std::atan2(static_cast<double>(Dot(Cross(zeroA, zeroB), axis)), static_cast<double>(Dot(zeroA, zeroB))));
        }
    }

    // ---- making them ----------------------------------------------------------------------------------------------------

    int Joints::Next(const JointKind kind, const int a, const int b)
    {
        Joint& j = joints.emplace_back();
        j.kind = kind;
        j.a = a;
        j.b = b;
        RebuildExcluded();
        return Count() - 1;
    }

    int Joints::AddBall(const Simulation& sim, const int a, const int b, const Vec3 at)
    {
        const int index = Next(JointKind::Ball, a, b);
        joints[index].anchorA = ToLocal(sim, a, at);
        joints[index].anchorB = ToLocal(sim, b, at);
        return index;
    }

    int Joints::AddHinge(const Simulation& sim, const int a, const int b, const Vec3 at, const Vec3 axis)
    {
        const int index = AddBall(sim, a, b, at);
        Joint& j = joints[index];
        j.kind = JointKind::Hinge;
        const Vec3 n = axis / Length(axis);
        j.axisA = DirectionToLocal(sim, a, n);
        j.axisB = DirectionToLocal(sim, b, n);
        // A direction across the axis marks angle 0, in both bodies.
        const Vec3 zero = Perpendicular(n);
        j.zeroA = DirectionToLocal(sim, a, zero);
        j.zeroB = DirectionToLocal(sim, b, zero);
        return index;
    }

    int Joints::AddDistance(const Simulation& sim, const int a, const int b, const Vec3 onA, const Vec3 onB, const bool rope)
    {
        const int index = Next(JointKind::Distance, a, b);
        Joint& j = joints[index];
        j.anchorA = ToLocal(sim, a, onA);
        j.anchorB = ToLocal(sim, b, onB);
        j.length = Length(onB - onA);
        j.rope = rope;
        return index;
    }

    void Joints::SetLimits(const int j, const float lower, const float upper)
    {
        joints[j].limited = true;
        joints[j].lowerAngle = lower;
        joints[j].upperAngle = upper;
    }

    void Joints::SetMotor(const int j, const float speed, const float maxTorque)
    {
        joints[j].motor = true;
        joints[j].motorSpeed = speed;
        joints[j].maxMotorTorque = maxTorque;
    }

    void Joints::SetCollideConnected(const int j, const bool collide)
    {
        joints[j].collideConnected = collide;
        RebuildExcluded();
    }

    bool Joints::Excludes(const int a, const int b) const
    {
        return !_excluded.empty() && std::ranges::binary_search(_excluded, PairKey(a, b));
    }

    void Joints::RebuildExcluded()
    {
        _excluded.clear();
        for (const Joint& j : joints)
            if (!j.collideConnected && j.a >= 0 && j.b >= 0) _excluded.push_back(PairKey(j.a, j.b));
        std::ranges::sort(_excluded);
    }

    void Joints::Anchors(const Simulation& sim, const int index, Vec3& onA, Vec3& onB) const
    {
        const Joint& j = joints[index];
        onA = ToWorld(sim, j.a, j.anchorA) + PositionOf(sim, j.a);
        onB = ToWorld(sim, j.b, j.anchorB) + PositionOf(sim, j.b);
    }

    float Joints::Gap(const Simulation& sim, const int j) const
    {
        Vec3 onA, onB;
        Anchors(sim, j, onA, onB);
        return Length(onB - onA);
    }

    // ---- solving them, each substep -------------------------------------------------------------------------------------

    void Joints::Prepare(Simulation& sim, const float h)
    {
        _h = h;
        for (Joint& j : joints) {
            j.armA = ToWorld(sim, j.a, j.anchorA);
            j.armB = ToWorld(sim, j.b, j.anchorB);
            const Vec3 gap = (PositionOf(sim, j.b) + j.armB) - (PositionOf(sim, j.a) + j.armA);

            if (j.kind == JointKind::Distance) {
                const float l = Length(gap);
                j.direction = l > 1e-6f ? gap / l : Vec3 { 0.f, 1.f, 0.f };
                j.lengthMass = Inverse(Response(sim, j.a, j.armA, j.direction) + Response(sim, j.b, j.armB, j.direction));
                j.lengthError = l - j.length;
                continue;
            }

            j.pointMass = PointMass(sim, j);
            j.pointBias = gap * (baumgarte / h);
            if (j.kind != JointKind::Hinge) continue;

            // The axes in the world: A's, and B's, which should be the same. A's × B's is how far B's has turned away.
            const Vec3 n = ToWorld(sim, j.a, j.axisA);
            const Vec3 turned = Cross(n, ToWorld(sim, j.b, j.axisB));
            j.axis = n;
            j.across[0] = Perpendicular(n);
            j.across[1] = Cross(n, j.across[0]);
            for (int k = 0; k < 2; ++k) {
                j.alignBias[k] = baumgarte / h * Dot(turned, j.across[k]);
                j.alignMass[k] = Inverse(AngularResponse(sim, j.a, j.across[k]) + AngularResponse(sim, j.b, j.across[k]));
            }
            j.axialMass = Inverse(AngularResponse(sim, j.a, n) + AngularResponse(sim, j.b, n));
            j.angle = HingeAngle(sim, j, n);
            if (!j.limited) { j.lowerImpulse = 0.f; j.upperImpulse = 0.f; }
            if (!j.motor) j.motorImpulse = 0.f;
        }
    }

    void Joints::WarmStart(Simulation& sim)
    {
        for (const Joint& j : joints) {
            if (j.kind == JointKind::Distance) { ApplyAt(sim, j, j.direction * j.lengthImpulse); continue; }
            ApplyAt(sim, j, j.pointImpulse);
            if (j.kind != JointKind::Hinge) continue;
            const float axial = j.lowerImpulse - j.upperImpulse + j.motorImpulse;
            ApplyAngular(sim, j, j.across[0] * j.alignImpulse[0] + j.across[1] * j.alignImpulse[1] + j.axis * axial);
        }
    }

    void Joints::Solve(Simulation& sim, const bool useBias)
    {
        _bias = useBias ? 1.f : 0.f;
        for (Joint& j : joints) {
            switch (j.kind) {
            case JointKind::Distance: SolveDistance(sim, j); break;
            case JointKind::Ball: SolvePoint(sim, j); break;
            case JointKind::Hinge:
                SolveHingeAxial(sim, j);
                SolveAlign(sim, j);
                SolvePoint(sim, j);
                break;
            }
        }
    }

    void Joints::SolvePoint(Simulation& sim, Joint& j) const
    {
        Vec3 impulse;
        if (!Solve3(j.pointMass, -(RelativePointVelocity(sim, j) + j.pointBias * _bias), impulse)) return;
        j.pointImpulse += impulse;
        ApplyAt(sim, j, impulse);
    }

    void Joints::SolveAlign(Simulation& sim, Joint& j) const
    {
        for (int k = 0; k < 2; ++k) {
            const Vec3 u = j.across[k];
            const float impulse = -j.alignMass[k] * (RelativeSpin(sim, j, u) + _bias * j.alignBias[k]);
            j.alignImpulse[k] += impulse;
            ApplyAngular(sim, j, u * impulse);
        }
    }

    void Joints::SolveHingeAxial(Simulation& sim, Joint& j) const
    {
        const Vec3 n = j.axis;
        if (j.motor) {
            const float cdot = RelativeSpin(sim, j, n) - j.motorSpeed;
            const float most = j.maxMotorTorque * _h;
            const float old = j.motorImpulse;
            j.motorImpulse = std::clamp(old - j.axialMass * cdot, -most, most);
            ApplyAngular(sim, j, n * (j.motorImpulse - old));
        }
        if (!j.limited) return;
        // Lower: the angle may only grow past it. Before reaching it, it may close the gap this step and no more.
        {
            const float c = j.angle - j.lowerAngle;
            const float bias = c > 0.f ? c / _h : _bias * baumgarte / _h * c;
            const float old = j.lowerImpulse;
            j.lowerImpulse = std::max(0.f, old - j.axialMass * (RelativeSpin(sim, j, n) + bias));
            ApplyAngular(sim, j, n * (j.lowerImpulse - old));
        }
        // Upper: the same, the other way.
        {
            const float c = j.upperAngle - j.angle;
            const float bias = c > 0.f ? c / _h : _bias * baumgarte / _h * c;
            const float old = j.upperImpulse;
            j.upperImpulse = std::max(0.f, old - j.axialMass * (-RelativeSpin(sim, j, n) + bias));
            ApplyAngular(sim, j, -n * (j.upperImpulse - old));
        }
    }

    void Joints::SolveDistance(Simulation& sim, Joint& j) const
    {
        const float cdot = Dot(RelativePointVelocity(sim, j), j.direction);
        const float c = j.lengthError;
        const float old = j.lengthImpulse;
        if (j.rope) {
            // A rope only pulls (an impulse bringing them together: negative), and lets them close the gap freely.
            const float bias = c < 0.f ? c / _h : _bias * baumgarte / _h * c;
            j.lengthImpulse = std::min(0.f, old - j.lengthMass * (cdot + bias));
        } else {
            j.lengthImpulse = old - j.lengthMass * (cdot + _bias * baumgarte / _h * c);
        }
        ApplyAt(sim, j, j.direction * (j.lengthImpulse - old));
    }

    // ---- position passes: closing the drift directly --------------------------------------------------------------------
    //
    // After the positions move, each joint's actual gap, closed by moving and turning its bodies directly: non-linear
    // Gauss–Seidel, as Box2D 2.x does it. A body swung fast on an arc moves along a straight line within a substep and
    // leaves the arc by about v²/r · h²: a chain's end whipping at 10 m/s opens 3 cm a substep.

    void Joints::SolvePositions(Simulation& sim)
    {
        for (int pass = 0; pass < positionIterations; ++pass)
            for (Joint& j : joints) {
                if (j.kind == JointKind::Distance) { CloseLength(sim, j); continue; }
                if (j.kind == JointKind::Hinge) {
                    CloseAlignment(sim, j);
                    if (j.limited) CloseLimit(sim, j);
                }
                ClosePoint(sim, j);
            }
    }

    void Joints::ClosePoint(Simulation& sim, Joint& j) const
    {
        j.armA = ToWorld(sim, j.a, j.anchorA);
        j.armB = ToWorld(sim, j.b, j.anchorB);
        const Vec3 error = (PositionOf(sim, j.b) + j.armB) - (PositionOf(sim, j.a) + j.armA);
        j.pointMass = PointMass(sim, j);
        Vec3 p;
        if (!Solve3(j.pointMass, -error, p)) return;
        DisplaceAt(sim, j.a, j.armA, -p);
        DisplaceAt(sim, j.b, j.armB, p);
    }

    void Joints::CloseAlignment(Simulation& sim, const Joint& j) const
    {
        // The turn from A's axis to B's, A's × B's: turned back by a share each body can give.
        const Vec3 e = Cross(ToWorld(sim, j.a, j.axisA), ToWorld(sim, j.b, j.axisB));
        const float size = Length(e);
        if (size < 1e-7f) return;
        const Vec3 u = e / size;
        const float lambda = Inverse(AngularResponse(sim, j.a, u) + AngularResponse(sim, j.b, u)) * static_cast<float>(std::asin(static_cast<double>(std::min(size, 1.f))));
        Rotate(sim, j.a, u * lambda);
        Rotate(sim, j.b, -u * lambda);
    }

    void Joints::CloseLimit(Simulation& sim, const Joint& j) const
    {
        const Vec3 n = ToWorld(sim, j.a, j.axisA);
        const float now = HingeAngle(sim, j, n);
        float error;
        if (now < j.lowerAngle) error = now - j.lowerAngle;
        else if (now > j.upperAngle) error = now - j.upperAngle;
        else return;
        const float lambda = Inverse(AngularResponse(sim, j.a, n) + AngularResponse(sim, j.b, n)) * error;
        Rotate(sim, j.a, n * lambda);
        Rotate(sim, j.b, -n * lambda);
    }

    void Joints::CloseLength(Simulation& sim, Joint& j) const
    {
        j.armA = ToWorld(sim, j.a, j.anchorA);
        j.armB = ToWorld(sim, j.b, j.anchorB);
        const Vec3 e = (PositionOf(sim, j.b) + j.armB) - (PositionOf(sim, j.a) + j.armA);
        const float l = Length(e);
        if (l < 1e-6f) return;
        const float error = l - j.length;
        if (j.rope && error < 0.f) return;                                  // a slack rope is no error
        const Vec3 n = e / l;
        const float k = Response(sim, j.a, j.armA, n) + Response(sim, j.b, j.armB, n);
        if (k <= 0.f) return;
        const Vec3 p = n * (-error / k);
        DisplaceAt(sim, j.a, j.armA, -p);
        DisplaceAt(sim, j.b, j.armB, p);
    }
}
