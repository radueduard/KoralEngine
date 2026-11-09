// The antialiasing module's passes: FXAA of `source` into `target`, and `source` made `target`'s size by Lanczos.

import antialiasing;
import koral_math;

Texture2D<float4> source;
SamplerState linearSampler;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(16, 16, 1)]
void fxaa(uint3 id : SV_DispatchThreadID, uniform float threshold, uniform float subpixel)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    KoralFxaaSettings settings = KoralFxaaSettings();
    settings.threshold = threshold;
    settings.subpixel = subpixel;
    const float2 size = float2(width, height);
    target[id.xy] = float4(KoralFxaa(source, linearSampler, KoralPixelUv(id.xy, uint2(width, height)), 1.0 / size, settings), 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void upscale(uint3 id : SV_DispatchThreadID)
{
    uint width, height, sourceWidth, sourceHeight;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    source.GetDimensions(sourceWidth, sourceHeight);
    target[id.xy] = float4(KoralLanczosUpscale(source, KoralPixelUv(id.xy, uint2(width, height)), uint2(sourceWidth, sourceHeight)), 1.0);
}
