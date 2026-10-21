//
// kphysics: the math. Value types, so a body's position is position[b] and a step reads as the equations it is:
// position[b] += velocity[b] * h. A Vec3 is three floats, a Quat four (x, y, z, w: the engine's order, so a copy to or
// from the ECS is a memcpy), a Mat3 three columns. A std::vector<Vec3> is the same floats in the same order as the
// Kotlin core's FloatArray, so the GPU version can still upload each array as it is.
//
// Conventions, the engine's: right-handed, a quarter turn about +Y takes +X to −Z. Quaternions are Hamilton's: a * b
// turns by b first, then by a.
//

#pragma once

#include <cmath>

namespace kphysics
{
    /** @brief Below this a length counts as zero. */
    inline constexpr float Epsilon = 1e-6f;
    inline constexpr float Pi = 3.14159265358979323846f;

    struct Vec3 {
        float x = 0.f, y = 0.f, z = 0.f;

        [[nodiscard]] constexpr float& operator[](const int c) { return c == 0 ? x : c == 1 ? y : z; }
        [[nodiscard]] constexpr float operator[](const int c) const { return c == 0 ? x : c == 1 ? y : z; }
        constexpr bool operator==(const Vec3&) const = default;
    };

    [[nodiscard]] constexpr Vec3 operator+(const Vec3 a, const Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    [[nodiscard]] constexpr Vec3 operator-(const Vec3 a, const Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    [[nodiscard]] constexpr Vec3 operator-(const Vec3 a) { return { -a.x, -a.y, -a.z }; }
    [[nodiscard]] constexpr Vec3 operator*(const Vec3 a, const float s) { return { a.x * s, a.y * s, a.z * s }; }
    [[nodiscard]] constexpr Vec3 operator*(const float s, const Vec3 a) { return { a.x * s, a.y * s, a.z * s }; }
    [[nodiscard]] constexpr Vec3 operator/(const Vec3 a, const float s) { return { a.x / s, a.y / s, a.z / s }; }
    constexpr Vec3& operator+=(Vec3& a, const Vec3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
    constexpr Vec3& operator-=(Vec3& a, const Vec3 b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
    constexpr Vec3& operator*=(Vec3& a, const float s) { a.x *= s; a.y *= s; a.z *= s; return a; }

    [[nodiscard]] constexpr float Dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    [[nodiscard]] constexpr Vec3 Cross(const Vec3 a, const Vec3 b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    [[nodiscard]] inline float Length(const Vec3 a) { return std::sqrt(Dot(a, a)); }
    [[nodiscard]] constexpr float LengthSquared(const Vec3 a) { return Dot(a, a); }
    /**
     * @brief Makes @p a length 1 and returns the length it had. A vector shorter than Epsilon is left as it is, and 0
     *        is returned: the caller decides what a direction of nothing means.
     */
    inline float Normalize(Vec3& a)
    {
        const float length = Length(a);
        if (length < Epsilon) return 0.f;
        a = a * (1 / length);
        return length;
    }

    struct Quat {
        float x = 0.f, y = 0.f, z = 0.f, w = 1.f;      ///< The identity, to begin with.
        constexpr bool operator==(const Quat&) const = default;
    };

    /** @brief a * b: turned by b, then by a. */
    [[nodiscard]] constexpr Quat operator*(const Quat a, const Quat b)
    {
        return { a.w * b.x + b.w * a.x + (a.y * b.z - a.z * b.y),
                 a.w * b.y + b.w * a.y + (a.z * b.x - a.x * b.z),
                 a.w * b.z + b.w * a.z + (a.x * b.y - a.y * b.x),
                 a.w * b.w - (a.x * b.x + a.y * b.y + a.z * b.z) };
    }
    /** @brief The conjugate: for a quaternion of length 1, the turn back. */
    [[nodiscard]] constexpr Quat Conjugate(const Quat q) { return { -q.x, -q.y, -q.z, q.w }; }
    /** @brief @p q made length 1; one shorter than Epsilon is the identity. */
    [[nodiscard]] inline Quat Normalized(const Quat q)
    {
        const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (length < Epsilon) return {};
        return { q.x / length, q.y / length, q.z / length, q.w / length };
    }
    /**
     * @brief The turn of @p angle radians about @p axis, counter-clockwise looking down the axis towards its origin. The
     *        axis need not be length 1; one shorter than Epsilon is no turn.
     */
    [[nodiscard]] inline Quat FromAxisAngle(const Vec3 axis, const float angle)
    {
        const float length = Length(axis);
        if (length < Epsilon) return {};
        // The sine and cosine in double, then rounded, as the JVM works them out: the same turn as the Kotlin core's.
        const float s = static_cast<float>(std::sin(static_cast<double>(angle / 2))) / length;
        return { axis.x * s, axis.y * s, axis.z * s, static_cast<float>(std::cos(static_cast<double>(angle / 2))) };
    }
    /** @brief @p v turned by @p q (length 1): t = 2 (q.xyz × v), v' = v + q.w t + q.xyz × t. */
    [[nodiscard]] constexpr Vec3 Rotate(const Quat q, const Vec3 v)
    {
        const Vec3 t { 2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x) };
        return { v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
                 v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
                 v.z + q.w * t.z + (q.x * t.y - q.y * t.x) };
    }
    /** @brief q turned on by the small rotation θ: q + ½ (θ, 0) q, made length 1. What integrating a spin is. */
    [[nodiscard]] inline Quat Turned(const Quat q, const Vec3 theta)
    {
        return Normalized({ q.x + 0.5f * (theta.x * q.w + theta.y * q.z - theta.z * q.y),
                            q.y + 0.5f * (theta.y * q.w + theta.z * q.x - theta.x * q.z),
                            q.z + 0.5f * (theta.z * q.w + theta.x * q.y - theta.y * q.x),
                            q.w + 0.5f * (-theta.x * q.x - theta.y * q.y - theta.z * q.z) });
    }

    /** @brief A 3×3 matrix, column after column: column[c][r] is row r of column c. */
    struct Mat3 {
        Vec3 column[3] { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };     ///< The identity, to begin with.
    };

    /** @brief The rotation matrix of @p q (length 1): its columns are q turning +X, +Y and +Z. */
    [[nodiscard]] constexpr Mat3 FromQuat(const Quat q)
    {
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        return { { { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) },
                   { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) },
                   { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) } } };
    }
    /** @brief m · v = x · column 0 + y · column 1 + z · column 2. */
    [[nodiscard]] constexpr Vec3 operator*(const Mat3& m, const Vec3 v)
    {
        return { m.column[0].x * v.x + m.column[1].x * v.y + m.column[2].x * v.z,
                 m.column[0].y * v.x + m.column[1].y * v.y + m.column[2].y * v.z,
                 m.column[0].z * v.x + m.column[1].z * v.y + m.column[2].z * v.z };
    }
    /** @brief a · b: column c of it is a times column c of b. */
    [[nodiscard]] constexpr Mat3 operator*(const Mat3& a, const Mat3& b) { return { { a * b.column[0], a * b.column[1], a * b.column[2] } }; }
    [[nodiscard]] constexpr Mat3 Transpose(const Mat3& m)
    {
        return { { { m.column[0].x, m.column[1].x, m.column[2].x },
                   { m.column[0].y, m.column[1].y, m.column[2].y },
                   { m.column[0].z, m.column[1].z, m.column[2].z } } };
    }
    /**
     * @brief R · diag(d) · Rᵀ for a rotation R and a diagonal d: a body's inverse inertia in the world, from its diagonal
     *        in its own frame. Element (row, column) is Σn R[row][n] · d[n] · R[column][n].
     */
    [[nodiscard]] constexpr Mat3 RotateDiagonal(const Mat3& r, const Vec3 d)
    {
        Mat3 out;
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row <= column; ++row) {
                const float value = r.column[0][row] * d.x * r.column[0][column] +
                                    r.column[1][row] * d.y * r.column[1][column] +
                                    r.column[2][row] * d.z * r.column[2][column];
                out.column[column][row] = value;
                out.column[row][column] = value;
            }
        return out;
    }
    /**
     * @brief Solves m · x = rhs into @p x. False, leaving @p x alone, when m cannot be inverted: its determinant is below
     *        Epsilon times the product of its columns' lengths (Hadamard's bound), so the test asks how flat the matrix
     *        is whatever its units: a 200 kg body's joint matrix (determinant near 1e-7) is solved, and one of two
     *        columns alike refused at any scale. Cramer's rule.
     */
    inline bool Solve3(const Mat3& m, const Vec3 rhs, Vec3& x)
    {
        const Vec3 c12 = Cross(m.column[1], m.column[2]);
        const float det = Dot(m.column[0], c12);
        if (std::abs(det) <= Epsilon * Length(m.column[0]) * Length(m.column[1]) * Length(m.column[2])) return false;
        x = Vec3 { Dot(rhs, c12), Dot(rhs, Cross(m.column[2], m.column[0])), Dot(rhs, Cross(m.column[0], m.column[1])) } * (1 / det);
        return true;
    }

    /** @brief Some unit vector across @p n: the same one for the same direction, every time. */
    inline Vec3 Perpendicular(const Vec3 n)
    {
        // Crossed with whichever world axis it is least along, so the result is never short.
        const Vec3 p = std::abs(n.x) < 0.57735f ? Vec3 { 0.f, n.z, -n.y } : Vec3 { -n.z, 0.f, n.x };
        return p / Length(p);
    }
}
