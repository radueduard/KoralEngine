// What the comparing sorts are tried on: a struct that compares by its key, each with the index it began at, and a float
// for each of them. And a struct that does not say how it compares.

struct Sortable : IComparable {
    uint key; uint index; float2 rest;
    bool equals(Sortable other) { return key == other.key; }
    bool lessThan(Sortable other) { return key < other.key; }
    bool lessThanOrEquals(Sortable other) { return key <= other.key; }
};

struct Unsortable { uint key; };

struct SeedPush { uint count; uint reach; };
[[vk::push_constant]] ConstantBuffer<SeedPush> push;

RWStructuredBuffer<Sortable> structs;
RWStructuredBuffer<float> floats;

uint Hash(uint i) { i *= 2654435761u; i ^= i >> 15; i *= 2246822519u; i ^= i >> 13; return i; }

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    Sortable made;
    made.key = Hash(id.x) % push.reach;
    made.index = id.x;
    made.rest = float2(id.x, 1);
    structs[id.x] = made;
    floats[id.x] = float(Hash(id.x)) * -1e-6;
}
