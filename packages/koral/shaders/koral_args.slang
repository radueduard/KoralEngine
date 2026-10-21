// How many groups cover a count a pass worked out on the GPU: three uints an indirect Dispatch reads — the count's
// first uint divided by the group's size, rounded up, by 1 by 1. The engine's IndirectArgs function runs it.

struct ArgsPush { uint groupSize; uint offset; };
[[vk::push_constant]] ConstantBuffer<ArgsPush> push;

RWStructuredBuffer<uint> counter;
RWStructuredBuffer<uint> args;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    const uint size = max(push.groupSize, 1);
    args[0] = (counter[push.offset] + size - 1) / size;
    args[1] = 1;
    args[2] = 1;
}
