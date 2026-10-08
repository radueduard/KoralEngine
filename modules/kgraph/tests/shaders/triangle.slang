// Two triangles a vertex shader makes out of nothing but the number of the vertex: the first covers the half of
// the picture above the diagonal, the second the half below. There is no mesh, and no vertex input.

struct VertexOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOut vertexMain(uint id : SV_VulkanVertexID)
{
    static const float2 corners[6] = {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
        float2(1.0, 1.0), float2(-1.0, 1.0), float2(1.0, -1.0),
    };
    VertexOut output;
    output.position = float4(corners[id], 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return float4(0.0, 1.0, 0.0, 1.0);
}
