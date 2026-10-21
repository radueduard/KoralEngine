//
// kphysics: what bodies are shaped as, and where two touch.
//
// Each body has one collider, added in the same order as the bodies, so collider b is body b's: centred on its
// position, turned by its orientation. For each pair that touches, the narrowphase writes a manifold: a normal from A
// towards B and up to four points, each halfway between the two surfaces, with how deep they overlap there (> 0
// overlapping, < 0 apart but within the margin) and a feature id saying which parts of A and B made it, so that the
// same point next step is known for the same one, and starts from the impulse it ended with (warm starting).
//

#pragma once

#include <kphysics/bodies.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace kphysics
{
    /** @brief The shapes, numbered as the engine's Collider component numbers them. */
    enum Shape : int {
        Sphere = 0,     ///< size: (radius, 0, 0).
        Box = 1,        ///< size: half-extents along the body's X, Y and Z.
        Capsule = 2,    ///< size: (radius, half segment, 0): a segment along the body's Y, swept by the radius.
        Plane = 3,      ///< The infinite plane through the body's position, facing its +Y. Static only.
        /// size: (radius, height, 0): a solid cone along the body's Y, its point up, about its centre of mass, which is a
        /// quarter of its height above its base: the base at −height/4, the point at +3·height/4. Collided as a pyramid
        /// of ConeSides sides.
        Cone = 4,
        /// A TriangleMesh, in the body's frame. Static or kinematic only: nothing turns a hollow surface. size: (thickness,
        /// 0, 0): how far behind a triangle a body's centre may be and still be pushed back out by it, as by a solid wall
        /// of that thickness; 0, as far as the body could have gone through in the step and a little more.
        Mesh = 5,
    };

    /** @brief How many sides the pyramid a cone is collided as has: its base rim is that many points. */
    inline constexpr int ConeSides = 16;

    /**
     * @brief Triangles, in a body's frame, with a tree over them, for a Mesh collider: level geometry. Each triangle is
     *        one-sided, facing the way its corners turn counter-clockwise (as the engine's meshes are made); and each of
     *        its edges is known to be sharp (a convex fold, or the edge of the mesh) or not (flat, or a hollow), so that
     *        a body sliding over the flat joins between triangles is not caught on them.
     */
    class TriangleMesh {
    public:
        /** @brief From @p positions (x, y, z each) and @p indices (three a triangle). Triangles of no area are left out. */
        TriangleMesh(const std::vector<Vec3>& positions, const std::vector<std::uint32_t>& indices);

        struct Triangle {
            std::uint32_t v[3];
            Vec3 normal;            ///< Out of its front, length 1.
            std::uint8_t sharp = 0; ///< Bit e: edge e (from v[e] to v[(e + 1) % 3]) is sharp.
        };
        [[nodiscard]] const std::vector<Vec3>& Vertices() const { return _vertices; }
        [[nodiscard]] const std::vector<Triangle>& Triangles() const { return _triangles; }
        /** @brief The box around every vertex. */
        [[nodiscard]] Vec3 Lower() const { return _lower; }
        [[nodiscard]] Vec3 Upper() const { return _upper; }
        /** @brief Every triangle whose box overlaps (lower, upper), handed to @p found by index. */
        template <typename F> void Query(Vec3 lower, Vec3 upper, F&& found) const;

    private:
        struct Node {
            Vec3 lower, upper;
            int left = -1, right = -1;      ///< Children; a leaf has none, and holds [first, first + count) of _order.
            int first = 0, count = 0;
        };
        int Build(int first, int count, std::vector<Vec3>& centres);

        std::vector<Vec3> _vertices;
        std::vector<Triangle> _triangles;
        std::vector<int> _order;            // the triangles, as the tree's leaves hold them
        std::vector<Node> _nodes;
        Vec3 _lower, _upper;
    };

    class Colliders {
    public:
        explicit Colliders(int capacity) : shape(capacity), size(capacity), mesh(capacity), _capacity(capacity) {}
        [[nodiscard]] int Count() const { return _count; }
        std::vector<int> shape;     ///< One of Shape's, a body.
        std::vector<Vec3> size;     ///< As Shape says for each.
        /// A Mesh's triangles. Shared: the same mesh on many bodies is held once.
        std::vector<std::shared_ptr<const TriangleMesh>> mesh;
        /** @brief The next body's collider; returns its index, which is the body's. Throws std::length_error when full. */
        int Add(int shape, Vec3 size);
        /** @brief The next body's collider, a Mesh of @p triangles, as thick as @p thickness (see Shape::Mesh). */
        int AddMesh(std::shared_ptr<const TriangleMesh> triangles, float thickness = 0.f);
        void Clear() { _count = 0; }

    private:
        int _capacity = 0;
        int _count = 0;
    };

    /** @brief A point's feature id: what made it on A (the high 16 bits) and on B (the low 16). */
    [[nodiscard]] constexpr int Feature(const int a, const int b) { return static_cast<int>((static_cast<std::uint32_t>(a) << 16) | (static_cast<std::uint32_t>(b) & 0xFFFFu)); }
    [[nodiscard]] constexpr int FeatureOfA(const int id) { return static_cast<int>(static_cast<std::uint32_t>(id) >> 16); }
    [[nodiscard]] constexpr int FeatureOfB(const int id) { return id & 0xFFFF; }
    /** @brief Bodies a and b as one number, the lower first: the same whichever order they come in. */
    [[nodiscard]] constexpr std::uint64_t PairKey(const int a, const int b)
    {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a < b ? a : b)) << 32) | static_cast<std::uint32_t>(a < b ? b : a);
    }
    [[nodiscard]] constexpr int LowerOf(const std::uint64_t key) { return static_cast<int>(key >> 32); }
    [[nodiscard]] constexpr int HigherOf(const std::uint64_t key) { return static_cast<int>(key & 0xFFFFFFFFu); }

    struct ContactPoint {
        Vec3 at;                    ///< Where it is in the world: halfway between the two surfaces.
        float depth = 0.f;          ///< How far they overlap along the normal: > 0 overlapping, < 0 apart.
        int feature = 0;            ///< Feature(a, b): which parts of A and B made it.
        /// The impulses so far, along the normal and the two directions across it: what the solver sums over its passes,
        /// and what the next step starts from when the same point is found again.
        float normalImpulse = 0.f, tangentImpulse1 = 0.f, tangentImpulse2 = 0.f;
    };

    struct Manifold {
        int a = 0, b = 0;
        Vec3 normal;                ///< Length 1, from A towards B.
        int count = 0;
        std::array<ContactPoint, 4> points;
        [[nodiscard]] std::uint64_t Key() const { return PairKey(a, b); }
    };

    class Contacts {
    public:
        std::vector<Manifold> manifolds;

        [[nodiscard]] int Count() const { return static_cast<int>(manifolds.size()); }
        void Clear() { manifolds.clear(); }
        /** @brief Starts a manifold between @p a and @p b, the normal from a towards b. Add its points, then End it. */
        Manifold& Begin(int a, int b, Vec3 normal);
        /** @brief Adds a point; false, adding nothing, when the manifold has four already. */
        static bool AddPoint(Manifold& m, Vec3 at, float depth, int feature);
        /** @brief Ends the manifold last begun: one given no point is taken away again. */
        void End();
        /** @brief Manifold @p m seen from the other body: A and B swapped, the normal turned round, each feature's halves swapped. */
        static void Flip(Manifold& m);
        /**
         * @brief Warm starting: each point here that @p previous also had (the same pair, the same feature, or failing
         *        that the nearest within 2 cm) starts from the impulses it ended last step with. Both in pair order.
         */
        void CarryImpulsesFrom(const Contacts& previous);
    };

    /**
     * @brief The contact between bodies @p a and @p b, if they touch, as manifolds from a to b in @p out: one, but for a
     *        mesh, where a body touching triangles that face different ways (a floor and a wall) has one for each way.
     *        @p sweep, the step's time: against a mesh's triangles, which have no inside to catch a body that has passed
     *        one, contacts are looked for as far as the two can close in that time, so a fast body is stopped at the
     *        surface (a speculative contact) rather than passing through it in one step.
     */
    void Collide(const Bodies& bodies, const Colliders& colliders, int a, int b, float margin, Contacts& out, float sweep = 0.f);

    // The narrowphase's parts, for the tests.
    namespace narrowphase
    {
        /** @brief The t in [0, 1] of the point of segment a→b closest to @p p; a segment of no length is its start. */
        float ClosestOnSegment(Vec3 p, Vec3 a, Vec3 b);
        /** @brief The closest points of p1→q1 and p2→q2, as s and t in [0, 1] (Ericson 5.1.9). */
        void ClosestBetweenSegments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, float& s, float& t);
        /** @brief Capsule @p a's segment ends: the one at local −half, then +half. */
        void CapsuleEnds(const Bodies& bodies, const Colliders& colliders, int a, Vec3& lower, Vec3& upper);
        /** @brief Corner @p corner (bit 0 +X, bit 1 +Y, bit 2 +Z) of box @p b, in the world. */
        Vec3 BoxCorner(const Bodies& bodies, const Colliders& colliders, int b, int corner);
        /** @brief The point of box @p b closest to @p p, and whether p is inside it. */
        bool ClosestOnBox(const Bodies& bodies, const Colliders& colliders, int b, Vec3 p, Vec3& closest);
        /**
         * @brief Sutherland–Hodgman against one plane: the part of the polygon with n · p ≤ d. A crossing gets the id
         *        4 · clipId + (its edge's start id mod 4). Returns how many points, written to @p out and @p outIds.
         */
        int ClipPolygon(const Vec3* points, const int* ids, int count, Vec3 n, float d, int clipId, Vec3* out, int* outIds);
        /** @brief Keeps four of @p count points, in place: the deepest, the farthest from it, the largest triangle each side. */
        int ReduceToFour(Vec3* points, float* depths, int* ids, int count, Vec3 normal);
        /**
         * @brief The closest points of the convex hulls of two sets of points (GJK): @p onA and @p onB, and the distance
         *        between them; 0, and the points meaningless, when the hulls overlap.
         */
        float ClosestPoints(const Vec3* a, int countA, const Vec3* b, int countB, Vec3& onA, Vec3& onB);
        /** @brief Cone @p b's pyramid: its point, then its base rim's ConeSides points, in the world. */
        void ConeCorners(const Bodies& bodies, const Colliders& colliders, int b, Vec3* corners);
    }

    template <typename F>
    void TriangleMesh::Query(const Vec3 lower, const Vec3 upper, F&& found) const
    {
        if (_nodes.empty()) return;
        int stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& node = _nodes[stack[--top]];
            if (lower.x > node.upper.x || upper.x < node.lower.x || lower.y > node.upper.y || upper.y < node.lower.y ||
                lower.z > node.upper.z || upper.z < node.lower.z) continue;
            if (node.left < 0) {
                for (int i = node.first; i < node.first + node.count; ++i) found(_order[i]);
                continue;
            }
            stack[top++] = node.left;
            stack[top++] = node.right;
        }
    }
}
