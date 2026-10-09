#pragma once

/**
 * @file transform.h
 * @brief The little arithmetic an entity's place in the world takes: a vector of three, and a matrix of four by four.
 *
 * kecs stands on its own, so these are its own: plain floats, laid out as everything else lays them out (a matrix
 * column after column, sixteen floats), to be copied into whatever math a program uses.
 */

#include <array>

namespace kecs
{
    struct Vec3 {
        float x = 0.f, y = 0.f, z = 0.f;
        [[nodiscard]] bool operator==(const Vec3&) const = default;
    };

    /** @brief A transform as a matrix, column after column: `m[column * 4 + row]`. The identity, to begin with. */
    struct Mat4 {
        std::array<float, 16> m { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };
        [[nodiscard]] float& At(const int column, const int row) { return m[static_cast<std::size_t>(column * 4 + row)]; }
        [[nodiscard]] float At(const int column, const int row) const { return m[static_cast<std::size_t>(column * 4 + row)]; }
        [[nodiscard]] bool operator==(const Mat4&) const = default;
    };

    /** @brief @p a after @p b: what @p b does, then what @p a does. */
    [[nodiscard]] Mat4 operator*(const Mat4& a, const Mat4& b);
    /** @brief What undoes @p m. The identity for one that nothing undoes. */
    [[nodiscard]] Mat4 Inverse(const Mat4& m);
    /**
     * @brief A position, a rotation and a scale as one matrix: scaled first, then turned, then moved. The rotation
     *        is Euler angles in degrees — about X, about Y, about Z.
     */
    [[nodiscard]] Mat4 Compose(const Vec3& position, const Vec3& rotationDegrees, const Vec3& scale);
    /**
     * @brief Splits a matrix back into what Compose took. A mirror is put in the X scale; shear is lost. False for
     *        one with no size along some axis, which says nothing of how it is turned.
     */
    bool Decompose(const Mat4& m, Vec3& position, Vec3& rotationDegrees, Vec3& scale);
}
