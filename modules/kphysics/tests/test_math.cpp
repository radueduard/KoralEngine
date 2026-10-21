// The math, as MathTest.kt checks it.
#include "support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace kphysics;
using namespace kphysics::test;

namespace
{
    constexpr float Half = 0.70710678f;
    constexpr float HalfPi = std::numbers::pi_v<float> / 2;

    void ExpectVec(const Vec3 expected, const Vec3 actual, const float tolerance = 1e-5f)
    {
        EXPECT_NEAR(expected.x, actual.x, tolerance);
        EXPECT_NEAR(expected.y, actual.y, tolerance);
        EXPECT_NEAR(expected.z, actual.z, tolerance);
    }

    Quat RandomQuat(KotlinRandom& random)
    {
        const float x = random.NextFloat() - 0.5f, y = random.NextFloat() - 0.5f, z = random.NextFloat() - 0.5f;
        return FromAxisAngle({ x, y, z }, random.NextFloat() * 6.f);
    }
}

TEST(Math, VectorsAddSubtractScaleDotAndCross)
{
    ExpectVec({ 11.f, 22.f, 33.f }, Vec3 { 1.f, 2.f, 3.f } + Vec3 { 10.f, 20.f, 30.f });
    ExpectVec({ 9.f, 18.f, 27.f }, Vec3 { 10.f, 20.f, 30.f } - Vec3 { 1.f, 2.f, 3.f });
    ExpectVec({ 6.f, 2.f, -2.f }, Vec3 { 1.f, 2.f, 3.f } + Vec3 { 10.f, 0.f, -10.f } * 0.5f);
    EXPECT_EQ(0.f, Dot({ 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }));
    EXPECT_EQ(32.f, Dot({ 1.f, 2.f, 3.f }, { 4.f, 5.f, 6.f }));
    ExpectVec({ 0.f, 0.f, 1.f }, Cross({ 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }));
    ExpectVec({ 0.f, 0.f, -1.f }, Cross({ 0.f, 1.f, 0.f }, { 1.f, 0.f, 0.f }));
    ExpectVec({ -3.f, 6.f, -3.f }, Cross({ 1.f, 2.f, 3.f }, { 4.f, 5.f, 6.f }));
}

TEST(Math, LengthAndNormalize)
{
    Vec3 a { 3.f, 4.f, 0.f };
    EXPECT_NEAR(5.f, Length(a), 1e-6f);
    EXPECT_NEAR(5.f, Normalize(a), 1e-6f) << "returns the length it had";
    ExpectVec({ 0.6f, 0.8f, 0.f }, a);
    Vec3 tiny { 1e-9f, 0.f, 0.f };
    EXPECT_EQ(0.f, Normalize(tiny)) << "a vector of nothing says so";
    EXPECT_EQ(1e-9f, tiny.x) << "and is left as it was";
}

TEST(Math, AQuarterTurnAboutYTakesXToMinusZ)
{
    const Quat q = FromAxisAngle({ 0.f, 5.f, 0.f }, HalfPi);       // an axis not of length 1
    EXPECT_NEAR(0.f, q.x, 1e-6f);
    EXPECT_NEAR(Half, q.y, 1e-6f);
    EXPECT_NEAR(Half, q.w, 1e-6f);
    ExpectVec({ 0.f, 0.f, -1.f }, Rotate(q, { 1.f, 0.f, 0.f }));
    EXPECT_EQ(Quat {}, FromAxisAngle({}, 1.f)) << "an axis of nothing is no turn";
    ExpectVec({ 1.f, 2.f, 3.f }, Rotate(Quat {}, { 1.f, 2.f, 3.f }));
}

TEST(Math, AProductTurnsByTheRightOneFirst)
{
    const Quat a = FromAxisAngle({ 0.f, 1.f, 0.f }, HalfPi), b = FromAxisAngle({ 1.f, 0.f, 0.f }, HalfPi);
    // b takes +Y to +Z, then a takes +Z to +X.
    ExpectVec({ 1.f, 0.f, 0.f }, Rotate(a * b, { 0.f, 1.f, 0.f }));
}

TEST(Math, TheConjugateTurnsBack)
{
    const Quat q = FromAxisAngle({ 1.f, 2.f, 3.f }, 1.1f);
    ExpectVec({ 0.3f, -2.f, 5.f }, Rotate(Conjugate(q), Rotate(q, { 0.3f, -2.f, 5.f })));
}

TEST(Math, NormalizedMakesALengthOfOne)
{
    const Quat q = Normalized({ 0.f, 2.f, 0.f, 2.f });
    EXPECT_NEAR(Half, q.y, 1e-6f);
    EXPECT_NEAR(Half, q.w, 1e-6f);
    EXPECT_EQ(Quat {}, Normalized({ 0.f, 0.f, 0.f, 0.f })) << "one of length 0 is the identity";
}

TEST(Math, AFullTurnInAThousandStepsComesBack)
{
    Quat q;
    const Quat step = FromAxisAngle({ 1.f, 2.f, 3.f }, 2 * std::numbers::pi_v<float> / 1000);
    for (int i = 0; i < 1000; ++i) q = Normalized(step * q);
    EXPECT_NEAR(1.f, std::abs(q.w), 1e-3f);
    ExpectVec({ 1.f, -1.f, 0.5f }, Rotate(q, { 1.f, -1.f, 0.5f }), 2e-3f);
}

TEST(Math, AQuaternionsMatrixTurnsAsItDoes)
{
    KotlinRandom random(1);
    for (int i = 0; i < 100; ++i) {
        const Quat q = RandomQuat(random);
        const Vec3 v { random.NextFloat() * 4.f - 2.f, random.NextFloat() * 4.f - 2.f, random.NextFloat() * 4.f - 2.f };
        ExpectVec(Rotate(q, v), FromQuat(q) * v);
    }
    ExpectVec({ 0.f, 0.f, -1.f }, FromQuat(FromAxisAngle({ 0.f, 1.f, 0.f }, HalfPi)).column[0]);      // the first column is where +X goes
}

TEST(Math, MatricesMultiplyAsTheirTurnsDo)
{
    KotlinRandom random(2);
    for (int i = 0; i < 50; ++i) {
        const Quat a = RandomQuat(random), b = RandomQuat(random);
        const Mat3 product = FromQuat(a) * FromQuat(b), turned = FromQuat(a * b);
        for (int c = 0; c < 3; ++c) ExpectVec(turned.column[c], product.column[c]);
    }
}

TEST(Math, ARotationsTransposeUndoesIt)
{
    const Mat3 r = FromQuat(FromAxisAngle({ 3.f, -1.f, 2.f }, 2.3f));
    const Mat3 identity = r * Transpose(r);
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row) EXPECT_NEAR(column == row ? 1.f : 0.f, identity.column[column][row], 1e-5f);
}

TEST(Math, ADiagonalTurnedIsRDRt)
{
    KotlinRandom random(3);
    const Vec3 d { 0.5f, 2.f, 7.f };
    for (int i = 0; i < 50; ++i) {
        const Mat3 r = FromQuat(RandomQuat(random));
        const Mat3 out = RotateDiagonal(r, d);
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row) {
                float expected = 0.f;
                for (int n = 0; n < 3; ++n) expected += r.column[n][row] * d[n] * r.column[n][column];
                EXPECT_NEAR(expected, out.column[column][row], 1e-5f);
            }
    }
    // A quarter turn about Y: the body's Z is the world's X, so their entries swap.
    const Mat3 out = RotateDiagonal(FromQuat(FromAxisAngle({ 0.f, 1.f, 0.f }, HalfPi)), d);
    ExpectVec({ 7.f, 0.f, 0.f }, out.column[0]);
    ExpectVec({ 0.f, 2.f, 0.f }, out.column[1]);
    ExpectVec({ 0.f, 0.f, 0.5f }, out.column[2]);
}

TEST(Math, Solve3SolvesAndRefusesTheSingular)
{
    const Vec3 x { 1.f, -2.f, 0.5f };
    const auto solved = [&](const Mat3& m, const char* what) {
        Vec3 out;
        ASSERT_TRUE(Solve3(m, m * x, out)) << what;
        ExpectVec(x, out);
    };
    solved({ { { 2.f, 0.f, 1.f }, { 1.f, 3.f, 0.f }, { 0.f, 1.f, 4.f } } }, "determinant 25");
    solved({ { { 1.f, 3.f, 0.f }, { 2.f, 0.f, 1.f }, { 0.f, 1.f, 4.f } } }, "determinant −25");
    // Scale does not make a matrix singular: a heavy body's joint matrix has entries near 1/m and a determinant near 1e-7.
    solved({ { { 2e-3f, 0.f, 1e-3f }, { 1e-3f, 3e-3f, 0.f }, { 0.f, 1e-3f, 4e-3f } } }, "times 1/1000");
    solved({ { { 2e3f, 0.f, 1e3f }, { 1e3f, 3e3f, 0.f }, { 0.f, 1e3f, 4e3f } } }, "times 1000");
    Vec3 untouched { 7.f, 7.f, 7.f };
    EXPECT_FALSE(Solve3({ { { 1.f, 2.f, 3.f }, { 2.f, 4.f, 6.f }, { 0.f, 1.f, 0.f } } }, { 1.f, 1.f, 1.f }, untouched)) << "two columns alike";
    EXPECT_EQ((Vec3 { 7.f, 7.f, 7.f }), untouched) << "and nothing written";
}

TEST(Math, PerpendicularIsAcrossAndTheSameEachTime)
{
    for (const Vec3 n : { Vec3 { 1.f, 0.f, 0.f }, Vec3 { 0.f, 1.f, 0.f }, Vec3 { 0.6f, 0.f, 0.8f } }) {
        const Vec3 p = Perpendicular(n);
        EXPECT_NEAR(0.f, Dot(p, n), 1e-6f);
        EXPECT_NEAR(1.f, Length(p), 1e-6f);
        EXPECT_EQ(p, Perpendicular(n));
    }
}
