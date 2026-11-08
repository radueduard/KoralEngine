// The engine's base library: numbers, and the shapes of space every shader works with. Matrices are as the engine's
// shaders use them: a row vector times a matrix, mul(v, m), the matrix's last row where it is.
//
//     import koral_math;
//
//     const float3 normal = KoralTransformNormal(vertex.normal, model);     // square to the surface however it is scaled
//     raster.position = float4(KoralFullscreenCorner(index), 0.0, 1.0);    // one triangle over the whole picture

module koral_math;

public static const float KoralPi = 3.14159265358979;
public static const float KoralTau = 6.28318530717959;
public static const float KoralHalfPi = 1.57079632679490;
public static const float KoralInvPi = 0.318309886183791;

// ---- numbers ------------------------------------------------------------------------------------------------------

public float KoralSquare(float x) { return x * x; }
public float KoralPow5(float x) { const float x2 = x * x; return x2 * x2 * x; }
public float KoralMax3(float3 v) { return max(v.x, max(v.y, v.z)); }
public float KoralMin3(float3 v) { return min(v.x, min(v.y, v.z)); }

/** @p x from [fromLow, fromHigh] to [toLow, toHigh], straight: past the ends as far past. */
public float KoralRemap(float x, float fromLow, float fromHigh, float toLow, float toHigh)
{
    return toLow + (x - fromLow) * (toHigh - toLow) / (fromHigh - fromLow);
}

/** The same, kept within [toLow, toHigh]. */
public float KoralRemapClamped(float x, float fromLow, float fromHigh, float toLow, float toHigh)
{
    return toLow + saturate((x - fromLow) / (fromHigh - fromLow)) * (toHigh - toLow);
}

/** A direction of unit length, or @p otherwise where @p v is too short to have one. */
public float3 KoralSafeNormalize(float3 v, float3 otherwise = float3(0.0, 0.0, 1.0))
{
    const float length2 = dot(v, v);
    return length2 > 1e-24 ? v * rsqrt(length2) : otherwise;
}

// ---- where things are --------------------------------------------------------------------------------------------

/** A point through a matrix: where @p model puts it. */
public float3 KoralTransformPoint(float3 point, float4x4 model) { return mul(float4(point, 1.0), model).xyz; }

/** A direction through a matrix's turn and scale, not its place. */
public float3 KoralTransformDirection(float3 direction, float4x4 model) { return mul(float4(direction, 0.0), model).xyz; }

/**
 * A normal through @p model, of unit length: through the cofactors of its 3x3 — its inverse transposed, but for a scale
 * that normalising takes away — so that it stays square to the surface however unevenly the model is scaled, and turned
 * back where a scale mirrors it.
 */
public float3 KoralTransformNormal(float3 normal, float4x4 model)
{
    const float3 a0 = model[0].xyz, a1 = model[1].xyz, a2 = model[2].xyz;
    const float3 n = normal.x * cross(a1, a2) + normal.y * cross(a2, a0) + normal.z * cross(a0, a1);
    return KoralSafeNormalize(n * sign(dot(a0, cross(a1, a2))));
}

/** @p v turned by the quaternion @p q (x, y, z, w). */
public float3 KoralRotate(float4 q, float3 v)
{
    const float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

/** A rotation (a quaternion), a scale the same every way and a place, as one matrix. */
public float4x4 KoralMatrixOf(float4 q, float scale, float3 position)
{
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return float4x4(
        float4((1.0 - 2.0 * (yy + zz)) * scale, 2.0 * (xy + wz) * scale, 2.0 * (xz - wy) * scale, 0.0),
        float4(2.0 * (xy - wz) * scale, (1.0 - 2.0 * (xx + zz)) * scale, 2.0 * (yz + wx) * scale, 0.0),
        float4(2.0 * (xz + wy) * scale, 2.0 * (yz - wx) * scale, (1.0 - 2.0 * (xx + yy)) * scale, 0.0),
        float4(position, 1.0));
}

/** The matrix that undoes @p m: by its cofactors, for one that can be undone. */
public float4x4 KoralInverse(float4x4 m)
{
    const float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
    const float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
    const float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
    const float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];
    const float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
    const float b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    const float b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
    const float b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    const float determinant = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    const float s = 1.0 / determinant;
    return float4x4(
        float4(a11 * b11 - a12 * b10 + a13 * b09, a02 * b10 - a01 * b11 - a03 * b09, a31 * b05 - a32 * b04 + a33 * b03, a22 * b04 - a21 * b05 - a23 * b03) * s,
        float4(a12 * b08 - a10 * b11 - a13 * b07, a00 * b11 - a02 * b08 + a03 * b07, a32 * b02 - a30 * b05 - a33 * b01, a20 * b05 - a22 * b02 + a23 * b01) * s,
        float4(a10 * b10 - a11 * b08 + a13 * b06, a01 * b08 - a00 * b10 - a03 * b06, a30 * b04 - a31 * b02 + a33 * b00, a21 * b02 - a20 * b04 - a23 * b00) * s,
        float4(a11 * b07 - a10 * b09 - a12 * b06, a00 * b09 - a01 * b07 + a02 * b06, a31 * b01 - a30 * b03 - a32 * b00, a20 * b03 - a21 * b01 + a22 * b00) * s);
}

// ---- the picture ---------------------------------------------------------------------------------------------------

/**
 * A corner of one triangle that covers the whole picture, in clip space, for vertex @p index (0, 1, 2) of a draw of
 * three: (-1, -1), (3, -1), (-1, 3).
 */
public float2 KoralFullscreenCorner(uint index)
{
    return float2((index << 1) & 2, index & 2) * 2.0 - 1.0;
}

/** Where pixel @p pixel's middle is in a picture of @p size, from 0 to 1. */
public float2 KoralPixelUv(uint2 pixel, uint2 size) { return (float2(pixel) + 0.5) / float2(size); }

// ---- groups --------------------------------------------------------------------------------------------------------

/** The most groups along x a device must take: more go along y. */
public static const uint KoralWideGroups = 65535;

/** @p groups as a dispatch takes them: along x up to KoralWideGroups, and rows of them along y past that. */
public uint3 KoralGroupsFor(uint groups)
{
    return groups == 0 ? uint3(0, 1, 1) : uint3(min(groups, KoralWideGroups), (groups + KoralWideGroups - 1) / KoralWideGroups, 1);
}

/** Which thread this is, counted from 0, of a dispatch of KoralGroupsFor's groups of @p groupSize threads along x. */
public uint KoralThreadIndex(uint3 dispatchThread, uint groupSize) { return dispatchThread.x + dispatchThread.y * KoralWideGroups * groupSize; }

/** Which group this is, counted from 0, of a dispatch of KoralGroupsFor's groups. */
public uint KoralGroupIndex(uint3 group) { return group.x + group.y * KoralWideGroups; }
