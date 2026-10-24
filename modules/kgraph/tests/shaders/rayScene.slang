// Entities seen by rays: a ray a pixel, straight along +Z through a RayScene, coloured by the instance it hits — its
// row of the instances, by InstanceID() — and black where it hits none.

import koral_bindless;

struct Instance {
    float4 place;
    float4 color;
};
StructuredBuffer<Instance> instances;
StructuredBuffer<uint4> scene;      // x: the structure's bindless index
[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

struct Payload {
    float4 color;
};

[shader("raygeneration")]
void raygenMain()
{
    const uint2 at = DispatchRaysIndex().xy;
    const float2 size = float2(DispatchRaysDimensions().xy);
    const float2 across = (float2(at) + 0.5) / size * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin = float3(across, -5.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.0;
    ray.TMax = 100.0;
    Payload payload;
    payload.color = float4(1.0, 0.0, 1.0, 1.0);
    TraceRay(kor::accelerationStructure(scene[0].x), RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    target[at] = payload.color;
}

[shader("miss")]
void missMain(inout Payload payload)
{
    payload.color = float4(0.0, 0.0, 0.0, 1.0);
}

[shader("closesthit")]
void hitMain(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.color = float4(instances[InstanceID()].color.rgb, 1.0);
}
