// The frustum cull: the pre-pass draws only what is in the camera's sight. Each instance's box, where it is, is tested
// against the camera, and what is in sight counted into its mesh's indirect command — whose instances start at none
// each frame (IndirectDraws in eGPU).
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
RWStructuredBuffer<Command> sightCommands;  // the pre-pass's: from none each frame
RWStructuredBuffer<uint> inSight;           // which instance each drawn one is
// The camera as the frame sees it, without the jitter: what the frame's advance wrote, before anything was drawn.
StructuredBuffer<FrameState> frameState;
[[vk::push_constant]] cbuffer Sight { uint sightCommandCount; };

/** Whether a box, moved by @p model, is wholly outside one of the camera's planes: then nothing of it can be seen. */
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

[numthreads(64, 1, 1)]
void frustum(uint3 id : SV_DispatchThreadID)
{
    const uint instance = id.x;
    // How many instances there are: `inSight` has room for each, and no more.
    uint sightLength, rangesLength, instancesLength, inSightLength, stride;
    sightCommands.GetDimensions(sightLength, stride);
    ranges.GetDimensions(rangesLength, stride);
    instances.GetDimensions(instancesLength, stride);
    inSight.GetDimensions(inSightLength, stride);
    const uint listed = min(sightCommandCount, min(sightLength, rangesLength));
    if (listed == 0 || instance >= min(inSightLength, instancesLength)) return;
    const uint command = commandOf(sightCommands, listed, instance);
    const MeshRange range = ranges[command];
    // A mesh whose box is not known (all of it nothing) is drawn: it cannot be told out of sight.
    const bool known = any(range.boundsMin.xyz != range.boundsMax.xyz);
    if (known && frameState[0].begun == Begun && outOfSight(range.boundsMin.xyz, range.boundsMax.xyz, instances[instance].model, frameState[0].viewProjection)) return;
    counted(sightCommands, listed, inSight, inSightLength, instance);
}
