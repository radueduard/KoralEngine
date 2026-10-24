// Screen-space reflections. For each pixel the ray from the eye is mirrored in the surface and walked through the
// scene as the camera sees it: where it passes just behind something in the picture, that is what the surface
// mirrors. One pass traces — it reads the picture at other pixels, so it cannot also write it — and one adds what
// was traced to the picture.
//
// `maxDistance`, `thickness`, `maxRoughness` and `intensity` are given on the trace pipeline's node.

import forwardCommon;
import koral_debug;

Sampler2D<float4> hdr;              // what is mirrored
Sampler2D<float4> positionTex;      // where each pixel's surface is; w is 0 where there is none
Sampler2D<float4> normalTex;
Sampler2D<float4> specularTex;      // rgb: what it mirrors straight on; a: how rough it is
[[vk::image_format("rgba16f")]] RWTexture2D<float4> reflection;
Sampler2D<float4> traced;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;

static const int Steps = 48;
static const int Refinements = 5;

int2 texelOf(float2 uv)
{
    return clamp(int2(uv * camera.screenSize), int2(0, 0), int2(camera.screenSize) - 1);
}

/** How far behind the scene a point is, along its own pixel: above 0, the ray has gone behind something. */
float behind(float3 point, float2 uv)
{
    float4 scene = positionTex.Load(int3(texelOf(uv), 0));
    if (scene.w == 0.0) return -1e30;
    return length(point - camera.position) - length(scene.xyz - camera.position);
}

/** Where in the picture a point of the world is; false once it is behind the eye or out of the picture. */
bool project(float3 point, out float2 uv)
{
    float4 clip = toClip(point);
    uv = clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5;
    return clip.w > 1e-4 && all(uv >= 0.0) && all(uv <= 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void trace(uint3 id : SV_DispatchThreadID, uniform float maxDistance, uniform float thickness, uniform float maxRoughness, uniform float intensity)
{
    uint width, height;
    reflection.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    int3 pixel = int3(id.xy, 0);
    float4 surface = positionTex.Load(pixel);
    float4 specular = specularTex.Load(pixel);
    float roughness = specular.a;
    if (surface.w == 0.0 || roughness >= maxRoughness) { reflection[id.xy] = 0.0; return; }

    float3 P = surface.xyz;
    float3 N = normalize(normalTex.Load(pixel).xyz);
    float3 V = normalize(P - camera.position);
    float3 R = reflect(V, N);

    // A step at a time, then closer and closer to the first step that went behind something.
    float stride = maxDistance / float(Steps);
    float3 origin = P + N * 0.02;
    float previous = 0.0;
    float t = stride * frac(52.9829189 * frac(0.06711056 * float(id.x) + 0.00583715 * float(id.y)));
    bool hit = false;
    float2 hitUv = 0.0;
    for (int i = 0; i < Steps; ++i) {
        t += stride;
        float2 uv;
        if (!project(origin + R * t, uv)) break;
        float depth = behind(origin + R * t, uv);
        if (depth > 0.0 && depth < thickness) {
            float lo = previous, hi = t;
            for (int j = 0; j < Refinements; ++j) {
                float mid = 0.5 * (lo + hi);
                float2 midUv;
                if (!project(origin + R * mid, midUv)) break;
                if (behind(origin + R * mid, midUv) > 0.0) hi = mid; else lo = mid;
            }
            hit = project(origin + R * hi, hitUv);
            t = hi;
            break;
        }
        previous = t;
    }
    // The back of something is the ray going through what is thin, and no reflection.
    if (hit && dot(normalize(normalTex.Load(int3(texelOf(hitUv), 0)).xyz), R) > 0.0) hit = false;
    if (!hit) { reflection[id.xy] = 0.0; return; }

    // Less where the answer is less sure: at the picture's edge, toward the ray's end, and the rougher the surface.
    float2 edge = abs(hitUv * 2.0 - 1.0);
    float confidence = 1.0 - smoothstep(0.85, 1.0, max(edge.x, edge.y));
    confidence *= 1.0 - smoothstep(0.6, 1.0, t / maxDistance);
    confidence *= 1.0 - smoothstep(0.5 * maxRoughness, maxRoughness, roughness);
    float3 straightOn = specular.rgb;
    float3 mirrored = straightOn + (max(float3(1.0 - roughness), straightOn) - straightOn) * pow(1.0 - saturate(dot(N, -V)), 5.0);
    float3 reflected = min(hdr.SampleLevel(hitUv, 0.0).rgb, 64.0);
    reflection[id.xy] = float4(reflected * mirrored * confidence * intensity, confidence);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void compose(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    if (koralDebugShowing()) return;    // a channel of a material shown in the editor: nothing mirrored onto it
    float4 color = hdrImage[id.xy];
    hdrImage[id.xy] = float4(color.rgb + traced.Load(int3(id.xy, 0)).rgb, color.a);
}
