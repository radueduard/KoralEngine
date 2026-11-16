// What a ScriptPass is tried on: each element doubled, and the round added — run round after round, the order shows.

struct AccumulatePush { uint round; uint count; };
[[vk::push_constant]] ConstantBuffer<AccumulatePush> push;

RWStructuredBuffer<uint> sums;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    sums[id.x] = sums[id.x] * 2 + push.round;
}
