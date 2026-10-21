// Prefix sums of 32-bit uints, exclusive — each element the sum of those before it: what counts become where each
// starts. A block of 1024 at a time (blocks), then the blocks' totals (sums), then each block's start added to it (add):
// up to 1024 blocks, 1,048,576 elements. The engine's Scan function runs the three.

struct ScanPush { uint count; };
[[vk::push_constant]] ConstantBuffer<ScanPush> push;

RWStructuredBuffer<uint> values;    // scanned where they are
RWStructuredBuffer<uint> sums;      // each block's total; then where each block starts
RWStructuredBuffer<uint> total;     // [0]: the sum of them all

groupshared uint ping[1024];
groupshared uint pong[1024];

/** The sum of a block's values up to and with this thread's: Hillis and Steele's, in turns that double how far back. */
uint Inclusive(const uint local, const uint value)
{
    ping[local] = value;
    GroupMemoryBarrierWithGroupSync();
    bool fromPing = true;
    for (uint offset = 1; offset < 1024; offset <<= 1) {
        if (fromPing) pong[local] = ping[local] + (local >= offset ? ping[local - offset] : 0);
        else ping[local] = pong[local] + (local >= offset ? pong[local - offset] : 0);
        GroupMemoryBarrierWithGroupSync();
        fromPing = !fromPing;
    }
    return fromPing ? ping[local] : pong[local];
}

[shader("compute")]
[numthreads(1024, 1, 1)]
void blocks(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint i = group.x * 1024 + thread.x;
    const uint value = i < push.count ? values[i] : 0;
    const uint sum = Inclusive(thread.x, value);
    if (i < push.count) values[i] = sum - value;
    if (thread.x == 1023) sums[group.x] = sum;
}

[shader("compute")]
[numthreads(1024, 1, 1)]
void totals(uint3 thread : SV_GroupThreadID)
{
    const uint blockCount = (push.count + 1023) / 1024;
    const uint value = thread.x < blockCount ? sums[thread.x] : 0;
    const uint sum = Inclusive(thread.x, value);
    if (thread.x < blockCount) sums[thread.x] = sum - value;
    if (thread.x == 1023) total[0] = sum;
}

[shader("compute")]
[numthreads(1024, 1, 1)]
void add(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint i = group.x * 1024 + thread.x;
    if (i < push.count) values[i] += sums[group.x];
}
