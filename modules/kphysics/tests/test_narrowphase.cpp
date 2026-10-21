// The pair functions, each through Collide as the simulation calls them, and each the other way round too: the same
// manifold, A and B swapped and the normal turned. As NarrowphaseTest.kt checks them.
#include "support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    const float R2 = std::sqrt(0.5f);

    void ExpectVec(const Vec3 expected, const Vec3 actual, const float tolerance = 1e-5f, const std::string& what = "")
    {
        EXPECT_NEAR(expected.x, actual.x, tolerance) << what;
        EXPECT_NEAR(expected.y, actual.y, tolerance) << what;
        EXPECT_NEAR(expected.z, actual.z, tolerance) << what;
    }

    void ExpectOne(const Contacts& c, const int a, const int b, const Vec3 normal, const float depth, const std::string& what)
    {
        ASSERT_EQ(1, c.Count()) << what << ": one manifold";
        const Manifold& m = c.manifolds[0];
        EXPECT_EQ(a, m.a) << what;
        EXPECT_EQ(b, m.b) << what;
        ExpectVec(normal, m.normal, 1e-4f, what + ": normal from A to B");
        for (int p = 0; p < m.count; ++p) EXPECT_NEAR(depth, m.points[p].depth, 1e-4f) << what << ": depth of point " << p;
    }

    /** Collide(b, a) is Collide(a, b) seen from b: swapped, the normal turned, the same points and depths. */
    void ExpectSymmetric(const Scene& s, const int a, const int b, const std::string& what)
    {
        const Contacts ab = s.Collide(a, b), ba = s.Collide(b, a);
        ASSERT_EQ(ab.Count(), ba.Count()) << what << ": both ways or neither";
        if (ab.Count() == 0) return;
        EXPECT_EQ(b, ba.manifolds[0].a) << what;
        ExpectVec(-ab.manifolds[0].normal, ba.manifolds[0].normal, 1e-5f, what + ": normal turned round");
        ASSERT_EQ(ab.manifolds[0].count, ba.manifolds[0].count) << what;
        for (int p = 0; p < ab.manifolds[0].count; ++p) {
            EXPECT_NEAR(ab.manifolds[0].points[p].depth, ba.manifolds[0].points[p].depth, 1e-5f) << what;
            const int id = ab.manifolds[0].points[p].feature;
            EXPECT_EQ(Feature(FeatureOfB(id), FeatureOfA(id)), ba.manifolds[0].points[p].feature) << what << ": halves swapped";
        }
    }

    Vec3 At(const Contacts& c, const int p) { return c.manifolds[0].points[p].at; }
    std::set<int> Features(const Contacts& c)
    {
        std::set<int> ids;
        for (int p = 0; p < c.manifolds[0].count; ++p) ids.insert(c.manifolds[0].points[p].feature);
        return ids;
    }
}

TEST(Narrowphase, ASphereOnAPlane)
{
    Scene s;
    const int ground = s.AddPlane({});
    const int ball = s.AddSphere({ 0.f, 0.49f, 0.f }, 0.5f);
    const Contacts c = s.Collide(ball, ground);
    ExpectOne(c, ball, ground, { 0.f, -1.f, 0.f }, 0.01f, "resting, 1 cm in");
    ExpectVec({ 0.f, -0.005f, 0.f }, At(c, 0));
    EXPECT_EQ(Feature(0, 0), c.manifolds[0].points[0].feature);
    ExpectSymmetric(s, ball, ground, "sphere and plane");
    s.sim.bodies.position[ball].y = 0.51f;
    ExpectOne(s.Collide(ball, ground), ball, ground, { 0.f, -1.f, 0.f }, -0.01f, "1 cm above: within the margin");
    s.sim.bodies.position[ball].y = 0.6f;
    EXPECT_EQ(0, s.Collide(ball, ground).Count());
    s.sim.bodies.position[ball].y = -0.3f;
    ExpectOne(s.Collide(ball, ground), ball, ground, { 0.f, -1.f, 0.f }, 0.8f, "sunk");
}

TEST(Narrowphase, ASphereOnATurnedPlane)
{
    Scene s;
    const int wall = s.AddPlane({}, Turn({ 0.f, 0.f, 1.f }, 90.f));        // facing −X
    const int ball = s.AddSphere({ -0.4f, 2.f, 3.f }, 0.5f);
    ExpectOne(s.Collide(ball, wall), ball, wall, { 1.f, 0.f, 0.f }, 0.1f, "against a wall");
}

TEST(Narrowphase, TwoSpheres)
{
    Scene s;
    const int a = s.AddSphere({}, 0.5f);
    const int b = s.AddSphere({ 0.9f, 0.f, 0.f }, 0.5f);
    const Contacts c = s.Collide(a, b);
    ExpectOne(c, a, b, { 1.f, 0.f, 0.f }, 0.1f, "overlapping 10 cm");
    ExpectVec({ 0.45f, 0.f, 0.f }, At(c, 0));
    ExpectSymmetric(s, a, b, "two spheres");
    s.sim.bodies.position[b].x = 1.1f;
    EXPECT_EQ(0, s.Collide(a, b).Count());
    s.sim.bodies.position[b].x = 0.f;
    ExpectOne(s.Collide(a, b), a, b, { 0.f, 1.f, 0.f }, 1.f, "one inside the other: apart along +Y");
}

TEST(Narrowphase, TwoSpheresOfDifferentSizes)
{
    Scene s;
    const int big = s.AddSphere({ 1.f, 2.f, 3.f }, 0.5f);
    const int small = s.AddSphere({ 1.36f, 2.48f, 3.f }, 0.2f);
    const Contacts c = s.Collide(big, small);
    ExpectOne(c, big, small, { 0.6f, 0.8f, 0.f }, 0.1f, "diagonally");
    ExpectVec({ 1.27f, 2.36f, 3.f }, At(c, 0), 1e-5f, "halfway between the surfaces, not the centres");
    ExpectSymmetric(s, big, small, "two spheres of different sizes");
    s.sim.bodies.position[small] = { 1.f, 2.f, 3.f };
    const Contacts same = s.Collide(big, small);
    ExpectOne(same, big, small, { 0.f, 1.f, 0.f }, 0.7f, "the same centre");
    ExpectVec({ 1.f, 2.15f, 3.f }, At(same, 0));
}

TEST(Narrowphase, TheClosestPointsOfSegments)
{
    using namespace narrowphase;
    EXPECT_NEAR(0.25f, ClosestOnSegment({ 0.5f, 1.f, 0.f }, {}, { 2.f, 0.f, 0.f }), 1e-6f);
    EXPECT_EQ(0.f, ClosestOnSegment({ -3.f, 1.f, 0.f }, {}, { 2.f, 0.f, 0.f }));
    EXPECT_EQ(1.f, ClosestOnSegment({ 5.f, 1.f, 0.f }, {}, { 2.f, 0.f, 0.f }));
    EXPECT_EQ(0.f, ClosestOnSegment({ 5.f, 1.f, 0.f }, {}, {})) << "a segment of no length";
    float s = 0.f, t = 0.f;
    ClosestBetweenSegments({ -1.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, -1.f }, { 0.f, 1.f, 1.f }, s, t);
    EXPECT_NEAR(0.5f, s, 1e-6f);
    EXPECT_NEAR(0.5f, t, 1e-6f);
    ClosestBetweenSegments({}, { 1.f, 0.f, 0.f }, { 3.f, 1.f, 0.f }, { 3.f, 2.f, 0.f }, s, t);
    EXPECT_NEAR(1.f, s, 1e-6f);
    EXPECT_NEAR(0.f, t, 1e-6f);
    ClosestBetweenSegments({}, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 1.f, 1.f, 0.f }, s, t);
    EXPECT_NEAR(s, t, 1e-5f) << "parallel: any pair facing each other";
    ClosestBetweenSegments({ 2.f, 2.f, 2.f }, { 2.f, 2.f, 2.f }, {}, {}, s, t);
    EXPECT_EQ(0.f, s);
    EXPECT_EQ(0.f, t);
}

TEST(Narrowphase, ACapsulesEnds)
{
    Scene s;
    const int c = s.AddCapsule({ 1.f, 2.f, 3.f }, 0.25f, 0.25f, Turn({ 0.f, 0.f, 1.f }, 90.f));
    Vec3 lower, upper;
    narrowphase::CapsuleEnds(s.sim.bodies, s.sim.colliders, c, lower, upper);
    ExpectVec({ 1.25f, 2.f, 3.f }, lower, 1e-6f);
    ExpectVec({ 0.75f, 2.f, 3.f }, upper, 1e-6f);
}

TEST(Narrowphase, ACapsuleOnAPlane)
{
    Scene s;
    const int ground = s.AddPlane({});
    const int standing = s.AddCapsule({ 0.f, 0.49f, 0.f }, 0.25f, 0.25f);
    const Contacts c = s.Collide(standing, ground);
    ExpectOne(c, standing, ground, { 0.f, -1.f, 0.f }, 0.01f, "standing");
    EXPECT_EQ(1, c.manifolds[0].count);
    const int lying = s.AddCapsule({ 3.f, 0.24f, 0.f }, 0.25f, 0.25f, Turn({ 0.f, 0.f, 1.f }, 90.f));
    const Contacts d = s.Collide(lying, ground);
    ExpectOne(d, lying, ground, { 0.f, -1.f, 0.f }, 0.01f, "lying");
    ASSERT_EQ(2, d.manifolds[0].count);
    const float x0 = std::min(At(d, 0).x, At(d, 1).x), x1 = std::max(At(d, 0).x, At(d, 1).x);
    EXPECT_NEAR(2.75f, x0, 1e-5f);
    EXPECT_NEAR(3.25f, x1, 1e-5f);
    EXPECT_EQ((std::set { Feature(0, 0), Feature(1, 0) }), Features(d));
    ExpectSymmetric(s, lying, ground, "capsule and plane");
}

TEST(Narrowphase, ACapsuleAndASphere)
{
    Scene s;
    const int cap = s.AddCapsule({}, 0.25f, 0.25f);
    const int ball = s.AddSphere({ 0.7f, 0.1f, 0.f }, 0.5f);
    const Contacts c = s.Collide(cap, ball);
    ExpectOne(c, cap, ball, { 1.f, 0.f, 0.f }, 0.05f, "beside the middle of the segment");
    ExpectVec({ 0.225f, 0.1f, 0.f }, At(c, 0));
    EXPECT_EQ(Feature(2, 0), c.manifolds[0].points[0].feature);
    ExpectSymmetric(s, cap, ball, "capsule and sphere");
}

TEST(Narrowphase, TwoCapsulesCrossingAndSideBySide)
{
    Scene s;
    const int a = s.AddCapsule({}, 0.25f, 0.25f);
    const int b = s.AddCapsule({ 0.45f, 0.f, 0.f }, 0.25f, 0.25f, Turn({ 1.f, 0.f, 0.f }, 90.f));
    const Contacts c = s.Collide(a, b);
    ExpectOne(c, a, b, { 1.f, 0.f, 0.f }, 0.05f, "crossed");
    ExpectVec({ 0.225f, 0.f, 0.f }, At(c, 0));
    EXPECT_EQ(Feature(2, 2), c.manifolds[0].points[0].feature);
    ExpectSymmetric(s, a, b, "crossed capsules");

    Scene t;
    const Quat lying = Turn({ 0.f, 0.f, 1.f }, 90.f);
    const int under = t.AddCapsule({}, 0.25f, 0.25f, lying);
    const int over = t.AddCapsule({ 0.2f, 0.45f, 0.f }, 0.25f, 0.25f, lying);
    const Contacts d = t.Collide(under, over);
    ExpectOne(d, under, over, { 0.f, 1.f, 0.f }, 0.05f, "one lying on the other");
    ASSERT_EQ(2, d.manifolds[0].count);
    const float x0 = std::min(At(d, 0).x, At(d, 1).x), x1 = std::max(At(d, 0).x, At(d, 1).x);
    EXPECT_NEAR(-0.05f, x0, 1e-4f);
    EXPECT_NEAR(0.25f, x1, 1e-4f);
    EXPECT_EQ((std::set { Feature(0, 0), Feature(1, 1) }), Features(d));
    ExpectSymmetric(t, under, over, "capsules side by side");
}

TEST(Narrowphase, ABoxsCornersAndClosestPoint)
{
    Scene s;
    const int box = s.AddBox({ 1.f, 2.f, 3.f }, { 0.5f, 1.f, 1.5f }, Turn({ 0.f, 1.f, 0.f }, 90.f));
    ExpectVec({ 2.5f, 1.f, 2.5f }, narrowphase::BoxCorner(s.sim.bodies, s.sim.colliders, box, 5));
    ExpectVec({ -0.5f, 3.f, 3.5f }, narrowphase::BoxCorner(s.sim.bodies, s.sim.colliders, box, 2));
    Scene t;
    const int cube = t.AddBox({ 1.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f }, Turn({ 0.f, 1.f, 0.f }, 90.f));
    Vec3 closest;
    EXPECT_FALSE(narrowphase::ClosestOnBox(t.sim.bodies, t.sim.colliders, cube, { 3.f, 2.f, 0.2f }, closest));
    ExpectVec({ 1.5f, 0.5f, 0.2f }, closest);
    EXPECT_TRUE(narrowphase::ClosestOnBox(t.sim.bodies, t.sim.colliders, cube, { 1.1f, 0.2f, -0.3f }, closest));
    ExpectVec({ 1.1f, 0.2f, -0.3f }, closest);
}

TEST(Narrowphase, ABoxOnAPlane)
{
    Scene s;
    const int ground = s.AddPlane({});
    const int flat = s.AddBox({ 0.f, 0.49f, 0.f }, { 0.5f, 0.5f, 0.5f });
    const Contacts c = s.Collide(flat, ground);
    ExpectOne(c, flat, ground, { 0.f, -1.f, 0.f }, 0.01f, "flat");
    ASSERT_EQ(4, c.manifolds[0].count);
    EXPECT_EQ((std::set { Feature(0, 0), Feature(1, 0), Feature(4, 0), Feature(5, 0) }), Features(c));
    for (int p = 0; p < 4; ++p) {
        EXPECT_NEAR(-0.005f, At(c, p).y, 1e-5f);
        EXPECT_NEAR(0.5f, std::abs(At(c, p).x), 1e-5f);
    }
    ExpectSymmetric(s, flat, ground, "box and plane");
    const int edge = s.AddBox({ 3.f, R2 - 0.01f, 0.f }, { 0.5f, 0.5f, 0.5f }, Turn({ 0.f, 0.f, 1.f }, 45.f));
    const Contacts d = s.Collide(edge, ground);
    ExpectOne(d, edge, ground, { 0.f, -1.f, 0.f }, 0.01f, "on an edge");
    EXPECT_EQ(2, d.manifolds[0].count);
    s.sim.bodies.position[flat].y = 0.6f;
    EXPECT_EQ(0, s.Collide(flat, ground).Count());
}

TEST(Narrowphase, ASphereOnABox)
{
    Scene s;
    const int box = s.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int ball = s.AddSphere({ 0.2f, 0.95f, 0.1f }, 0.5f);
    const Contacts c = s.Collide(box, ball);
    ExpectOne(c, box, ball, { 0.f, 1.f, 0.f }, 0.05f, "on its top face");
    ExpectVec({ 0.2f, 0.475f, 0.1f }, At(c, 0));
    ExpectSymmetric(s, box, ball, "box and sphere");
    s.sim.bodies.position[ball] = { 0.8f, 0.8f, 0.f };
    ExpectOne(s.Collide(box, ball), box, ball, { R2, R2, 0.f }, 0.5f - 0.3f * std::sqrt(2.f), "on its edge");
    s.sim.bodies.position[ball] = { 0.1f, 0.4f, 0.f };
    s.sim.colliders.size[ball].x = 0.2f;
    const Contacts inside = s.Collide(box, ball);
    ExpectOne(inside, box, ball, { 0.f, 1.f, 0.f }, 0.3f, "its centre inside: out through the top");
    ExpectVec({ 0.1f, 0.35f, 0.f }, At(inside, 0));
}

TEST(Narrowphase, ASphereOnATurnedBox)
{
    Scene s;
    const int box = s.AddBox({}, { 0.5f, 0.5f, 0.5f }, Turn({ 0.f, 1.f, 0.f }, 45.f));
    const int ball = s.AddSphere({ 0.f, 0.95f, 0.f }, 0.5f);
    ExpectOne(s.Collide(box, ball), box, ball, { 0.f, 1.f, 0.f }, 0.05f, "the same top");
    s.sim.bodies.position[ball] = { R2 + 0.51f, 0.f, 0.f };
    ExpectOne(s.Collide(box, ball), box, ball, { 1.f, 0.f, 0.f }, -0.01f, "1 cm from its edge");
    s.sim.bodies.position[ball].x = R2 + 0.6f;
    EXPECT_EQ(0, s.Collide(box, ball).Count());
}

TEST(Narrowphase, CapsulesOnABox)
{
    Scene s;
    const int box = s.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int lying = s.AddCapsule({ 0.f, 0.74f, 0.f }, 0.25f, 0.25f, Turn({ 0.f, 0.f, 1.f }, 90.f));
    const Contacts c = s.Collide(box, lying);
    ExpectOne(c, box, lying, { 0.f, 1.f, 0.f }, 0.01f, "lying on its top");
    EXPECT_EQ((std::set { Feature(0, 0), Feature(0, 1) }), Features(c));
    ExpectSymmetric(s, box, lying, "box and capsule");

    Scene t;
    const int cube = t.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int standing = t.AddCapsule({ 0.1f, 0.99f, 0.f }, 0.25f, 0.25f);
    const Contacts d = t.Collide(cube, standing);
    ExpectOne(d, cube, standing, { 0.f, 1.f, 0.f }, 0.01f, "standing on its top");
    EXPECT_EQ((std::set { Feature(0, 0) }), Features(d));

    Scene u;
    const int under = u.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int across = u.AddCapsule({ 0.f, 0.74f, 0.f }, 0.25f, 2.f, Turn({ 0.f, 0.f, 1.f }, 90.f));
    const Contacts e = u.Collide(under, across);
    ExpectOne(e, under, across, { 0.f, 1.f, 0.f }, 0.01f, "across the top: from its middle");
    EXPECT_EQ((std::set { Feature(0, 2) }), Features(e));
    EXPECT_LE(std::abs(At(e, 0).x), 0.5f);
}

TEST(Narrowphase, FindContactsTestsEveryPairOnce)
{
    Scene s;
    const int ground = s.AddPlane({});
    s.AddSphere({ 0.f, 0.49f, 0.f }, 0.5f);
    s.AddBox({ 3.f, 0.49f, 0.f }, { 0.5f, 0.5f, 0.5f });
    s.AddPlane({ 0.f, -5.f, 0.f });                                         // two planes: never tested against each other
    s.sim.FindContacts();
    ASSERT_EQ(2, s.sim.contacts.Count());
    for (const Manifold& m : s.sim.contacts.manifolds) EXPECT_EQ(ground, m.a);
}
