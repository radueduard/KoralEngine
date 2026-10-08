// Writes what an indirect draw reads: three vertices, one instance, from the fourth vertex on. And beside it how
// many groups an indirect dispatch runs.

RWStructuredBuffer<uint> draws;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    draws[0] = 3;   // vertexCount
    draws[1] = 1;   // instanceCount
    draws[2] = 3;   // firstVertex
    draws[3] = 0;   // firstInstance
    draws[4] = 4;   // groups, x
    draws[5] = 4;   // y
    draws[6] = 1;   // z
    draws[7] = 1;   // how many draws
}
