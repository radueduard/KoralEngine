// Fills an image with a colour: each entity's own image and its own colour, over a backdrop every entity shares.

struct Stamp {
    float4 tint;
    float gain;
};
[[vk::push_constant]] ConstantBuffer<Stamp> stamp;

[[vk::image_format("rgba8")]] RWTexture2D<float4> target;
Texture2D<float4> backdrop;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = stamp.tint * stamp.gain + backdrop.Load(int3(0, 0, 0));
}
