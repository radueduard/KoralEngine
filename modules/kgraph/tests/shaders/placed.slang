// Draws a mesh where its entity is: `model` is the entity's place in the world, which the engine gives — from its
// transform and those of everything it is under — and `color` is the entity's own.

struct Object {
    float4x4 model;
    float4 color;
};
[[vk::push_constant]] ConstantBuffer<Object> object;

struct VertexOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : UV)
{
    VertexOut output;
    const float4 world = mul(float4(position, 1.0), object.model);
    output.position = float4(world.xy, 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return object.color;
}
