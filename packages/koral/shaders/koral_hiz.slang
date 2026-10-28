// A depth pyramid: each level a texel of which is the farthest depth of the four of the level before it — the first
// level the farthest of what each texel covers of the depth image — so that a box whose nearest point is farther than
// what a level says of where it is, is behind everything drawn there. The engine's HiZ and InstanceCull functions run it.
//
// `reversed` says the depth is reversed (1 near, 0 far): the farthest is then the least.

Texture2D<float> depth;                                     // what was drawn: any size
Texture2D<float> source;                                    // the level before
[[vk::image_format("r32f")]] RWTexture2D<float> target;     // this level

float farther(float a, float b, uint reversed) { return reversed != 0 ? min(a, b) : max(a, b); }

/** The first level: the farthest of every texel of the depth image a texel of it covers. */
[shader("compute")]
[numthreads(8, 8, 1)]
void first(uint3 id : SV_DispatchThreadID, uniform uint reversed)
{
    uint width, height, depthWidth, depthHeight;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    depth.GetDimensions(depthWidth, depthHeight);
    const uint2 size = uint2(width, height), seen = uint2(depthWidth, depthHeight);
    const uint2 low = id.xy * seen / size;
    const uint2 high = min(max(((id.xy + 1) * seen + size - 1) / size, low + 1), seen);
    float far = reversed != 0 ? 1.0 : 0.0;
    for (uint y = low.y; y < high.y; ++y)
        for (uint x = low.x; x < high.x; ++x) far = farther(far, depth.Load(int3(x, y, 0)), reversed);
    target[id.xy] = far;
}

/** A level of the one before: the farthest of the two by two it halves — and of the row and the column past them, of a side that is odd. */
[shader("compute")]
[numthreads(8, 8, 1)]
void reduce(uint3 id : SV_DispatchThreadID, uniform uint reversed)
{
    uint width, height, sourceWidth, sourceHeight;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    source.GetDimensions(sourceWidth, sourceHeight);
    const uint2 low = id.xy * 2;
    const uint2 high = min(low + uint2(sourceWidth > width * 2 && id.x + 1 == width ? 3 : 2, sourceHeight > height * 2 && id.y + 1 == height ? 3 : 2),
                           uint2(sourceWidth, sourceHeight));
    float far = reversed != 0 ? 1.0 : 0.0;
    for (uint y = low.y; y < high.y; ++y)
        for (uint x = low.x; x < high.x; ++x) far = farther(far, source.Load(int3(x, y, 0)), reversed);
    target[id.xy] = far;
}
