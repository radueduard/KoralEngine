module lit;

import engineCommon;

struct Vertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct RasterInfo {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
};

// One block of push constants both stages read. `model` and `entityId` are the engine's to give: where the entity
// is in the world, and which entity it is. `color` and `smoothness` are the entity's own: the fields of the
// component the pipeline's node makes.
struct DrawData {
    float4x4 model;
    float3 color;
    float smoothness;
    uint entityId;                     // which entity this is: what picking and the outline read
};
[[vk::push_constant]] ConstantBuffer<DrawData> draw;

struct Output {
    float4 color : SV_Target0;
    uint id : SV_Target1;
};

[shader("vertex")]
RasterInfo vertex(Vertex vertex)
{
    float4x4 model = draw.model;
    RasterInfo raster;
    float4 world = mul(float4(vertex.position, 1.0), model);
    raster.worldPosition = world.xyz;
    raster.position = mul(world, frame.viewProjection);
    // The normal through the cofactors of the model's 3x3 — its inverse transpose, but for a scale that normalize
    // takes away — so that it stays square to the surface however unevenly the model is scaled.
    float3 a0 = model[0].xyz, a1 = model[1].xyz, a2 = model[2].xyz;
    float3 normal = vertex.normal.x * cross(a1, a2) + vertex.normal.y * cross(a2, a0) + vertex.normal.z * cross(a0, a1);
    raster.normal = normal * sign(dot(a0, cross(a1, a2)));   // a mirroring scale turns the cofactors inside out
    return raster;
}

/** How much of a light at [distance] is left, out to [range]: falling with the square of the distance, to 0 at the end. */
float falloff(float distance, float range)
{
    float edge = saturate(1.0 - pow(distance / range, 4.0));
    return edge * edge / (distance * distance + 1.0);
}

[shader("fragment")]
Output fragment(RasterInfo raster)
{
    float3 color = toLinear(draw.color);
    float smoothness = draw.smoothness;
    float3 normal = normalize(raster.normal);
    float3 toEye = normalize(frame.cameraPosition.xyz - raster.worldPosition);
    // Blinn-Phong: smoother, a tighter and brighter highlight.
    float shininess = exp2(1.0 + smoothness * 10.0);
    float strength = 0.04 + smoothness * 0.5;

    float3 lit = skyLight(normal) * color;
    for (uint i = 0; i < frame.counts.x; i++) {
        Light light = lights[i];
        uint type = uint(light.direction.w);
        float3 toLight;
        float amount = 1.0;
        if (type == LightDirectional) {
            toLight = -light.direction.xyz;
        } else {
            float3 offset = light.position.xyz - raster.worldPosition;
            float distance = length(offset);
            toLight = offset / max(distance, 1e-4);
            amount = falloff(distance, light.position.w);
            if (type == LightSpot)
                amount *= smoothstep(light.cone.y, light.cone.x, dot(-toLight, light.direction.xyz));
        }
        float facing = dot(normal, toLight);
        if (facing <= 0.0 || amount <= 0.0) continue;
        float3 halfway = normalize(toLight + toEye);
        float highlight = pow(saturate(dot(normal, halfway)), shininess) * strength * (shininess + 8.0) / 8.0;
        lit += light.color.rgb * amount * (color * facing + highlight * facing);
    }

    Output output;
    output.color = float4(present(lit), 1.0);
    output.id = draw.entityId;
    return output;
}
