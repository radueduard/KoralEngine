// The sdf module: shapes as signed distances — how far a point is from a shape's surface, negative inside — put
// together by booleans hard and smooth, and marched along a ray. For what is drawn without a mesh: UI shapes, debug
// shapes, clouds' and fluids' surfaces, anything raymarched.
//
//     import sdf;
//
//     const float d = KoralSmoothUnion(KoralSdfSphere(p, 1.0), KoralSdfBox(p - float3(1, 0, 0), float3(0.5)), 0.2);

module sdf;

import koral_math;

// ---- 3D shapes, about the origin ---------------------------------------------------------------------------------

public float KoralSdfSphere(float3 p, float radius) { return length(p) - radius; }

/** A box of half-size @p half. */
public float KoralSdfBox(float3 p, float3 half)
{
    const float3 q = abs(p) - half;
    return length(max(q, 0.0)) + min(KoralMax3(q), 0.0);
}

/** A box of half-size @p half, its edges rounded @p radius. */
public float KoralSdfRoundBox(float3 p, float3 half, float radius) { return KoralSdfBox(p, half - radius) - radius; }

/** A capsule from @p a to @p b, @p radius round. */
public float KoralSdfCapsule(float3 p, float3 a, float3 b, float radius)
{
    const float3 pa = p - a, ba = b - a;
    const float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-12));
    return length(pa - ba * h) - radius;
}

/** A torus about y: @p major from the middle to the tube's, @p minor the tube's radius. */
public float KoralSdfTorus(float3 p, float major, float minor) { return length(float2(length(p.xz) - major, p.y)) - minor; }

/** A cylinder about y, @p radius round and @p halfHeight up and down. */
public float KoralSdfCylinder(float3 p, float radius, float halfHeight)
{
    const float2 d = abs(float2(length(p.xz), p.y)) - float2(radius, halfHeight);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}

/** The plane through the origin square to @p normal (of unit length): its side the normal points to positive. */
public float KoralSdfPlane(float3 p, float3 normal) { return dot(p, normal); }

// ---- 2D shapes ---------------------------------------------------------------------------------------------------

public float KoralSdfCircle(float2 p, float radius) { return length(p) - radius; }

public float KoralSdfRect(float2 p, float2 half)
{
    const float2 q = abs(p) - half;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

/** A rectangle of half-size @p half, its corners rounded @p radius: a button, a panel. */
public float KoralSdfRoundRect(float2 p, float2 half, float radius) { return KoralSdfRect(p, half - radius) - radius; }

/** A line from @p a to @p b, @p width wide. */
public float KoralSdfSegment(float2 p, float2 a, float2 b, float width)
{
    const float2 pa = p - a, ba = b - a;
    const float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-12));
    return length(pa - ba * h) - width * 0.5;
}

/** How much of a pixel a shape covers, at a distance of @p d pixels from its edge: smooth over one pixel. */
public float KoralSdfCoverage(float d) { return saturate(0.5 - d); }

// ---- putting shapes together -------------------------------------------------------------------------------------

public float KoralUnion(float a, float b) { return min(a, b); }
public float KoralSubtract(float a, float b) { return max(a, -b); }       // a without b
public float KoralIntersect(float a, float b) { return max(a, b); }

/** The union of two, their seam rounded over @p k (Quílez's polynomial smooth minimum). */
public float KoralSmoothUnion(float a, float b, float k)
{
    const float h = saturate(0.5 + 0.5 * (b - a) / max(k, 1e-6));
    return lerp(b, a, h) - k * h * (1.0 - h);
}

public float KoralSmoothSubtract(float a, float b, float k) { return -KoralSmoothUnion(-a, b, k); }
public float KoralSmoothIntersect(float a, float b, float k) { return -KoralSmoothUnion(-a, -b, k); }

/** A shape @p r thicker all round: rounded. */
public float KoralRound(float d, float r) { return d - r; }

/** A shape's shell @p thickness thick: hollow. */
public float KoralOnion(float d, float thickness) { return abs(d) - thickness; }

/** @p p repeated every @p spacing: the shape about the origin, everywhere. */
public float3 KoralRepeat(float3 p, float3 spacing) { return p - spacing * round(p / spacing); }

// ---- marching ----------------------------------------------------------------------------------------------------

/** A field of distance a ray can march: anything with a distance at a point. */
public interface IKoralField {
    float Distance(float3 p);
};

/** Where a ray meets a field's surface: how far along it, and whether it did within @p far. */
public struct KoralMarchHit {
    public bool hit;
    public float distance;
    public uint steps;
};

/** Sphere tracing: steps of as far as the field says nothing is, until within @p epsilon of its surface or past @p far. */
public KoralMarchHit KoralMarch<F : IKoralField>(F field, float3 origin, float3 direction, float far, uint steps = 128, float epsilon = 1e-3)
{
    KoralMarchHit hit;
    hit.hit = false;
    hit.distance = 0.0;
    hit.steps = 0;
    for (uint i = 0; i < steps; ++i) {
        const float d = field.Distance(origin + direction * hit.distance);
        hit.steps = i + 1;
        if (d < epsilon * max(hit.distance, 1.0)) { hit.hit = true; return hit; }
        hit.distance += d;
        if (hit.distance > far) break;
    }
    return hit;
}

/** Which way a field's surface faces at @p p: its gradient, by four samples (a tetrahedron's corners). */
public float3 KoralFieldNormal<F : IKoralField>(F field, float3 p, float epsilon = 1e-3)
{
    const float2 k = float2(1.0, -1.0);
    return KoralSafeNormalize(k.xyy * field.Distance(p + k.xyy * epsilon) + k.yyx * field.Distance(p + k.yyx * epsilon)
                            + k.yxy * field.Distance(p + k.yxy * epsilon) + k.xxx * field.Distance(p + k.xxx * epsilon));
}

/** How much a point sees a light along @p direction past the field: a soft shadow, sharper the larger @p k (Quílez's). */
public float KoralFieldSoftShadow<F : IKoralField>(F field, float3 origin, float3 direction, float near, float far, float k = 8.0)
{
    float light = 1.0, t = near;
    for (uint i = 0; i < 64 && t < far; ++i) {
        const float d = field.Distance(origin + direction * t);
        if (d < 1e-4) return 0.0;
        light = min(light, k * d / t);
        t += clamp(d, 0.01, 0.5);
    }
    return saturate(light);
}
