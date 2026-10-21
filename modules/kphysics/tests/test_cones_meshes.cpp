// Cones and meshes: a cone's mass and box; the closest points of two hulls; cones standing, lying, struck and stood on;
// a mesh's sharp edges; bodies resting on, sliding over (without catching on the joins), rolling down and wedged in
// meshes; and the same run, to the bit, on threads.
#include "support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    int AddCone(Simulation& sim, const Vec3 at, const float radius = 0.5f, const float height = 1.f, const Quat turn = {}, const float friction = 0.6f)
    {
        Vec3 inertia;
        const float mass = mass::Cone(1000.f, radius, height, inertia);
        const int b = sim.Add(at, turn, mass, inertia, { .friction = friction });
        sim.colliders.Add(Cone, { radius, height, 0.f });
        return b;
    }

    int AddMesh(Simulation& sim, std::shared_ptr<const TriangleMesh> mesh, const Vec3 at = {}, const Quat turn = {}, const float friction = 0.6f)
    {
        const int b = sim.Add(at, turn, 0.f, { 1.f, 1.f, 1.f }, { .friction = friction });
        sim.colliders.AddMesh(std::move(mesh));
        return b;
    }

    /** A flat square floor of @p cells × @p cells squares of @p size, at y = 0, facing up: two triangles a square. */
    std::shared_ptr<const TriangleMesh> Floor(const int cells, const float size)
    {
        std::vector<Vec3> vertices;
        std::vector<std::uint32_t> indices;
        const float half = cells * size / 2.f;
        for (int z = 0; z <= cells; ++z)
            for (int x = 0; x <= cells; ++x) vertices.push_back({ x * size - half, 0.f, z * size - half });
        const auto at = [&](const int x, const int z) { return static_cast<std::uint32_t>(z * (cells + 1) + x); };
        for (int z = 0; z < cells; ++z)
            for (int x = 0; x < cells; ++x)
                // Counter-clockwise seen from above: (x, z), (x, z + 1), (x + 1, z + 1) faces +Y.
                indices.insert(indices.end(), { at(x, z), at(x, z + 1), at(x + 1, z + 1), at(x, z), at(x + 1, z + 1), at(x + 1, z) });
        return std::make_shared<TriangleMesh>(vertices, indices);
    }

    /** A cube of half-extent @p half, as a mesh made for drawing is: four corners of its own a face, so its edges are welded. */
    std::shared_ptr<const TriangleMesh> CubeMesh(const float half)
    {
        std::vector<Vec3> vertices;
        std::vector<std::uint32_t> indices;
        for (int axis = 0; axis < 3; ++axis)
            for (const float side : { 1.f, -1.f }) {
                const int u = (axis + 1) % 3, w = (axis + 2) % 3;
                const auto base = static_cast<std::uint32_t>(vertices.size());
                for (const auto [a, b] : { std::pair { -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } }) {
                    Vec3 v;
                    v[axis] = side * half;
                    v[u] = a * half;
                    v[w] = b * half;
                    vertices.push_back(v);
                }
                // (u, w) turns counter-clockwise about +axis: so for the +side; the other way round for −.
                if (side > 0.f) indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
                else indices.insert(indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
            }
        return std::make_shared<TriangleMesh>(vertices, indices);
    }

    Vec3 Up(const Simulation& sim, const int b) { return Rotate(sim.bodies.orientation[b], { 0.f, 1.f, 0.f }); }
}

TEST(Cones, AConesMassAgainstVoxels)
{
    Vec3 inertia;
    const float r = 0.5f, h = 1.2f;
    const float m = mass::Cone(1000.f, r, h, inertia);
    const int n = 120;
    const double size = 2.0 * r / n;
    double mass = 0, ixx = 0, iyy = 0;
    for (int a = 0; a < n; ++a)
        for (int b = 0; b < static_cast<int>(h / size) + 1; ++b)
            for (int c = 0; c < n; ++c) {
                const double x = -r + (a + 0.5) * size, y = -h / 4.0 + (b + 0.5) * size, z = -r + (c + 0.5) * size;
                if (y > 0.75 * h) continue;
                const double radius = r * (0.75 * h - y) / h;
                if (x * x + z * z > radius * radius) continue;
                const double dm = 1000.0 * size * size * size;
                mass += dm;
                ixx += dm * (y * y + z * z);
                iyy += dm * (x * x + z * z);
            }
    EXPECT_NEAR(mass, m, mass * 0.01);
    EXPECT_NEAR(ixx, inertia.x, ixx * 0.02) << "about its centre of mass, a quarter of its height above its base";
    EXPECT_NEAR(iyy, inertia.y, iyy * 0.01);
}

TEST(Cones, AConesBox)
{
    Simulation sim(2);
    sim.contactMargin = 0.f;
    const int upright = AddCone(sim, {});
    const int lying = AddCone(sim, { 5.f, 0.f, 0.f }, 0.5f, 1.f, Turn({ 0.f, 0.f, 1.f }, -90.f));     // its point towards +X
    sim.broadphase.Bounds(sim, upright, 0.f);
    sim.broadphase.Bounds(sim, lying, 0.f);
    EXPECT_NEAR(-0.25f, sim.broadphase.lower[upright].y, 1e-5f);
    EXPECT_NEAR(0.75f, sim.broadphase.upper[upright].y, 1e-5f);
    EXPECT_NEAR(0.5f, sim.broadphase.upper[upright].x, 1e-5f);
    EXPECT_NEAR(5.75f, sim.broadphase.upper[lying].x, 1e-5f) << "its point";
    EXPECT_NEAR(4.75f, sim.broadphase.lower[lying].x, 1e-5f) << "its base";
    EXPECT_NEAR(0.5f, sim.broadphase.upper[lying].y, 1e-5f);
}

TEST(Cones, TheClosestPointsOfTwoHulls)
{
    const Vec3 square[] { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 0.f, 1.f }, { 0.f, 0.f, 1.f } };
    const Vec3 above[] { { 0.5f, 2.f, 0.5f } };
    Vec3 onA, onB;
    EXPECT_NEAR(2.f, narrowphase::ClosestPoints(square, 4, above, 1, onA, onB), 1e-5f);
    EXPECT_NEAR(0.f, onA.y, 1e-5f);
    EXPECT_NEAR(0.5f, onA.x, 1e-5f);
    const Vec3 segment[] { { 2.f, 0.5f, 0.5f }, { 2.f, 0.5f, 3.f } };
    EXPECT_NEAR(std::sqrt(1.25f), narrowphase::ClosestPoints(square, 4, segment, 2, onA, onB), 1e-5f) << "from the edge x = 1";
    const Vec3 through[] { { 0.5f, -1.f, 0.5f }, { 0.5f, 1.f, 0.5f } };
    EXPECT_EQ(0.f, narrowphase::ClosestPoints(square, 4, through, 2, onA, onB)) << "crossing: they overlap";
}

TEST(Cones, AConeStandsOnItsBase)
{
    Simulation sim(4);
    Ground(sim);
    const int cone = AddCone(sim, { 0.f, 0.27f, 0.f });
    RunFor(sim, 3.f);
    EXPECT_NEAR(0.25f, sim.bodies.position[cone].y, 0.01f) << "its centre of mass a quarter of its height up";
    EXPECT_GT(Up(sim, cone).y, 0.999f) << "upright";
    EXPECT_FALSE(sim.awake[cone]) << "and asleep";
    ASSERT_EQ(0, sim.contacts.Count());
}

TEST(Cones, AConeLaidOnItsSideStaysThere)
{
    // On its side a cone touches the ground along a line from its point to its rim (as a pyramid, along one of its
    // faces): it settles there, its point where it was, its axis risen by its half-angle from the ground.
    Simulation sim(4);
    Ground(sim);
    const float half = std::atan(0.5f);                         // the half-angle: the base's radius over the height
    const int cone = AddCone(sim, {}, 0.5f, 1.f, Turn({ 0.f, 0.f, 1.f }, -90.f + half * 180.f / std::numbers::pi_v<float>));
    sim.broadphase.Bounds(sim, cone, 0.f);
    sim.bodies.position[cone].y -= sim.broadphase.lower[cone].y - 0.002f;
    Vec3 corners[ConeSides + 1];
    narrowphase::ConeCorners(sim.bodies, sim.colliders, cone, corners);
    const Vec3 point = corners[0];
    RunFor(sim, 3.f);
    narrowphase::ConeCorners(sim.bodies, sim.colliders, cone, corners);
    EXPECT_LT(Length(corners[0] - point), 0.05f) << "its point stayed where it was";
    EXPECT_NEAR(std::sin(half), Up(sim, cone).y, 0.05f) << "on its side";
    EXPECT_FALSE(sim.awake[cone]) << "and at rest";
}

TEST(Cones, AConeStandsOnABoxAndABoxOnACone)
{
    Simulation sim(8);
    Ground(sim);
    const int box = AddBox(sim, { 0.f, 0.5f, 0.f });
    const int cone = AddCone(sim, { 0.f, 1.27f, 0.f });
    const int base = AddCone(sim, { 4.f, 0.75f, 0.f }, 0.5f, 1.f, Turn({ 1.f, 0.f, 0.f }, 180.f));    // its point up
    const int lid = AddBox(sim, { 4.f, 1.f + 0.26f, 0.f }, { .half = { 0.3f, 0.25f, 0.3f } });
    (void)base;
    RunFor(sim, 3.f);
    EXPECT_NEAR(1.25f, sim.bodies.position[cone].y, 0.01f);
    EXPECT_GT(Up(sim, cone).y, 0.999f);
    EXPECT_NEAR(0.5f, sim.bodies.position[box].y, 0.01f);
    EXPECT_NEAR(1.25f, sim.bodies.position[lid].y, 0.02f) << "a box on an upturned cone's flat base";
}

TEST(Cones, ASphereSlidesOffAConesPoint)
{
    Simulation sim(4);
    Ground(sim);
    AddCone(sim, { 0.f, 0.25f, 0.f }, 0.5f, 1.f);
    const int ball = AddBall(sim, { 0.05f, 2.f, 0.f }, 0.2f);
    RunFor(sim, 3.f);
    EXPECT_NEAR(0.2f, sim.bodies.position[ball].y, 0.01f) << "on the ground, not balanced on the point";
    EXPECT_GT(sim.bodies.position[ball].x, 0.3f) << "off the side it leant to";
}

TEST(Cones, TwoConesSideBySide)
{
    Scene s;
    const int a = s.sim.Add({}, {}, 1.f, { 1.f, 1.f, 1.f });
    s.sim.colliders.Add(Cone, { 0.5f, 1.f, 0.f });
    const int b = s.sim.Add({ 0.95f, 0.f, 0.f }, {}, 1.f, { 1.f, 1.f, 1.f });
    s.sim.colliders.Add(Cone, { 0.5f, 1.f, 0.f });
    const Contacts ab = s.Collide(a, b), ba = s.Collide(b, a);
    ASSERT_EQ(1, ab.Count());
    ASSERT_EQ(1, ba.Count());
    EXPECT_GT(ab.manifolds[0].normal.x, 0.8f) << "from A towards B";
    EXPECT_LT(ba.manifolds[0].normal.x, -0.8f) << "and the other way round";
    // The rims overlap by 5 cm where they meet; the other points are of the rims close by, within the margin.
    float deepest = -1.f;
    for (int p = 0; p < ab.manifolds[0].count; ++p) deepest = std::max(deepest, ab.manifolds[0].points[p].depth);
    EXPECT_GT(deepest, 0.04f) << "as pyramids of 16 sides, a little less than round cones";
    EXPECT_LE(deepest, 0.0501f);
}

TEST(Meshes, SharpEdgesAreTheFoldsAndTheEdgesOfTheMesh)
{
    const auto floor = Floor(1, 2.f);
    ASSERT_EQ(2u, floor->Triangles().size());
    int sharp = 0;
    for (const auto& t : floor->Triangles()) sharp += std::popcount(t.sharp);
    EXPECT_EQ(4, sharp) << "a square's four sides; its diagonal is flat";
    const auto cube = CubeMesh(0.5f);
    ASSERT_EQ(12u, cube->Triangles().size());
    sharp = 0;
    for (const auto& t : cube->Triangles()) sharp += std::popcount(t.sharp);
    EXPECT_EQ(24, sharp) << "every one of a cube's twelve edges, from both its faces, though each face has corners of its own";
    for (const auto& t : cube->Triangles()) EXPECT_GT(Length(t.normal), 0.99f);
}

TEST(Meshes, TheTreeFindsEveryTriangleItShould)
{
    const auto floor = Floor(30, 0.5f);
    const Vec3 lower { -1.1f, -1.f, 0.3f }, upper { 0.4f, 1.f, 1.6f };
    std::vector<int> found;
    floor->Query(lower, upper, [&](const int t) { found.push_back(t); });
    std::vector<int> brute;
    for (int t = 0; t < static_cast<int>(floor->Triangles().size()); ++t) {
        Vec3 l { 1e30f, 1e30f, 1e30f }, u { -1e30f, -1e30f, -1e30f };
        for (const auto v : floor->Triangles()[t].v)
            for (int c = 0; c < 3; ++c) {
                l[c] = std::min(l[c], floor->Vertices()[v][c]);
                u[c] = std::max(u[c], floor->Vertices()[v][c]);
            }
        if (l.x <= upper.x && u.x >= lower.x && l.y <= upper.y && u.y >= lower.y && l.z <= upper.z && u.z >= lower.z) brute.push_back(t);
    }
    // A leaf's every triangle, when its box is reached: a few more than overlap, every one that does.
    std::ranges::sort(found);
    EXPECT_TRUE(std::ranges::includes(found, brute));
    EXPECT_LT(found.size(), brute.size() * 2);
}

TEST(Meshes, ABoxRestsOnAMeshFloorAndSleeps)
{
    Simulation sim(4);
    sim.substeps = 2;
    AddMesh(sim, Floor(20, 1.f));
    const int box = AddBox(sim, { 0.3f, 0.52f, 0.2f }, { .turn = Turn({ 0.f, 1.f, 0.f }, 20.f) });
    RunFor(sim, 3.f);
    EXPECT_NEAR(0.5f, sim.bodies.position[box].y, 0.01f);
    EXPECT_GT(Up(sim, box).y, 0.999f);
    EXPECT_FALSE(sim.awake[box]);
}

TEST(Meshes, ABoxSlidesOverTheJoinsBetweenTrianglesWithoutCatching)
{
    // No friction, over a floor of 800 triangles: a box sliding at 4 m/s keeps its speed and never hops, as it would on
    // every join if it were pushed by the next triangle's edge rather than the floor.
    Simulation sim(4);
    sim.substeps = 2;
    sim.islands.allowSleep = false;
    AddMesh(sim, Floor(20, 1.f), {}, {}, 0.f);
    const int box = AddBox(sim, { -8.f, 0.5f, 0.3f }, { .half = { 0.5f, 0.5f, 0.5f }, .friction = 0.f });
    sim.bodies.velocity[box] = { 4.f, 0.f, 1.f };
    float hop = 0.f, slowest = 1e9f;
    RunFor(sim, 3.f, [&](int) {
        hop = std::max(hop, std::abs(sim.bodies.velocity[box].y));
        slowest = std::min(slowest, sim.bodies.velocity[box].x);
    });
    EXPECT_LT(hop, 0.05f) << "it hopped on a join";
    EXPECT_GT(slowest, 3.95f) << "it caught on a join";
    EXPECT_NEAR(0.5f, sim.bodies.position[box].y, 0.01f);
}

TEST(Meshes, ABallRollsDownARamp)
{
    Simulation sim(4);
    const auto ramp = Floor(4, 2.f);
    AddMesh(sim, ramp, {}, Turn({ 0.f, 0.f, 1.f }, 15.f));                     // falling towards −X
    const int ball = AddBall(sim, { 2.f, 0.f, 0.f }, 0.3f);
    sim.bodies.position[ball].y = 2.f * std::tan(15.f * std::numbers::pi_v<float> / 180.f) + 0.3f / std::cos(15.f * std::numbers::pi_v<float> / 180.f) + 0.005f;
    RunFor(sim, 1.f);
    // 5/7 g sin 15° = 1.8 m/s² down the slope, rolling: 0.9 m in the second.
    EXPECT_LT(sim.bodies.position[ball].x, 1.3f) << "down the slope";
    EXPECT_LT(sim.bodies.velocity[ball].x, -1.f);
    // Rolling, not sliding: ω r matches the speed along the slope.
    const float along = Length(sim.bodies.velocity[ball]), turning = Length(sim.bodies.angularVelocity[ball]) * 0.3f;
    EXPECT_NEAR(along, turning, 0.05f * along);
}

TEST(Meshes, ABoxInACornerTouchesTheFloorAndTheWall)
{
    // A floor and a wall, one mesh, meeting in a hollow edge: a box pushed into the corner has two manifolds with it, one
    // up from the floor and one out from the wall.
    std::vector<Vec3> v { { -2.f, 0.f, -2.f }, { -2.f, 0.f, 2.f }, { 2.f, 0.f, 2.f }, { 2.f, 0.f, -2.f },     // floor
                          { 2.f, 0.f, -2.f }, { 2.f, 0.f, 2.f }, { 2.f, 4.f, 2.f }, { 2.f, 4.f, -2.f } };    // wall at x = 2, facing −X
    std::vector<std::uint32_t> i { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    Simulation sim(4);
    sim.substeps = 2;
    sim.islands.allowSleep = false;
    AddMesh(sim, std::make_shared<TriangleMesh>(v, i));
    const int box = AddBox(sim, { 1.f, 0.5f, 0.f });
    RunFor(sim, 1.f, [&](int) { sim.AddForce(box, { 20000.f, 0.f, 0.f }); });      // a tonne: more than friction holds
    EXPECT_NEAR(1.5f, sim.bodies.position[box].x, 0.01f) << "against the wall";
    EXPECT_NEAR(0.5f, sim.bodies.position[box].y, 0.01f) << "on the floor";
    int manifolds = 0;
    bool up = false, out = false;
    for (const Manifold& m : sim.contacts.manifolds) {
        if (m.Key() != PairKey(0, box)) continue;
        ++manifolds;
        up |= m.normal.y > 0.99f || m.normal.y < -0.99f;
        out |= m.normal.x > 0.99f || m.normal.x < -0.99f;
    }
    EXPECT_EQ(2, manifolds);
    EXPECT_TRUE(up && out);
}

TEST(Meshes, BodiesFallOntoAMeshCubeAndStayOnIt)
{
    Simulation sim(8);
    sim.substeps = 2;
    Ground(sim);
    AddMesh(sim, CubeMesh(1.f), { 0.f, 1.f, 0.f });
    const int box = AddBox(sim, { 0.4f, 3.f, -0.4f });
    const int cone = AddCone(sim, { -0.5f, 4.f, 0.5f }, 0.3f, 0.6f);
    const int ball = AddBall(sim, { 0.4f, 6.f, -0.4f }, 0.3f);
    RunFor(sim, 4.f);
    EXPECT_NEAR(2.5f, sim.bodies.position[box].y, 0.02f);
    EXPECT_NEAR(2.15f, sim.bodies.position[cone].y, 0.02f) << "standing on the cube's top";
    EXPECT_GT(sim.bodies.position[ball].y, 2.f) << "on the cube or the box, not through them";
}

TEST(Meshes, AMeshRunIsTheSameToTheBitOnThreads)
{
    const auto run = [](const int threads) {
        Simulation sim(260);
        sim.substeps = 2;
        sim.threads = threads;
        sim.solver.byColour = true;
        AddMesh(sim, Floor(20, 2.f));
        KotlinRandom random(4);
        for (int i = 0; i < 250; ++i) {
            const Vec3 at { (random.NextFloat() - 0.5f) * 10.f, 0.6f + i * 0.05f, (random.NextFloat() - 0.5f) * 10.f };
            const Quat turn = Turn({ random.NextFloat(), random.NextFloat(), random.NextFloat() }, random.NextFloat() * 90.f);
            if (i % 3 == 0) AddCone(sim, at, 0.4f, 0.8f, turn);
            else if (i % 3 == 1) AddBox(sim, at, { .half = { 0.3f, 0.3f, 0.3f }, .turn = turn });
            else AddBall(sim, at, 0.3f);
        }
        for (int k = 0; k < 180; ++k) sim.Step(Step);
        std::vector<float> state;
        for (int b = 0; b < sim.bodies.Count(); ++b) {
            const Vec3 p = sim.bodies.position[b];
            state.insert(state.end(), { p.x, p.y, p.z });
        }
        return state;
    };
    const auto one = run(1), many = run(4);
    EXPECT_EQ(0, std::memcmp(one.data(), many.data(), one.size() * sizeof(float)));
    float lowest = 1e9f;
    for (std::size_t b = 1; b < one.size() / 3; ++b) lowest = std::min(lowest, one[b * 3 + 1]);
    EXPECT_GT(lowest, 0.18f) << "nothing fell through the floor (an upright cone's centre is 0.2 m up)";
}

TEST(Meshes, AFastBodyIsStoppedByAThinWallNotPassedThroughIt)
{
    // A wall of two triangles, nothing behind it: a small ball at 20 m/s travels 0.33 m a step, far more than its own
    // size and the margin. Contacts are looked for as far as it can go in the step, so it stops at the wall.
    std::vector<Vec3> v { { 0.f, 0.f, -2.f }, { 0.f, 0.f, 2.f }, { 0.f, 4.f, 2.f }, { 0.f, 4.f, -2.f } };     // x = 0, facing −X
    std::vector<std::uint32_t> i { 0, 1, 2, 0, 2, 3 };
    for (const float speed : { 20.f, 40.f }) {
        Simulation sim(4);
        sim.gravity = {};
        AddMesh(sim, std::make_shared<TriangleMesh>(v, i));
        const int ball = AddBall(sim, { -2.f, 2.f, 0.f }, 0.08f);
        sim.bodies.velocity[ball] = { speed, 0.f, 0.f };
        RunFor(sim, 0.5f);
        EXPECT_LT(sim.bodies.position[ball].x, 0.f) << "at " << speed << " m/s it passed through";
    }
}

TEST(Meshes, AThickMeshPushesOutABodyDeepInItsWall)
{
    // A floor given a thickness of 1 m: a ball sunk 0.6 m below it (its centre behind the floor, far more than a thin
    // floor would catch) is pushed back up onto it. The same floor without thickness lets it go.
    for (const float thickness : { 1.f, 0.f }) {
        Simulation sim(4);
        sim.substeps = 2;
        const int floor = sim.Add({}, {}, 0.f, { 1.f, 1.f, 1.f });
        sim.colliders.AddMesh(Floor(4, 1.f), thickness);
        (void)floor;
        const int ball = AddBall(sim, { 0.3f, -0.6f, 0.2f }, 0.2f);
        RunFor(sim, 2.f);
        if (thickness > 0.f) EXPECT_NEAR(0.2f, sim.bodies.position[ball].y, 0.01f) << "back on the floor";
        else EXPECT_LT(sim.bodies.position[ball].y, -1.f) << "a thin floor does not catch what is far behind it";
    }
}
