module sky;

import engineCommon;

struct RasterInfo {
    float4 position : SV_Position;
    float2 ndc;
};

// One triangle over the whole image, at the far plane: drawn after the scene with a depth test of less-or-equal,
// it covers only what nothing else did.
[shader("vertex")]
RasterInfo vertex(uint index : SV_VertexID)
{
    float2 corner = float2((index << 1) & 2, index & 2);
    RasterInfo raster;
    raster.ndc = corner * 2.0 - 1.0;
    raster.position = float4(raster.ndc, 1.0, 1.0);
    return raster;
}

[shader("fragment")]
float4 fragment(RasterInfo raster) : SV_Target
{
    // The point under the pixel on the near plane and on the far one: the way the eye looks through it.
    float4 near = mul(float4(raster.ndc, 0.0, 1.0), frame.inverseViewProjection);
    float4 far = mul(float4(raster.ndc, 1.0, 1.0), frame.inverseViewProjection);
    float3 direction = normalize(far.xyz / far.w - near.xyz / near.w);
    return float4(present(skyColor(direction)), 1.0);
}
