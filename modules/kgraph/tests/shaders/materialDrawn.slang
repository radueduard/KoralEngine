// An indirect draw whose instances' colours are their entities' PbrMaterials': its base colour, by roughness (a
// field the member is named for), and whether its picture is one at all.

import entity;
import PbrMaterial;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    [PbrMaterial("baseColor")] float4 color;
    [PbrMaterial] float roughness;                  // named for the field it is
    [PbrMaterial("baseColorMap")] uint map;
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
    // Magenta: no picture, where every material has one.
    output.color = instance.map == ~0u ? float4(1, 0, 1, 1) : float4(instance.color.rgb * instance.roughness * 2.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
