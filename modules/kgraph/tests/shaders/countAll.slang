// Counts every instance into an IndirectDraws' commands, as a cull that keeps all of them would: one more of its mesh's,
// and its number where the draw finds it.

struct Command {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
RWStructuredBuffer<Command> commands;
RWStructuredBuffer<uint> visible;
[[vk::push_constant]] cbuffer Counts { uint instances; uint commandCount; };

[numthreads(64, 1, 1)]
void countAll(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= instances) return;
    uint command = 0;
    for (uint k = 1; k < commandCount; ++k)
        if (commands[k].firstInstance <= id.x) command = k;
    uint before;
    InterlockedAdd(commands[command].instanceCount, 1, before);
    visible[commands[command].firstInstance + before] = id.x;
}
