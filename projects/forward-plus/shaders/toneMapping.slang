// The picture as light, made the picture as the display shows it: exposed, tone mapped (Narkowicz's fit of ACES) so
// that brighter than white rolls off rather than clipping, and encoded for the display's curve.

[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;
[[vk::image_format("rgba8")]] RWTexture2D<float4> ldrImage;

// `exposure` is given on the pipeline's node: push.exposure.

float3 aces(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 id : SV_DispatchThreadID, uniform float exposure)
{
    uint width, height;
    ldrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float3 color = aces(hdrImage[id.xy].rgb * exposure);
    ldrImage[id.xy] = float4(pow(color, 1.0 / 2.2), 1.0);
}
