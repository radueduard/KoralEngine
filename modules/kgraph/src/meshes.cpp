//
// kgraph: the default meshes, made in code. The first seven — cube to torus — and the quad and the triangle are, in
// their shapes, sizes and how finely each is cut, those of the Kotlin engine project's Meshes, so that a scene means the
// same in both; the rest are this engine's own.
//

#include <kgraph/meshes.h>

#include <algorithm>
#include <array>
#include <map>
#include <bit>
#include <format>
#include <cmath>

namespace kgraph
{
    namespace {
        constexpr float Pi = 3.14159265358979323846f;

        struct V3 { float x, y, z; };

        V3 operator+(const V3 a, const V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        V3 operator-(const V3 a, const V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        V3 operator*(const V3 a, const float k) { return { a.x * k, a.y * k, a.z * k }; }
        float Dot(const V3 a, const V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 Cross(const V3 a, const V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        V3 Normalize(const V3 a)
        {
            const float length = std::sqrt(Dot(a, a));
            return length > 0.f ? a * (1.f / length) : V3 { 0.f, 1.f, 0.f };
        }
        /** Any way at right angles to @p n. */
        V3 Across(const V3 n) { return Normalize(Cross(std::abs(n.y) < 0.9f ? V3 { 0.f, 1.f, 0.f } : V3 { 1.f, 0.f, 0.f }, n)); }

        /** A point of a profile as it is turned: how far out, how high. */
        struct P2 { float r, y; };

        /** A point of a profile turned about Y: how far out it is, how high, and its normal's outward and upward parts. */
        struct Ring { float radius, y, nr, ny; };

        struct Maker {
            MeshData data;

            std::uint32_t Vertex(const V3 position, const V3 normal, const float u, const float v)
            {
                data.vertices.push_back({ { position.x, position.y, position.z }, { normal.x, normal.y, normal.z }, { u, v } });
                return static_cast<std::uint32_t>(data.vertices.size() - 1);
            }
            /** One triangle, counter-clockwise seen from the side it faces. */
            void Triangle(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) { data.indices.insert(data.indices.end(), { a, b, c }); }
            [[nodiscard]] V3 At(const std::uint32_t i) const { const auto& p = data.vertices[i].position; return { p[0], p[1], p[2] }; }
            /** One triangle, its corners put counter-clockwise seen from where @p out points. */
            void TriangleFacing(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c, const V3 out)
            {
                if (Dot(Cross(At(b) - At(a), At(c) - At(a)), out) >= 0.f) Triangle(a, b, c);
                else Triangle(a, c, b);
            }
            /** Two triangles of four corners, going round: facing where @p out points. */
            void QuadFacing(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c, const std::uint32_t d, const V3 out)
            {
                TriangleFacing(a, b, c, out);
                TriangleFacing(a, c, d, out);
            }

            /**
             * A flat face of corners going round it (convex), facing @p n, lit flat: its picture laid on it, u across it
             * level (or along X, for a face up or down) and v up it, a unit of the picture a unit of the face.
             */
            void Flat(const std::vector<V3>& corners, const V3 n)
            {
                const V3 out = Normalize(n), u = Across(out), v = Cross(out, u);
                const auto first = static_cast<std::uint32_t>(data.vertices.size());
                for (const V3& p : corners) Vertex(p, out, Dot(p, u) + 0.5f, Dot(p, v) + 0.5f);
                for (std::uint32_t i = 1; i + 1 < corners.size(); ++i) TriangleFacing(first, first + i, first + i + 1, out);
            }

            /**
             * A profile turned about Y, a smooth surface: each point's normal at right angles to the profile there —
             * the profile going down the outside, so that the normal is out.
             */
            void Lathe(const std::vector<P2>& profile, const int segments)
            {
                std::vector<Ring> rings;
                for (std::size_t i = 0; i < profile.size(); ++i) {
                    const P2 before = profile[i == 0 ? 0 : i - 1], after = profile[std::min(i + 1, profile.size() - 1)];
                    const float dr = after.r - before.r, dy = after.y - before.y, length = std::max(std::sqrt(dr * dr + dy * dy), 1e-9f);
                    rings.push_back({ profile[i].r, profile[i].y, -dy / length, dr / length });
                }
                Revolve(rings, segments);
            }

            /** A flat ring about Y at a height, between two radii: facing up or down. */
            void Annulus(const float inner, const float outer, const float y, const int segments, const bool up)
            {
                const V3 normal { 0.f, up ? 1.f : -1.f, 0.f };
                const auto first = static_cast<std::uint32_t>(data.vertices.size());
                for (int s = 0; s <= segments; ++s) {
                    const float phi = static_cast<float>(s) / static_cast<float>(segments) * 2.f * Pi;
                    const float x = std::sin(phi), z = std::cos(phi);
                    Vertex({ inner * x, y, inner * z }, normal, 0.5f + 0.5f * inner / outer * x, 0.5f + 0.5f * inner / outer * z);
                    Vertex({ outer * x, y, outer * z }, normal, 0.5f + 0.5f * x, 0.5f + 0.5f * z);
                }
                for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(segments); ++s) {
                    const std::uint32_t a = first + 2 * s;
                    QuadFacing(a, a + 1, a + 3, a + 2, normal);
                }
            }

            /**
             * A tube along a path, a circle of @p radii (one a point) round it: its frames carried along it without
             * turning (parallel transport), and — @p closed, the path's last point its first — the turn they come back
             * with taken out along the way, so that its ends meet. An open one is closed by a flat end each side.
             */
            void Tube(const std::vector<V3>& path, const std::vector<float>& radii, const int sides, const bool closed)
            {
                const std::size_t n = path.size();
                std::vector<V3> along(n), normal(n);
                for (std::size_t i = 0; i < n; ++i) {
                    const V3 next = i + 1 < n ? path[i + 1] : closed ? path[1] : path[i];
                    const V3 before = i > 0 ? path[i - 1] : closed ? path[n - 2] : path[i];
                    along[i] = Normalize(next - before);
                }
                normal[0] = Across(along[0]);
                for (std::size_t i = 1; i < n; ++i) normal[i] = Normalize(normal[i - 1] - along[i] * Dot(normal[i - 1], along[i]));
                if (closed) {
                    const float twist = std::atan2(Dot(Cross(normal[0], normal[n - 1]), along[0]), Dot(normal[0], normal[n - 1]));
                    for (std::size_t i = 1; i < n; ++i) {
                        const float turn = -twist * static_cast<float>(i) / static_cast<float>(n - 1);
                        normal[i] = normal[i] * std::cos(turn) + Cross(along[i], normal[i]) * std::sin(turn);
                    }
                }
                const auto first = static_cast<std::uint32_t>(data.vertices.size());
                const auto row = static_cast<std::uint32_t>(sides + 1);
                for (std::size_t i = 0; i < n; ++i) {
                    const V3 binormal = Cross(along[i], normal[i]);
                    for (int s = 0; s <= sides; ++s) {
                        const float phi = static_cast<float>(s) / static_cast<float>(sides) * 2.f * Pi;
                        const V3 out = normal[i] * std::cos(phi) + binormal * std::sin(phi);
                        Vertex(path[i] + out * radii[i], out, static_cast<float>(s) / static_cast<float>(sides), static_cast<float>(i) / static_cast<float>(n - 1));
                    }
                }
                for (std::uint32_t i = 0; i + 1 < n; ++i)
                    for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(sides); ++s) {
                        const std::uint32_t a = first + i * row + s, b = a + row;
                        const V3 out = Normalize(Normalize(At(a) - path[i]) + Normalize(At(b + 1) - path[i + 1]));
                        QuadFacing(a, a + 1, b + 1, b, out);
                    }
                if (closed) return;
                for (const std::size_t end : { std::size_t { 0 }, n - 1 }) {
                    const V3 out = along[end] * (end == 0 ? -1.f : 1.f), binormal = Cross(along[end], normal[end]);
                    const std::uint32_t centre = Vertex(path[end], out, 0.5f, 0.5f);
                    const auto rim = static_cast<std::uint32_t>(data.vertices.size());
                    for (int s = 0; s <= sides; ++s) {
                        const float phi = static_cast<float>(s) / static_cast<float>(sides) * 2.f * Pi;
                        Vertex(path[end] + (normal[end] * std::cos(phi) + binormal * std::sin(phi)) * radii[end], out, 0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi));
                    }
                    for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(sides); ++s) TriangleFacing(centre, rim + s, rim + s + 1, out);
                }
            }

            /** Another mesh's vertices and triangles, each point turned and moved as @p place says (normals turned). */
            template <typename Turn>
            void Add(const MeshData& other, Turn turn, const V3 offset)
            {
                const auto first = static_cast<std::uint32_t>(data.vertices.size());
                for (const auto& v : other.vertices)
                    Vertex(turn(V3 { v.position[0], v.position[1], v.position[2] }) + offset, turn(V3 { v.normal[0], v.normal[1], v.normal[2] }), v.uv[0], v.uv[1]);
                for (const std::uint32_t i : other.indices) data.indices.push_back(first + i);
            }

            /**
             * A profile turned once about Y, in so many segments: a band between each ring and the next. The profile
             * goes down the outside of the surface — its normal on its left as it goes — so that the triangles face out.
             */
            void Revolve(const std::vector<Ring>& profile, const int segments)
            {
                const auto first = static_cast<std::uint32_t>(data.vertices.size());
                for (std::size_t r = 0; r < profile.size(); ++r)
                    for (int s = 0; s <= segments; ++s) {
                        const float phi = static_cast<float>(s) / static_cast<float>(segments) * 2.f * Pi;
                        const float x = std::sin(phi), z = std::cos(phi);
                        const Ring& ring = profile[r];
                        Vertex({ ring.radius * x, ring.y, ring.radius * z }, { ring.nr * x, ring.ny, ring.nr * z },
                               static_cast<float>(s) / static_cast<float>(segments), static_cast<float>(r) / static_cast<float>(profile.size() - 1));
                    }
                const auto row = static_cast<std::uint32_t>(segments + 1);
                for (std::uint32_t r = 0; r + 1 < profile.size(); ++r)
                    for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(segments); ++s) {
                        const std::uint32_t a = first + r * row + s, b = a + row;
                        Triangle(a, b, a + 1);
                        Triangle(a + 1, b, b + 1);
                    }
            }

            /** A flat disc at a height, closing a revolved surface: facing up or down. */
            void Cap(const float radius, const float y, const int segments, const bool up)
            {
                const V3 normal { 0.f, up ? 1.f : -1.f, 0.f };
                const std::uint32_t centre = Vertex({ 0.f, y, 0.f }, normal, 0.5f, 0.5f);
                const auto first = static_cast<std::uint32_t>(data.vertices.size());
                for (int s = 0; s <= segments; ++s) {
                    const float phi = static_cast<float>(s) / static_cast<float>(segments) * 2.f * Pi;
                    Vertex({ radius * std::sin(phi), y, radius * std::cos(phi) }, normal, 0.5f + 0.5f * std::sin(phi), 0.5f + 0.5f * std::cos(phi));
                }
                for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(segments); ++s) {
                    if (up) Triangle(centre, first + s, first + s + 1);
                    else Triangle(centre, first + s + 1, first + s);
                }
            }
        };

        /** A cube a unit across: four corners a face, so that each face's corners have its own normal and it is lit flat. */
        MeshData Cube()
        {
            Maker make;
            const float h = 0.5f;
            // Each face: its normal, then two edges along it whose cross product is the normal — which makes the
            // corners, taken (-,-) (+,-) (+,+) (-,+), go counter-clockwise seen from outside.
            const V3 faces[6][3] {
                { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
                { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
                { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
            };
            for (const auto& [n, u, w] : faces) {
                const auto first = static_cast<std::uint32_t>(make.data.vertices.size());
                for (const auto& [su, sw] : { std::pair { -1.f, -1.f }, std::pair { 1.f, -1.f }, std::pair { 1.f, 1.f }, std::pair { -1.f, 1.f } })
                    make.Vertex({ (n.x + u.x * su + w.x * sw) * h, (n.y + u.y * su + w.y * sw) * h, (n.z + u.z * su + w.z * sw) * h }, n, (su + 1.f) * 0.5f, (sw + 1.f) * 0.5f);
                make.Triangle(first, first + 1, first + 2);
                make.Triangle(first, first + 2, first + 3);
            }
            return std::move(make.data);
        }

        /** A sphere half a unit in radius, its poles on Y: bands of latitude and of longitude. */
        MeshData Sphere(const float radius = 0.5f, const int rings = 40, const int segments = 80)
        {
            Maker make;
            const auto rowSize = static_cast<std::uint32_t>(segments + 1);   // the seam's column is repeated, so each row closes on itself
            for (int r = 0; r <= rings; ++r) {
                const float theta = static_cast<float>(r) / static_cast<float>(rings) * Pi;   // 0 at the top pole, Pi at the bottom
                for (int s = 0; s <= segments; ++s) {
                    const float phi = static_cast<float>(s) / static_cast<float>(segments) * 2.f * Pi;
                    const V3 n { std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi) };
                    make.Vertex({ n.x * radius, n.y * radius, n.z * radius }, n, static_cast<float>(s) / static_cast<float>(segments), static_cast<float>(r) / static_cast<float>(rings));
                }
            }
            for (std::uint32_t r = 0; r < static_cast<std::uint32_t>(rings); ++r)
                for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(segments); ++s) {
                    const std::uint32_t a = r * rowSize + s, b = a + rowSize;   // this ring, and the one below
                    make.Triangle(a, a + 1, b);
                    make.Triangle(a + 1, b + 1, b);
                }
            return std::move(make.data);
        }

        /** A square a unit across on the XZ plane, facing up. */
        MeshData Plane()
        {
            Maker make;
            const float h = 0.5f;
            const V3 up { 0, 1, 0 };
            const std::uint32_t a = make.Vertex({ -h, 0, h }, up, 0, 1), b = make.Vertex({ h, 0, h }, up, 1, 1);
            const std::uint32_t c = make.Vertex({ h, 0, -h }, up, 1, 0), d = make.Vertex({ -h, 0, -h }, up, 0, 0);
            make.Triangle(a, b, c);
            make.Triangle(a, c, d);
            return std::move(make.data);
        }

        /** A cylinder about Y, closed at both ends. */
        MeshData Cylinder(const float radius = 0.5f, const float height = 1.f, const int segments = 48)
        {
            Maker make;
            const float h = height / 2.f;
            make.Revolve({ { radius, h, 1.f, 0.f }, { radius, -h, 1.f, 0.f } }, segments);
            make.Cap(radius, h, segments, true);
            make.Cap(radius, -h, segments, false);
            return std::move(make.data);
        }

        /** A cone about Y, its point up, closed at its base. */
        MeshData Cone(const float radius = 0.5f, const float height = 1.f, const int segments = 48)
        {
            Maker make;
            const float h = height / 2.f;
            // The side's normal: out and up, square to the slope from the base's rim to the point.
            const float length = std::sqrt(height * height + radius * radius);
            const float nr = height / length, ny = radius / length;
            make.Revolve({ { 0.f, h, nr, ny }, { radius, -h, nr, ny } }, segments);
            make.Cap(radius, -h, segments, false);
            return std::move(make.data);
        }

        Ring HemisphereRing(const float radius, const float centre, const float theta)
        {
            return { radius * std::sin(theta), centre + radius * std::cos(theta), std::sin(theta), std::cos(theta) };
        }

        /** A capsule about Y: a cylinder a unit tall overall, its ends half spheres. */
        MeshData Capsule(const float radius = 0.25f, const float height = 1.f, const int rings = 12, const int segments = 48)
        {
            Maker make;
            const float middle = std::max(height / 2.f - radius, 0.f);
            std::vector<Ring> profile;
            // Down the top half sphere, then — the same normal twice — down the cylinder, then round the bottom.
            for (int i = 0; i <= rings; ++i) profile.push_back(HemisphereRing(radius, middle, static_cast<float>(i) / static_cast<float>(rings) * Pi / 2.f));
            for (int i = 0; i <= rings; ++i) profile.push_back(HemisphereRing(radius, -middle, Pi / 2.f + static_cast<float>(i) / static_cast<float>(rings) * Pi / 2.f));
            make.Revolve(profile, segments);
            return std::move(make.data);
        }

        /** A ring about Y: a tube round a circle. */
        MeshData Torus(const float radius = 0.35f, const float tube = 0.15f, const int rings = 32, const int segments = 64)
        {
            Maker make;
            // Round the tube the way that keeps its outside on the outside: as the profile goes, its normal is on its left.
            std::vector<Ring> profile;
            for (int i = 0; i <= rings; ++i) {
                const float t = 2.f * Pi * (1.f - static_cast<float>(i) / static_cast<float>(rings));
                profile.push_back({ radius + tube * std::cos(t), tube * std::sin(t), std::cos(t), std::sin(t) });
            }
            make.Revolve(profile, segments);
            return std::move(make.data);
        }

        /** A unit square facing +z. */
        MeshData Quad()
        {
            Maker make;
            const V3 out { 0, 0, 1 };
            const std::uint32_t a = make.Vertex({ -0.5f, -0.5f, 0 }, out, 0, 0), b = make.Vertex({ 0.5f, -0.5f, 0 }, out, 1, 0);
            const std::uint32_t c = make.Vertex({ 0.5f, 0.5f, 0 }, out, 1, 1), d = make.Vertex({ -0.5f, 0.5f, 0 }, out, 0, 1);
            make.Triangle(a, b, c);
            make.Triangle(a, c, d);
            return std::move(make.data);
        }

        MeshData OneTriangle()
        {
            Maker make;
            const V3 out { 0, 0, 1 };
            const std::uint32_t a = make.Vertex({ -0.5f, -0.5f, 0 }, out, 0, 0), b = make.Vertex({ 0.5f, -0.5f, 0 }, out, 1, 0);
            make.Triangle(a, b, make.Vertex({ 0, 0.5f, 0 }, out, 0.5f, 1));
            return std::move(make.data);
        }

        // ---- the engine's own ----------------------------------------------------------------------------------

        /**
         * A sphere half a unit in radius of triangles all about the same size: an icosahedron's cut in four, and again,
         * @p cuts times (1280 triangles at 3), pushed out onto the sphere. Its picture is the sphere's, the seam's
         * triangles given their own corners.
         */
        MeshData Icosphere(const float radius = 0.5f, const int cuts = 3)
        {
            const float t = (1.f + std::sqrt(5.f)) / 2.f;
            std::vector<V3> points { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                                     { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
            for (V3& p : points) p = Normalize(p);
            std::vector<std::array<std::uint32_t, 3>> faces { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 },
                { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
            for (int cut = 0; cut < cuts; ++cut) {
                std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> middles;
                const auto middle = [&](const std::uint32_t a, const std::uint32_t b) {
                    const auto key = std::minmax(a, b);
                    if (const auto it = middles.find(key); it != middles.end()) return it->second;
                    points.push_back(Normalize((points[a] + points[b]) * 0.5f));
                    return middles[key] = static_cast<std::uint32_t>(points.size() - 1);
                };
                std::vector<std::array<std::uint32_t, 3>> finer;
                for (const auto& [a, b, c] : faces) {
                    const std::uint32_t ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
                    finer.insert(finer.end(), { { a, ab, ca }, { b, bc, ab }, { c, ca, bc }, { ab, bc, ca } });
                }
                faces = std::move(finer);
            }
            Maker make;
            for (const auto& face : faces) {
                // As the sphere has it: u round from +X towards +Z, v from the top pole down.
                float u[3], v[3];
                for (int k = 0; k < 3; ++k) {
                    const V3 n = points[face[k]];
                    float phi = std::atan2(n.z, n.x);
                    if (phi < 0.f) phi += 2.f * Pi;
                    u[k] = phi / (2.f * Pi);
                    v[k] = std::acos(std::clamp(n.y, -1.f, 1.f)) / Pi;
                }
                if (std::max({ u[0], u[1], u[2] }) - std::min({ u[0], u[1], u[2] }) > 0.5f)
                    for (float& each : u) if (each < 0.5f) each += 1.f;      // across the seam: on its far side
                for (int k = 0; k < 3; ++k)                                     // a pole is at the u of the others
                    if (const V3 n = points[face[k]]; std::abs(n.y) > 0.9999f) u[k] = (u[(k + 1) % 3] + u[(k + 2) % 3]) / 2.f;
                std::uint32_t corner[3];
                for (int k = 0; k < 3; ++k) corner[k] = make.Vertex(points[face[k]] * radius, points[face[k]], u[k], v[k]);
                make.TriangleFacing(corner[0], corner[1], corner[2], points[face[0]] + points[face[1]] + points[face[2]]);
            }
            return std::move(make.data);
        }

        /** The plane, cut in @p cells by as many: a unit square on the XZ plane facing up, for what is moved vertex by vertex. */
        MeshData Grid(const int cells = 64)
        {
            Maker make;
            const auto row = static_cast<std::uint32_t>(cells + 1);
            for (int j = 0; j <= cells; ++j)
                for (int i = 0; i <= cells; ++i) {
                    const float u = static_cast<float>(i) / static_cast<float>(cells), v = static_cast<float>(j) / static_cast<float>(cells);
                    make.Vertex({ u - 0.5f, 0.f, v - 0.5f }, { 0.f, 1.f, 0.f }, u, v);
                }
            for (std::uint32_t j = 0; j < static_cast<std::uint32_t>(cells); ++j)
                for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(cells); ++i) {
                    const std::uint32_t a = j * row + i;
                    make.QuadFacing(a, a + 1, a + row + 1, a + row, { 0.f, 1.f, 0.f });
                }
            return std::move(make.data);
        }

        /** A shape seen from inside: its triangles turned over, its normals in, and its picture the right way round from there. */
        MeshData Inside(MeshData data)
        {
            for (auto& vertex : data.vertices) {
                for (float& n : vertex.normal) n = -n;
                vertex.uv[0] = 1.f - vertex.uv[0];
            }
            for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3) std::swap(data.indices[i + 1], data.indices[i + 2]);
            return data;
        }

        /**
         * A cube a unit across with its edges and corners rounded, @p radius round: each face a grid, fine where it
         * turns, every point pulled onto the box shrunk by the radius and then out by it again.
         */
        MeshData RoundedBox(const float radius = 0.1f, const int steps = 6)
        {
            Maker make;
            const float inner = 0.5f - radius;
            std::vector<float> at;      // across a face: out at its edge, round the turn, straight over the middle, round again
            for (int i = steps; i >= 0; --i) at.push_back(-(inner + radius * std::tan(static_cast<float>(i) / static_cast<float>(steps) * Pi / 4.f)));
            for (int i = 0; i <= steps; ++i) at.push_back(inner + radius * std::tan(static_cast<float>(i) / static_cast<float>(steps) * Pi / 4.f));
            const V3 faces[6][3] {
                { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
                { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
                { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
            };
            const auto row = static_cast<std::uint32_t>(at.size());
            for (const auto& [n, u, w] : faces) {
                const auto first = static_cast<std::uint32_t>(make.data.vertices.size());
                for (const float b : at)
                    for (const float a : at) {
                        const V3 q = n * 0.5f + u * a + w * b;
                        const V3 core { std::clamp(q.x, -inner, inner), std::clamp(q.y, -inner, inner), std::clamp(q.z, -inner, inner) };
                        const V3 out = Normalize(q - core);
                        make.Vertex(core + out * radius, out, a + 0.5f, b + 0.5f);
                    }
                for (std::uint32_t j = 0; j + 1 < row; ++j)
                    for (std::uint32_t i = 0; i + 1 < row; ++i) {
                        const std::uint32_t a = first + j * row + i;
                        make.QuadFacing(a, a + 1, a + row + 1, a + row, n);
                    }
            }
            return std::move(make.data);
        }

        /** A ramp a unit across: its floor, its back a unit tall at -Z, sloping down to nothing at +Z. */
        MeshData Wedge()
        {
            Maker make;
            const float h = 0.5f;
            const V3 a { -h, -h, h }, b { h, -h, h }, c { h, -h, -h }, d { -h, -h, -h }, e { -h, h, -h }, f { h, h, -h };
            make.Flat({ a, b, c, d }, { 0, -1, 0 });
            make.Flat({ d, c, f, e }, { 0, 0, -1 });
            make.Flat({ a, b, f, e }, { 0, 1, 1 });
            make.Flat({ a, d, e }, { -1, 0, 0 });
            make.Flat({ b, c, f }, { 1, 0, 0 });
            return std::move(make.data);
        }

        /** A pyramid a unit across and tall: a square floor, its point up. */
        MeshData Pyramid()
        {
            Maker make;
            const float h = 0.5f;
            const V3 top { 0, h, 0 };
            const V3 corners[4] { { -h, -h, h }, { h, -h, h }, { h, -h, -h }, { -h, -h, -h } };
            make.Flat({ corners[0], corners[1], corners[2], corners[3] }, { 0, -1, 0 });
            for (int i = 0; i < 4; ++i) {
                const V3 p = corners[i], q = corners[(i + 1) % 4];
                const V3 side = Cross(q - p, top - p);
                make.Flat({ p, q, top }, Dot(side, p + q + top) >= 0.f ? side : side * -1.f);
            }
            return std::move(make.data);
        }

        /** Half a sphere half a unit in radius, closed by its flat side down: a dome, from -0.25 to 0.25 high. */
        MeshData Hemisphere(const float radius = 0.5f, const int rings = 20, const int segments = 80)
        {
            Maker make;
            const float base = -radius / 2.f;
            std::vector<Ring> profile;
            for (int i = 0; i <= rings; ++i) profile.push_back(HemisphereRing(radius, base, static_cast<float>(i) / static_cast<float>(rings) * Pi / 2.f));
            make.Revolve(profile, segments);
            make.Cap(radius, base, segments, false);
            return std::move(make.data);
        }

        /** A tube: a cylinder a unit across and tall with a hole down it, its wall @p wall thick. */
        MeshData Pipe(const float wall = 0.15f, const int segments = 48)
        {
            Maker make;
            const float outer = 0.5f, inner = outer - wall, h = 0.5f;
            make.Revolve({ { outer, h, 1.f, 0.f }, { outer, -h, 1.f, 0.f } }, segments);
            make.Revolve({ { inner, -h, -1.f, 0.f }, { inner, h, -1.f, 0.f } }, segments);   // up the inside: facing in
            make.Annulus(inner, outer, h, segments, true);
            make.Annulus(inner, outer, -h, segments, false);
            return std::move(make.data);
        }

        /**
         * An arch: a block a unit wide and tall and half a unit deep, with a way through it — straight sides 0.6 apart
         * up to -0.1, then round over the top to 0.2.
         */
        MeshData Arch(const int segments = 24)
        {
            Maker make;
            const float h = 0.5f, depth = 0.25f, opening = 0.3f, spring = -0.1f;
            const V3 front { 0, 0, 1 }, back { 0, 0, -1 };
            // The legs, and the solid over the round: from each point of the round out to the block's edge that way.
            for (const float side : { -1.f, 1.f })
                for (const float z : { depth, -depth }) {
                    const float x0 = side * h, x1 = side * opening;
                    make.Flat({ { x0, -h, z }, { x1, -h, z }, { x1, spring, z }, { x0, spring, z } }, z > 0.f ? front : back);
                }
            std::vector<float> angles;
            for (int i = 0; i <= segments; ++i) angles.push_back(Pi * (1.f - static_cast<float>(i) / static_cast<float>(segments)));
            const float corner = std::atan2(h - spring, h);
            angles.insert(angles.end(), { Pi - corner, corner });
            std::ranges::sort(angles, std::greater {});
            const auto [same, end] = std::ranges::unique(angles, [](const float a, const float b) { return std::abs(a - b) < 1e-5f; });
            angles.erase(same, end);
            const auto round = [&](const float a) { return V3 { opening * std::cos(a), spring + opening * std::sin(a), 0.f }; };
            const auto edge = [&](const float a) {
                const float c = std::cos(a), s = std::sin(a);
                const float reach = std::min(std::abs(c) > 1e-6f ? h / std::abs(c) : 1e9f, s > 1e-6f ? (h - spring) / s : 1e9f);
                return V3 { reach * c, spring + reach * s, 0.f };
            };
            for (const float z : { depth, -depth }) {
                const V3 out = z > 0.f ? front : back, shift { 0.f, 0.f, z };
                for (std::size_t i = 0; i + 1 < angles.size(); ++i) {
                    const V3 corners[4] { round(angles[i]) + shift, edge(angles[i]) + shift, edge(angles[i + 1]) + shift, round(angles[i + 1]) + shift };
                    std::uint32_t at[4];
                    for (int k = 0; k < 4; ++k) at[k] = make.Vertex(corners[k], out, z > 0.f ? corners[k].x + 0.5f : 0.5f - corners[k].x, corners[k].y + 0.5f);
                    make.QuadFacing(at[0], at[1], at[2], at[3], out);
                }
            }
            // Its outside: the sides, the top, the legs' feet and their insides.
            make.Flat({ { -h, -h, depth }, { -h, h, depth }, { -h, h, -depth }, { -h, -h, -depth } }, { -1, 0, 0 });
            make.Flat({ { h, -h, depth }, { h, h, depth }, { h, h, -depth }, { h, -h, -depth } }, { 1, 0, 0 });
            make.Flat({ { -h, h, depth }, { h, h, depth }, { h, h, -depth }, { -h, h, -depth } }, { 0, 1, 0 });
            for (const float side : { -1.f, 1.f }) {
                const float x0 = side * h, x1 = side * opening;
                make.Flat({ { x0, -h, depth }, { x1, -h, depth }, { x1, -h, -depth }, { x0, -h, -depth } }, { 0, -1, 0 });
                make.Flat({ { x1, -h, depth }, { x1, spring, depth }, { x1, spring, -depth }, { x1, -h, -depth } }, { -side, 0, 0 });
            }
            // The underside of the round, facing down into the way through.
            const auto first = static_cast<std::uint32_t>(make.data.vertices.size());
            for (int i = 0; i <= segments; ++i) {
                const float a = Pi * static_cast<float>(i) / static_cast<float>(segments);
                const V3 p = round(a), in { -std::cos(a), -std::sin(a), 0.f };
                make.Vertex(p + V3 { 0, 0, depth }, in, static_cast<float>(i) / static_cast<float>(segments), 1.f);
                make.Vertex(p + V3 { 0, 0, -depth }, in, static_cast<float>(i) / static_cast<float>(segments), 0.f);
            }
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(segments); ++i) {
                const std::uint32_t a = first + 2 * i;
                const float mid = Pi * (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
                make.QuadFacing(a, a + 1, a + 3, a + 2, { -std::cos(mid), -std::sin(mid), 0.f });
            }
            return std::move(make.data);
        }

        /** A flight of @p steps stairs a unit wide, deep and tall: the first at +Z, climbing towards -Z. */
        MeshData Stairs(const int steps = 5)
        {
            Maker make;
            const float h = 0.5f, rise = 1.f / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) {
                const float top = -h + rise * static_cast<float>(i + 1), below = -h + rise * static_cast<float>(i);
                const float near = h - rise * static_cast<float>(i), far = h - rise * static_cast<float>(i + 1);
                make.Flat({ { -h, top, near }, { h, top, near }, { h, top, far }, { -h, top, far } }, { 0, 1, 0 });           // the tread
                make.Flat({ { -h, below, near }, { h, below, near }, { h, top, near }, { -h, top, near } }, { 0, 0, 1 });     // the riser
                for (const float x : { -h, h })                                                                                   // its sides
                    make.Flat({ { x, -h, near }, { x, top, near }, { x, top, far }, { x, -h, far } }, { x, 0, 0 });
            }
            make.Flat({ { -h, -h, -h }, { h, -h, -h }, { h, h, -h }, { -h, h, -h } }, { 0, 0, -1 });
            make.Flat({ { -h, -h, h }, { h, -h, h }, { h, -h, -h }, { -h, -h, -h } }, { 0, -1, 0 });
            return std::move(make.data);
        }

        /** Points along a curve through @p points (Catmull-Rom), @p each between two of them: a smooth profile or path. */
        template <typename T>
        std::vector<T> Smooth(const std::vector<T>& points, const int each)
        {
            std::vector<T> out;
            const auto at = [&](const std::ptrdiff_t i) { return points[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i, 0, static_cast<std::ptrdiff_t>(points.size()) - 1))]; };
            for (std::ptrdiff_t i = 0; i + 1 < static_cast<std::ptrdiff_t>(points.size()); ++i)
                for (int k = 0; k < each; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(each), t2 = t * t, t3 = t2 * t;
                    const float w0 = -0.5f * t3 + t2 - 0.5f * t, w1 = 1.5f * t3 - 2.5f * t2 + 1.f, w2 = -1.5f * t3 + 2.f * t2 + 0.5f * t, w3 = 0.5f * t3 - 0.5f * t2;
                    out.push_back(at(i - 1) * w0 + at(i) * w1 + at(i + 1) * w2 + at(i + 2) * w3);
                }
            out.push_back(points.back());
            return out;
        }
        P2 operator*(const P2 a, const float k) { return { a.r * k, a.y * k }; }
        P2 operator+(const P2 a, const P2 b) { return { a.r + b.r, a.y + b.y }; }

        /**
         * A teapot, made of what this file makes — its body and lid turned, a spout and a handle that are tubes — not
         * the Utah teapot's patches, but as curved, with as many hollows and saddles: a unit across, spout towards +X.
         */
        MeshData Teapot()
        {
            Maker make;
            // From the knob down the lid, over the rim and down the body to its foot: going down the outside.
            const std::vector<P2> body { { 0.f, 0.30f }, { 0.04f, 0.295f }, { 0.055f, 0.27f }, { 0.035f, 0.235f }, { 0.09f, 0.215f }, { 0.2f, 0.185f },
                                         { 0.27f, 0.15f }, { 0.29f, 0.12f }, { 0.33f, 0.09f }, { 0.38f, 0.04f }, { 0.41f, -0.02f }, { 0.415f, -0.08f },
                                         { 0.4f, -0.15f }, { 0.37f, -0.22f }, { 0.33f, -0.27f }, { 0.3f, -0.3f } };
            make.Lathe(Smooth(body, 6), 64);
            make.Cap(0.3f, -0.3f, 64, false);
            const auto tube = [&](const std::vector<V3>& through, const float from, const float to) {
                const std::vector<V3> path = Smooth(through, 10);
                std::vector<float> radii;
                for (std::size_t i = 0; i < path.size(); ++i) radii.push_back(from + (to - from) * static_cast<float>(i) / static_cast<float>(path.size() - 1));
                make.Tube(path, radii, 20, false);
            };
            tube({ { 0.28f, -0.12f, 0.f }, { 0.45f, -0.1f, 0.f }, { 0.52f, 0.08f, 0.f }, { 0.6f, 0.2f, 0.f } }, 0.08f, 0.035f);          // the spout
            tube({ { -0.33f, 0.08f, 0.f }, { -0.52f, 0.12f, 0.f }, { -0.58f, -0.04f, 0.f }, { -0.5f, -0.16f, 0.f }, { -0.36f, -0.18f, 0.f } }, 0.035f, 0.035f);
            return std::move(make.data);
        }

        /** A trefoil knot: a tube round a (2, 3) torus knot, a unit across. */
        MeshData Knot(const int samples = 256, const int sides = 16)
        {
            Maker make;
            std::vector<V3> path;
            for (int i = 0; i <= samples; ++i) {
                const float t = 2.f * Pi * static_cast<float>(i % samples) / static_cast<float>(samples);
                const float reach = 2.f + std::cos(3.f * t);
                path.push_back(V3 { reach * std::cos(2.f * t), -std::sin(3.f * t), reach * std::sin(2.f * t) } * 0.14f);
            }
            make.Tube(path, std::vector<float>(path.size(), 0.07f), sides, true);
            return std::move(make.data);
        }

        /** An arrow up Y from @p from to @p to: a shaft, and a cone for its head. */
        MeshData Arrow(const float from, const float to, const float shaft, const float head, const float headLength, const int segments = 24)
        {
            Maker make;
            const float neck = to - headLength;
            const float slope = std::sqrt(headLength * headLength + head * head), nr = headLength / slope, ny = head / slope;
            make.Revolve({ { 0.f, to, nr, ny }, { head, neck, nr, ny } }, segments);
            make.Annulus(shaft, head, neck, segments, false);
            make.Revolve({ { shaft, neck, 1.f, 0.f }, { shaft, from, 1.f, 0.f } }, segments);
            make.Cap(shaft, from, segments, false);
            return std::move(make.data);
        }

        /** The three axes from the origin, half a unit each: arrows along +X, +Y and +Z, and a ball where they meet. */
        MeshData Axes()
        {
            Maker make;
            const MeshData arrow = Arrow(0.f, 0.5f, 0.02f, 0.05f, 0.12f);
            make.Add(arrow, [](const V3 p) { return V3 { p.y, -p.x, p.z }; }, {});     // Y turned to X
            make.Add(arrow, [](const V3 p) { return p; }, {});
            make.Add(arrow, [](const V3 p) { return V3 { p.x, -p.z, p.y }; }, {});     // Y turned to Z
            make.Add(Sphere(0.04f, 12, 24), [](const V3 p) { return p; }, {});
            return std::move(make.data);
        }

        /**
         * A cylinder a unit tall, 0.3 across, cut in 16 bands, of three bones — at its foot, its middle and its top
         * — each vertex of the two nearest it, by how near (BONE_IDS, BONE_WEIGHTS): what skinning is tried on.
         */
        MeshData SkinnedCylinder(const float radius = 0.15f, const int bands = 16, const int segments = 24)
        {
            Maker make;
            std::vector<Ring> profile;
            for (int i = 0; i <= bands; ++i) profile.push_back({ radius, 0.5f - static_cast<float>(i) / static_cast<float>(bands), 1.f, 0.f });
            make.Revolve(profile, segments);
            make.Cap(radius, 0.5f, segments, true);
            make.Cap(radius, -0.5f, segments, false);
            for (const auto& vertex : make.data.vertices) {
                const float t = std::clamp((vertex.position[1] + 0.5f) * 2.f, 0.f, 2.f);
                const std::uint32_t low = t < 1.f ? 0u : 1u;
                const float w = t - static_cast<float>(low);
                make.data.boneIds.push_back({ low, low + 1, 0u, 0u });
                make.data.boneWeights.push_back({ 1.f - w, w, 0.f, 0.f });
            }
            return std::move(make.data);
        }
    }

    const std::vector<std::string>& DefaultMeshNames()
    {
        static const std::vector<std::string> names { "cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus", "quad", "triangle",
            "icosphere", "grid", "insideCube", "insideSphere", "roundedBox", "wedge", "pyramid", "hemisphere", "pipe", "arch", "stairs",
            "teapot", "knot", "arrow", "axes", "skinnedCylinder" };
        return names;
    }

    std::optional<MeshData> ShapeData(const std::string_view name)
    {
        if (name == "cube") return Cube();
        if (name == "sphere") return Sphere();
        if (name == "plane") return Plane();
        if (name == "cylinder") return Cylinder();
        if (name == "cone") return Cone();
        if (name == "capsule") return Capsule();
        if (name == "torus") return Torus();
        if (name == "quad") return Quad();
        if (name == "triangle") return OneTriangle();
        if (name == "icosphere") return Icosphere();
        if (name == "grid") return Grid();
        if (name == "insideCube") return Inside(Cube());
        if (name == "insideSphere") return Inside(Sphere());
        if (name == "roundedBox") return RoundedBox();
        if (name == "wedge") return Wedge();
        if (name == "pyramid") return Pyramid();
        if (name == "hemisphere") return Hemisphere();
        if (name == "pipe") return Pipe();
        if (name == "arch") return Arch();
        if (name == "stairs") return Stairs();
        if (name == "teapot") return Teapot();
        if (name == "knot") return Knot();
        if (name == "arrow") return Arrow(-0.5f, 0.5f, 0.04f, 0.1f, 0.3f);
        if (name == "axes") return Axes();
        if (name == "skinnedCylinder") return SkinnedCylinder();
        return std::nullopt;
    }

    std::string DefaultMeshId(const std::string_view name) { return "$" + std::string(name); }

    std::string_view TangentsName(const Tangents tangents)
    {
        switch (tangents) {
        case Tangents::eWithBitangent: return "tangentBitangent";
        case Tangents::eNone: return "none";
        default: return "tangent";
        }
    }

    std::optional<Tangents> TangentsNamed(const std::string_view name)
    {
        for (const Tangents tangents : { Tangents::eFour, Tangents::eWithBitangent, Tangents::eNone })
            if (TangentsName(tangents) == name) return tangents;
        return std::nullopt;
    }

    void AddTangents(VertexData& data, const Tangents form)
    {
        std::erase_if(data.attributes, [](const VertexAttribute& a) { return a.semantic == "TANGENT" || a.semantic == "BITANGENT"; });
        for (auto& buffer : data.buffers) std::erase_if(buffer, [](const std::string& s) { return s == "TANGENT" || s == "BITANGENT"; });
        std::erase_if(data.buffers, [](const auto& buffer) { return buffer.empty(); });
        const VertexAttribute* position = data.Find("POSITION");
        const VertexAttribute* normal = data.Find("NORMAL");
        const VertexAttribute* uv = data.Find("UV");
        if (form == Tangents::eNone || !position || !normal || !uv || position->count < 3 || normal->count < 3 || uv->count < 2) return;
        const auto read = [](const VertexAttribute& a, const std::uint32_t v, const std::uint32_t c) { return std::bit_cast<float>(a.words[static_cast<std::size_t>(v) * a.count + c]); };
        const auto at = [&](const VertexAttribute& a, const std::uint32_t v) { return V3 { read(a, v, 0), read(a, v, 1), read(a, v, 2) }; };
        const auto minus = [](const V3 a, const V3 b) { return V3 { a.x - b.x, a.y - b.y, a.z - b.z }; };
        const auto dot = [](const V3 a, const V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        const auto cross = [](const V3 a, const V3 b) { return V3 { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; };

        // Each triangle's ways u and v grow, over its corners: the vertices' are the sum of their triangles' (Lengyel's).
        const std::uint32_t count = data.vertexCount;
        std::vector<V3> along(count, V3 {}), across(count, V3 {});
        const auto corner = [&](const std::size_t i) { return data.indices.empty() ? static_cast<std::uint32_t>(i) : data.indices[i]; };
        const std::size_t corners = data.indices.empty() ? count : data.indices.size();
        for (std::size_t i = 0; i + 2 < corners; i += 3) {
            const std::uint32_t a = corner(i), b = corner(i + 1), c = corner(i + 2);
            const V3 e1 = minus(at(*position, b), at(*position, a)), e2 = minus(at(*position, c), at(*position, a));
            const float u1 = read(*uv, b, 0) - read(*uv, a, 0), v1 = read(*uv, b, 1) - read(*uv, a, 1);
            const float u2 = read(*uv, c, 0) - read(*uv, a, 0), v2 = read(*uv, c, 1) - read(*uv, a, 1);
            const float area = u1 * v2 - u2 * v1;
            if (std::abs(area) < 1e-12f) continue;      // its picture is a line or a point on it: no way to say
            const float r = 1.f / area;
            const V3 t { (e1.x * v2 - e2.x * v1) * r, (e1.y * v2 - e2.y * v1) * r, (e1.z * v2 - e2.z * v1) * r };
            const V3 s { (e2.x * u1 - e1.x * u2) * r, (e2.y * u1 - e1.y * u2) * r, (e2.z * u1 - e1.z * u2) * r };
            for (const std::uint32_t v : { a, b, c }) {
                along[v] = { along[v].x + t.x, along[v].y + t.y, along[v].z + t.z };
                across[v] = { across[v].x + s.x, across[v].y + s.y, across[v].z + s.z };
            }
        }

        VertexAttribute tangent { "TANGENT", AttributeType::eFloat, form == Tangents::eFour ? 4u : 3u, {} }, bitangent { "BITANGENT", AttributeType::eFloat, 3, {} };
        const auto put = [](VertexAttribute& into, const float value) { into.words.push_back(std::bit_cast<std::uint32_t>(value)); };
        for (std::uint32_t v = 0; v < count; ++v) {
            V3 n = at(*normal, v);
            const float nl = std::sqrt(dot(n, n));
            n = nl > 0.f ? V3 { n.x / nl, n.y / nl, n.z / nl } : V3 { 0.f, 1.f, 0.f };
            // At right angles to the normal; where the picture gives no way (a point it is pinched to), any such way.
            V3 t = minus(along[v], V3 { n.x * dot(n, along[v]), n.y * dot(n, along[v]), n.z * dot(n, along[v]) });
            float tl = std::sqrt(dot(t, t));
            if (tl < 1e-8f) {
                const V3 other = std::abs(n.x) < 0.9f ? V3 { 1.f, 0.f, 0.f } : V3 { 0.f, 1.f, 0.f };
                t = minus(other, V3 { n.x * dot(n, other), n.y * dot(n, other), n.z * dot(n, other) });
                tl = std::sqrt(dot(t, t));
            }
            t = { t.x / tl, t.y / tl, t.z / tl };
            const float w = dot(cross(n, t), across[v]) < 0.f ? -1.f : 1.f;
            put(tangent, t.x); put(tangent, t.y); put(tangent, t.z);
            if (form == Tangents::eFour) put(tangent, w);
            else {
                const V3 b = cross(n, t);
                put(bitangent, b.x * w); put(bitangent, b.y * w); put(bitangent, b.z * w);
            }
        }
        // After the normal, as a model file's are.
        const auto after = std::ranges::find(data.attributes, "NORMAL", &VertexAttribute::semantic) + 1;
        const auto placed = data.attributes.insert(after, std::move(tangent));
        if (form == Tangents::eWithBitangent) data.attributes.insert(placed + 1, std::move(bitangent));
        // In the buffer the normal is in, when it is in one named.
        for (auto& buffer : data.buffers)
            if (const auto it = std::ranges::find(buffer, "NORMAL"); it != buffer.end()) {
                const auto in = buffer.insert(it + 1, "TANGENT");
                if (form == Tangents::eWithBitangent) buffer.insert(in + 1, "BITANGENT");
                break;
            }
    }

    VertexData ToVertexData(const MeshData& data, const Tangents tangents)
    {
        VertexData out;
        out.vertexCount = static_cast<std::uint32_t>(data.vertices.size());
        out.indices = data.indices;
        VertexAttribute position { "POSITION", AttributeType::eFloat, 3, {} }, normal { "NORMAL", AttributeType::eFloat, 3, {} }, uv { "UV", AttributeType::eFloat, 2, {} };
        const auto put = [](VertexAttribute& into, const float* from, const int n) {
            for (int i = 0; i < n; ++i) into.words.push_back(std::bit_cast<std::uint32_t>(from[i]));
        };
        for (const auto& vertex : data.vertices) { put(position, vertex.position, 3); put(normal, vertex.normal, 3); put(uv, vertex.uv, 2); }
        out.attributes = { std::move(position), std::move(normal), std::move(uv) };
        if (data.boneIds.size() == data.vertices.size() && data.boneWeights.size() == data.vertices.size() && !data.vertices.empty()) {
            // After the picture, as a model file's are.
            VertexAttribute ids { "BONE_IDS", AttributeType::eUInt, 4, {} }, weights { "BONE_WEIGHTS", AttributeType::eFloat, 4, {} };
            for (std::size_t v = 0; v < data.vertices.size(); ++v) {
                ids.words.insert(ids.words.end(), data.boneIds[v].begin(), data.boneIds[v].end());
                put(weights, data.boneWeights[v].data(), 4);
            }
            out.attributes.push_back(std::move(ids));
            out.attributes.push_back(std::move(weights));
        }
        AddTangents(out, tangents);
        return out;
    }
}
