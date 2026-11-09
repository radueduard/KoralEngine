// What a module's function is tried on: a picture (a white dot in the middle, grey round it), how far each pixel is (a
// slope, near at the top), and which way it faces (towards the eye, tilted along x).

[[vk::image_format("rgba16f")]] RWTexture2D<float4> picture;
[[vk::image_format("r32f")]] RWTexture2D<float> depth;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> normals;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    picture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    picture[id.xy] = all(id.xy == uint2(width, height) / 2) ? float4(8.0, 8.0, 8.0, 1.0) : float4(uv * 0.5, 0.25, 1.0);
    depth[id.xy] = 0.5 + 0.4 * uv.y;
    normals[id.xy] = float4(normalize(float3(uv.x - 0.5, 0.0, 1.0)), 0.0);
}
