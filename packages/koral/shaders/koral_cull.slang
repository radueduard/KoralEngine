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

struct CullPush {
    float4x4 viewProjection;    // what the picture is seen with: a lens after a view
    uint drawCount;             // how many commands: IndirectDraws' drawCount
    uint stride;                // the bytes of an element of `instances`
    uint matrixOffset;          // where in one its matrix is
};
[[vk::push_constant]] ConstantBuffer<CullPush> push;

struct Command {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct MeshRange {
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint unused;
    float4 boundsMin;
    float4 boundsMax;
};

ByteAddressBuffer instances;                // the Draw's buffer of instances
StructuredBuffer<MeshRange> ranges;         // the MeshPack's rangeBuffer: each mesh's box, in its own space
RWStructuredBuffer<Command> commands;       // IndirectDraws' commands: from no instances each frame
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

/** Which of the commands an instance is drawn by: the last whose instances start at or before it (they are in order). */
uint commandOf(uint count, uint instance)
{
    uint low = 0, high = count;
    while (high - low > 1) {
        const uint middle = (low + high) / 2;
        if (commands[middle].firstInstance <= instance) low = middle;
        else high = middle;
    }
    return low;
}

/** Whether a box, placed by @p model, is wholly outside one of the planes of what the picture is seen with. */
bool outOfSight(float3 low, float3 high, float4x4 model)
{
    uint outside[6] = { 0, 0, 0, 0, 0, 0 };
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        const float3 at = float3(corner & 1 ? high.x : low.x, corner & 2 ? high.y : low.y, corner & 4 ? high.z : low.z);
        const float4 clip = mul(mul(float4(at, 1.0), model), push.viewProjection);
        outside[0] += clip.x < -clip.w ? 1 : 0;
        outside[1] += clip.x > clip.w ? 1 : 0;
        outside[2] += clip.y < -clip.w ? 1 : 0;
        outside[3] += clip.y > clip.w ? 1 : 0;
        outside[4] += clip.z < 0.0 ? 1 : 0;
        outside[5] += clip.z > clip.w ? 1 : 0;
    }
    [unroll] for (uint plane = 0; plane < 6; ++plane) if (outside[plane] == 8) return true;
    return false;
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
    const uint command = commandOf(listed, instance);
    const MeshRange range = ranges[command];
    const uint at = instance * push.stride + push.matrixOffset;
    const float4x4 model = float4x4(asfloat(instances.Load4(at)), asfloat(instances.Load4(at + 16)), asfloat(instances.Load4(at + 32)), asfloat(instances.Load4(at + 48)));
    // A mesh whose box is not known (all of it nothing) is drawn: it cannot be told out of sight.
    if (any(range.boundsMin.xyz != range.boundsMax.xyz) && outOfSight(range.boundsMin.xyz, range.boundsMax.xyz, model)) return;
    const uint first = commands[command].firstInstance;
    const uint end = command + 1 < listed ? commands[command + 1].firstInstance : visibleLength;
    if (instance < first || instance >= end) return;
    uint before;
    InterlockedAdd(commands[command].instanceCount, 1, before);
    if (first + before >= end) return;
    visible[first + before] = instance;
    // Its element, where it is drawn, a word at a time.
    uint room;
    drawn.GetDimensions(room);
    const uint to = (first + before) * push.stride;
    if (to + push.stride <= room)
        for (uint word = 0; word < push.stride; word += 4) drawn.Store(to + word, instances.Load(instance * push.stride + word));
}
