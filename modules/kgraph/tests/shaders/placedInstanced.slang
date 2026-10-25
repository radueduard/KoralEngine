// placed.slang, batched: what each draw had as push constants — where its entity is, and its colour — is each instance's,
// in a buffer the Draw fills. A Draw `batched` of a ForEach's meshes draws all of them with it in one indirect draw.

import entity;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    float4 color;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

struct VertexOut {
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : UV, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    const Instance instance = instances[first + index];
    VertexOut output;
    const float4 world = mul(float4(position, 1.0), instance.model);
    output.position = float4(world.xy, 0.5, 1.0);
    output.color = instance.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
