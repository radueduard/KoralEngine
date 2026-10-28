// What of an indirect draw of entities is seen, in two draws, on the GPU (Wihlidal's two phases): the first — `early` —
// draws what was seen the frame before and is in sight; a depth pyramid is made of what it drew (koral_hiz); the second
// — `late` — draws what is in sight, was not seen the frame before, and is not behind what the pyramid says is there.
// Whether each was seen is kept for the next frame: in sight, and not behind the pyramid. The engine's InstanceCull
// function runs it, its two culls about the draws the one who wires it gives.
//
// The instances are as koral_cull's: a Draw's [entity("PER_INSTANCE")] buffer, `stride` bytes an element with its
// MATRIX `matrixOffset` bytes in, found by the draw's shader through `visible`: instances[visible[first + index]].

struct OcclusionPush {
    float4x4 viewProjection;    // what the picture is seen with
    uint drawCount;             // how many commands: IndirectDraws' drawCount
    uint stride;                // the bytes of an element of `instances`
    uint matrixOffset;          // where in one its matrix is
    uint reversed;              // 1: the depth is reversed, 1 near and 0 far
    uint levels;                // how many levels the pyramid has
};
[[vk::push_constant]] ConstantBuffer<OcclusionPush> push;

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

ByteAddressBuffer instances;
StructuredBuffer<MeshRange> ranges;
RWStructuredBuffer<Command> commands;       // this draw's: from no instances each frame
RWStructuredBuffer<uint> visible;           // this draw's
RWStructuredBuffer<uint> seen;              // 1 for each instance seen the frame before; the late cull writes this frame's
Texture2D<float> pyramid;                   // the depth pyramid of what the early draw drew: every level

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

float4x4 modelOf(uint instance)
{
    const uint at = instance * push.stride + push.matrixOffset;
    return float4x4(asfloat(instances.Load4(at)), asfloat(instances.Load4(at + 16)), asfloat(instances.Load4(at + 32)), asfloat(instances.Load4(at + 48)));
}

/**
 * Where a box is in the picture: whether it is wholly out of sight, and — when every corner of it is in front of the eye —
 * the rectangle it covers, from 0 to 1, and the depth of its nearest point.
 */
struct Placed {
    bool outOfSight;
    bool inFront;
    float2 low, high;
    float nearest;
};
Placed place(float3 low, float3 high, float4x4 model)
{
    Placed placed;
    uint outside[6] = { 0, 0, 0, 0, 0, 0 };
    placed.inFront = true;
    placed.low = float2(1e30, 1e30);
    placed.high = float2(-1e30, -1e30);
    placed.nearest = push.reversed != 0 ? 0.0 : 1.0;
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        const float3 at = float3(corner & 1 ? high.x : low.x, corner & 2 ? high.y : low.y, corner & 4 ? high.z : low.z);
        const float4 clip = mul(mul(float4(at, 1.0), model), push.viewProjection);
        outside[0] += clip.x < -clip.w ? 1 : 0;
        outside[1] += clip.x > clip.w ? 1 : 0;
        outside[2] += clip.y < -clip.w ? 1 : 0;
        outside[3] += clip.y > clip.w ? 1 : 0;
        outside[4] += clip.z < 0.0 ? 1 : 0;
        outside[5] += clip.z > clip.w ? 1 : 0;
        if (clip.w <= 1e-6) { placed.inFront = false; continue; }
        const float3 ndc = clip.xyz / clip.w;
        placed.low = min(placed.low, ndc.xy * 0.5 + 0.5);
        placed.high = max(placed.high, ndc.xy * 0.5 + 0.5);
        placed.nearest = push.reversed != 0 ? max(placed.nearest, ndc.z) : min(placed.nearest, ndc.z);
    }
    placed.outOfSight = false;
    [unroll] for (uint plane = 0; plane < 6; ++plane) if (outside[plane] == 8) placed.outOfSight = true;
    return placed;
}

/** Whether what the pyramid says is drawn where a box is is all nearer than the box: the box is behind it. */
bool behind(Placed placed)
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

/** Counts an instance into its mesh's command, and writes its number where the draw finds it. */
void draw(uint instance, uint command, uint listed, uint visibleLength)
{
    const uint first = commands[command].firstInstance;
    const uint end = command + 1 < listed ? commands[command + 1].firstInstance : visibleLength;
    if (instance < first || instance >= end) return;
    uint before;
    InterlockedAdd(commands[command].instanceCount, 1, before);
    if (first + before < end) visible[first + before] = instance;
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
    const uint command = commandOf(listed, instance);
    const MeshRange range = ranges[command];
    const bool boxed = any(range.boundsMin.xyz != range.boundsMax.xyz);
    if (boxed && place(range.boundsMin.xyz, range.boundsMax.xyz, modelOf(instance)).outOfSight) return;
    draw(instance, command, listed, visibleLength);
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
    const uint command = commandOf(listed, instance);
    const MeshRange range = ranges[command];
    const bool boxed = any(range.boundsMin.xyz != range.boundsMax.xyz);
    bool shown = true;
    if (boxed) {
        const Placed placed = place(range.boundsMin.xyz, range.boundsMax.xyz, modelOf(instance));
        shown = !placed.outOfSight && !behind(placed);
    }
    const bool before = instance < seenLength && seen[instance] != 0;
    if (instance < seenLength) seen[instance] = shown ? 1 : 0;
    if (shown && !before) draw(instance, command, listed, visibleLength);
}
