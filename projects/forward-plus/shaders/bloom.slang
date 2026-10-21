// Bloom: what is brighter than the threshold bleeds light round it. The picture is filtered down a chain of images
// each half the last (a 13-tap filter, the threshold on the first step), then back up it (a tent filter, each level
// adding its own), and the top of that is added to the picture before it is tone mapped.
//
// `threshold` and `knee` are given on the downsample pipeline's node, and `level` is which turn of the way down it
// is: the threshold is for the first. `strength` is given on the composite's.

Sampler2D<float4> source;
Sampler2D<float4> detail;           // on the way up: this level as it was filtered down
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 sane(float3 v) { return any(isnan(v)) || any(isinf(v)) ? float3(0.0, 0.0, 0.0) : v; }

/** Four samples averaged, each weighed down by how bright it is: one very bright pixel does not flicker through. */
float3 karis(float3 a, float3 b, float3 c, float3 d)
{
    a = sane(a); b = sane(b); c = sane(c); d = sane(d);
    float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b)), wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1e-4);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void downsample(uint3 id : SV_DispatchThreadID, uniform float threshold, uniform float knee, uniform int level)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    uint sourceWidth, sourceHeight;
    source.GetDimensions(sourceWidth, sourceHeight);
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float x = 1.0 / float(sourceWidth), y = 1.0 / float(sourceHeight);

    float3 a = source.SampleLevel(uv + float2(-2.0 * x, 2.0 * y), 0.0).rgb, b = source.SampleLevel(uv + float2(0.0, 2.0 * y), 0.0).rgb;
    float3 c = source.SampleLevel(uv + float2(2.0 * x, 2.0 * y), 0.0).rgb, d = source.SampleLevel(uv + float2(-2.0 * x, 0.0), 0.0).rgb;
    float3 e = source.SampleLevel(uv, 0.0).rgb, f = source.SampleLevel(uv + float2(2.0 * x, 0.0), 0.0).rgb;
    float3 g = source.SampleLevel(uv + float2(-2.0 * x, -2.0 * y), 0.0).rgb, h = source.SampleLevel(uv + float2(0.0, -2.0 * y), 0.0).rgb;
    float3 i = source.SampleLevel(uv + float2(2.0 * x, -2.0 * y), 0.0).rgb, j = source.SampleLevel(uv + float2(-x, y), 0.0).rgb;
    float3 k = source.SampleLevel(uv + float2(x, y), 0.0).rgb, l = source.SampleLevel(uv + float2(-x, -y), 0.0).rgb;
    float3 m = source.SampleLevel(uv + float2(x, -y), 0.0).rgb;
    float3 result = sane(karis(j, k, l, m) * 0.5 + (karis(a, b, j, d) + karis(b, c, k, e) + karis(d, j, g, l) + karis(k, f, m, i)) * 0.125);

    if (level == 0) {
        // Only what is brighter than the threshold goes on, coming in softly over the knee.
        float brightness = max(result.r, max(result.g, result.b));
        float soft = clamp(brightness - threshold + knee, 0.0, knee * 2.0);
        soft = soft * soft / (4.0 * knee + 1e-4);
        result *= max(brightness - threshold, soft) / max(brightness, 1e-4);
    }
    target[id.xy] = float4(result, 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void upsample(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float x = 0.5 / float(width), y = 0.5 / float(height);
    float3 blurred = source.SampleLevel(uv + float2(-x, y), 0.0).rgb + source.SampleLevel(uv + float2(x, y), 0.0).rgb
                   + source.SampleLevel(uv + float2(-x, -y), 0.0).rgb + source.SampleLevel(uv + float2(x, -y), 0.0).rgb
                   + (source.SampleLevel(uv + float2(0.0, y), 0.0).rgb + source.SampleLevel(uv + float2(-x, 0.0), 0.0).rgb
                    + source.SampleLevel(uv + float2(x, 0.0), 0.0).rgb + source.SampleLevel(uv + float2(0.0, -y), 0.0).rgb) * 2.0
                   + source.SampleLevel(uv, 0.0).rgb * 4.0;
    target[id.xy] = float4(detail.SampleLevel(uv, 0.0).rgb + blurred / 16.0, 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void composite(uint3 id : SV_DispatchThreadID, uniform float strength)
{
    uint width, height;
    hdrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    uint sourceWidth, sourceHeight;
    source.GetDimensions(sourceWidth, sourceHeight);
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float x = 1.0 / float(sourceWidth), y = 1.0 / float(sourceHeight);
    float3 light = source.SampleLevel(uv + float2(-x, y), 0.0).rgb + source.SampleLevel(uv + float2(x, y), 0.0).rgb
                 + source.SampleLevel(uv + float2(-x, -y), 0.0).rgb + source.SampleLevel(uv + float2(x, -y), 0.0).rgb
                 + (source.SampleLevel(uv + float2(0.0, y), 0.0).rgb + source.SampleLevel(uv + float2(-x, 0.0), 0.0).rgb
                  + source.SampleLevel(uv + float2(x, 0.0), 0.0).rgb + source.SampleLevel(uv + float2(0.0, -y), 0.0).rgb) * 2.0
                 + source.SampleLevel(uv, 0.0).rgb * 4.0;
    float4 color = hdrImage[id.xy];
    hdrImage[id.xy] = float4(color.rgb + light / 16.0 * strength, color.a);
}
