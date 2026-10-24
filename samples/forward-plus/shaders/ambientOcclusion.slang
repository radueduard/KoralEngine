// Horizon-based ambient occlusion: what is in a corner sees less of the sky, and is darker. Three passes over the
// picture — how occluded each pixel is, from the images of where each pixel's surface is and which way it faces; that,
// blurred without bleeding across an edge or round a crease; and the picture multiplied by it. Before the sky, which
// nothing occludes.
//
// `radius`, `intensity` and `bias` are given on the first pipeline's node (push.radius, ...).

import forwardCommon;
import koral_debug;

Sampler2D<float4> positionTex;      // where each pixel's surface is in the world; w is 0 where there is none
Sampler2D<float4> normalTex;        // which way it faces in the world, as the forward pass shaded it
[[vk::image_format("r8")]] RWTexture2D<float> occlusionOut;
Sampler2D<float> occlusionIn;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;

static const int Directions = 8;
static const int Steps = 6;
static const float Pi = 3.14159265359;

/** Interleaved gradient noise: the same for a pixel every frame, and unlike its neighbours'. */
float noise(int2 p)
{
    return frac(52.9829189 * frac(0.06711056 * float(p.x) + 0.00583715 * float(p.y)));
}

/** Which way a place in the picture's surface faces, as the camera sees it. */
float3 facingAt(float2 uv) { return normalize(mul(float4(normalTex.SampleLevel(uv, 0.0).xyz, 0.0), camera.view).xyz); }

/** Where a place in the picture's surface is, as the camera sees it; w is 1 where there is one. */
float4 seenAt(float2 uv)
{
    float4 world = positionTex.SampleLevel(uv, 0.0);
    return float4(mul(float4(world.xyz, 1.0), camera.view).xyz, world.w);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void occlusion(uint3 id : SV_DispatchThreadID, uniform float radius, uniform float intensity, uniform float bias)
{
    uint width, height;
    occlusionOut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(id.xy) + 0.5) * texel;

    float4 centre = seenAt(uv);
    if (centre.w < 0.5) { occlusionOut[id.xy] = 1.0; return; }
    float3 P = centre.xyz;

    // Which way the surface faces: as it was shaded, not worked out from its neighbours — which, at an edge, are
    // another surface's, and leave a rim of the edge unoccluded.
    float3 N = facingAt(uv);
    if (dot(N, P) > 0.0) N = -N;
    // A little off the surface, so that its own facets do not occlude it.
    P += N * radius * 0.02;

    // The radius in the world, as a part of the picture at this depth: at least a few pixels, at most a third of it.
    float2 reach = radius * 0.5 / max(-P.z, 1e-3) * float2(abs(camera.projection[0][0]), abs(camera.projection[1][1]));
    reach = clamp(reach, texel * 4.0, float2(0.3, 0.3));
    // Each pixel turned, and its steps started, a little otherwise than its neighbours': the blur after makes one of them.
    float turn = noise(int2(id.xy)) * 2.0 * Pi;
    float start = noise(int2(id.yx) + int2(17, 59));
    float tangent = sin(bias);
    float occluded = 0.0;
    for (int d = 0; d < Directions; ++d) {
        float angle = turn + float(d) * (2.0 * Pi / float(Directions));
        float2 direction = float2(cos(angle), sin(angle));
        // Out along the direction, each rise of its horizon above the surface occludes it: by how far it rose, the
        // less the further off what raised it is (Bavoil and Sainz's HBAO). What is out of reach raises nothing.
        float horizon = tangent;
        for (int s = 0; s < Steps; ++s) {
            float t = (float(s) + start) / float(Steps);
            float2 offset = direction * max(reach * t * t + texel, texel * 1.5);     // closer together near the pixel
            float2 sampleUv = uv + offset;
            if (any(sampleUv < 0.0) || any(sampleUv > 1.0)) break;
            float4 S = seenAt(sampleUv);
            if (S.w < 0.5) continue;
            float3 V = S.xyz - P;
            float distance2 = dot(V, V);
            if (distance2 < 1e-8 || distance2 > radius * radius) continue;
            float height = dot(N, V) * rsqrt(distance2);
            if (height > horizon) {
                occluded += (height - horizon) * (1.0 - distance2 / (radius * radius));
                horizon = height;
            }
        }
    }
    occlusionOut[id.xy] = pow(saturate(1.0 - occluded / float(Directions)), intensity);
}

/**
 * A 7x7 blur that weighs each neighbour by how near it is, by how near its depth is to the pixel's own — as a part of
 * that depth, so that it keeps to a surface near and far — and by how alike the two face; what is not a surface is left out.
 */
[shader("compute")]
[numthreads(16, 16, 1)]
void blur(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    occlusionOut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(id.xy) + 0.5) * texel;
    float4 own = seenAt(uv);
    if (own.w < 0.5) { occlusionOut[id.xy] = 1.0; return; }
    float depth = -own.z;
    float tolerance = max(depth * 0.03, 0.02);
    float3 facing = facingAt(uv);
    float sum = 0.0, weight = 0.0;
    for (int x = -3; x <= 3; ++x)
        for (int y = -3; y <= 3; ++y) {
            float2 at = uv + float2(x, y) * texel;
            float4 seen = seenAt(at);
            if (seen.w < 0.5) continue;
            float difference = (depth + seen.z) / tolerance;
            float bent = saturate(dot(facing, facingAt(at)));     // nor round a crease
            float w = exp(-difference * difference) * exp(-float(x * x + y * y) / 8.0) * bent * bent * bent * bent;
            sum += occlusionIn.SampleLevel(at, 0.0) * w;
            weight += w;
        }
    occlusionOut[id.xy] = weight > 0.0 ? sum / weight : 1.0;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void compose(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float4 color = hdrImage[id.xy];
    // In the editor's scene view: the occlusion itself, as a grey; any other channel of a material left as it is.
    if (koralDebugView == KoralDebugOcclusion) { hdrImage[id.xy] = float4(float3(occlusionIn.SampleLevel(uv, 0.0)), color.a); return; }
    if (koralDebugShowing()) return;
    hdrImage[id.xy] = float4(color.rgb * occlusionIn.SampleLevel(uv, 0.0), color.a);
}
