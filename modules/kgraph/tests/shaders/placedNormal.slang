// As placed.slang, but its colour is made brighter by how much the surface faces the viewer: it reads NORMAL, so a
// mesh without one cannot be drawn by it.

struct Object {
    float4x4 model;
    float4 color;
};
[[vk::push_constant]] ConstantBuffer<Object> object;

struct VertexOut {
    float4 position : SV_Position;
    float facing;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL)
{
    VertexOut output;
    const float4 world = mul(float4(position, 1.0), object.model);
    output.position = float4(world.xy, 0.5, 1.0);
    output.facing = saturate(normal.z);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return object.color * input.facing;
}
