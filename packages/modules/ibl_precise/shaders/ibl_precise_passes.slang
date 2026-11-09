// The ibl-precise module's pass: the split sum's table, a texel a roughness (down) and an nv (across). The BrdfLut
// function bakes it.

import ibl_precise;
import koral_math;

[[vk::image_format("rg16f")]] RWTexture2D<float2> table;

[shader("compute")]
[numthreads(8, 8, 1)]
void bake(uint3 id : SV_DispatchThreadID, uniform uint samples)
{
    uint width, height;
    table.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float2 uv = KoralPixelUv(id.xy, uint2(width, height));
    table[id.xy] = KoralIntegrateBrdf(uv.y, max(uv.x, 1e-3), samples);
}
