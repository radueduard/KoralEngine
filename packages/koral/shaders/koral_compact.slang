// What is kept of a buffer, one after another: each element whose flag is not 0 put where the prefix sum of the flags
// says (offsets: the Scan function's, of a copy of the flags). The engine's Compact function runs it.

struct CompactPush { uint count; };
[[vk::push_constant]] ConstantBuffer<CompactPush> push;

RWStructuredBuffer<uint> flags;
RWStructuredBuffer<uint> offsets;
RWStructuredBuffer<uint> values;
RWStructuredBuffer<uint> kept;

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i < push.count && flags[i] != 0) kept[offsets[i]] = values[i];
}
