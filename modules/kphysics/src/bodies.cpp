#include <kphysics/bodies.h>

#include <stdexcept>

namespace kphysics
{
    namespace mass
    {
        float Sphere(const float density, const float radius, Vec3& inertia)
        {
            const float radiusSquared = radius * radius;
            const float m = density * 4.f / 3.f * Pi * radius * radiusSquared;
            const float i = 2.f / 5.f * m * radiusSquared;
            inertia = { i, i, i };
            return m;
        }

        float Box(const float density, const Vec3 half, Vec3& inertia)
        {
            const float m = density * 8 * half.x * half.y * half.z;
            const float third = m / 3;
            const float x2 = half.x * half.x, y2 = half.y * half.y, z2 = half.z * half.z;
            inertia = { third * (y2 + z2), third * (x2 + z2), third * (x2 + y2) };
            return m;
        }

        float Capsule(const float density, const float radius, const float halfSegment, Vec3& inertia)
        {
            const float height = halfSegment * 2.f;         // the cylinder's alone: the caps are the hemispheres
            const float radiusSquared = radius * radius;
            // The cylinder.
            const float cylinderMass = density * Pi * radiusSquared * height;
            const float cylinderAlong = cylinderMass * radiusSquared / 2.f;
            const float cylinderAcross = cylinderMass * (3.f * radiusSquared + height * height) / 12.f;
            // Each hemisphere: across the axis about its own centre of mass (83/320 m r²), then moved out to that centre's
            // distance from the capsule's, halfSegment + 3r/8 (parallel axes: I + m d²).
            const float hemisphereMass = density * 2.f / 3.f * Pi * radius * radiusSquared;
            const float hemisphereAlong = 2.f / 5.f * hemisphereMass * radiusSquared;
            const float distance = halfSegment + 3.f * radius / 8.f;
            const float hemisphereAcross = 83.f / 320.f * hemisphereMass * radiusSquared + hemisphereMass * distance * distance;
            const float across = cylinderAcross + 2.f * hemisphereAcross;
            inertia = { across, cylinderAlong + 2.f * hemisphereAlong, across };
            return cylinderMass + 2.f * hemisphereMass;
        }

        float Cone(const float density, const float radius, const float height, Vec3& inertia)
        {
            const float m = density * Pi * radius * radius * height / 3.f;
            const float across = 3.f / 20.f * m * radius * radius + 3.f / 80.f * m * height * height;
            inertia = { across, 3.f / 10.f * m * radius * radius, across };
            return m;
        }
    }

    Bodies::Bodies(const int capacity)
        : position(capacity), orientation(capacity), velocity(capacity), angularVelocity(capacity), force(capacity),
          torque(capacity), inverseMass(capacity), inverseInertiaLocal(capacity), inverseInertiaWorld(capacity),
          _capacity(capacity)
    {
    }

    int Bodies::Add(const Vec3 at, const Quat turn, const float mass, const Vec3 inertia)
    {
        if (_count >= _capacity) throw std::length_error("kphysics::Bodies: already full");
        const int b = _count++;
        position[b] = at;
        orientation[b] = Normalized(turn);
        velocity[b] = {};
        angularVelocity[b] = {};
        force[b] = {};
        torque[b] = {};
        if (mass == 0.f) {
            inverseMass[b] = 0.f;
            inverseInertiaLocal[b] = {};
        } else {
            inverseMass[b] = 1.f / mass;
            inverseInertiaLocal[b] = { 1.f / inertia.x, 1.f / inertia.y, 1.f / inertia.z };
        }
        inverseInertiaWorld[b] = RotateDiagonal(FromQuat(orientation[b]), inverseInertiaLocal[b]);
        return b;
    }

    void Bodies::UpdateWorldInertia()
    {
        for (int b = 0; b < _count; ++b) inverseInertiaWorld[b] = RotateDiagonal(FromQuat(orientation[b]), inverseInertiaLocal[b]);
    }
}
