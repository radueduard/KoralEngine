// The engine's base library: boxes and rays — whether what a box holds can be seen, where in the picture it is, and
// where a ray meets a box, a plane or a triangle. What culls, picks and traces works out of these.
//
//     import koral_bounds;
//
//     if (KoralBoxOutOfSight(low, high, model, viewProjection)) return;      // nothing of it can be seen
//
// Clip space is the engine's: x and y from -w to w, z from 0 to w.

module koral_bounds;

// ---- boxes in the picture ---------------------------------------------------------------------------------------

/** How many of a box's corners are past each of the six planes of the picture, and what else is known of where it is. */
public struct KoralBoxInPicture {
    /** Wholly past one of the planes: nothing of it can be seen. */
    public bool outOfSight;
    /** Every corner in front of the eye: the rectangle and the depth below say where it is. */
    public bool inFront;
    /** The rectangle it covers, from 0 to 1: of the corners in front of the eye. */
    public float2 low;
    public float2 high;
    /** The depth of its nearest corner: the largest, where depth is reversed. */
    public float nearest;
};

/** Where the box from @p low to @p high, placed by @p model, is in the picture @p viewProjection sees. */
public KoralBoxInPicture KoralPlaceBox(float3 low, float3 high, float4x4 model, float4x4 viewProjection, bool reversed)
{
    KoralBoxInPicture placed;
    uint outside[6] = { 0, 0, 0, 0, 0, 0 };
    placed.inFront = true;
    placed.low = float2(1e30, 1e30);
    placed.high = float2(-1e30, -1e30);
    placed.nearest = reversed ? 0.0 : 1.0;
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        const float3 at = float3((corner & 1) != 0 ? high.x : low.x, (corner & 2) != 0 ? high.y : low.y, (corner & 4) != 0 ? high.z : low.z);
        const float4 clip = mul(mul(float4(at, 1.0), model), viewProjection);
        outside[0] += clip.x < -clip.w ? 1 : 0;
        outside[1] += clip.x > clip.w ? 1 : 0;
        outside[2] += clip.y < -clip.w ? 1 : 0;
        outside[3] += clip.y > clip.w ? 1 : 0;
        outside[4] += clip.z < 0.0 ? 1 : 0;
        outside[5] += clip.z > clip.w ? 1 : 0;
        if (clip.w <= 1e-6) { placed.inFront = false; continue; }
        const float3 ndc = clip.xyz / clip.w;
        placed.low = min(placed.low, ndc.xy * 0.5 + 0.5);
        placed.high = max(placed.high, ndc.xy * 0.5 + 0.5);
        placed.nearest = reversed ? max(placed.nearest, ndc.z) : min(placed.nearest, ndc.z);
    }
    placed.outOfSight = false;
    [unroll] for (uint plane = 0; plane < 6; ++plane) if (outside[plane] == 8) placed.outOfSight = true;
    return placed;
}

/** Whether the box from @p low to @p high, placed by @p model, is wholly past one of the planes of what @p viewProjection sees. */
public bool KoralBoxOutOfSight(float3 low, float3 high, float4x4 model, float4x4 viewProjection)
{
    uint outside[6] = { 0, 0, 0, 0, 0, 0 };
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        const float3 at = float3((corner & 1) != 0 ? high.x : low.x, (corner & 2) != 0 ? high.y : low.y, (corner & 4) != 0 ? high.z : low.z);
        const float4 clip = mul(mul(float4(at, 1.0), model), viewProjection);
        outside[0] += clip.x < -clip.w ? 1 : 0;
        outside[1] += clip.x > clip.w ? 1 : 0;
        outside[2] += clip.y < -clip.w ? 1 : 0;
        outside[3] += clip.y > clip.w ? 1 : 0;
        outside[4] += clip.z < 0.0 ? 1 : 0;
        outside[5] += clip.z > clip.w ? 1 : 0;
    }
    [unroll] for (uint plane = 0; plane < 6; ++plane) if (outside[plane] == 8) return true;
    return false;
}

/** A box seen from afar: whether it is out of sight, how far along the view its nearest part is, and how many pixels it spans. */
public struct KoralBoxSeen {
    public bool outOfSight;
    /** The w of its nearest part: how far along the view, in the world's units. */
    public float nearest;
    /** How many pixels it spans, the wider way: very many when it reaches the eye. */
    public float pixels;
};

/**
 * The box about @p centre, @p half its size each way, as @p toClip sees it — its corners worked out from the centre and
 * three axes, for many boxes cheaply — in a picture @p halfHeight pixels high from its middle.
 */
public KoralBoxSeen KoralSeeBox(float3 centre, float3 half, float4x4 toClip, float halfHeight)
{
    const float4 c = mul(float4(centre, 1.0), toClip);
    const float4 e[3] = { mul(float4(half.x, 0.0, 0.0, 0.0), toClip), mul(float4(0.0, half.y, 0.0, 0.0), toClip),
                          mul(float4(0.0, 0.0, half.z, 0.0), toClip) };
    int past[6] = { 0, 0, 0, 0, 0, 0 };
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        float4 at = c;
        [unroll] for (uint axis = 0; axis < 3; ++axis) at += (corner & (1u << axis)) != 0 ? e[axis] : -e[axis];
        past[0] += at.x < -at.w; past[1] += at.x > at.w; past[2] += at.y < -at.w; past[3] += at.y > at.w; past[4] += at.z < 0.0; past[5] += at.z > at.w;
    }
    KoralBoxSeen seen;
    seen.outOfSight = false;
    [unroll] for (uint side = 0; side < 6; ++side) seen.outOfSight = seen.outOfSight || past[side] == 8;
    const float reach = abs(e[0].w) + abs(e[1].w) + abs(e[2].w);
    seen.nearest = c.w - reach;
    const float across = max(abs(e[0].x) + abs(e[1].x) + abs(e[2].x), abs(e[0].y) + abs(e[1].y) + abs(e[2].y));
    seen.pixels = seen.nearest > 0.0 ? across / seen.nearest * halfHeight : 1e30;
    return seen;
}

/** Whether a sphere is wholly past one of the planes of what @p viewProjection sees: its planes worked out of the matrix. */
public bool KoralSphereOutOfSight(float3 centre, float radius, float4x4 viewProjection)
{
    const float4x4 m = transpose(viewProjection);
    const float4 planes[6] = { m[3] + m[0], m[3] - m[0], m[3] + m[1], m[3] - m[1], m[2], m[3] - m[2] };
    [unroll] for (uint i = 0; i < 6; ++i)
        if (dot(planes[i].xyz, centre) + planes[i].w < -radius * length(planes[i].xyz)) return true;
    return false;
}

// ---- rays --------------------------------------------------------------------------------------------------------

/** How far along a ray it meets the plane through @p point square to @p normal: negative behind it, infinite along it. */
public float KoralRayPlane(float3 origin, float3 direction, float3 point, float3 normal)
{
    const float facing = dot(direction, normal);
    return abs(facing) < 1e-12 ? 1e30 : dot(point - origin, normal) / facing;
}

/** Where along a ray it is inside a box: (enters, leaves); it misses where enters > leaves or leaves < 0. */
public float2 KoralRayBox(float3 origin, float3 direction, float3 low, float3 high)
{
    const float3 inverse = 1.0 / direction;
    const float3 a = (low - origin) * inverse, b = (high - origin) * inverse;
    const float3 near = min(a, b), far = max(a, b);
    return float2(max(near.x, max(near.y, near.z)), min(far.x, min(far.y, far.z)));
}

/** Where a ray meets a triangle: how far along it, and where in the triangle (the second and third corners' weights). */
public struct KoralTriangleHit {
    public bool hit;
    public float distance;
    public float2 barycentrics;
};

/** Möller and Trumbore's test, both faces. */
public KoralTriangleHit KoralRayTriangle(float3 origin, float3 direction, float3 a, float3 b, float3 c)
{
    KoralTriangleHit hit;
    hit.hit = false;
    hit.distance = 1e30;
    hit.barycentrics = 0.0;
    const float3 ab = b - a, ac = c - a;
    const float3 p = cross(direction, ac);
    const float determinant = dot(ab, p);
    if (abs(determinant) < 1e-12) return hit;
    const float inverse = 1.0 / determinant;
    const float3 t = origin - a;
    const float u = dot(t, p) * inverse;
    if (u < 0.0 || u > 1.0) return hit;
    const float3 q = cross(t, ab);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0 || u + v > 1.0) return hit;
    hit.distance = dot(ac, q) * inverse;
    hit.hit = hit.distance > 0.0;
    hit.barycentrics = float2(u, v);
    return hit;
}
