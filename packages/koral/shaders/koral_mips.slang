// A picture's mip levels, each the average of what it halves of the level before — and of the row and the column past
// them, of a side that is odd — the first a copy of the picture. The engine's MipChain function runs it, a level a turn.

Texture2D<float4> source;                                       // the level before: the picture itself, for the first
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;     // this level

[shader("compute")]
[numthreads(8, 8, 1)]
void downsample(uint3 id : SV_DispatchThreadID, uniform int level)
{
    uint width, height, sourceWidth, sourceHeight;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    source.GetDimensions(sourceWidth, sourceHeight);
    if (level == 0) {
        target[id.xy] = source.Load(int3(min(id.xy, uint2(sourceWidth, sourceHeight) - 1), 0));
        return;
    }
    const uint2 low = id.xy * 2;
    const uint2 high = min(low + uint2(sourceWidth > width * 2 && id.x + 1 == width ? 3 : 2, sourceHeight > height * 2 && id.y + 1 == height ? 3 : 2),
                           uint2(sourceWidth, sourceHeight));
    float4 sum = 0.0;
    for (uint y = low.y; y < high.y; ++y)
        for (uint x = low.x; x < high.x; ++x) sum += source.Load(int3(x, y, 0));
    target[id.xy] = sum / float(max((high.x - low.x) * (high.y - low.y), 1u));
}
