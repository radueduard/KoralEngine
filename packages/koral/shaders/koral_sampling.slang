// The engine's base library: directions and points drawn from two numbers from 0 to 1 (koral_random's), spread as a
// light, a surface or a lens spreads them — for what is traced, gathered or blurred.
//
//     import koral_sampling;
//
//     const KoralBasis basis = KoralBasisOf(normal);
//     const float3 ray = basis.ToWorld(KoralCosineHemisphere(u));     // as a matte surface scatters light

module koral_sampling;

import koral_math;

/** Three directions square to one another, the third a given one: what turns a direction about it into the world. */
public struct KoralBasis {
    public float3 tangent;
    public float3 bitangent;
    public float3 normal;
    /** @p local (z along the normal) in the world. */
    public float3 ToWorld(float3 local) { return tangent * local.x + bitangent * local.y + normal * local.z; }
    /** A direction in the world in terms of this (z along the normal). */
    public float3 ToLocal(float3 world) { return float3(dot(world, tangent), dot(world, bitangent), dot(world, normal)); }
};

/** A basis about @p normal (of unit length), with no seam (Duff and others' branchless way). */
public KoralBasis KoralBasisOf(float3 normal)
{
    const float s = normal.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + normal.z);
    const float b = normal.x * normal.y * a;
    KoralBasis basis;
    basis.tangent = float3(1.0 + s * normal.x * normal.x * a, s * b, -s * normal.x);
    basis.bitangent = float3(b, s + normal.y * normal.y * a, -normal.y);
    basis.normal = normal;
    return basis;
}

/** A point of the unit disc, as evenly as the square @p u is: Shirley's concentric map, which keeps neighbours near. */
public float2 KoralConcentricDisc(float2 u)
{
    const float2 o = 2.0 * u - 1.0;
    if (all(o == 0.0)) return float2(0.0);
    float r, theta;
    if (abs(o.x) > abs(o.y)) { r = o.x; theta = KoralPi * 0.25 * (o.y / o.x); }
    else { r = o.y; theta = KoralHalfPi - KoralPi * 0.25 * (o.x / o.y); }
    return r * float2(cos(theta), sin(theta));
}

/** A direction about +z, more of them the nearer z it is, as cos θ: how a matte surface scatters (its pdf is cos θ / π). */
public float3 KoralCosineHemisphere(float2 u)
{
    const float2 d = KoralConcentricDisc(u);
    return float3(d, sqrt(max(0.0, 1.0 - dot(d, d))));
}

/** A direction about +z, as many any way: pdf 1 / 2π. */
public float3 KoralUniformHemisphere(float2 u)
{
    const float z = u.x, r = sqrt(max(0.0, 1.0 - z * z)), phi = KoralTau * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

/** A direction, as many any way: pdf 1 / 4π. */
public float3 KoralUniformSphere(float2 u)
{
    const float z = 1.0 - 2.0 * u.x, r = sqrt(max(0.0, 1.0 - z * z)), phi = KoralTau * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

/** A direction within a cone about +z whose half-angle's cosine is @p cosMax, as many any way within it. */
public float3 KoralUniformCone(float2 u, float cosMax)
{
    const float z = lerp(1.0, cosMax, u.x), r = sqrt(max(0.0, 1.0 - z * z)), phi = KoralTau * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

/**
 * A microfacet's normal seen from @p toEye (both about +z, the surface's normal) on a GGX surface of roughness @p alpha
 * (roughness squared): Heitz's sampling of the visible normals, with Dupuy and Benyoub's spherical caps. Reflecting
 * toEye about it is a direction as the surface's highlight spreads them.
 */
public float3 KoralGgxVisibleNormal(float3 toEye, float alpha, float2 u)
{
    const float3 stretched = normalize(float3(alpha * toEye.xy, toEye.z));
    const float phi = KoralTau * u.x;
    const float z = (1.0 - u.y) * (1.0 + stretched.z) - stretched.z;
    const float r = sqrt(saturate(1.0 - z * z));
    const float3 h = float3(r * cos(phi), r * sin(phi), z) + stretched;
    return normalize(float3(alpha * h.xy, max(h.z, 0.0)));
}

// ---- even directions ----------------------------------------------------------------------------------------------

/** The @p index'th of @p count directions spread evenly over the sphere: a spiral at the golden angle. */
public float3 KoralFibonacciSphere(uint index, uint count)
{
    const float z = 1.0 - (2.0 * float(index) + 1.0) / float(count);
    const float r = sqrt(max(0.0, 1.0 - z * z)), phi = float(index) * 2.39996322972865;
    return float3(r * cos(phi), r * sin(phi), z);
}

/**
 * Point @p index of @p count on the unit disc, spread evenly out from the middle along Vogel's spiral (the golden
 * angle), the whole turned by @p turn radians: taps for a soft shadow's or a blur's disc, another turn each pixel.
 */
public float2 KoralVogelDisk(uint index, uint count, float turn)
{
    const float r = sqrt((float(index) + 0.5) / float(count));
    const float angle = float(index) * 2.39996322972865 + turn;
    return r * float2(cos(angle), sin(angle));
}
