// The screen-space module's passes.
//
//   horizons, across, down   ambient occlusion: found at half the picture's size (it changes slowly), then blurred, keeping
//                            to what is at one depth, across at that size and down at the picture's own, which brings it
//                            up to that size as it goes (the GroundTruthOcclusion function)
//   reflections              what each pixel mirrors of the picture: a ray marched through the depth, and refined where it
//                            passes behind what is drawn — its colour, and how sure it is (alpha) (ScreenSpaceReflections)
//   subsurface               the light under a skin blurred along a direction by how far it scatters, where `mask` says
//                            (SubsurfaceBlur)
//
// What each is seen with comes on its Dispatch: the lens (`projection`) and what undoes it (`inverseProjection`).

import screen_space;
import koral_math;
import koral_random;
import koral_depth;

Texture2D<float> depth;

// ---- ambient occlusion --------------------------------------------------------------------------------------------

struct OcclusionPush {
    float4x4 inverseProjection;
    float projectionScale;      // the lens's [1][1]: how big a metre is on the picture at a metre away
    float radius;               // how far the rays it stands for reach, in the world
    float intensity;            // a power on what is found: above 1 for what the screen cannot show
    float fadeOut;              // the share of the radius over which what is near stops counting
};
[[vk::push_constant]] ConstantBuffer<OcclusionPush> occlusionPush;

static const uint Slices = 4;       // each two ways: eight directions
static const uint Steps = 6;

[[vk::image_format("r16f")]] RWTexture2D<float> raw;        // half the picture's size

int2 FullOf(int2 texel, uint width, uint height) { return min(texel * 2, int2(width - 1, height - 1)); }

/**
 * In each of a few slices through the view, the highest horizon each way within `radius`, and the cosine-weighted share
 * of the slice's half-circle above both: what a ray traced from the point, cosine weighted, would find open.
 */
[shader("compute")]
[numthreads(8, 8, 1)]
void horizons(uint3 id : SV_DispatchThreadID)
{
    uint halfWidth, halfHeight, width, height;
    raw.GetDimensions(halfWidth, halfHeight);
    if (id.x >= halfWidth || id.y >= halfHeight) return;
    depth.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float4x4 inverseProjection = occlusionPush.inverseProjection;
    const float radius = occlusionPush.radius;
    const int2 pixel = FullOf(int2(id.xy), width, height);
    if (depth.Load(int3(pixel, 0)) >= 1.0) { raw[id.xy] = 1.0; return; }
    const float3 p = KoralSeenAt(depth, pixel, size, inverseProjection);
    const float3 n = KoralNormalOfDepth(depth, pixel, size, inverseProjection);
    const float distance = -p.z;
    const float scale = 0.5 * size.y * abs(occlusionPush.projectionScale);
    const float reach = clamp(radius * scale / distance, 4.0, 0.3 * size.y);
    const float noise = KoralInterleavedGradientNoise(float2(pixel)), turn = KoralInterleavedGradientNoise(float2(pixel) + 37.0);
    const float3 v = KoralSafeNormalize(-p);
    float visible = 0.0;
    for (uint slice = 0; slice < Slices; ++slice) {
        const float angle = (float(slice) + turn) * KoralPi / float(Slices);
        const float2 across = float2(cos(angle), sin(angle));
        // The slice's way in the view, as the screen's way is: a point a little along it, at the same depth, minus this one.
        const float4 aside = mul(float4((float2(pixel) + 0.5 + across * 4.0) / size * 2.0 - 1.0, depth.Load(int3(pixel, 0)), 1.0), inverseProjection);
        const float3 way = KoralSafeNormalize(aside.xyz / aside.w - p);
        const float3 ortho = KoralSafeNormalize(way - dot(way, v) * v);
        const float3 axis = KoralSafeNormalize(cross(ortho, v));
        const float3 projected = n - axis * dot(n, axis);
        const float projectedLength = length(projected);
        const float cosN = saturate(dot(projected, v) / max(projectedLength, 1e-4));
        const float nAngle = sign(dot(ortho, projected)) * acos(cosN);
        float h[2];
        for (int side = 0; side < 2; ++side) {
            const float sideSign = side == 0 ? -1.0 : 1.0;
            const float lowest = cos(nAngle + sideSign * KoralHalfPi);    // flat on the surface: nothing hides anything
            float highest = lowest;
            for (uint s = 0; s < Steps; ++s) {
                const float t = (float(s) + noise) / float(Steps);
                const float offset = max(reach * t * t + 1.0, 1.5);
                const int2 at = int2(float2(pixel) + 0.5 + sideSign * across * offset);
                if (any(at < 0) || at.x >= int(width) || at.y >= int(height)) break;
                const float3 to = KoralSeenAt(depth, at, size, inverseProjection) - p;
                const float apart = length(to);
                if (apart < 1e-4) continue;
                // Fading out over the last of the radius, as a ray of that length would just miss it.
                const float near = saturate((radius - apart) / max(radius * occlusionPush.fadeOut, 1e-4));
                highest = max(highest, lerp(lowest, dot(to / apart, v), near));
            }
            h[side] = sideSign * acos(clamp(highest, -1.0, 1.0));
        }
        const float h0 = nAngle + clamp(h[0] - nAngle, -KoralHalfPi, KoralHalfPi), h1 = nAngle + clamp(h[1] - nAngle, -KoralHalfPi, KoralHalfPi);
        const float arc0 = (cosN + 2.0 * h0 * sin(nAngle) - cos(2.0 * h0 - nAngle)) * 0.25;
        const float arc1 = (cosN + 2.0 * h1 * sin(nAngle) - cos(2.0 * h1 - nAngle)) * 0.25;
        visible += projectedLength * (arc0 + arc1);
    }
    raw[id.xy] = pow(saturate(visible / float(Slices)), occlusionPush.intensity);
}

Texture2D<float> rough;                                 // what is blurred: `raw` across, then that down
[[vk::image_format("r16f")]] RWTexture2D<float> blurred;

float Alike(float here, float there) { return there >= 1.0 ? 0.0 : KoralDepthAlike(here, KoralViewDistance(there, occlusionPush.inverseProjection)); }

/** Blurred across, five texels of half the picture's size, each as much as it is at the texel's depth. */
[shader("compute")]
[numthreads(8, 8, 1)]
void across(uint3 id : SV_DispatchThreadID)
{
    uint halfWidth, halfHeight, width, height;
    blurred.GetDimensions(halfWidth, halfHeight);
    if (id.x >= halfWidth || id.y >= halfHeight) return;
    depth.GetDimensions(width, height);
    const int2 texel = int2(id.xy);
    const float d = depth.Load(int3(FullOf(texel, width, height), 0));
    if (d >= 1.0) { blurred[texel] = 1.0; return; }
    const float here = KoralViewDistance(d, occlusionPush.inverseProjection);
    float sum = 0.0, weights = 0.0;
    for (int x = -2; x <= 2; ++x) {
        const int2 at = clamp(texel + int2(x, 0), int2(0, 0), int2(halfWidth - 1, halfHeight - 1));
        const float w = Alike(here, depth.Load(int3(FullOf(at, width, height), 0))) * exp(-float(x * x) / 2.0);
        sum += rough.Load(int3(at, 0)) * w;
        weights += w;
    }
    blurred[texel] = weights > 0.0 ? sum / weights : 1.0;
}

/** Blurred down, at the picture's own size: five rows of what was blurred across, from the two columns either side of each pixel. */
[shader("compute")]
[numthreads(8, 8, 1)]
void down(uint3 id : SV_DispatchThreadID)
{
    uint width, height, halfWidth, halfHeight;
    depth.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    rough.GetDimensions(halfWidth, halfHeight);
    const int2 pixel = int2(id.xy);
    const float d = depth.Load(int3(pixel, 0));
    if (d >= 1.0) { blurred[pixel] = 1.0; return; }
    const float here = KoralViewDistance(d, occlusionPush.inverseProjection);
    const float2 c = (float2(pixel) + 0.5) * 0.5 - 0.5;
    const int x0 = int(floor(c.x)), y0 = int(round(c.y));
    const float right = c.x - float(x0);
    float sum = 0.0, weights = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int side = 0; side < 2; ++side) {
            const int2 at = clamp(int2(x0 + side, y0 + y), int2(0, 0), int2(halfWidth - 1, halfHeight - 1));
            const float dy = float(y0 + y) - c.y;
            const float w = Alike(here, depth.Load(int3(FullOf(at, width, height), 0))) * exp(-dy * dy / 2.0) * (side == 0 ? 1.0 - right : right);
            sum += rough.Load(int3(at, 0)) * w;
            weights += w;
        }
    blurred[pixel] = weights > 0.0 ? sum / weights : 1.0;
}

// ---- reflections --------------------------------------------------------------------------------------------------

Texture2D<float4> picture;                  // what is mirrored: the lit picture
Texture2D<float4> normals;                  // each pixel's, in the world (xyz)
SamplerState linearSampler;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> reflected;

struct ReflectPush {
    float4x4 viewProjection;
    float4x4 inverseViewProjection;
    float maxDistance;          // how far along a ray it is marched, in the world
    float thickness;            // how far behind what is drawn a ray may pass and still hit it
    uint steps;
    uint frame;
};
[[vk::push_constant]] ConstantBuffer<ReflectPush> reflectPush;

[shader("compute")]
[numthreads(8, 8, 1)]
void reflections(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    reflected.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float2 size = float2(width, height);
    const float2 uv = KoralPixelUv(id.xy, uint2(width, height));
    const float d = depth.Load(int3(id.xy, 0));
    if (d >= 1.0) { reflected[id.xy] = 0.0; return; }
    const float3 p = KoralWorldFromDepth(uv, d, reflectPush.inverseViewProjection);
    const float3 eye = KoralRayThrough(float2(0.0), reflectPush.inverseViewProjection).origin;
    const float3 n = KoralSafeNormalize(normals.Load(int3(id.xy, 0)).xyz);
    const float3 r = reflect(KoralSafeNormalize(p - eye), n);
    const uint steps = max(reflectPush.steps, 1u);
    const float step = reflectPush.maxDistance / float(steps);
    const float jitter = KoralInterleavedGradientNoise(float2(id.xy) + float(reflectPush.frame % 64u) * 5.588238);
    float t = step * jitter;
    float lastT = 0.0;
    for (uint i = 0; i < steps; ++i) {
        t += step;
        const float4 clip = mul(float4(p + r * t, 1.0), reflectPush.viewProjection);
        if (clip.w <= 0.0) break;
        const float2 at = KoralUvFromClip(clip.xy / clip.w);
        if (!KoralInPicture(at)) break;
        const float drawn = depth.SampleLevel(linearSampler, at, 0.0);
        const float behind = clip.z / clip.w - drawn;
        if (behind > 0.0) {
            // Passed behind what is drawn: refined between the last step and this one, then hit if not too far behind.
            float low = lastT, high = t;
            float2 hit = at;
            for (uint k = 0; k < 6; ++k) {
                const float mid = (low + high) * 0.5;
                const float4 c = mul(float4(p + r * mid, 1.0), reflectPush.viewProjection);
                const float2 m = KoralUvFromClip(c.xy / c.w);
                if (c.z / c.w > depth.SampleLevel(linearSampler, m, 0.0)) { high = mid; hit = m; } else low = mid;
            }
            const float3 there = KoralWorldFromDepth(hit, depth.SampleLevel(linearSampler, hit, 0.0), reflectPush.inverseViewProjection);
            if (length(there - (p + r * high)) > reflectPush.thickness) break;
            // Less sure toward the picture's edges, and the further it went.
            const float2 edge = min(hit, 1.0 - hit);
            const float sure = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - saturate(high / reflectPush.maxDistance));
            reflected[id.xy] = float4(picture.SampleLevel(linearSampler, hit, 0.0).rgb, sure);
            return;
        }
        lastT = t;
    }
    reflected[id.xy] = 0.0;
}


// ---- subsurface ---------------------------------------------------------------------------------------------------

Texture2D<float4> source;
Texture2D<float4> mask;                     // its red: how much of a pixel is skin, 0 to 1
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;

/** The light under a skin blurred along @p direction, as wide as @p width in the world, by Jimenez's profile, kept to its depth. */
[shader("compute")]
[numthreads(16, 16, 1)]
void subsurface(uint3 id : SV_DispatchThreadID, uniform float2 direction, uniform float width, uniform float4 falloff, uniform float4x4 inverseProjection,
                uniform float projectionScale)
{
    uint w, h;
    target.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const float4 centre = source.Load(int3(id.xy, 0));
    const float skin = mask.Load(int3(id.xy, 0)).r;
    const float d = depth.Load(int3(id.xy, 0));
    if (skin <= 0.0 || d >= 1.0) { target[id.xy] = centre; return; }
    const float here = KoralViewDistance(d, inverseProjection);
    // How many pixels the width is, at this distance.
    const float pixels = width * 0.5 * float(h) * abs(projectionScale) / max(here, 1e-3);
    float3 sum = 0.0, weights = 0.0;
    for (int i = -6; i <= 6; ++i) {
        const float offset = float(i) / 6.0;
        const int2 at = clamp(int2(float2(id.xy) + direction * offset * pixels), int2(0, 0), int2(w, h) - 1);
        const float3 weight = KoralSubsurfaceProfile(abs(offset) * 2.0, falloff.rgb) * KoralDepthAlike(here, KoralViewDistance(depth.Load(int3(at, 0)), inverseProjection), 0.05);
        sum += source.Load(int3(at, 0)).rgb * weight;
        weights += weight;
    }
    target[id.xy] = float4(lerp(centre.rgb, sum / max(weights, 1e-6), skin), centre.a);
}
