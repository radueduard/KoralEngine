// instanced.slang, culled on the GPU: which instance each drawn one is, CullInSight says (visible). Its matrix places it
// in the picture as it is; the cull's viewProjection is then the identity. An element is 80 bytes, its matrix first.

import entity;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    float4 color;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;
StructuredBuffer<uint> visible;

struct VertexOut {
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    const Instance instance = instances[visible[first + index]];
    VertexOut output;
    output.position = mul(float4(position, 1.0), instance.model);
    output.color = instance.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
