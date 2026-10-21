// Box against box, in its parts, then whole poses, as BoxBoxTest.kt checks it.
#include "support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <set>
#include <tuple>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    const float R2 = std::sqrt(0.5f);
    Vec3 At(const Contacts& c, const int p) { return c.manifolds[0].points[p].at; }
}

TEST(BoxBox, ClippingASquareByAPlane)
{
    const Vec3 square[] { { -1.f, -1.f, 0.f }, { 1.f, -1.f, 0.f }, { 1.f, 1.f, 0.f }, { -1.f, 1.f, 0.f } };
    const int ids[] { 0, 1, 2, 3 };
    Vec3 out[8];
    int outIds[8];
    ASSERT_EQ(4, narrowphase::ClipPolygon(square, ids, 4, { 1.f, 0.f, 0.f }, 0.5f, 1, out, outIds));
    EXPECT_EQ(0, outIds[0]);
    EXPECT_EQ(4, outIds[1]) << "a crossing is 4 · clipId + its edge's start";
    EXPECT_EQ(6, outIds[2]);
    EXPECT_EQ(3, outIds[3]);
    EXPECT_EQ((Vec3 { 0.5f, -1.f, 0.f }), out[1]);
    EXPECT_EQ((Vec3 { 0.5f, 1.f, 0.f }), out[2]);
    EXPECT_EQ(0, narrowphase::ClipPolygon(square, ids, 4, { 1.f, 0.f, 0.f }, -2.f, 1, out, outIds));
    EXPECT_EQ(4, narrowphase::ClipPolygon(square, ids, 4, { 1.f, 0.f, 0.f }, 2.f, 1, out, outIds));
}

TEST(BoxBox, ReducingEightToFour)
{
    Vec3 points[8];
    float depths[8];
    int ids[8];
    for (int p = 0; p < 8; ++p) {
        const double a = p * std::numbers::pi / 4;
        points[p] = { static_cast<float>(std::cos(a)), 0.f, static_cast<float>(std::sin(a)) };
        depths[p] = 0.01f;
        ids[p] = p;
    }
    depths[3] = 0.02f;
    EXPECT_EQ(4, narrowphase::ReduceToFour(points, depths, ids, 8, { 0.f, 1.f, 0.f }));
    EXPECT_EQ((std::set { 3, 7, 1, 5 }), (std::set { ids[0], ids[1], ids[2], ids[3] }));
    EXPECT_EQ(0.02f, depths[0]) << "the depths moved with their points";
}

TEST(BoxBox, ABoxRestingOnABox)
{
    Scene s;
    const int a = s.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int b = s.AddBox({ 0.f, 0.99f, 0.f }, { 0.5f, 0.5f, 0.5f });
    const Contacts c = s.Collide(a, b);
    ASSERT_EQ(1, c.Count());
    EXPECT_NEAR(1.f, c.manifolds[0].normal.y, 1e-5f);
    ASSERT_EQ(4, c.manifolds[0].count);
    std::set<int> features;
    for (int p = 0; p < 4; ++p) {
        EXPECT_NEAR(0.01f, c.manifolds[0].points[p].depth, 1e-5f);
        EXPECT_NEAR(0.495f, At(c, p).y, 1e-5f);
        EXPECT_NEAR(0.5f, std::abs(At(c, p).x), 1e-5f);
        EXPECT_EQ(2, FeatureOfA(c.manifolds[0].points[p].feature)) << "A's +Y face is the reference";
        features.insert(c.manifolds[0].points[p].feature);
    }
    EXPECT_EQ(4u, features.size()) << "each point its own feature";
}

TEST(BoxBox, TheSameFeaturesAStepLater)
{
    Scene s;
    const int a = s.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int b = s.AddBox({ 0.1f, 0.99f, 0.05f }, { 0.5f, 0.5f, 0.5f }, Turn({ 0.f, 1.f, 0.f }, 10.f));
    const auto features = [&] {
        const Contacts c = s.Collide(a, b);
        std::set<int> ids;
        for (int p = 0; p < c.manifolds[0].count; ++p) ids.insert(c.manifolds[0].points[p].feature);
        return ids;
    };
    const auto before = features();
    s.sim.bodies.position[b] += Vec3 { 1e-3f, -1e-3f, 0.f };
    EXPECT_EQ(before, features()) << "moved a millimetre: the same features, so they warm-start";
}

TEST(BoxBox, ABoxTurnedOnABox)
{
    Scene s;
    const int a = s.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int b = s.AddBox({ 0.f, 0.99f, 0.f }, { 0.5f, 0.5f, 0.5f }, Turn({ 0.f, 1.f, 0.f }, 45.f));
    const Contacts c = s.Collide(a, b);
    ASSERT_EQ(1, c.Count());
    ASSERT_EQ(4, c.manifolds[0].count) << "eight where the squares overlap, reduced to four";
    for (int p = 0; p < 4; ++p) {
        const Vec3 q = At(c, p);
        EXPECT_NEAR(0.01f, c.manifolds[0].points[p].depth, 1e-5f);
        EXPECT_TRUE(std::abs(q.x) <= 0.5f + 1e-5f && std::abs(q.z) <= 0.5f + 1e-5f) << "on A's face";
        EXPECT_TRUE(std::abs(q.x * R2 - q.z * R2) <= 0.5f + 1e-5f && std::abs(q.x * R2 + q.z * R2) <= 0.5f + 1e-5f) << "and under B's";
    }
}

TEST(BoxBox, EdgeOnEdge)
{
    Scene s;
    const int a = s.AddBox({}, { 0.5f, 0.5f, 0.5f }, Turn({ 1.f, 0.f, 0.f }, 45.f));
    const int b = s.AddBox({ 0.f, 2 * R2 - 0.02f, 0.f }, { 0.5f, 0.5f, 0.5f }, Turn({ 0.f, 0.f, 1.f }, 45.f));
    const Contacts c = s.Collide(a, b);
    ASSERT_EQ(1, c.Count());
    EXPECT_NEAR(1.f, c.manifolds[0].normal.y, 1e-4f);
    ASSERT_EQ(1, c.manifolds[0].count);
    EXPECT_NEAR(0.02f, c.manifolds[0].points[0].depth, 1e-4f);
    EXPECT_NEAR(R2 - 0.01f, At(c, 0).y, 1e-4f);
    EXPECT_EQ(Feature(8, 10), c.manifolds[0].points[0].feature) << "A's X edge, B's Z edge";
}

TEST(BoxBox, BoxesApart)
{
    Scene s;
    const int a = s.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int b = s.AddBox({ 0.f, 1.1f, 0.f }, { 0.5f, 0.5f, 0.5f });
    EXPECT_EQ(0, s.Collide(a, b).Count());
    s.sim.bodies.position[b].y = 1.01f;
    const Contacts c = s.Collide(a, b);
    ASSERT_EQ(1, c.Count()) << "1 cm apart: within the margin";
    EXPECT_NEAR(-0.01f, c.manifolds[0].points[0].depth, 1e-5f);
    const int turned = s.AddBox({ 0.75f + 2 * R2, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f }, Turn({ 0.f, 0.f, 1.f }, 45.f));
    EXPECT_EQ(0, s.Collide(a, turned).Count());
}

TEST(BoxBox, TheOtherWayRound)
{
    Scene s;
    const int a = s.AddBox({}, { 0.5f, 0.5f, 0.5f });
    const int b = s.AddBox({ 0.2f, 0.99f, -0.1f }, { 0.5f, 0.5f, 0.5f });
    const Contacts ab = s.Collide(a, b), ba = s.Collide(b, a);
    ASSERT_EQ(1, ab.Count());
    ASSERT_EQ(1, ba.Count());
    EXPECT_NEAR(1.f, ab.manifolds[0].normal.y, 1e-5f);
    EXPECT_NEAR(-1.f, ba.manifolds[0].normal.y, 1e-5f);
    ASSERT_EQ(ab.manifolds[0].count, ba.manifolds[0].count);
    const auto rounded = [](const Contacts& c) {
        std::set<std::tuple<long, long, long>> points;
        for (int p = 0; p < c.manifolds[0].count; ++p) {
            const Vec3 q = c.manifolds[0].points[p].at;
            points.insert({ std::lround(q.x * 1e4f), std::lround(q.y * 1e4f), std::lround(q.z * 1e4f) });
        }
        return points;
    };
    EXPECT_EQ(rounded(ab), rounded(ba));
}
