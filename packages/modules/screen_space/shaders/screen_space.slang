// The screen-space module: what is worked out of the picture's own depth — how much of the sky each point sees (ground-
// truth ambient occlusion, after Jimenez et al. and Intel's XeGTAO), what a shiny surface mirrors of what is on the
// picture (reflections traced through the depth), and light blurred under a skin by how far it scatters. What is off the
// picture, or behind what is on it, they cannot see: rays can (the raytracing_extras module).
//
//     import screen_space;
//
//     const float3 seen = KoralSeenAt(depth, pixel, size, inverseProjection);     // a pixel's place, seen from the eye

module screen_space;

import koral_math;
import koral_depth;

/** Where pixel @p pixel of a picture @p size is, seen from the eye: its depth back through the lens alone (view space). */
public float3 KoralSeenAt(Texture2D<float> depth, int2 pixel, float2 size, float4x4 inverseProjection)
{
    const float d = depth.Load(int3(pixel, 0));
    const float4 seen = mul(float4((float2(pixel) + 0.5) / size * 2.0 - 1.0, d, 1.0), inverseProjection);
    return seen.xyz / seen.w;
}

/** How far along the view a depth is, through the lens's inverse: what is compared across an edge. */
public float KoralViewDistance(float depth, float4x4 inverseProjection)
{
    const float4 seen = mul(float4(0.0, 0.0, depth, 1.0), inverseProjection);
    return -seen.z / seen.w;
}

/** Which way the surface a pixel shows faces, seen from the eye: from its neighbours nearer in depth each way, so that an edge does not bend it. */
public float3 KoralNormalOfDepth(Texture2D<float> depth, int2 pixel, float2 size, float4x4 inverseProjection)
{
    const float3 p = KoralSeenAt(depth, pixel, size, inverseProjection);
    const float3 left = KoralSeenAt(depth, pixel - int2(1, 0), size, inverseProjection), right = KoralSeenAt(depth, pixel + int2(1, 0), size, inverseProjection);
    const float3 up = KoralSeenAt(depth, pixel - int2(0, 1), size, inverseProjection), down = KoralSeenAt(depth, pixel + int2(0, 1), size, inverseProjection);
    const float3 dx = abs(right.z - p.z) < abs(p.z - left.z) ? right - p : p - left;
    const float3 dy = abs(down.z - p.z) < abs(p.z - up.z) ? down - p : p - up;
    float3 n = KoralSafeNormalize(cross(dy, dx));
    if (dot(n, -p) < 0.0) n = -n;
    return n;
}

/** How much a texel @p there counts toward one @p here (both distances along the view): one at another depth, nothing. */
public float KoralDepthAlike(float here, float there, float tolerance = 0.03)
{
    const float apart = (there - here) / max(here * tolerance, 0.02);
    return exp(-apart * apart);
}

/** Jimenez's separable subsurface profile: how much of the light scattered under a skin comes out @p distance (in its widths) away. */
public float3 KoralSubsurfaceProfile(float distance, float3 falloff)
{
    const float3 d = distance / max(falloff, 1e-4);
    return exp(-d * d * 0.5) * 0.7 + exp(-d * 0.5) * 0.3;
}
