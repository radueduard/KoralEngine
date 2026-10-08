// A mesh shader that makes one triangle, over the half of the picture above the diagonal: no vertex buffer, no mesh.

struct Vertex {
    float4 position : SV_Position;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void meshMain(out indices uint3 triangles[1], out vertices Vertex vertices[3])
{
    SetMeshOutputCounts(3, 1);
    vertices[0].position = float4(-1.0, -1.0, 0.0, 1.0);
    vertices[1].position = float4(1.0, -1.0, 0.0, 1.0);
    vertices[2].position = float4(-1.0, 1.0, 0.0, 1.0);
    triangles[0] = uint3(0, 1, 2);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return float4(0.0, 1.0, 0.0, 1.0);
}
