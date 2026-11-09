// One white pixel in the middle of a black picture: what a blur spreads, and a mip chain averages.

[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = all(id.xy == uint2(width, height) / 2) ? float4(1.0) : float4(0.0, 0.0, 0.0, 1.0);
}
