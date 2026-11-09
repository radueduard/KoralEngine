// A Gaussian blur, a direction at a time: across, then down, each of 2 radius + 1 taps weighed by a Gaussian of spread
// `sigma` and normalised — two passes of that many taps rather than one of its square. The engine's GaussianBlur
// function runs it.
//
// `radius` (taps each side) and `sigma` (pixels) are given on the Dispatch: push.radius, push.sigma.

import koral_math;

Texture2D<float4> source;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;

float4 blurred(int2 at, int2 step, int radius, float sigma)
{
    uint width, height;
    source.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    float4 sum = 0.0;
    float total = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const float w = KoralGaussianWeight(float(i), sigma);
        sum += source.Load(int3(clamp(at + step * i, int2(0, 0), last), 0)) * w;
        total += w;
    }
    return sum / max(total, 1e-8);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void horizontal(uint3 id : SV_DispatchThreadID, uniform int radius, uniform float sigma)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = blurred(int2(id.xy), int2(1, 0), radius, sigma);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void vertical(uint3 id : SV_DispatchThreadID, uniform int radius, uniform float sigma)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = blurred(int2(id.xy), int2(0, 1), radius, sigma);
}
