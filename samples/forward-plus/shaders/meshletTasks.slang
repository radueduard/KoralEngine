// What the meshlet pass draws: for each instance in the camera's sight, a task for each run of up to 32 meshlets of its
// mesh — counted into one indirect DrawMeshTasks, a task shader's group a task. The task shader then culls each
// meshlet by its own sphere. The meshlet branch's first page.
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

/** Whether a box, moved by @p model, is wholly outside one of the camera's planes. */
bool outOfSight(float3 low, float3 high, float4x4 model, float4x4 viewProjection)
{
    uint outside[6] = { 0, 0, 0, 0, 0, 0 };
    for (uint corner = 0; corner < 8; ++corner) {
        const float3 at = float3(corner & 1 ? high.x : low.x, corner & 2 ? high.y : low.y, corner & 4 ? high.z : low.z);
        const float4 clip = mul(float4(mul(float4(at, 1.0), model).xyz, 1.0), viewProjection);
        outside[0] += clip.x < -clip.w ? 1 : 0;
        outside[1] += clip.x > clip.w ? 1 : 0;
        outside[2] += clip.y < -clip.w ? 1 : 0;
        outside[3] += clip.y > clip.w ? 1 : 0;
        outside[4] += clip.z < 0.0 ? 1 : 0;
        outside[5] += clip.z > clip.w ? 1 : 0;
    }
    for (uint plane = 0; plane < 6; ++plane) if (outside[plane] == 8) return true;
    return false;
}

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
    if (known && frameState[0].begun == Begun && outOfSight(range.boundsMin.xyz, range.boundsMax.xyz, instances[instance].model, frameState[0].viewProjection)) return;
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
