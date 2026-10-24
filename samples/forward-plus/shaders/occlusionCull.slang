// The occlusion cull: the forward pass draws only what some pixel of the pre-pass showed. The pre-pass draws every
// instance and says, of each pixel, which one it shows (one more than its number); `mark` notes those, and `gather`
// counts each noted one into its mesh's indirect command — whose instances start at none each frame (IndirectDraws in
// eGPU) — and writes its number where the forward pass finds it: the command's first instance, plus how many before it.

import cullCommon;

Texture2D<uint> visibility;                 // the pre-pass's: one more than the instance each pixel shows; 0, none
RWStructuredBuffer<uint> seen;              // a uint an instance: 1 where some pixel showed it. Left at 0 again for the next frame

[numthreads(16, 16, 1)]
void mark(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    visibility.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const uint shown = visibility[id.xy];
    uint length, stride;
    seen.GetDimensions(length, stride);
    // Most pixels show what their neighbours do: written only by the first to find it unwritten, not by every one. (A
    // number past the buffer — a picture not drawn into yet, just made — is no instance.)
    if (shown != 0 && shown <= length && seen[shown - 1] == 0) seen[shown - 1] = 1;
}


RWStructuredBuffer<Command> commands;       // the forward Draw's: a mesh each, its instances from firstInstance on
RWStructuredBuffer<uint> visible;           // which instance each drawn one is: what the forward pass reads
[[vk::push_constant]] cbuffer Counts { uint commandCount; };

[numthreads(64, 1, 1)]
void gather(uint3 id : SV_DispatchThreadID)
{
    // How many instances there are: `visible` has room for each, and no more. Past them, what `seen` holds is nothing —
    // a buffer just made holds whatever its memory did.
    uint length, stride, commandsLength, visibleLength;
    seen.GetDimensions(length, stride);
    commands.GetDimensions(commandsLength, stride);
    visible.GetDimensions(visibleLength, stride);
    const uint instance = id.x;
    const uint listed = min(commandCount, commandsLength);
    if (listed == 0 || instance >= min(length, visibleLength) || seen[instance] == 0) return;
    seen[instance] = 0;
    counted(commands, listed, visible, visibleLength, instance);
}

