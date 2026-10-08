// Halves the red of what is in the image and sets its green to `level`: so a test can tell that it ran after
// whatever cleared the image, and with which constant.

[vk::constant_id(0)] const float level = 1.0;

[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float4 was = target[id.xy];
    target[id.xy] = float4(was.r * 0.5, level, 0.0, 1.0);
}
