// Every entity in one dispatch, PerElement groups along z each: a group counts one for its element, element
// SV_GroupID.z / PerElement.

import entity;

static const uint PerElement = 20000;

struct Instance {
    float which;        // where in `counts` it counts
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

RWStructuredBuffer<uint> counts;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 group : SV_GroupID)
{
    const Instance instance = instances[group.z / PerElement];
    InterlockedAdd(counts[uint(instance.which)], 1u);
}
