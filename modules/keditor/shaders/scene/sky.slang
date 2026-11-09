module sky;

import engineCommon;
import koral_depth;

struct RasterInfo {
    float4 position : SV_Position;
    float2 ndc;
};

// One triangle over the whole image, at the far plane: drawn after the scene with a depth test of less-or-equal,
// it covers only what nothing else did.
[shader("vertex")]
RasterInfo vertex(uint index : SV_VertexID)
{
    RasterInfo raster;
    raster.ndc = KoralFullscreenCorner(index);
    raster.position = float4(raster.ndc, 1.0, 1.0);
    return raster;
}

[shader("fragment")]
float4 fragment(RasterInfo raster) : SV_Target
{
    // The way the eye looks through the pixel.
    const float3 direction = frame.camera.RayThrough(raster.ndc).direction;
    return float4(KoralDisplay(SkyColor(direction)), 1.0);
}
