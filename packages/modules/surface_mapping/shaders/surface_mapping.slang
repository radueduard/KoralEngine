// The surface-mapping module: pictures put on a surface without its UVs — from the three axes (triplanar), or the two
// that suit it best (biplanar) — a height map's depth seen in parallax, layers blended by height, and a detail normal
// map over a base one.
//
//     import surface_mapping;
//
//     const float4 rock = KoralTriplanar(picture, smooth, world * 0.5, normal, 4.0);

module surface_mapping;

import koral_math;
import koral_shading;

/** How much each axis's projection counts for a surface facing @p normal: sharper the larger @p sharpness. */
public float3 KoralTriplanarWeights(float3 normal, float sharpness)
{
    const float3 w = pow(abs(normal), sharpness);
    return w / max(w.x + w.y + w.z, 1e-6);
}

/** A picture put on from the three axes, each where the surface faces it: no seams, three reads. */
public float4 KoralTriplanar(Texture2D<float4> picture, SamplerState sampler, float3 position, float3 normal, float sharpness = 4.0)
{
    const float3 w = KoralTriplanarWeights(normal, sharpness);
    return picture.Sample(sampler, position.zy) * w.x + picture.Sample(sampler, position.xz) * w.y + picture.Sample(sampler, position.xy) * w.z;
}

/** A normal map put on from the three axes (Golus's whiteout blend): which way the surface faces, in the world. */
public float3 KoralTriplanarNormal(Texture2D<float4> normalMap, SamplerState sampler, float3 position, float3 normal, float sharpness = 4.0)
{
    const float3 w = KoralTriplanarWeights(normal, sharpness);
    float3 tx = normalMap.Sample(sampler, position.zy).xyz * 2.0 - 1.0;
    float3 ty = normalMap.Sample(sampler, position.xz).xyz * 2.0 - 1.0;
    float3 tz = normalMap.Sample(sampler, position.xy).xyz * 2.0 - 1.0;
    tx = float3(tx.xy + normal.zy, abs(tx.z) * normal.x);
    ty = float3(ty.xy + normal.xz, abs(ty.z) * normal.y);
    tz = float3(tz.xy + normal.xy, abs(tz.z) * normal.z);
    return KoralSafeNormalize(tx.zyx * w.x + ty.xzy * w.y + tz.xyz * w.z, normal);
}

/** A picture put on from the two axes the surface faces most (Quílez's biplanar): two reads rather than three. */
public float4 KoralBiplanar(Texture2D<float4> picture, SamplerState sampler, float3 position, float3 normal, float sharpness = 8.0)
{
    const float3 n = abs(normal);
    const int3 major = n.x > n.y && n.x > n.z ? int3(0, 1, 2) : n.y > n.z ? int3(1, 2, 0) : int3(2, 0, 1);
    const int3 minor = n.x < n.y && n.x < n.z ? int3(0, 1, 2) : n.y < n.z ? int3(1, 2, 0) : int3(2, 0, 1);
    const int3 median = int3(3, 3, 3) - minor - major;
    const float4 x = picture.Sample(sampler, float2(position[major.y], position[major.z]));
    const float4 y = picture.Sample(sampler, float2(position[median.y], position[median.z]));
    float2 w = pow(saturate((float2(n[major.x], n[median.x]) - 0.5773) / (1.0 - 0.5773)), sharpness / 8.0);
    return (x * w.x + y * w.y) / max(w.x + w.y, 1e-6);
}

/**
 * Where on a height map the eye really sees, looking through @p uv at a surface @p toEyeTangent (in its tangent space,
 * z out) of @p depth: marched in @p steps layers and refined between the last two (parallax occlusion mapping). The
 * height map's 1 is the surface, 0 its deepest.
 */
public float2 KoralParallax(Texture2D<float4> heights, SamplerState sampler, float2 uv, float3 toEyeTangent, float depth, uint steps = 16)
{
    const float layer = 1.0 / float(max(steps, 1u));
    const float2 shift = toEyeTangent.xy / max(toEyeTangent.z, 0.05) * depth * layer;
    float2 at = uv;
    float below = 0.0, height = heights.SampleLevel(sampler, at, 0.0).r;
    float2 last = at;
    float lastGap = 0.0;
    for (uint i = 0; i < steps && below < 1.0 - height; ++i) {
        last = at;
        lastGap = (1.0 - height) - below;
        at -= shift;
        below += layer;
        height = heights.SampleLevel(sampler, at, 0.0).r;
    }
    const float gap = (1.0 - height) - below;
    const float t = gap / min(gap - lastGap, -1e-6);
    return lerp(at, last, saturate(t));
}

/** Two layers blended by their heights as well as by @p blend (0 the first, 1 the second): the higher shows through first, as sand between stones. */
public float KoralHeightBlend(float firstHeight, float secondHeight, float blend, float softness = 0.2)
{
    const float a = firstHeight + 1.0 - blend, b = secondHeight + blend;
    const float top = max(a, b) - softness;
    const float wa = max(a - top, 0.0), wb = max(b - top, 0.0);
    return wb / max(wa + wb, 1e-6);
}

/** A detail normal map's texel over a base one's, @p strength of it: koral_shading's reoriented blend. */
public float3 KoralDetailNormal(float3 baseTexel, float3 detailTexel, float strength)
{
    return KoralBlendNormalTexels(baseTexel, lerp(float3(0.5, 0.5, 1.0), detailTexel, saturate(strength)));
}
