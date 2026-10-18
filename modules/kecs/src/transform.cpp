//
// kecs: the arithmetic of transforms. The Euler angles, and the quaternion they go through, are composed as the
// framework's math composes them, so that an angle means the same here as there.
//

#include <kecs/transform.h>

#include <algorithm>
#include <cmath>

namespace kecs
{
    namespace {
        constexpr float Radians = 3.14159265358979323846f / 180.f;

        Quat FromEuler(const Vec3& radians)
        {
            const float cx = std::cos(radians.x * 0.5f), cy = std::cos(radians.y * 0.5f), cz = std::cos(radians.z * 0.5f);
            const float sx = std::sin(radians.x * 0.5f), sy = std::sin(radians.y * 0.5f), sz = std::sin(radians.z * 0.5f);
            return { sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz };
        }

        Vec3 EulerAngles(const Quat& q)
        {
            const float py = 2.f * (q.y * q.z + q.w * q.x), px = q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;
            const float pitch = (std::abs(px) < 1e-7f && std::abs(py) < 1e-7f) ? 2.f * std::atan2(q.x, q.w) : std::atan2(py, px);
            const float yaw = std::asin(std::clamp(-2.f * (q.x * q.z - q.w * q.y), -1.f, 1.f));
            const float roll = std::atan2(2.f * (q.x * q.y + q.w * q.z), q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z);
            return { pitch, yaw, roll };
        }

        /** The rotation of three orthonormal columns: `c[column][row]`. */
        Quat FromColumns(const float c[3][3])
        {
            const float fourX = c[0][0] - c[1][1] - c[2][2], fourY = c[1][1] - c[0][0] - c[2][2];
            const float fourZ = c[2][2] - c[0][0] - c[1][1], fourW = c[0][0] + c[1][1] + c[2][2];
            int biggest = 0;
            float most = fourW;
            if (fourX > most) { most = fourX; biggest = 1; }
            if (fourY > most) { most = fourY; biggest = 2; }
            if (fourZ > most) { most = fourZ; biggest = 3; }
            const float big = std::sqrt(most + 1.f) * 0.5f;
            const float mult = 0.25f / big;
            switch (biggest) {
            case 0: return { (c[1][2] - c[2][1]) * mult, (c[2][0] - c[0][2]) * mult, (c[0][1] - c[1][0]) * mult, big };
            case 1: return { big, (c[0][1] + c[1][0]) * mult, (c[2][0] + c[0][2]) * mult, (c[1][2] - c[2][1]) * mult };
            case 2: return { (c[0][1] + c[1][0]) * mult, big, (c[1][2] + c[2][1]) * mult, (c[2][0] - c[0][2]) * mult };
            default: return { (c[2][0] + c[0][2]) * mult, (c[1][2] + c[2][1]) * mult, big, (c[0][1] - c[1][0]) * mult };
            }
        }
    }

    Mat4 operator*(const Mat4& a, const Mat4& b)
    {
        Mat4 out;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) {
                float sum = 0.f;
                for (int k = 0; k < 4; ++k) sum += a.At(k, row) * b.At(column, k);
                out.At(column, row) = sum;
            }
        return out;
    }

    Mat4 Inverse(const Mat4& matrix)
    {
        const auto& m = matrix.m;
        std::array<float, 16> inv;
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        const float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (determinant == 0.f) return {};
        Mat4 out;
        for (std::size_t i = 0; i < 16; ++i) out.m[i] = inv[i] / determinant;
        return out;
    }

    Quat ToQuat(const Vec3& rotationDegrees) { return FromEuler({ rotationDegrees.x * Radians, rotationDegrees.y * Radians, rotationDegrees.z * Radians }); }

    Vec3 ToEuler(const Quat& rotation)
    {
        const Vec3 radians = EulerAngles(Normalize(rotation));
        return { radians.x / Radians, radians.y / Radians, radians.z / Radians };
    }

    Quat Normalize(const Quat& q)
    {
        const float size = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!(size > 0.f)) return {};
        return { q.x / size, q.y / size, q.z / size, q.w / size };
    }

    Mat4 Compose(const Vec3& position, const Vec3& rotationDegrees, const Vec3& scale) { return ComposeUnit(position, ToQuat(rotationDegrees), scale); }
    Mat4 ComposeQuat(const Vec3& position, const Quat& rotation, const Vec3& scale) { return ComposeUnit(position, Normalize(rotation), scale); }

    Mat4 ComposeUnit(const Vec3& position, const Quat& q, const Vec3& scale)
    {
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xz = q.x * q.z, xy = q.x * q.y, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 out;
        out.m = { (1.f - 2.f * (yy + zz)) * scale.x, 2.f * (xy + wz) * scale.x, 2.f * (xz - wy) * scale.x, 0.f,
                  2.f * (xy - wz) * scale.y, (1.f - 2.f * (xx + zz)) * scale.y, 2.f * (yz + wx) * scale.y, 0.f,
                  2.f * (xz + wy) * scale.z, 2.f * (yz - wx) * scale.z, (1.f - 2.f * (xx + yy)) * scale.z, 0.f,
                  position.x, position.y, position.z, 1.f };
        return out;
    }

    bool Decompose(const Mat4& m, Vec3& position, Vec3& rotationDegrees, Vec3& scale)
    {
        Quat q;
        if (!Decompose(m, position, q, scale)) return false;
        rotationDegrees = ToEuler(q);
        return true;
    }

    bool Decompose(const Mat4& m, Vec3& position, Quat& rotation, Vec3& scale)
    {
        position = { m.At(3, 0), m.At(3, 1), m.At(3, 2) };
        float c[3][3];
        float length[3];
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) c[column][row] = m.At(column, row);
            length[column] = std::sqrt(c[column][0] * c[column][0] + c[column][1] * c[column][1] + c[column][2] * c[column][2]);
        }
        scale = { length[0], length[1], length[2] };
        if (length[0] == 0.f || length[1] == 0.f || length[2] == 0.f) return false;
        // A mirror: the first column against the cross of the other two.
        const float cross[3] { c[1][1] * c[2][2] - c[1][2] * c[2][1], c[1][2] * c[2][0] - c[1][0] * c[2][2], c[1][0] * c[2][1] - c[1][1] * c[2][0] };
        if (c[0][0] * cross[0] + c[0][1] * cross[1] + c[0][2] * cross[2] < 0.f) { scale.x = -scale.x; length[0] = -length[0]; }
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row) c[column][row] /= length[column];
        rotation = Normalize(FromColumns(c));
        return true;
    }
}
