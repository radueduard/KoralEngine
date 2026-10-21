// A Mesh collider's triangles: their normals, which of their edges are sharp, and a tree over them.

#include <kphysics/collision.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace kphysics
{
    namespace
    {
        /** A fold sharper than this (the cosine of the angle between the two faces' normals) is an edge to collide with. */
        constexpr float FlatCosine = 0.9848f;       // 10°
        constexpr int LeafSize = 4;
    }

    TriangleMesh::TriangleMesh(const std::vector<Vec3>& positions, const std::vector<std::uint32_t>& indices) : _vertices(positions)
    {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
            if (a >= positions.size() || b >= positions.size() || c >= positions.size()) continue;
            Vec3 n = Cross(positions[b] - positions[a], positions[c] - positions[a]);
            if (Normalize(n) < 1e-10f) continue;                            // no area: no face to touch
            _triangles.push_back({ { a, b, c }, n, 0 });
        }

        // Each edge's two triangles, by its two corners, lower first; welded by position, since a mesh made for drawing
        // has a vertex per face at a crease.
        std::map<std::tuple<float, float, float>, std::uint32_t> welded;
        std::vector<std::uint32_t> weld(_vertices.size());
        for (std::uint32_t v = 0; v < _vertices.size(); ++v)
            weld[v] = welded.try_emplace({ _vertices[v].x, _vertices[v].y, _vertices[v].z }, v).first->second;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::pair<int, int>>> edges;   // (triangle, its edge)
        for (int t = 0; t < static_cast<int>(_triangles.size()); ++t)
            for (int e = 0; e < 3; ++e) {
                const std::uint32_t p = weld[_triangles[t].v[e]], q = weld[_triangles[t].v[(e + 1) % 3]];
                edges[{ std::min(p, q), std::max(p, q) }].emplace_back(t, e);
            }
        for (const auto& [corners, sharing] : edges) {
            if (sharing.size() != 2) {
                // The mesh's own edge (or one shared by more than two): sharp, a body falls off it or catches on it.
                for (const auto& [t, e] : sharing) _triangles[t].sharp |= static_cast<std::uint8_t>(1u << e);
                continue;
            }
            const auto [t, e] = sharing[0];
            const auto [u, f] = sharing[1];
            const Triangle& first = _triangles[t];
            const Triangle& second = _triangles[u];
            // Convex when the other triangle's far corner is behind this one's face; flat when their faces nearly agree.
            const Vec3 far = _vertices[second.v[(f + 2) % 3]];
            const bool convex = Dot(first.normal, far - _vertices[first.v[e]]) < -1e-6f;
            if (convex && Dot(first.normal, second.normal) < FlatCosine) {
                _triangles[t].sharp |= static_cast<std::uint8_t>(1u << e);
                _triangles[u].sharp |= static_cast<std::uint8_t>(1u << f);
            }
        }

        _lower = { FLT_MAX, FLT_MAX, FLT_MAX };
        _upper = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const Vec3 v : _vertices)
            for (int c = 0; c < 3; ++c) {
                _lower[c] = std::min(_lower[c], v[c]);
                _upper[c] = std::max(_upper[c], v[c]);
            }
        if (_triangles.empty()) return;
        std::vector<Vec3> centres(_triangles.size());
        _order.resize(_triangles.size());
        for (int t = 0; t < static_cast<int>(_triangles.size()); ++t) {
            const Triangle& tri = _triangles[t];
            centres[t] = (_vertices[tri.v[0]] + _vertices[tri.v[1]] + _vertices[tri.v[2]]) * (1.f / 3.f);
            _order[t] = t;
        }
        _nodes.reserve(2 * _triangles.size() / LeafSize + 2);
        Build(0, static_cast<int>(_triangles.size()), centres);
    }

    /** A node over _order[first, first + count): its box, and, past a few triangles, split at the median of the widest axis. */
    int TriangleMesh::Build(const int first, const int count, std::vector<Vec3>& centres)
    {
        const int index = static_cast<int>(_nodes.size());
        _nodes.emplace_back();
        Vec3 lower { FLT_MAX, FLT_MAX, FLT_MAX }, upper { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        Vec3 low { FLT_MAX, FLT_MAX, FLT_MAX }, high { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int i = first; i < first + count; ++i) {
            const Triangle& tri = _triangles[_order[i]];
            for (const std::uint32_t v : tri.v)
                for (int c = 0; c < 3; ++c) {
                    lower[c] = std::min(lower[c], _vertices[v][c]);
                    upper[c] = std::max(upper[c], _vertices[v][c]);
                }
            for (int c = 0; c < 3; ++c) {
                low[c] = std::min(low[c], centres[_order[i]][c]);
                high[c] = std::max(high[c], centres[_order[i]][c]);
            }
        }
        _nodes[index].lower = lower;
        _nodes[index].upper = upper;
        if (count <= LeafSize) {
            _nodes[index].first = first;
            _nodes[index].count = count;
            return index;
        }
        const Vec3 spread = high - low;
        const int axis = spread.x >= spread.y && spread.x >= spread.z ? 0 : spread.y >= spread.z ? 1 : 2;
        const int middle = count / 2;
        std::nth_element(_order.begin() + first, _order.begin() + first + middle, _order.begin() + first + count,
                         [&](const int a, const int b) { return centres[a][axis] < centres[b][axis]; });
        const int left = Build(first, middle, centres);
        const int right = Build(first + middle, count - middle, centres);
        _nodes[index].left = left;
        _nodes[index].right = right;
        return index;
    }
}
