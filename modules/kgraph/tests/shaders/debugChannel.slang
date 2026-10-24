// Writes, as its green, which channel of a material it is asked to show (koral_debug's `koralDebugView`, a tenth a
// channel): so a test can tell that the runtime built it with what the view asked.

import koral_debug;

[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = float4(1.0, float(koralDebugView) / 10.0, 0.0, 1.0);
}
