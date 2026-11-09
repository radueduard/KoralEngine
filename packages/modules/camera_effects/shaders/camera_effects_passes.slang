// The camera-effects module's passes, over a picture of light (`source`) into `target`:
//
//   circles      each pixel's circle of confusion, from its depth (`depth`, and the lens): signed, near behind far
//   focus        the picture blurred by it: a disc of taps as wide as each pixel's circle, a tap counting where its own
//                circle reaches the pixel — so what is in focus does not bleed into what is behind it
//   motion       the camera's own motion: each pixel's way across the picture since the frame before, from its depth
//                and the two frames' views, and taps along it
//   lens         chromatic aberration, vignetting and grain at once
//   brights      a flare's start: what is brighter than a threshold, at a picture's size
//   ghosts       a flare: the bright parts mirrored through the middle, several times smaller and larger, and a halo

import camera_effects;
import koral_math;
import koral_depth;
import koral_sampling;
import koral_color;

Texture2D<float4> source;
SamplerState linearSampler;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;
Texture2D<float> depth;
Texture2D<float4> circles;          // circles' own: the circle of confusion of each pixel, in its red
[[vk::image_format("rgba16f")]] RWTexture2D<float4> circlesOut;

bool Size(uint2 id, out uint2 size) { uint w, h; target.GetDimensions(w, h); size = uint2(w, h); return all(id < size); }

[shader("compute")]
[numthreads(16, 16, 1)]
void circleOfConfusion(uint3 id : SV_DispatchThreadID, uniform float near, uniform float far, uniform uint reversed, uniform float focus,
                       uniform float fStop, uniform float focalLength)
{
    uint width, height;
    circlesOut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float d = depth.Load(int3(id.xy, 0));
    const float distance = KoralLinearDepth(d, near, far, reversed != 0);
    circlesOut[id.xy] = float4(KoralCircleOfConfusion(distance, focus, fStop, focalLength), 0.0, 0.0, 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void focus(uint3 id : SV_DispatchThreadID, uniform float most)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float2 uv = KoralPixelUv(id.xy, size);
    const float own = circles.Load(int3(id.xy, 0)).r;
    const float radius = min(abs(own), most) * float(size.y) * 0.5;      // in pixels
    if (radius < 0.5) { target[id.xy] = source.Load(int3(id.xy, 0)); return; }
    const uint taps = 32;
    float4 sum = 0.0;
    float total = 0.0;
    for (uint i = 0; i < taps; ++i) {
        const float2 offset = KoralVogelDisk(i, taps, 0.0) * radius;
        const float2 at = uv + offset / float2(size);
        const float theirs = min(abs(circles.SampleLevel(linearSampler, at, 0.0).r), most) * float(size.y) * 0.5;
        // A tap counts where its own circle reaches this pixel; one nearer than this one always does (it blurs over it).
        const float w = theirs >= length(offset) || circles.SampleLevel(linearSampler, at, 0.0).r < own ? 1.0 : 0.0;
        sum += source.SampleLevel(linearSampler, at, 0.0) * w;
        total += w;
    }
    target[id.xy] = total > 0.0 ? sum / total : source.Load(int3(id.xy, 0));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void motion(uint3 id : SV_DispatchThreadID, uniform float4x4 currentToPrevious, uniform float strength, uniform uint taps)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float2 uv = KoralPixelUv(id.xy, size);
    const float2 was = KoralReproject(uv, depth.Load(int3(id.xy, 0)), currentToPrevious);
    const float2 way = was.x < -0.5 ? float2(0.0) : (uv - was) * strength;
    const uint count = max(taps, 1u);
    float4 sum = 0.0;
    for (uint i = 0; i < count; ++i) sum += source.SampleLevel(linearSampler, uv - way * ((float(i) + 0.5) / float(count) - 0.5), 0.0);
    target[id.xy] = sum / float(count);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void lens(uint3 id : SV_DispatchThreadID, uniform float aberration, uniform float vignette, uniform float grain, uniform uint frame)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float2 uv = KoralPixelUv(id.xy, size);
    float3 c = aberration > 0.0 ? KoralChromaticAberration(source, linearSampler, uv, aberration) : source.Load(int3(id.xy, 0)).rgb;
    c *= KoralVignette(uv, vignette, float(size.x) / float(size.y));
    const float y = KoralLuminance(c);
    c = max(c + c * KoralGrain(id.xy, frame, saturate(y)) * grain, 0.0);
    target[id.xy] = float4(c, 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void brights(uint3 id : SV_DispatchThreadID, uniform float threshold)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float3 c = KoralSane(source.SampleLevel(linearSampler, KoralPixelUv(id.xy, size), 0.0).rgb);
    target[id.xy] = float4(max(c - threshold, 0.0), 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void ghosts(uint3 id : SV_DispatchThreadID, uniform float strength, uniform float halo)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float2 uv = 1.0 - KoralPixelUv(id.xy, size);         // through the middle
    const float2 toMiddle = 0.5 - uv;
    float3 sum = 0.0;
    const float scales[4] = { 0.4, -0.7, 1.3, -1.8 };
    const float3 tints[4] = { float3(1.0, 0.8, 0.6), float3(0.6, 0.8, 1.0), float3(0.9, 0.6, 1.0), float3(0.7, 1.0, 0.7) };
    [unroll] for (uint i = 0; i < 4; ++i) {
        const float2 at = uv + toMiddle * scales[i];
        const float fade = 1.0 - smoothstep(0.0, 0.75, length(at - 0.5));
        sum += source.SampleLevel(linearSampler, at, 0.0).rgb * tints[i] * fade;
    }
    // The halo: a ring of what is bright, at a fixed width from the middle.
    const float2 ring = uv + KoralSafeNormalize(float3(toMiddle, 0.0), float3(1.0, 0.0, 0.0)).xy * 0.35;
    sum += source.SampleLevel(linearSampler, ring, 0.0).rgb * pow(1.0 - abs(length(ring - 0.5) - 0.35) * 4.0, 4.0) * halo;
    target[id.xy] = float4(sum * strength, 1.0);
}

/** A flare added to the picture it came of: `flare` read at its size. */
Texture2D<float4> flare;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> picture;

[shader("compute")]
[numthreads(16, 16, 1)]
void addFlare(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    picture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    picture[id.xy] = float4(picture[id.xy].rgb + flare.SampleLevel(linearSampler, KoralPixelUv(id.xy, uint2(width, height)), 0.0).rgb, picture[id.xy].a);
}
