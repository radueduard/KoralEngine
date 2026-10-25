// Copies `source` into `target`, texel by texel: a pass that reads one image and writes another.

Texture2D<float4> source;
[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = source.Load(int3(id.xy, 0));
}
