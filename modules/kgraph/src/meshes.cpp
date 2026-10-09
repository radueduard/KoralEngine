//
// kgraph: the default meshes, made in code. The shapes, their sizes and how finely each is cut are those of the
// Kotlin engine project's Meshes, so that a scene means the same in both.
//

#include <kgraph/meshes.h>

#include <algorithm>
#include <cmath>

namespace kgraph
{
    namespace {
        constexpr float Pi = 3.14159265358979323846f;

        struct V3 { float x, y, z; };

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
                for (const auto [su, sw] : { std::pair { -1.f, -1.f }, std::pair { 1.f, -1.f }, std::pair { 1.f, 1.f }, std::pair { -1.f, 1.f } })
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
    }

    const std::vector<std::string>& DefaultMeshNames()
    {
        static const std::vector<std::string> names { "cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus" };
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
        return std::nullopt;
    }

    std::string DefaultMeshId(const std::string_view name) { return "$" + std::string(name); }
}
