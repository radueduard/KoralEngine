// An image of floats, its left half 0.25 and its right half 0.75: a depth whose pyramid a test reads back.

[[vk::image_format("r32f")]] RWTexture2D<float> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = id.x < width / 2 ? 0.25 : 0.75;
}
