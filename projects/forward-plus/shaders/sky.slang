// The sky behind the scene: every pixel nothing was drawn into — still at the far plane — gets the sky seen along
// its ray. After the forward pass and the ambient occlusion, which have no business with it.

import forwardCommon;

ConstantBuffer<Sun> sun;
Sampler2D<float> depthTex;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    if (depthTex.Load(int3(id.xy, 0)) < 0.999999) return;

    float2 ndc = (float2(id.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
    float4 seen = mul(float4(ndc, 1.0, 1.0), camera.inverseProjection);
    float3 direction = normalize(mul(float4(seen.xyz / seen.w, 0.0), camera.inverseView).xyz);
    hdrImage[id.xy] = float4(skyColor(sun, direction), 1.0);
}

// The same, from a picture of the whole sky: an environment map, longitude across and latitude down, as light.
// `intensity` scales it, and `rotation` turns it about the vertical, in radians: both given on the pipeline's node.

Sampler2D<float4> environment;

[shader("compute")]
[numthreads(16, 16, 1)]
void environmentMain(uint3 id : SV_DispatchThreadID, uniform float intensity, uniform float rotation)
{
    uint width, height;
    hdrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    if (depthTex.Load(int3(id.xy, 0)) < 0.999999) return;

    float2 ndc = (float2(id.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
    float4 seen = mul(float4(ndc, 1.0, 1.0), camera.inverseProjection);
    float3 direction = normalize(mul(float4(seen.xyz / seen.w, 0.0), camera.inverseView).xyz);
    float c = cos(rotation), s = sin(rotation);
    direction = float3(c * direction.x - s * direction.z, direction.y, s * direction.x + c * direction.z);
    // -Z is the picture's middle, and +Y its top row.
    float2 uv = float2(atan2(direction.x, -direction.z) / (2.0 * 3.14159265359) + 0.5, acos(clamp(direction.y, -1.0, 1.0)) / 3.14159265359);
    hdrImage[id.xy] = float4(environment.SampleLevel(uv, 0.0).rgb * intensity, 1.0);
}
