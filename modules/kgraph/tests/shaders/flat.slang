// Draws a mesh where its vertices are, in one colour: nothing for an entity to supply.

struct VertexOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION)
{
    VertexOut output;
    output.position = float4(position.xy, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return float4(1.0, 1.0, 0.0, 1.0);
}
