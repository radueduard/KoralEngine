// The ambient occlusion laid over the picture: how much of the sky each pixel's point sees — the screen-space
// module's GroundTruthOcclusion, from the depth — multiplying what was shaded. After the forward pass, before the sky.

import forwardCommon;
import koral_debug;

Sampler2D<float> occlusionIn;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;

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
