// Entities seen by rays, each in its own colour: a field of the ray tracing pipeline's component — its material — which
// the TraceRays fills a buffer of, an element an entity of its `entities`, in the order a RayScene of them numbers them.

import koral_bindless;
import entity;

struct Material {
    float4 tint;
    [entity("ID")] uint id;
};
[entity("PER_INSTANCE")] StructuredBuffer<Material> materials;
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
    const Material material = materials[InstanceID()];
    payload.color = material.id == 0 ? float4(1.0, 0.0, 1.0, 1.0) : float4(material.tint.rgb, 1.0);    // magenta: no entity said
}
