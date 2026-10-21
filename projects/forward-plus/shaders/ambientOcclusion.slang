// Horizon-based ambient occlusion: what is in a corner sees less of the sky, and is darker. Three passes over the
// picture — how occluded each pixel is, from the image of where each pixel's surface is; that, blurred without
// bleeding across an edge; and the picture multiplied by it. Before the sky, which nothing occludes.
//
// `radius`, `intensity` and `bias` are given on the first pipeline's node (push.radius, ...).

import forwardCommon;

Sampler2D<float4> positionTex;      // where each pixel's surface is in the world; w is 0 where there is none
[[vk::image_format("r8")]] RWTexture2D<float> occlusionOut;
Sampler2D<float> occlusionIn;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;

static const int Directions = 8;
static const int Steps = 6;
static const float Pi = 3.14159265359;

/** Interleaved gradient noise: the same for a pixel every frame. */
float noise(int2 p)
{
    return frac(52.9829189 * frac(0.06711056 * float(p.x) + 0.00583715 * float(p.y)));
}

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

    // Which way the surface faces, from where its neighbours are: the nearer of each pair, so that an edge is not smeared.
    float4 right = seenAt(uv + float2(texel.x, 0.0)), left = seenAt(uv - float2(texel.x, 0.0));
    float4 down = seenAt(uv + float2(0.0, texel.y)), up = seenAt(uv - float2(0.0, texel.y));
    float3 across = abs(right.z - P.z) < abs(P.z - left.z) ? right.xyz - P : P - left.xyz;
    float3 along = abs(down.z - P.z) < abs(P.z - up.z) ? down.xyz - P : P - up.xyz;
    float3 N = normalize(cross(across, along));
    if (dot(N, P) > 0.0) N = -N;

    // The radius in the world, as a part of the picture at this depth.
    float2 reach = min(radius * 0.5 / max(-P.z, 1e-3) * float2(abs(camera.projection[0][0]), abs(camera.projection[1][1])), 0.3);
    float turn = noise(int2(id.xy)) * 2.0 * Pi;
    float tangent = sin(bias);
    float occluded = 0.0;
    for (int d = 0; d < Directions; ++d) {
        float angle = turn + float(d) * (2.0 * Pi / float(Directions));
        float2 direction = float2(cos(angle), sin(angle));
        // Along the direction, the highest thing there is above the surface: its horizon.
        float horizon = tangent, at = radius;
        for (int s = 1; s <= Steps; ++s) {
            float2 sampleUv = uv + direction * reach * (float(s) / float(Steps));
            if (any(sampleUv < 0.0) || any(sampleUv > 1.0)) break;
            float4 S = seenAt(sampleUv);
            if (S.w < 0.5) continue;
            float3 V = S.xyz - P;
            float distance = length(V);
            if (distance < 1e-4 || distance > radius) continue;
            float height = dot(N, V) / distance;
            if (height > horizon) { horizon = height; at = distance; }
        }
        occluded += max(horizon - tangent, 0.0) * saturate(1.0 - at * at / (radius * radius));
    }
    occlusionOut[id.xy] = pow(saturate(1.0 - occluded / float(Directions)), intensity);
}

/** A 5x5 blur that weighs each neighbour by how near its depth is to the pixel's own. */
[shader("compute")]
[numthreads(16, 16, 1)]
void blur(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    occlusionOut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(id.xy) + 0.5) * texel;
    float depth = -seenAt(uv).z;
    float sum = 0.0, weight = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            float2 at = uv + float2(x, y) * texel;
            float4 seen = seenAt(at);
            if (seen.w < 0.5) continue;
            float difference = depth + seen.z;
            float w = exp(-difference * difference / 0.25);
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
    hdrImage[id.xy] = float4(color.rgb * occlusionIn.SampleLevel(uv, 0.0), color.a);
}
