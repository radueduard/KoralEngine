// What the scale test draws a hundred thousand of: forward-plus's instance, near enough — where it is, which way its
// surface faces, its own colour and which it is — and its depth pass's, where it is alone.

import entity;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    [entity("NORMAL_MATRIX")] float4x4 normalMatrix;
    float4 color;
    [entity("ID")] uint id;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

struct Placed {
    [entity("MATRIX")] float4x4 model;
};
[entity("PER_INSTANCE")] StructuredBuffer<Placed> placed;

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    const Instance instance = instances[first + index];
    VertexOut output;
    output.position = mul(float4(position, 1.0), instance.model);
    const float facing = saturate(mul(float4(normal, 0.0), instance.normalMatrix).z);
    output.color = instance.id == 0 ? float4(1, 0, 1, 1) : float4(instance.color.rgb * (0.5 + 0.5 * facing), 1);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}

[shader("vertex")]
float4 depthVertex(float3 position : POSITION, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation) : SV_Position
{
    return mul(float4(position, 1.0), placed[first + index].model);
}

[shader("fragment")]
float4 depthFragment() : SV_Target
{
    return float4(0, 0, 0, 1);
}
