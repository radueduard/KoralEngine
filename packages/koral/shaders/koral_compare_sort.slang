// Sorts of any type that says how two of it compare (Slang's IComparable), in place, a pass at a time. A pass compares
// pairs that share no element, so that its threads need not wait for one another, and puts the lesser of each pair first.
//
// The buffer's type is known only to the module generated for it (a ComputePipeline's `compute.element`), whose entry
// points, `bitonic` and `oddEven`, call these. The engine's BitonicSort and OddEvenSort functions run the passes.

struct KoralSortPush { uint count; uint turn; uint start; };

void CompareSwap<T : IComparable>(RWStructuredBuffer<T> elements, const uint low, const uint high)
{
    const T a = elements[low], b = elements[high];
    if (b.lessThan(a)) { elements[low] = b; elements[high] = a; }
}

/**
 * One pass of a bitonic sort. Stage k, from 0, has k + 1 passes: a block of 2^(k+1) is folded in half, its first half met
 * by its second turned round, and each half then met by its own halves, down to neighbours. Every pair puts the lesser
 * at the lower index, so that what is past `count` could hold what is greater than anything and never move: a pair that
 * reaches past it is left out, and any count sorts in as many passes as the next power of two.
 */
void BitonicStep<T : IComparable>(RWStructuredBuffer<T> elements, const KoralSortPush push, const uint i)
{
    uint stage = 0, turn = push.turn;
    while (turn > stage) { turn -= stage + 1; ++stage; }
    const uint half = 1u << (stage - turn);
    if ((i & half) != 0) return;
    const uint other = turn == 0 ? (i | (2 * half - 1)) - (i & (half - 1)) : i + half;
    if (other < push.count) CompareSwap<T>(elements, i, other);
}

/** One phase of an odd-even transposition sort: each element meets the next, from the even ones or the odd ones in turn. */
void OddEvenStep<T : IComparable>(RWStructuredBuffer<T> elements, const KoralSortPush push, const uint i)
{
    if (((i ^ (push.start + push.turn)) & 1) == 0 && i + 1 < push.count) CompareSwap<T>(elements, i, i + 1);
}
