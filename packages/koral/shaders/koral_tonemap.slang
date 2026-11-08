// The picture as light, made the picture as the display shows it: exposed, tone mapped (Narkowicz's fit of ACES) so
// that brighter than white rolls off rather than clipping, and encoded for the display's curve. The engine's Tonemap
// function runs it.

import koral_debug;
import koral_display;

[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;
[[vk::image_format("rgba8")]] RWTexture2D<float4> ldrImage;

// `exposure` is given on the Dispatch: push.exposure.

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 id : SV_DispatchThreadID, uniform float exposure)
{
    uint width, height;
    ldrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    // A channel of a material shown is already as the display shows it.
    if (koralDebugShowing()) { ldrImage[id.xy] = float4(hdrImage[id.xy].rgb, 1.0); return; }
    ldrImage[id.xy] = float4(KoralDisplay(hdrImage[id.xy].rgb * exposure), 1.0);
}
