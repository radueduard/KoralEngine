// The shadows-soft module's passes: a map of depth made EVSM's four moments, and blurred a direction at a time, for
// reading with one tap. The ShadowMoments function runs them.

import shadows_soft;
import koral_math;

Texture2D<float> depth;
[[vk::image_format("rgba32f")]] RWTexture2D<float4> moments;

[shader("compute")]
[numthreads(16, 16, 1)]
void evsm(uint3 id : SV_DispatchThreadID, uniform float positive, uniform float negative)
{
    uint width, height;
    moments.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    moments[id.xy] = KoralEvsmMoments(depth.Load(int3(id.xy, 0)), float2(positive, negative));
}

// The moments blurred, a direction at a time, at 32 bits a channel: an EVSM's exponentials are too large for half floats.
Texture2D<float4> source;
[[vk::image_format("rgba32f")]] RWTexture2D<float4> target;

float4 Blurred(int2 at, int2 step, int radius)
{
    uint width, height;
    source.GetDimensions(width, height);
    float4 sum = 0.0;
    float total = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const float w = KoralGaussianWeight(float(i), max(float(radius) * 0.5, 0.5));
        sum += source.Load(int3(clamp(at + step * i, int2(0, 0), int2(width, height) - 1), 0)) * w;
        total += w;
    }
    return sum / total;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void blurAcross(uint3 id : SV_DispatchThreadID, uniform int radius)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x < width && id.y < height) target[id.xy] = Blurred(int2(id.xy), int2(1, 0), radius);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void blurDown(uint3 id : SV_DispatchThreadID, uniform int radius)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x < width && id.y < height) target[id.xy] = Blurred(int2(id.xy), int2(0, 1), radius);
}
