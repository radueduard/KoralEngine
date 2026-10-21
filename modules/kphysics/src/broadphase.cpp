#include <kphysics/simulation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kphysics
{
    namespace
    {
        constexpr float Infinite = 1e30f;
        /** More than this share of the tree's leaves to put back in one step, and it is built again instead. */
        constexpr float RebuildFraction = 0.25f;

        bool IsPlane(const std::vector<Vec3>& lower, const int b) { return lower[b].x == -Infinite; }
    }

    Broadphase::Broadphase(const int capacity)
        : lower(capacity), upper(capacity), _order(capacity), _open(capacity), _isLarge(capacity), _tree(capacity), _moved(capacity)
    {
        pairs.reserve(256);
    }

    void Broadphase::Update(const Simulation& sim)
    {
        const int n = std::min(sim.bodies.Count(), sim.colliders.Count());
        for (int b = 0; b < n; ++b) Bounds(sim, b, sim.contactMargin);
        pairs.clear();
        switch (kind) {
        case BroadphaseKind::AllPairs: AllPairs(sim, n); break;
        case BroadphaseKind::SweepAndPrune: SweepAndPrune(sim, n); break;
        case BroadphaseKind::Grid: Grid(sim, n); break;
        case BroadphaseKind::Tree: TreePairs(sim, n); break;
        }
        std::ranges::sort(pairs);
        if (kind == BroadphaseKind::Grid || kind == BroadphaseKind::Tree) Unique();
    }

    void Broadphase::Bounds(const Simulation& sim, const int b, const float margin)
    {
        const int shape = sim.colliders.shape[b];
        if (shape == Plane) {
            lower[b] = { -Infinite, -Infinite, -Infinite };
            upper[b] = { Infinite, Infinite, Infinite };
            return;
        }
        const Vec3 s = sim.colliders.size[b];
        const Mat3 r = FromQuat(sim.bodies.orientation[b]);
        if (shape == Cone) {
            // Along each world axis, as far as its point or its rim reaches: the axis in the cone's frame is d, and the rim
            // reaches r·|d across Y| from its centre a quarter of the height below.
            for (int c = 0; c < 3; ++c) {
                const Vec3 d { r.column[0][c], r.column[1][c], r.column[2][c] };
                const float rim = s.x * std::sqrt(d.x * d.x + d.z * d.z);
                const float up = std::max(0.75f * s.y * d.y, rim - 0.25f * s.y * d.y);
                const float down = std::max(-0.75f * s.y * d.y, rim + 0.25f * s.y * d.y);
                lower[b][c] = sim.bodies.position[b][c] - down - margin;
                upper[b][c] = sim.bodies.position[b][c] + up + margin;
            }
            return;
        }
        if (shape == Mesh) {
            const TriangleMesh* mesh = sim.colliders.mesh[b].get();
            const Vec3 centre = mesh ? (mesh->Lower() + mesh->Upper()) * 0.5f : Vec3 {};
            const Vec3 half = mesh ? (mesh->Upper() - mesh->Lower()) * 0.5f : Vec3 {};
            const Vec3 at = sim.bodies.position[b] + r * centre;
            const float thickness = s.x;                        // as far behind its faces as it holds bodies
            for (int c = 0; c < 3; ++c) {
                const float reach = std::abs(r.column[0][c]) * half.x + std::abs(r.column[1][c]) * half.y + std::abs(r.column[2][c]) * half.z + thickness;
                lower[b][c] = at[c] - reach - margin;
                upper[b][c] = at[c] + reach + margin;
            }
            return;
        }
        for (int c = 0; c < 3; ++c) {
            // How far the shape reaches along world axis c: the rotation's row c.
            const float r0 = std::abs(r.column[0][c]), r1 = std::abs(r.column[1][c]), r2 = std::abs(r.column[2][c]);
            const float reach = shape == Sphere ? s.x : shape == Box ? r0 * s.x + r1 * s.y + r2 * s.z
                                                                     : r1 * s.y + s.x;      // capsule: its segment plus the radius
            lower[b][c] = sim.bodies.position[b][c] - reach - margin;
            upper[b][c] = sim.bodies.position[b][c] + reach + margin;
        }
    }

    bool Broadphase::Overlap(const int a, const int b) const
    {
        return lower[a].x <= upper[b].x && lower[b].x <= upper[a].x && lower[a].y <= upper[b].y && lower[b].y <= upper[a].y &&
               lower[a].z <= upper[b].z && lower[b].z <= upper[a].z;
    }

    /** Whether a and b are tested at all: something can move one of them, one of them is active, and no joint keeps them apart. */
    bool Broadphase::Wanted(const Simulation& sim, const int a, const int b)
    {
        if (sim.bodies.inverseMass[a] == 0.f && sim.bodies.inverseMass[b] == 0.f) return false;
        if (!sim.IsActive(a) && !sim.IsActive(b)) return false;
        return !sim.joints.Excludes(a, b);
    }

    // ---- every pair -----------------------------------------------------------------------------------------------------

    void Broadphase::AllPairs(const Simulation& sim, const int n)
    {
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b)
                if (Overlap(a, b) && Wanted(sim, a, b)) pairs.push_back(PairKey(a, b));
    }

    // ---- sweep and prune ------------------------------------------------------------------------------------------------

    void Broadphase::SweepAndPrune(const Simulation& sim, const int n)
    {
        if (_ordered != n) {
            for (int b = 0; b < n; ++b) _order[b] = b;
            _ordered = n;
        }
        _axis = WidestAxis(n);
        // Insertion sort by the box's low end: nearly nothing to do when little moved since.
        for (int i = 1; i < n; ++i) {
            const int body = _order[i];
            const float key = lower[body][_axis];
            int j = i - 1;
            while (j >= 0 && lower[_order[j]][_axis] > key) { _order[j + 1] = _order[j]; --j; }
            _order[j + 1] = body;
        }
        // Sweep: the bodies whose interval is still open; each new body is tested against them alone.
        int opened = 0;
        for (int i = 0; i < n; ++i) {
            const int b = _order[i];
            const float start = lower[b][_axis];
            int kept = 0;
            for (int k = 0; k < opened; ++k) {
                const int o = _open[k];
                if (upper[o][_axis] < start) continue;                      // ended before this one starts: closed for good
                _open[kept++] = o;
                if (Overlap(o, b) && Wanted(sim, o, b)) pairs.push_back(PairKey(o, b));
            }
            opened = kept;
            _open[opened++] = b;
        }
    }

    /** The axis along which the boxes' centres spread most (planes left out), with a 10% margin so it does not flip-flop. */
    int Broadphase::WidestAxis(const int n) const
    {
        int best = _axis;
        double widest = -1.0;
        for (int c = 0; c < 3; ++c) {
            double sum = 0.0, sumOfSquares = 0.0;
            int count = 0;
            for (int b = 0; b < n; ++b) {
                if (lower[b][c] == -Infinite) continue;
                const double centre = (static_cast<double>(lower[b][c]) + upper[b][c]) * 0.5;
                sum += centre;
                sumOfSquares += centre * centre;
                ++count;
            }
            if (count == 0) continue;
            const double variance = sumOfSquares / count - (sum / count) * (sum / count);
            if (variance > widest * 1.1) { widest = variance; best = c; }
        }
        return best;
    }

    // ---- uniform grid ---------------------------------------------------------------------------------------------------

    void Broadphase::Grid(const Simulation& sim, const int n)
    {
        const float size = cellSize > 0.f ? cellSize : AverageBox(n) * 2.f;
        const float inverse = 1.f / size;
        _cells.clear();
        int larges = 0;
        _large.clear();
        for (int b = 0; b < n; ++b) {
            _isLarge[b] = 0;
            if (IsPlane(lower, b)) { _large.push_back(b); _isLarge[b] = 1; ++larges; continue; }
            const int x0 = static_cast<int>(std::floor(lower[b].x * inverse)), x1 = static_cast<int>(std::floor(upper[b].x * inverse));
            const int y0 = static_cast<int>(std::floor(lower[b].y * inverse)), y1 = static_cast<int>(std::floor(upper[b].y * inverse));
            const int z0 = static_cast<int>(std::floor(lower[b].z * inverse)), z1 = static_cast<int>(std::floor(upper[b].z * inverse));
            if (static_cast<std::int64_t>(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 64) {         // too big for cells
                _large.push_back(b);
                _isLarge[b] = 1;
                ++larges;
                continue;
            }
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    for (int z = z0; z <= z1; ++z) {
                        // The cell's hash in the high 32 bits, the body in the low: sorting groups each cell's bodies.
                        const std::uint32_t hash = (static_cast<std::uint32_t>(x) * 73856093u) ^ (static_cast<std::uint32_t>(y) * 19349663u) ^
                                                   (static_cast<std::uint32_t>(z) * 83492791u);
                        _cells.push_back((static_cast<std::uint64_t>(hash) << 32) | static_cast<std::uint32_t>(b));
                    }
        }
        std::ranges::sort(_cells);
        for (std::size_t start = 0; start < _cells.size();) {
            std::size_t end = start + 1;
            while (end < _cells.size() && (_cells[end] >> 32) == (_cells[start] >> 32)) ++end;
            for (std::size_t i = start; i < end; ++i)
                for (std::size_t j = i + 1; j < end; ++j) {
                    const int a = static_cast<int>(_cells[i] & 0xFFFFFFFFu), b = static_cast<int>(_cells[j] & 0xFFFFFFFFu);
                    if (Overlap(a, b) && Wanted(sim, a, b)) pairs.push_back(PairKey(a, b));
                }
            start = end;
        }
        Larges(sim, n, larges);
    }

    void Broadphase::Larges(const Simulation& sim, const int n, const int count)
    {
        for (int i = 0; i < count; ++i) {
            const int a = _large[i];
            for (int b = 0; b < n; ++b) {
                if (b == a || (_isLarge[b] && b < a)) continue;              // two large ones: once
                if (Overlap(a, b) && Wanted(sim, a, b)) pairs.push_back(PairKey(a, b));
            }
        }
    }

    float Broadphase::AverageBox(const int n) const
    {
        float sum = 0.f;
        int count = 0;
        for (int b = 0; b < n; ++b) {
            if (IsPlane(lower, b)) continue;
            const Vec3 e = upper[b] - lower[b];
            sum += std::max(e.x, std::max(e.y, e.z));
            ++count;
        }
        return count == 0 ? 1.f : sum / static_cast<float>(count);
    }

    // ---- dynamic AABB tree ----------------------------------------------------------------------------------------------
    //
    // Every pair whose leaves' fat boxes overlap is kept from step to step as a candidate, in order (Box2D keeps the same,
    // as its contacts). Only a leaf that moved changes them, so a step where little leaves its fat box asks the tree almost
    // nothing. The pairs are the candidates whose bodies' own boxes overlap: exact, since boxes overlapping means fat ones do.

    void Broadphase::TreePairs(const Simulation& sim, const int n)
    {
        int moved = 0;
        if (_treeBodies != n) {                                             // bodies added or taken away: built again
            _tree.Clear();
            for (int b = 0; b < n; ++b)
                if (!IsPlane(lower, b)) _moved[moved++] = b;
            _tree.Rebuild(_moved.data(), moved, lower, upper, true);
            _treeBodies = n;
            _candidates.clear();
        } else {
            int leaves = 0;
            for (int b = 0; b < n; ++b) {
                if (IsPlane(lower, b)) continue;
                ++leaves;
                if (_tree.NeedsMove(b, lower, upper)) _moved[moved++] = b;
            }
            if (static_cast<float>(moved) > static_cast<float>(leaves) * RebuildFraction) {
                // So many leaves to put back that building the tree again is cheaper; then every leaf's pairs are new.
                moved = 0;
                for (int b = 0; b < n; ++b)
                    if (!IsPlane(lower, b)) _moved[moved++] = b;
                _tree.Rebuild(_moved.data(), moved, lower, upper, false);
                _candidates.clear();
            } else {
                for (int i = 0; i < moved; ++i) _tree.Reinsert(_moved[i], lower, upper);
            }
        }
        if (moved > 0) {
            // Drop what a moved leaf no longer overlaps, then ask the tree what each moved leaf overlaps now.
            std::erase_if(_candidates, [&](const std::uint64_t key) { return !_tree.FatOverlap(LowerOf(key), HigherOf(key)); });
            _fresh.clear();
            for (int i = 0; i < moved; ++i) {
                const int m = _moved[i];
                _tree.QueryLeaf(m, [&](const int o) { if (o != m) _fresh.push_back(PairKey(m, o)); });
            }
            MergeFresh();
        }
        for (const std::uint64_t key : _candidates) {
            const int a = LowerOf(key), b = HigherOf(key);
            if (Overlap(a, b) && Wanted(sim, a, b)) pairs.push_back(key);
        }
        // Planes against everything, as the grid does.
        _large.clear();
        for (int b = 0; b < n; ++b) {
            _isLarge[b] = IsPlane(lower, b);
            if (_isLarge[b]) _large.push_back(b);
        }
        Larges(sim, n, static_cast<int>(_large.size()));
    }

    /** The fresh pairs, sorted and each once, merged into the candidates, which stay sorted and each once. */
    void Broadphase::MergeFresh()
    {
        std::ranges::sort(_fresh);
        _spare.clear();
        _spare.reserve(_candidates.size() + _fresh.size());
        std::size_t i = 0, j = 0;
        while (i < _candidates.size() || j < _fresh.size()) {
            const std::uint64_t next = j == _fresh.size() ? _candidates[i++]
                                     : i == _candidates.size() ? _fresh[j++]
                                     : _candidates[i] <= _fresh[j] ? _candidates[i++] : _fresh[j++];
            if (_spare.empty() || _spare.back() != next) _spare.push_back(next);
        }
        std::swap(_candidates, _spare);
    }

    /** Two bodies sharing several cells are found once a cell: after sorting, each pair once. */
    void Broadphase::Unique()
    {
        const auto [first, last] = std::ranges::unique(pairs);
        pairs.erase(first, last);
    }
}
