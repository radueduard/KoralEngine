// The raytracing-extras module: what rays traced inline (RayQuery) into a scene's acceleration structure are used for
// beside the picture itself — whether a light is seen past what is in the way (a shadow ray, of a light with a size),
// how much of the sky a point sees (ambient occlusion), and how wide a ray's footprint is where it lands (a ray cone),
// for reading a picture at the level that suits it. Their noise is the filters module's Denoise's to take out.
//
//     import raytracing_extras;
//
//     const float lit = KoralTracedShadow(scene, position, normal, toLight, distance, 0.02, KoralRandom2(pixel, frame));

module raytracing_extras;

import koral_math;
import koral_sampling;
import koral_random;

/** Whether anything is in the way from @p origin along @p direction within @p distance: the first hit ends it. */
public bool KoralTraceAny(RaytracingAccelerationStructure scene, float3 origin, float3 direction, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {}
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

/** How far along a ray the nearest hit is, or @p distance where nothing is. */
public float KoralTraceDistance(RaytracingAccelerationStructure scene, float3 origin, float3 direction, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {}
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : distance;
}

/** Where a ray starts off a surface so as not to hit it: along its normal, by as much as a float's precision needs there. */
public float3 KoralOffsetRay(float3 position, float3 normal) { return position + normal * max(1e-3, 1e-4 * KoralMax3(abs(position))); }

/**
 * Whether a light @p distance away along @p toLight is seen from a point: one ray, within a cone as wide as the light
 * (@p spread, the sine of half its angle) — a soft edge, once many frames are averaged (@p u, two numbers 0 to 1).
 */
public float KoralTracedShadow(RaytracingAccelerationStructure scene, float3 position, float3 normal, float3 toLight, float distance, float spread, float2 u)
{
    const KoralBasis basis = KoralBasisOf(toLight);
    const float3 way = basis.ToWorld(KoralUniformCone(u, sqrt(max(1.0 - spread * spread, 0.0))));
    return KoralTraceAny(scene, KoralOffsetRay(position, normal), way, distance) ? 0.0 : 1.0;
}

/** How much of what is round a point it sees within @p radius: @p rays cosine-spread rays, the share that hit nothing. */
public float KoralTracedOcclusion(RaytracingAccelerationStructure scene, float3 position, float3 normal, float radius, uint rays, uint2 pixel, uint frame)
{
    const KoralBasis basis = KoralBasisOf(normal);
    const float3 from = KoralOffsetRay(position, normal);
    float open = 0.0;
    for (uint i = 0; i < rays; ++i) {
        const float2 u = frac(KoralR2(i) + KoralRandom2(pixel, frame));
        const float hit = KoralTraceDistance(scene, from, basis.ToWorld(KoralCosineHemisphere(u)), radius);
        open += saturate(hit / radius);
    }
    return open / float(max(rays, 1u));
}

/** A ray cone: how wide it is where it starts, and how fast it widens (radians). A pixel's: 0 wide, as wide as a pixel sees. */
public struct KoralRayCone {
    public float width;
    public float spread;
};

/** A camera's ray's cone through a pixel of a picture @p height pixels high seen through @p fieldOfView radians. */
public KoralRayCone KoralPixelCone(float fieldOfView, uint height)
{
    KoralRayCone cone;
    cone.width = 0.0;
    cone.spread = atan(2.0 * tan(fieldOfView * 0.5) / float(height));
    return cone;
}

/** The cone @p distance on: as wide as it has grown, and widened by what it lands on (@p curvature, 0 for flat). */
public KoralRayCone KoralPropagateCone(KoralRayCone cone, float distance, float curvature = 0.0)
{
    KoralRayCone next;
    next.width = cone.width + cone.spread * distance;
    next.spread = cone.spread + curvature;
    return next;
}

/**
 * The mip level a picture is read at where a cone lands: of the picture's @p size, the triangle's area in its uv over
 * its area in the world (@p uvArea, @p worldArea), and how square the ray meets it (@p cosine) — Akenine-Möller's.
 */
public float KoralConeLevel(KoralRayCone cone, float2 size, float uvArea, float worldArea, float cosine)
{
    const float lambda = 0.5 * log2(size.x * size.y * uvArea / max(worldArea, 1e-12));
    return max(lambda + log2(abs(cone.width) / max(abs(cosine), 1e-4)), 0.0);
}
