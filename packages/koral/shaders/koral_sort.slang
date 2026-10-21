// A radix sort of 32-bit uint keys, each with a uint value carried along: four bits a turn, eight turns, and each turn
// stable — what had the same digit stays in the order it was in — so that the turns, lowest digit first, sort the whole.
// A turn counts each block's digits (count), sums the counts into where each block's of each digit go (the Scan
// function), and puts each key there, after the ones before it of its digit in its block (scatter). The engine's
// RadixSort function runs the turns, from one pair of buffers into the other and back.

struct SortPush { uint count; uint shift; uint blocks; };
[[vk::push_constant]] ConstantBuffer<SortPush> push;

RWStructuredBuffer<uint> keysIn;
RWStructuredBuffer<uint> valuesIn;
RWStructuredBuffer<uint> keysOut;
RWStructuredBuffer<uint> valuesOut;
RWStructuredBuffer<uint> counts;    // how many of each digit each block has, digit after digit: [digit * blocks + block]

groupshared uint digits[256];

uint DigitOf(const uint i) { return i < push.count ? (keysIn[i] >> push.shift) & 15 : 16; }

[shader("compute")]
[numthreads(256, 1, 1)]
void count(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    digits[thread.x] = DigitOf(group.x * 256 + thread.x);
    GroupMemoryBarrierWithGroupSync();
    if (thread.x >= 16) return;
    uint n = 0;
    for (uint k = 0; k < 256; ++k) n += digits[k] == thread.x ? 1 : 0;
    counts[thread.x * push.blocks + group.x] = n;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void scatter(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint i = group.x * 256 + thread.x;
    const uint digit = DigitOf(i);
    digits[thread.x] = digit;
    GroupMemoryBarrierWithGroupSync();
    if (i >= push.count) return;
    uint before = 0;
    for (uint k = 0; k < thread.x; ++k) before += digits[k] == digit ? 1 : 0;
    const uint at = counts[digit * push.blocks + group.x] + before;
    keysOut[at] = keysIn[i];
    valuesOut[at] = valuesIn[i];
}
