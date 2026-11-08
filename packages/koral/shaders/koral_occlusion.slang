// What of an indirect draw of entities is seen, in two draws, on the GPU (Wihlidal's two phases): the first — `early` —
// draws what was seen the frame before and is in sight; a depth pyramid is made of what it drew (koral_hiz); the second
// — `late` — draws what is in sight, was not seen the frame before, and is not behind what the pyramid says is there.
// Whether each was seen is kept for the next frame: in sight, and not behind the pyramid. The engine's InstanceCull
// function runs it, its two culls about the draws the one who wires it gives.
//
// The instances are as koral_cull's: a Draw's [entity("PER_INSTANCE")] buffer, `stride` bytes an element with its
// MATRIX `matrixOffset` bytes in, found by the draw's shader through `visible`: instances[visible[first + index]].

import koral_indirect;
import koral_bounds;

struct OcclusionPush {
    float4x4 viewProjection;    // what the picture is seen with
    uint drawCount;             // how many commands: IndirectDraws' drawCount
    uint stride;                // the bytes of an element of `instances`
    uint matrixOffset;          // where in one its matrix is
    uint reversed;              // 1: the depth is reversed, 1 near and 0 far
    uint levels;                // how many levels the pyramid has
};
[[vk::push_constant]] ConstantBuffer<OcclusionPush> push;

ByteAddressBuffer instances;
StructuredBuffer<KoralMeshRange> ranges;
RWStructuredBuffer<KoralDrawCommand> commands;    // this draw's: from no instances each frame
RWStructuredBuffer<uint> visible;           // this draw's
RWStructuredBuffer<uint> seen;              // 1 for each instance seen the frame before; the late cull writes this frame's
Texture2D<float> pyramid;                   // the depth pyramid of what the early draw drew: every level

float4x4 modelOf(uint instance) { return KoralLoadMatrix(instances, instance * push.stride + push.matrixOffset); }

KoralBoxInPicture place(float3 low, float3 high, float4x4 model) { return KoralPlaceBox(low, high, model, push.viewProjection, push.reversed != 0); }

/** Whether what the pyramid says is drawn where a box is is all nearer than the box: the box is behind it. */
bool behind(KoralBoxInPicture placed)
{
    if (!placed.inFront) return false;     // across the eye: it cannot be told
    uint width, height, levels;
    pyramid.GetDimensions(0, width, height, levels);
    const float2 low = saturate(placed.low), high = saturate(placed.high);
    const float2 size = (high - low) * float2(width, height);
    // The level at which it is no more than two texels each way: four of them cover it.
    const uint level = min(uint(ceil(log2(max(max(size.x, size.y), 1.0)))), min(levels, push.levels) - 1);
    const uint2 extent = max(uint2(width, height) >> level, uint2(1, 1));
    const uint2 a = min(uint2(low * float2(extent)), extent - 1), b = min(uint2(high * float2(extent)), extent - 1);
    float far = push.reversed != 0 ? 1.0 : 0.0;
    for (uint y = a.y; y <= b.y && y <= a.y + 1; ++y)
        for (uint x = a.x; x <= b.x && x <= a.x + 1; ++x) {
            const float d = pyramid.Load(int3(x, y, level));
            far = push.reversed != 0 ? min(far, d) : max(far, d);
        }
    // Wider than two texels at that level (b past a + 1): the two by two do not cover it — it cannot be told.
    if (b.x > a.x + 1 || b.y > a.y + 1) return false;
    return push.reversed != 0 ? placed.nearest < far : placed.nearest > far;
}

/** The first draw's: what was seen the frame before and is in sight. */
[shader("compute")]
[numthreads(64, 1, 1)]
void early(uint3 id : SV_DispatchThreadID)
{
    const uint instance = id.x;
    uint commandsLength, rangesLength, visibleLength, seenLength, stride;
    commands.GetDimensions(commandsLength, stride);
    ranges.GetDimensions(rangesLength, stride);
    visible.GetDimensions(visibleLength, stride);
    seen.GetDimensions(seenLength, stride);
    const uint listed = min(push.drawCount, min(commandsLength, rangesLength));
    if (listed == 0 || instance >= visibleLength) return;
    if (instance >= seenLength || seen[instance] == 0) return;
    const uint command = KoralCommandOf(commands, listed, instance);
    const KoralMeshRange range = ranges[command];
    const bool boxed = KoralHasBounds(range);
    if (boxed && place(range.boundsMin.xyz, range.boundsMax.xyz, modelOf(instance)).outOfSight) return;
    KoralDrawInstance(commands, visible, instance, command, listed, visibleLength);
}

/** The second draw's: what is in sight, was not seen, and is not behind what the first drew. Whether each is seen, kept. */
[shader("compute")]
[numthreads(64, 1, 1)]
void late(uint3 id : SV_DispatchThreadID)
{
    const uint instance = id.x;
    uint commandsLength, rangesLength, visibleLength, seenLength, stride;
    commands.GetDimensions(commandsLength, stride);
    ranges.GetDimensions(rangesLength, stride);
    visible.GetDimensions(visibleLength, stride);
    seen.GetDimensions(seenLength, stride);
    const uint listed = min(push.drawCount, min(commandsLength, rangesLength));
    if (listed == 0 || instance >= visibleLength) return;
    const uint command = KoralCommandOf(commands, listed, instance);
    const KoralMeshRange range = ranges[command];
    const bool boxed = KoralHasBounds(range);
    bool shown = true;
    if (boxed) {
        const KoralBoxInPicture placed = place(range.boundsMin.xyz, range.boundsMax.xyz, modelOf(instance));
        shown = !placed.outOfSight && !behind(placed);
    }
    const bool before = instance < seenLength && seen[instance] != 0;
    if (instance < seenLength) seen[instance] = shown ? 1 : 0;
    if (shown && !before) KoralDrawInstance(commands, visible, instance, command, listed, visibleLength);
}
