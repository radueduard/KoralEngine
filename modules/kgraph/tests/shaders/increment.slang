// One more in the first number of the buffer each time it runs: a count of frames, for what reads it a frame later.

RWStructuredBuffer<uint> counter;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    counter[0] = counter[0] + 1;
}
