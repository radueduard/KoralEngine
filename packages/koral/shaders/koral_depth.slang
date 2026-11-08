// The engine's base library: what a depth image and a view say of where things are — a pixel's place in the world, its
// distance, the way the eye looks through it, and where it was the frame before.
//
//     import koral_depth;
//
//     const float3 world = KoralWorldFromDepth(uv, depth.Load(int3(pixel, 0)), frame.inverseViewProjection);
//
// A picture's uv runs from 0 to 1 as its clip space's x and y run from -1 to 1: uv * 2 - 1, as the engine's passes
// have it (a lens that flips y flips both).

module koral_depth;

/** Where clip-space x and y are for a uv: uv * 2 - 1. */
public float2 KoralClipFromUv(float2 uv) { return uv * 2.0 - 1.0; }
public float2 KoralUvFromClip(float2 clip) { return clip * 0.5 + 0.5; }

/** Where in the world what a pixel shows is: its uv and depth, back through the view and the lens. */
public float3 KoralWorldFromDepth(float2 uv, float depth, float4x4 inverseViewProjection)
{
    const float4 world = mul(float4(KoralClipFromUv(uv), depth, 1.0), inverseViewProjection);
    return world.xyz / world.w;
}

/** How far along the view a perspective depth is, in the world's units: 0 at the eye. */
public float KoralLinearDepth(float depth, float near, float far, bool reversed)
{
    const float d = reversed ? 1.0 - depth : depth;
    return near * far / max(far - d * (far - near), 1e-12);
}

/** A ray from the eye through a point of the picture: from the near plane, of unit length. */
public struct KoralRay {
    public float3 origin;
    public float3 direction;
    /** How long it is to the far plane. */
    public float length;
};

/** The ray through clip-space @p clip (x and y from -1 to 1): from where it is at depth 0 to where it is at depth 1. */
public KoralRay KoralRayThrough(float2 clip, float4x4 inverseViewProjection)
{
    const float4 near = mul(float4(clip, 0.0, 1.0), inverseViewProjection);
    const float4 far = mul(float4(clip, 1.0, 1.0), inverseViewProjection);
    const float3 from = near.xyz / near.w, to = far.xyz / far.w;
    KoralRay ray;
    ray.origin = from;
    ray.length = length(to - from);
    ray.direction = (to - from) / max(ray.length, 1e-12);
    return ray;
}

/**
 * Where a pixel was the frame before: its uv and depth put back by @p currentToPrevious (the frame before's
 * viewProjection after this frame's inverse). (-1, -1) where it was behind the eye.
 */
public float2 KoralReproject(float2 uv, float depth, float4x4 currentToPrevious)
{
    const float4 before = mul(float4(KoralClipFromUv(uv), depth, 1.0), currentToPrevious);
    return before.w > 0.0 ? KoralUvFromClip(before.xy / before.w) : float2(-1.0, -1.0);
}

/** Whether a uv is within the picture. */
public bool KoralInPicture(float2 uv) { return all(uv >= 0.0) && all(uv <= 1.0); }
