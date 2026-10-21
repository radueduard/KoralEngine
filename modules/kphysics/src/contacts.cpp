#include <kphysics/collision.h>

#include <stdexcept>
#include <utility>

namespace kphysics
{
    namespace
    {
        /** How far a point may have moved since last step and still be taken for the same one, in metres. */
        constexpr float Reach = 0.02f;

        void Take(ContactPoint& to, const ContactPoint& from)
        {
            to.normalImpulse = from.normalImpulse;
            to.tangentImpulse1 = from.tangentImpulse1;
            to.tangentImpulse2 = from.tangentImpulse2;
        }
    }

    int Colliders::Add(const int kind, const Vec3 extent)
    {
        if (_count >= _capacity) throw std::length_error("kphysics::Colliders: no room for another collider");
        const int c = _count++;
        shape[c] = kind;
        size[c] = extent;
        return c;
    }

    int Colliders::AddMesh(std::shared_ptr<const TriangleMesh> triangles, const float thickness)
    {
        const int c = Add(Mesh, { thickness, 0.f, 0.f });
        mesh[c] = std::move(triangles);
        return c;
    }

    Manifold& Contacts::Begin(const int a, const int b, const Vec3 normal)
    {
        Manifold& m = manifolds.emplace_back();
        m.a = a;
        m.b = b;
        m.normal = normal;
        m.count = 0;
        return m;
    }

    bool Contacts::AddPoint(Manifold& m, const Vec3 at, const float depth, const int feature)
    {
        if (m.count == 4) return false;
        m.points[m.count++] = { at, depth, feature, 0.f, 0.f, 0.f };
        return true;
    }

    void Contacts::End()
    {
        if (!manifolds.empty() && manifolds.back().count == 0) manifolds.pop_back();
    }

    void Contacts::Flip(Manifold& m)
    {
        std::swap(m.a, m.b);
        m.normal = -m.normal;
        for (int p = 0; p < m.count; ++p) m.points[p].feature = Feature(FeatureOfB(m.points[p].feature), FeatureOfA(m.points[p].feature));
    }

    void Contacts::CarryImpulsesFrom(const Contacts& previous)
    {
        const int before = previous.Count();
        int q = 0;
        for (Manifold& m : manifolds) {
            const std::uint64_t key = m.Key();
            while (q < before && previous.manifolds[q].Key() < key) ++q;
            if (q == before) return;
            if (previous.manifolds[q].Key() != key) continue;
            // A pair may have several (a body against a mesh's floor and wall): the previous one that pushed most nearly
            // the same way. Of one, that one.
            int match = q;
            for (int r = q + 1; r < before && previous.manifolds[r].Key() == key; ++r)
                if (Dot(previous.manifolds[r].normal, m.normal) > Dot(previous.manifolds[match].normal, m.normal)) match = r;
            const Manifold& old = previous.manifolds[match];
            // The same pair: seen from the same side? A flipped pair has its features' halves swapped.
            const bool flipped = old.a != m.a;
            unsigned claimed = 0;           // a bit for each of the previous points already matched
            unsigned unmatched = 0;         // a bit for each point here still without one
            for (int p = 0; p < m.count; ++p) {
                const int id = m.points[p].feature;
                const int wanted = flipped ? Feature(FeatureOfB(id), FeatureOfA(id)) : id;
                int found = -1;
                for (int r = 0; r < old.count; ++r)
                    if (old.points[r].feature == wanted && (claimed & (1u << r)) == 0) { found = r; break; }
                if (found < 0) { unmatched |= 1u << p; continue; }
                claimed |= 1u << found;
                Take(m.points[p], old.points[found]);
            }
            // A point whose feature changed (clipping a corner that sits right on an edge flips between the corner and
            // the crossing, from step to step): the nearest previous point not yet taken, if it is close.
            for (int p = 0; p < m.count; ++p) {
                if ((unmatched & (1u << p)) == 0) continue;
                int best = -1;
                float nearest = Reach * Reach;
                for (int r = 0; r < old.count; ++r) {
                    if ((claimed & (1u << r)) != 0) continue;
                    const float d2 = LengthSquared(m.points[p].at - old.points[r].at);
                    if (d2 < nearest) { nearest = d2; best = r; }
                }
                if (best < 0) continue;
                claimed |= 1u << best;
                Take(m.points[p], old.points[best]);
            }
        }
    }
}
