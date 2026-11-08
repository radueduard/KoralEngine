// The engine's base library: what the threads of a wave do together — many appending to one list with one atomic
// between them, rather than one each.
//
//     import koral_wave;
//
//     const uint at = KoralWaveAppend(keep, counters, 0);     // where this thread's goes, of those kept; ~0 when not kept
//     if (keep && at < capacity) list[at] = mine;

module koral_wave;

/** What KoralWaveAppend gives a thread that keeps nothing. */
public static const uint KoralNotAppended = 0xFFFFFFFFu;

/**
 * Where in a list this thread's goes, when @p keep: counter @p counter of @p counters says how many the list has, and is
 * moved on by as many as the wave keeps, by its first thread alone. Every thread of the wave calls it.
 */
public uint KoralWaveAppend(bool keep, RWStructuredBuffer<uint> counters, uint counter)
{
    const uint kept = WaveActiveCountBits(keep);
    uint first = 0;
    if (WaveIsFirstLane() && kept > 0) InterlockedAdd(counters[counter], kept, first);
    first = WaveReadLaneFirst(first);
    return keep ? first + WavePrefixCountBits(keep) : KoralNotAppended;
}

/** The sum of @p value over the wave's threads before this one. */
public uint KoralWaveExclusiveSum(uint value) { return WavePrefixSum(value); }
