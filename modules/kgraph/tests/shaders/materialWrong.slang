// A shader that asks a kind for what it has not, and for what it has as another type: its pipeline says which.

import entity;
import PbrMaterial;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    [PbrMaterial("shininess")] float shininess;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

struct Misfit {
    [entity("MATRIX")] float4x4 model;
    [PbrMaterial("baseColor")] float2 color;
};
[entity("PER_INSTANCE")] StructuredBuffer<Misfit> misfits;

[shader("vertex")]
float4 vertexMain(float3 position : POSITION, uint index : SV_InstanceID) : SV_Position
{
    return mul(float4(position, instances[index].shininess + misfits[index].color.x), instances[index].model);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return float4(1, 1, 1, 1);
}
