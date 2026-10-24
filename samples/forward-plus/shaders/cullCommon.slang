// What the culls share: an indirect draw's command, and how an instance is counted into the command of its mesh.

module cullCommon;

public struct Command {
    public uint indexCount;
    public uint instanceCount;
    public uint firstIndex;
    public int vertexOffset;
    public uint firstInstance;
};

/**
 * One more of @p instance's mesh's command, and its number where the draw finds it. Each command's instances run to the
 * next's first (the last's, to the end of @p visible): an instance is of one, and counted once a frame, so there is room.
 */
public void counted(RWStructuredBuffer<Command> commands, uint count, RWStructuredBuffer<uint> visible, uint visibleLength, uint instance)
{
    const uint command = commandOf(commands, count, instance);
    const uint first = commands[command].firstInstance;
    const uint end = command + 1 < count ? commands[command + 1].firstInstance : visibleLength;
    if (instance < first || instance >= end) return;
    uint before;
    InterlockedAdd(commands[command].instanceCount, 1, before);
    if (first + before < end) visible[first + before] = instance;
}

/** Which of @p count commands an instance is drawn by: the last whose instances start at or before it (they are in order). */
public uint commandOf(RWStructuredBuffer<Command> commands, uint count, uint instance)
{
    uint low = 0, high = count;
    while (high - low > 1) {
        const uint middle = (low + high) / 2;
        if (commands[middle].firstInstance <= instance) low = middle;
        else high = middle;
    }
    return low;
}

