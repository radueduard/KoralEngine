// Draws every instance of an indirect draw where its element says: each command draws its mesh's instances, from the
// command's first instance on, and an instance is found at SV_StartInstanceLocation + SV_InstanceID.

struct Instance {
    float4 place;   // x, y, and its size in w
    float4 color;
};
StructuredBuffer<Instance> instances;

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : UV, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    const Instance instance = instances[first + index];
    VertexOut output;
    output.position = float4(position.xy * instance.place.w + instance.place.xy, 0.5, 1.0);
    output.color = instance.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
