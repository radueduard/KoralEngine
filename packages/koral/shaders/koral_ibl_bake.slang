// What koral_ibl lights with, baked of a cube map of the surroundings: each mip of `target` the cube as a surface of a
// roughness mirrors it (GGX, importance sampled, each sample from the mip whose texels are as wide as it covers —
// filtered importance sampling), and its nine spherical harmonics. The engine's PrefilterEnvironment function runs it.
//
// `roughness` (0 to 1) and `samples` are given on prefilter's Dispatch; `size` is how many texels across the source's
// first mip is.

import koral_ibl;
import koral_math;
import koral_random;
import koral_sampling;
import koral_brdf;

TextureCube<float4> source;
SamplerState sourceSampler;
[[vk::image_format("rgba16f")]] RWTexture2DArray<float4> target;     // a mip of the prefiltered cube: its six faces
RWStructuredBuffer<float4> sh;                                       // the nine harmonics

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilter(uint3 id : SV_DispatchThreadID, uniform float roughness, uniform uint samples, uniform uint size)
{
    uint width, height, faces;
    target.GetDimensions(width, height, faces);
    if (id.x >= width || id.y >= height) return;
    uint levels, w, h;
    source.GetDimensions(0, w, h, levels);
    const float2 uv = KoralPixelUv(id.xy, uint2(width, height));
    for (uint face = 0; face < 6; ++face) {
        const float3 n = KoralCubeDirection(face, uv);
        if (roughness <= 0.0) { target[uint3(id.xy, face)] = float4(source.SampleLevel(sourceSampler, n, 0.0).rgb, 1.0); continue; }
        // Seen square on: the eye, the normal and the way mirrored are one (Karis's split sum).
        const KoralBasis basis = KoralBasisOf(n);
        const float alpha = KoralAlphaOf(roughness);
        const float texel = 4.0 * KoralPi / (6.0 * float(size) * float(size));
        float3 sum = 0.0;
        float total = 0.0;
        const uint count = max(samples, 1u);
        for (uint i = 0; i < count; ++i) {
            const float3 m = basis.ToWorld(KoralGgxVisibleNormal(float3(0.0, 0.0, 1.0), alpha, KoralR2(i)));
            const float3 l = reflect(-n, m);
            const float nl = dot(n, l);
            if (nl <= 0.0) continue;
            // How much of the sphere this sample stands for, and the mip whose texels are as wide.
            const float nh = saturate(dot(n, m));
            const float pdf = KoralGgxDistribution(nh, alpha) * 0.25;
            const float covered = 1.0 / (float(count) * max(pdf, 1e-6));
            const float level = clamp(0.5 * log2(covered / texel) + 1.0, 0.0, float(levels - 1));
            sum += source.SampleLevel(sourceSampler, l, level).rgb * nl;
            total += nl;
        }
        target[uint3(id.xy, face)] = float4(sum / max(total, 1e-6), 1.0);
    }
}

groupshared float3 shared[64][9];

/** The nine harmonics of the source, a sum over a 32 x 32 grid of each face weighed by the sphere each texel covers: one group. */
[shader("compute")]
[numthreads(64, 1, 1)]
void projectSh(uint thread : SV_GroupIndex)
{
    float3 sum[9];
    [unroll] for (uint k = 0; k < 9; ++k) sum[k] = 0.0;
    const uint grid = 32;
    for (uint i = thread; i < 6 * grid * grid; i += 64) {
        const uint face = i / (grid * grid), texel = i % (grid * grid);
        const float2 uv = KoralPixelUv(uint2(texel % grid, texel / grid), uint2(grid, grid));
        const float3 d = KoralCubeDirection(face, uv);
        const float3 light = source.SampleLevel(sourceSampler, d, 2.0).rgb;
        float b[9];
        KoralShBasis(d, b);
        const float weight = KoralCubeTexelSolidAngle(uv, grid);
        [unroll] for (uint k = 0; k < 9; ++k) sum[k] += light * b[k] * weight;
    }
    [unroll] for (uint k = 0; k < 9; ++k) shared[thread][k] = sum[k];
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 32; step > 0; step >>= 1) {
        if (thread < step)
            [unroll] for (uint k = 0; k < 9; ++k) shared[thread][k] += shared[thread + step][k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread < 9) sh[thread] = float4(shared[0][thread], 1.0);
}
