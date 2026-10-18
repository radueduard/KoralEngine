// An indirect draw whose instances the engine fills: where each entity is, and its own colour.

import entity;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    float4 color;
    [entity("ID")] uint id;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : UV, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    const Instance instance = instances[first + index];
    VertexOut output;
    output.position = mul(float4(position, 1.0), instance.model);
    output.color = instance.id == 0 ? float4(1, 0, 1, 1) : instance.color;    // magenta: no entity said
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
