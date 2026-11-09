// What the meshlet pass draws: for each instance in the camera's sight, a task for each run of up to 32 meshlets of its
// mesh — counted into one indirect DrawMeshTasks, a task shader's group a task. The task shader then culls each
// meshlet by its own sphere: out of the camera's sight, or behind what the depth pyramid (`reduce`, below) holds. The
// meshlet branch's first page.
//
// A module of its own, with its own push constants: a shader has one block of them.

import forwardCommon;
import cullCommon;

struct MeshRange {
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint unused;
    float4 boundsMin;
    float4 boundsMax;
};
StructuredBuffer<Instance> instances;       // the forward Draw's: where each is
StructuredBuffer<MeshRange> ranges;         // the pack's: each mesh's box, in its own space
RWStructuredBuffer<Command> meshCommands;   // a command a mesh, its instances' first and count: which mesh an instance is
StructuredBuffer<uint4> meshRanges;         // the Meshlets': each mesh's first meshlet, and how many
RWStructuredBuffer<uint4> tasks;            // instance, first meshlet, how many (up to 32)
RWStructuredBuffer<uint> taskArgs;          // DrawMeshTasks' x, y, z: x counted into, from none each frame
StructuredBuffer<FrameState> frameState;
[[vk::push_constant]] cbuffer Expand { uint commandCount; };

static const uint MeshletsATask = 32;

[shader("compute")]
[numthreads(64, 1, 1)]
void expand(uint3 id : SV_DispatchThreadID)
{
    if (id.x == 0) { taskArgs[1] = 1; taskArgs[2] = 1; }
    uint instancesLength, commandsLength, rangesLength, tasksLength, stride;
    instances.GetDimensions(instancesLength, stride);
    meshCommands.GetDimensions(commandsLength, stride);
    ranges.GetDimensions(rangesLength, stride);
    tasks.GetDimensions(tasksLength, stride);
    const uint listed = min(commandCount, min(commandsLength, rangesLength));
    const uint instance = id.x;
    if (listed == 0 || instance >= instancesLength) return;
    const uint command = commandOf(meshCommands, listed, instance);
    const Command counted = meshCommands[command];
    if (instance < counted.firstInstance || instance >= counted.firstInstance + counted.instanceCount) return;     // a row past the entities
    const MeshRange range = ranges[command];
    const bool known = any(range.boundsMin.xyz != range.boundsMax.xyz);
    if (known && frameState[0].begun == Begun && KoralBoxOutOfSight(range.boundsMin.xyz, range.boundsMax.xyz, instances[instance].model, frameState[0].viewProjection)) return;
    const uint4 meshlets = meshRanges[command];
    const uint count = (meshlets.y + MeshletsATask - 1) / MeshletsATask;
    uint first;
    InterlockedAdd(taskArgs[0], count, first);
    for (uint task = 0; task < count && first + task < tasksLength; ++task)
        tasks[first + task] = uint4(instance, meshlets.x + task * MeshletsATask, min(MeshletsATask, meshlets.y - task * MeshletsATask), 0);
}

/** What was counted, held to what the list has room for: the draw reads x as how many tasks. Run after expand. */
[shader("compute")]
[numthreads(1, 1, 1)]
void bound(uint3 id : SV_DispatchThreadID)
{
    uint tasksLength, stride;
    tasks.GetDimensions(tasksLength, stride);
    taskArgs[0] = min(taskArgs[0], tasksLength);
}

// ---- the depth pyramid the meshlets are culled by (forward.slang's sphereHidden) ------------------------------------
// Each texel the farthest depth of the texels it covers in the level before: the first level's, of the pixels of the
// picture's depth. A level of any size from one of any other — the first is 1024 by 512 whatever the picture is — so a
// texel takes every texel that any part of it covers, and what it holds is never nearer than what is there.

Texture2D<float> finer;         // the level before; for the first, the picture's depth
RWTexture2D<float> coarser;     // this level

[shader("compute")]
[numthreads(8, 8, 1)]
void reduce(uint3 id : SV_DispatchThreadID)
{
    uint width, height, finerWidth, finerHeight;
    coarser.GetDimensions(width, height);
    finer.GetDimensions(finerWidth, finerHeight);
    if (id.x >= width || id.y >= height) return;
    const uint2 from = uint2(id.x * finerWidth / width, id.y * finerHeight / height);
    const uint2 to = min(max(from + 1, uint2(((id.x + 1) * finerWidth + width - 1) / width, ((id.y + 1) * finerHeight + height - 1) / height)),
                         uint2(finerWidth, finerHeight));
    float farthest = 0.0;
    for (uint y = from.y; y < to.y; ++y)
        for (uint x = from.x; x < to.x; ++x) farthest = max(farthest, finer.Load(int3(x, y, 0)));
    coarser[id.xy] = farthest;
}
