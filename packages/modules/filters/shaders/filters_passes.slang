// The filters module's passes: Kawase's blur a turn at a time, the dual-filter blur down and up (Bjørge's), a
// bilateral, an à-trous step kept to edges by depth and normals, and sharpening. Each reads `source` and writes `target`.

import filters;
import koral_math;

Texture2D<float4> source;
SamplerState linearSampler;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;
Texture2D<float> depth;             // what the joint filters keep to: how far each pixel is (linear or not: compared relatively)
Texture2D<float4> normals;          // and which way it faces (xyz, -1 to 1)

bool Size(uint2 id, out uint2 size) { uint w, h; target.GetDimensions(w, h); size = uint2(w, h); return all(id < size); }

/** One turn of Kawase's blur: four bilinear reads @p turn + 0.5 texels off on the diagonals. Turns 0, 1, 2, 2, 3 blur as a wide Gaussian. */
[shader("compute")]
[numthreads(16, 16, 1)]
void kawase(uint3 id : SV_DispatchThreadID, uniform float turn)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float2 uv = KoralPixelUv(id.xy, size), o = (turn + 0.5) / float2(size);
    const float4 sum = source.SampleLevel(linearSampler, uv + float2(-o.x, -o.y), 0.0) + source.SampleLevel(linearSampler, uv + float2(o.x, -o.y), 0.0)
                     + source.SampleLevel(linearSampler, uv + float2(-o.x, o.y), 0.0) + source.SampleLevel(linearSampler, uv + float2(o.x, o.y), 0.0);
    target[id.xy] = sum * 0.25;
}

/** The dual filter's way down: to a target half the size, the centre and four diagonals. */
[shader("compute")]
[numthreads(16, 16, 1)]
void dualDown(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float2 uv = KoralPixelUv(id.xy, size), o = 0.5 / float2(size);
    float4 sum = source.SampleLevel(linearSampler, uv, 0.0) * 4.0;
    sum += source.SampleLevel(linearSampler, uv - o, 0.0) + source.SampleLevel(linearSampler, uv + o, 0.0);
    sum += source.SampleLevel(linearSampler, uv + float2(o.x, -o.y), 0.0) + source.SampleLevel(linearSampler, uv + float2(-o.x, o.y), 0.0);
    target[id.xy] = sum / 8.0;
}

/** The dual filter's way up: to a target twice the size, eight reads round it. */
[shader("compute")]
[numthreads(16, 16, 1)]
void dualUp(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float2 uv = KoralPixelUv(id.xy, size), o = 1.0 / float2(size);
    float4 sum = source.SampleLevel(linearSampler, uv + float2(-o.x * 2.0, 0.0), 0.0) + source.SampleLevel(linearSampler, uv + float2(o.x * 2.0, 0.0), 0.0)
               + source.SampleLevel(linearSampler, uv + float2(0.0, -o.y * 2.0), 0.0) + source.SampleLevel(linearSampler, uv + float2(0.0, o.y * 2.0), 0.0);
    sum += (source.SampleLevel(linearSampler, uv + float2(-o.x, o.y), 0.0) + source.SampleLevel(linearSampler, uv + float2(o.x, o.y), 0.0)
          + source.SampleLevel(linearSampler, uv + float2(-o.x, -o.y), 0.0) + source.SampleLevel(linearSampler, uv + float2(o.x, -o.y), 0.0)) * 2.0;
    target[id.xy] = sum / 12.0;
}

/** A bilateral: a Gaussian of @p sigma pixels over @p radius each way, each neighbour counting less the more its colour differs (@p range). */
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral(uint3 id : SV_DispatchThreadID, uniform int radius, uniform float sigma, uniform float range)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float4 centre = source.Load(int3(id.xy, 0));
    float4 sum = 0.0;
    float total = 0.0;
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            const float4 other = source.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(size) - 1), 0));
            const float w = KoralGaussianWeight(length(float2(x, y)), sigma) * KoralRangeWeight(centre.rgb, other.rgb, range);
            sum += other * w;
            total += w;
        }
    target[id.xy] = sum / max(total, 1e-8);
}

/**
 * One step of an à-trous filter, its taps @p step pixels apart (1, 2, 4...), kept to edges by depth and normals and by
 * how its colours differ (@p range; 0 for none): a few steps are a wide, edge-stopping blur — a denoiser's.
 */
[shader("compute")]
[numthreads(16, 16, 1)]
void atrous(uint3 id : SV_DispatchThreadID, uniform int step, uniform float depthSigma, uniform float normalPower, uniform float range)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const float4 centre = source.Load(int3(id.xy, 0));
    const float centreDepth = depth.Load(int3(id.xy, 0));
    const float3 centreNormal = normals.Load(int3(id.xy, 0)).xyz;
    float4 sum = 0.0;
    float total = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            const int2 at = clamp(int2(id.xy) + int2(x, y) * step, int2(0, 0), int2(size) - 1);
            const float4 other = source.Load(int3(at, 0));
            float w = KoralAtrousWeight(x) * KoralAtrousWeight(y)
                    * KoralGeometryWeight(centreDepth, depth.Load(int3(at, 0)), centreNormal, normals.Load(int3(at, 0)).xyz, depthSigma, normalPower);
            if (range > 0.0) w *= KoralRangeWeight(centre.rgb, other.rgb, range);
            sum += other * w;
            total += w;
        }
    target[id.xy] = sum / max(total, 1e-8);
}

/** Contrast-adaptive sharpening of each pixel by its four neighbours, @p sharpness 0 to 1. */
[shader("compute")]
[numthreads(16, 16, 1)]
void sharpen(uint3 id : SV_DispatchThreadID, uniform float sharpness)
{
    uint2 size;
    if (!Size(id.xy, size)) return;
    const int2 at = int2(id.xy), last = int2(size) - 1;
    const float4 centre = source.Load(int3(at, 0));
    const float3 up = source.Load(int3(clamp(at + int2(0, -1), 0, last), 0)).rgb, down = source.Load(int3(clamp(at + int2(0, 1), 0, last), 0)).rgb;
    const float3 left = source.Load(int3(clamp(at + int2(-1, 0), 0, last), 0)).rgb, right = source.Load(int3(clamp(at + int2(1, 0), 0, last), 0)).rgb;
    target[id.xy] = float4(KoralSharpen(centre.rgb, up, left, right, down, sharpness), centre.a);
}
