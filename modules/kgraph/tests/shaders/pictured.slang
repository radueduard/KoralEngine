// An indirect draw whose instances each read a picture of their own: the entity holds it, and the engine writes its
// bindless index into the instance.

import entity;
import koral_bindless;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    [entity("TEXTURE")] uint picture;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

struct VertexOut {
    float4 position : SV_Position;
    nointerpolation uint instance : INSTANCE;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : UV, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    VertexOut output;
    output.position = mul(float4(position, 1.0), instances[first + index].model);
    output.instance = first + index;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    const uint picture = instances[input.instance].picture;
    if (picture == ~0u) return float4(1, 0, 1, 1);      // magenta: it holds none
    return kor::texture2D(picture).Load(int3(0, 0, 0));
}
