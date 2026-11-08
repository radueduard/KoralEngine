// The engine's base library: what an indirect draw of entities is made of on the GPU — IndirectDraws' commands, a
// MeshPack's ranges, a Draw's buffer of instances — for the passes that work out what of it is drawn (CullInSight,
// InstanceCull).
//
//     import koral_indirect;
//
//     const uint command = KoralCommandOf(commands, listed, instance);
//     const float4x4 model = KoralLoadMatrix(instances, instance * stride + matrixOffset);

module koral_indirect;

/** One indexed draw, as the device reads it (VkDrawIndexedIndirectCommand). */
public struct KoralDrawCommand {
    public uint indexCount;
    public uint instanceCount;
    public uint firstIndex;
    public int vertexOffset;
    public uint firstInstance;
};

/** A mesh's part of a MeshPack's buffers, and its box in its own space (all of it nothing where it is not known). */
public struct KoralMeshRange {
    public uint firstIndex;
    public uint indexCount;
    public int vertexOffset;
    public uint unused;
    public float4 boundsMin;
    public float4 boundsMax;
};

/** Whether a mesh's box is known: one that is not cannot be told out of sight, and is drawn. */
public bool KoralHasBounds(KoralMeshRange range) { return any(range.boundsMin.xyz != range.boundsMax.xyz); }

/** Which of the first @p count commands draws @p instance: the last whose instances start at or before it (they are in order). */
public uint KoralCommandOf(RWStructuredBuffer<KoralDrawCommand> commands, uint count, uint instance)
{
    uint low = 0, high = count;
    while (high - low > 1) {
        const uint middle = (low + high) / 2;
        if (commands[middle].firstInstance <= instance) low = middle;
        else high = middle;
    }
    return low;
}

/** The matrix @p at bytes into a buffer: four rows of four floats. */
public float4x4 KoralLoadMatrix(ByteAddressBuffer buffer, uint at)
{
    return float4x4(asfloat(buffer.Load4(at)), asfloat(buffer.Load4(at + 16)), asfloat(buffer.Load4(at + 32)), asfloat(buffer.Load4(at + 48)));
}

/**
 * Counts @p instance into its command and writes its number where the draw finds it: after those already counted, so
 * long as there is room before the next command's. Where it went, or ~0 when there was none.
 */
public uint KoralDrawInstance(RWStructuredBuffer<KoralDrawCommand> commands, RWStructuredBuffer<uint> visible, uint instance, uint command, uint listed, uint visibleLength)
{
    const uint first = commands[command].firstInstance;
    const uint end = command + 1 < listed ? commands[command + 1].firstInstance : visibleLength;
    if (instance < first || instance >= end) return 0xFFFFFFFFu;
    uint before;
    InterlockedAdd(commands[command].instanceCount, 1, before);
    if (first + before >= end) return 0xFFFFFFFFu;
    visible[first + before] = instance;
    return first + before;
}
