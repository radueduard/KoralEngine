// What the engine's functions are tried on: a key for each element, from a hash of its index; its index as its value;
// a flag, the key's lowest bit; and how many there are, in a counter.

struct SeedPush { uint count; };
[[vk::push_constant]] ConstantBuffer<SeedPush> push;

RWStructuredBuffer<uint> keys;
RWStructuredBuffer<uint> values;
RWStructuredBuffer<uint> flags;
RWStructuredBuffer<uint> counter;

uint Hash(uint i) { i = i * 2654435761u; i ^= i >> 15; i *= 2246822519u; i ^= i >> 13; return i; }

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i == 0) counter[0] = push.count;
    if (i >= push.count) return;
    keys[i] = Hash(i) & 0xFFFFF;     // twenty bits: many the same, so that a sort that is not stable shows
    values[i] = i;
    flags[i] = keys[i] & 1;
}
