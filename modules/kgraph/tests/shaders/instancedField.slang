// The near instances of an Instancer, each its mesh: which one each drawn one is, CullInstances says (visible); where
// it is, the Instances buffer; and toClip places all of them. Lit by how much each face looks up, in the field's red.

import koral_instances;

ByteAddressBuffer instances;
StructuredBuffer<uint> visible;
struct Placing { float4x4 toClip; };
[[vk::push_constant]] ConstantBuffer<Placing> push;

struct VertexOut {
    float4 position : SV_Position;
    float shade;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL, uint index : SV_InstanceID)
{
    const float4x4 model = instanceMatrix(instances, visible[index]);
    VertexOut output;
    output.position = mul(mul(float4(position, 1.0), model), push.toClip);
    output.shade = 0.6 + 0.4 * saturate(normalize(mul(float4(normal, 0.0), model).xyz).y);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return float4(input.shade, 0.0, 0.0, 1.0);
}
