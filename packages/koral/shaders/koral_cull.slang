// What of an indirect draw of entities is in sight, worked out on the GPU: a thread an instance tests its mesh's box,
// placed where its matrix says, against what the picture is seen with, and counts what is in sight into its mesh's
// command — one with no instances each frame (IndirectDraws in eGPU) — writing its number where the draw finds it: the
// command's first instance, plus how many are before it. The engine's CullInSight function runs it.
//
// The instances are a Draw's [entity("PER_INSTANCE")] buffer, whatever else each holds: an element is `stride` bytes,
// and its [entity("MATRIX")] is `matrixOffset` bytes into it. The draw's shader finds its instance through `visible`:
//
//     StructuredBuffer<uint> visible;          // IndirectDraws' `visible`, which this fills
//     const Instance instance = instances[visible[first + index]];
//
// Or, given `drawn` — a buffer as big as the instances — each element in sight is copied there too, where it is drawn:
// a shader that reads its instance at first + index, as any of [entity("PER_INSTANCE")] does, reads `drawn` as it is,
// with no `visible` (what a batched Draw given `cull` does). Without room in it, nothing is copied.

import koral_indirect;
import koral_bounds;

struct CullPush {
    float4x4 viewProjection;    // what the picture is seen with: a lens after a view
    uint drawCount;             // how many commands: IndirectDraws' drawCount
    uint stride;                // the bytes of an element of `instances`
    uint matrixOffset;          // where in one its matrix is
};
[[vk::push_constant]] ConstantBuffer<CullPush> push;

ByteAddressBuffer instances;                // the Draw's buffer of instances
StructuredBuffer<KoralMeshRange> ranges;         // the MeshPack's rangeBuffer: each mesh's box, in its own space
RWStructuredBuffer<KoralDrawCommand> commands;       // IndirectDraws' commands: from no instances each frame
RWStructuredBuffer<uint> visible;           // IndirectDraws' visible: which instance each drawn one is, a uint an instance
RWStructuredBuffer<uint> args;              // how many groups `cull` runs: one thread an instance
RWByteAddressBuffer drawn;                  // the elements in sight, one after another as they are drawn: when it has room

/** How many groups of 64 cover the instances — as many as `visible` has room for — for `cull` to be dispatched by. */
[shader("compute")]
[numthreads(1, 1, 1)]
void size()
{
    uint length, stride;
    visible.GetDimensions(length, stride);
    args[0] = (length + 63) / 64;
    args[1] = 1;
    args[2] = 1;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cull(uint3 id : SV_DispatchThreadID)
{
    const uint instance = id.x;
    uint commandsLength, rangesLength, visibleLength, stride;
    commands.GetDimensions(commandsLength, stride);
    ranges.GetDimensions(rangesLength, stride);
    visible.GetDimensions(visibleLength, stride);
    const uint listed = min(push.drawCount, min(commandsLength, rangesLength));
    if (listed == 0 || instance >= visibleLength) return;
    const uint command = KoralCommandOf(commands, listed, instance);
    const KoralMeshRange range = ranges[command];
    const float4x4 model = KoralLoadMatrix(instances, instance * push.stride + push.matrixOffset);
    // A mesh whose box is not known (all of it nothing) is drawn: it cannot be told out of sight.
    if (KoralHasBounds(range) && KoralBoxOutOfSight(range.boundsMin.xyz, range.boundsMax.xyz, model, push.viewProjection)) return;
    const uint at = KoralDrawInstance(commands, visible, instance, command, listed, visibleLength);
    if (at == 0xFFFFFFFFu) return;
    // Its element, where it is drawn, a word at a time.
    uint room;
    drawn.GetDimensions(room);
    const uint to = at * push.stride;
    if (to + push.stride <= room)
        for (uint word = 0; word < push.stride; word += 4) drawn.Store(to + word, instances.Load(instance * push.stride + word));
}
