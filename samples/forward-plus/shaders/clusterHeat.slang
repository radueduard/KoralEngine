// How many lights each pixel's cluster holds, as a colour over the picture: none grey, a few blue, through green and
// yellow to red at `most` (given on the pipeline's node) or more. A line where one cluster ends and the next begins
// across the picture; where the colour steps with distance is where one ends into it.
//
// The cluster is found as the forward pass finds it, but from the depth image: so this shows what that pass looked
// up, and whether the lights were sorted where it looks for them.

import forwardCommon;

Sampler2D<float> depthTex;
Sampler2D<float4> picture;
StructuredBuffer<uint2> lightGrid;
[[vk::image_format("rgba8")]] RWTexture2D<float4> heat;

float3 ramp(float t)
{
    const float3 stops[5] = { float3(0.05, 0.15, 0.9), float3(0.0, 0.75, 0.85), float3(0.1, 0.8, 0.15), float3(0.95, 0.85, 0.1), float3(0.9, 0.1, 0.05) };
    float at = saturate(t) * 4.0;
    int lower = min(int(at), 3);
    return lerp(stops[lower], stops[lower + 1], at - float(lower));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 id : SV_DispatchThreadID, uniform float most)
{
    uint width, height;
    heat.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    int3 pixel = int3(id.xy, 0);
    float3 under = dot(picture.Load(pixel).rgb, float3(0.2126, 0.7152, 0.0722));
    float depth = depthTex.Load(pixel);
    if (depth >= 0.999999) { heat[id.xy] = float4(under * 0.35, 1.0); return; }

    // How far in front of the camera the pixel's surface is, back through the projection.
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float4 seen = mul(float4(uv * 2.0 - 1.0, depth, 1.0), camera.inverseProjection);
    float distance = -seen.z / seen.w;
    float slice = log(max(distance, camera.zNear) / camera.zNear) / log(camera.zFar / camera.zNear) * float(camera.gridSize.z);
    float2 across = uv * float2(camera.gridSize.xy);
    uint3 cluster = min(uint3(uint2(across), uint(max(slice, 0.0))), camera.gridSize - 1);
    uint count = lightGrid[cluster.x + cluster.y * camera.gridSize.x + cluster.z * camera.gridSize.x * camera.gridSize.y].y;

    float3 color = count == 0 ? float3(0.22, 0.22, 0.22) : ramp(float(count) / max(most, 1.0));
    float2 edge = frac(across) * float2(width, height) / float2(camera.gridSize.xy);
    if (edge.x < 1.0 || edge.y < 1.0) color *= 0.55;
    heat[id.xy] = float4(lerp(under * 0.6, color, 0.72), 1.0);
}
