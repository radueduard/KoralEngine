// Entities drawn by meshlets: a task shader's group for each instance, which sends a mesh shader's group for each
// meshlet of its mesh; that one writes out the meshlet's vertices — where its instance is — and its triangles.

struct Instance {
    float4 place;   // where, and how big
    float4 color;   // and w: which mesh of the pack, as the Filter groups them
};
StructuredBuffer<Instance> instances;

struct Meshlet {
    uint vertexOffset, vertexCount, triangleOffset, triangleCount;
    float4 sphere;
    float4 cone;
};
StructuredBuffer<Meshlet> meshlets;
StructuredBuffer<uint> meshletVertices;
StructuredBuffer<uint> meshletTriangles;
StructuredBuffer<uint4> meshRanges;
ByteAddressBuffer positions;

struct Payload {
    uint instance;
    uint first;
};
groupshared Payload payload;

[shader("amplification")]
[numthreads(1, 1, 1)]
void taskMain(uint3 group : SV_GroupID)
{
    const uint4 range = meshRanges[uint(instances[group.x].color.w)];
    payload.instance = group.x;
    payload.first = range.x;
    DispatchMesh(range.y, 1, 1, payload);
}

struct Vertex {
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("mesh")]
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void meshMain(uint thread : SV_GroupThreadID, uint3 group : SV_GroupID, in payload Payload task,
              out vertices Vertex vertices[64], out indices uint3 triangles[124], uniform uint stride, uniform uint offset)
{
    const Meshlet meshlet = meshlets[task.first + group.x];
    const Instance instance = instances[task.instance];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);
    if (thread < meshlet.vertexCount) {
        const uint v = meshletVertices[meshlet.vertexOffset + thread];
        const float3 position = asfloat(positions.Load3(v * stride + offset));
        vertices[thread].position = float4(instance.place.xyz + position * instance.place.w, 1.0);
        vertices[thread].color = float4(instance.color.rgb, 1.0);
    }
    for (uint t = thread; t < meshlet.triangleCount; t += 64) {
        const uint packed = meshletTriangles[meshlet.triangleOffset + t];
        triangles[t] = uint3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    }
}

[shader("fragment")]
float4 fragmentMain(Vertex input) : SV_Target
{
    return input.color;
}
