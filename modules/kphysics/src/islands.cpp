#include <kphysics/simulation.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <set>

namespace kphysics
{
    void Islands::Update(Simulation& sim, const float dt)
    {
        Bodies& bodies = sim.bodies;
        const int n = bodies.Count();
        const float lin2 = linearTolerance * linearTolerance;
        const float ang2 = angularTolerance * angularTolerance;
        // How long each awake body has been still.
        for (int b = 0; b < n; ++b) {
            _parent[b] = b;
            if (bodies.inverseMass[b] == 0.f || !sim.awake[b]) continue;
            const bool moving = LengthSquared(bodies.velocity[b]) > lin2 || LengthSquared(bodies.angularVelocity[b]) > ang2;
            sim.stillTime[b] = moving ? 0.f : sim.stillTime[b] + dt;
        }
        // The islands: contacts and joints between dynamic bodies.
        for (const Manifold& m : sim.contacts.manifolds) Join(sim, m.a, m.b);
        for (const Joint& j : sim.joints.joints)
            if (j.a >= 0 && j.b >= 0) Join(sim, j.a, j.b);
        // Each island's shortest stillness, and whether any of it is awake.
        std::fill_n(_still.begin(), n, FLT_MAX);
        std::fill_n(_awake.begin(), n, 0);
        for (int b = 0; b < n; ++b) {
            if (bodies.inverseMass[b] == 0.f) continue;
            const int r = Find(b);
            _still[r] = std::min(_still[r], sim.stillTime[b]);
            if (sim.awake[b]) _awake[r] = 1;
        }
        // Asleep when all of it has been still long enough; otherwise awake, all of it.
        for (int b = 0; b < n; ++b) {
            if (bodies.inverseMass[b] == 0.f) continue;
            const int r = Find(b);
            if (allowSleep && _still[r] >= timeToSleep) {
                if (sim.awake[b]) {
                    sim.awake[b] = 0;
                    bodies.velocity[b] = {};
                    bodies.angularVelocity[b] = {};
                }
            } else if (_awake[r] && !sim.awake[b]) {
                sim.awake[b] = 1;
                sim.stillTime[b] = 0.f;
            }
        }
    }

    void Islands::Join(const Simulation& sim, const int a, const int b)
    {
        if (sim.bodies.inverseMass[a] == 0.f || sim.bodies.inverseMass[b] == 0.f) return;
        const int ra = Find(a), rb = Find(b);
        if (ra != rb) _parent[std::max(ra, rb)] = std::min(ra, rb);            // the lower root wins: the same islands every run
    }

    int Islands::Find(const int b)
    {
        int r = b;
        while (_parent[r] != r) r = _parent[r];
        int x = b;
        while (_parent[x] != r) {
            const int next = _parent[x];
            _parent[x] = r;
            x = next;
        }
        return r;
    }

    void Colouring::Colour(const Simulation& sim)
    {
        const auto& manifolds = sim.contacts.manifolds;
        const int count = sim.contacts.Count();
        if (static_cast<int>(colour.size()) < count) {
            colour.resize(count * 2);
            order.resize(count * 2);
        }
        if (static_cast<int>(_used.size()) < sim.bodies.Capacity()) _used.resize(sim.bodies.Capacity());
        std::fill_n(_used.begin(), sim.bodies.Count(), 0u);
        _colourCount = 0;
        for (int m = 0; m < count; ++m) {
            const int a = manifolds[m].a, b = manifolds[m].b;
            const bool dynamicA = sim.bodies.inverseMass[a] != 0.f;
            const bool dynamicB = sim.bodies.inverseMass[b] != 0.f;
            const std::uint64_t taken = (dynamicA ? _used[a] : 0u) | (dynamicB ? _used[b] : 0u);
            const int k = taken == ~std::uint64_t { 0 } ? Overflow : std::countr_one(taken);
            colour[m] = k;
            if (k < Overflow) {
                if (dynamicA) _used[a] |= std::uint64_t { 1 } << k;
                if (dynamicB) _used[b] |= std::uint64_t { 1 } << k;
            }
            _colourCount = std::max(_colourCount, k + 1);
        }
        // Counting sort by colour, keeping each colour's manifolds in their order.
        std::ranges::fill(start, 0);
        for (int m = 0; m < count; ++m) ++start[colour[m] + 1];
        for (std::size_t k = 1; k < start.size(); ++k) start[k] += start[k - 1];
        _next = start;
        for (int m = 0; m < count; ++m) order[_next[colour[m]]++] = m;
    }

    bool Colouring::IsValid(const Simulation& sim) const
    {
        const auto& manifolds = sim.contacts.manifolds;
        for (int k = 0; k < std::min(_colourCount, Overflow); ++k) {
            std::set<int> seen;
            for (int i = start[k]; i < start[k + 1]; ++i) {
                const int m = order[i];
                for (const int b : { manifolds[m].a, manifolds[m].b })
                    if (sim.bodies.inverseMass[b] != 0.f && !seen.insert(b).second) return false;
            }
        }
        return true;
    }
}
