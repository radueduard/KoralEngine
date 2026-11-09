// The engine's base library: light from all round, baked — a cube map of the surroundings prefiltered for each
// roughness (a mip a roughness), and the light a matte surface is given of it as nine spherical harmonics. The engine's
// PrefilterEnvironment function bakes both of a cube (koral_ibl_bake); KoralShEnvironment lights with them.
//
//     import koral_ibl;
//
//     const KoralShEnvironment around = KoralShEnvironment(KoralShOf(shBuffer), prefiltered, smooth, mips);
//     const float3 lit = KoralLit(material, surface, camera, lights, around);

module koral_ibl;

import koral_math;
__exported import koral_shading;

/** Nine spherical harmonics of a colour: bands 0 to 2, enough for the light a matte surface is given from all round. */
public struct KoralSh9 {
    public float3 c[9];
};

/** The nine as a buffer of float4 keeps them (koral_ibl_bake's projectSh): rgb each, a fourth unused. */
public KoralSh9 KoralShOf(StructuredBuffer<float4> coefficients)
{
    KoralSh9 sh;
    [unroll] for (uint i = 0; i < 9; ++i) sh.c[i] = coefficients[i].rgb;
    return sh;
}

/** The nine basis functions at a direction of unit length. */
public void KoralShBasis(float3 d, out float b[9])
{
    b[0] = 0.282095;
    b[1] = 0.488603 * d.y;
    b[2] = 0.488603 * d.z;
    b[3] = 0.488603 * d.x;
    b[4] = 1.092548 * d.x * d.y;
    b[5] = 1.092548 * d.y * d.z;
    b[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    b[7] = 1.092548 * d.x * d.z;
    b[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

/**
 * What a surface facing @p normal is given of the light the harmonics hold, over π: what a white matte surface sends
 * back — an environment's Diffuse. The bands convolved with the cosine (Ramamoorthi and Hanrahan): π, 2π/3, π/4.
 */
public float3 KoralShDiffuse(KoralSh9 sh, float3 normal)
{
    float b[9];
    KoralShBasis(normal, b);
    const float band[3] = { 1.0, 2.0 / 3.0, 0.25 };
    float3 sum = 0.0;
    [unroll] for (uint i = 0; i < 9; ++i) sum += sh.c[i] * b[i] * band[i == 0 ? 0 : i < 4 ? 1 : 2];
    return max(sum, 0.0);
}

/** A cube prefiltered for each roughness — its mip `roughness × (mips - 1)` — and its harmonics for what is matte. */
public struct KoralShEnvironment : IKoralEnvironment {
    KoralSh9 sh;
    TextureCube<float4> prefiltered;
    SamplerState sampler;
    float mips;
    public __init(KoralSh9 sh, TextureCube<float4> prefiltered, SamplerState sampler, float mips)
    {
        this.sh = sh; this.prefiltered = prefiltered; this.sampler = sampler; this.mips = mips;
    }
    public float3 Diffuse(float3 normal) { return KoralShDiffuse(sh, normal); }
    public float3 Specular(float3 direction, float roughness) { return prefiltered.SampleLevel(sampler, direction, saturate(roughness) * max(mips - 1.0, 0.0)).rgb; }
};

/** The direction through texel @p uv (0 to 1) of face @p face of a cube: +x, -x, +y, -y, +z, -z, as Vulkan lays them. */
public float3 KoralCubeDirection(uint face, float2 uv)
{
    const float2 p = uv * 2.0 - 1.0;
    float3 d;
    switch (face) {
    case 0: d = float3(1.0, -p.y, -p.x); break;
    case 1: d = float3(-1.0, -p.y, p.x); break;
    case 2: d = float3(p.x, 1.0, p.y); break;
    case 3: d = float3(p.x, -1.0, -p.y); break;
    case 4: d = float3(p.x, -p.y, 1.0); break;
    default: d = float3(-p.x, -p.y, -1.0); break;
    }
    return normalize(d);
}

/** How much of the sphere a texel at @p uv of a face @p size texels across covers: what a sum over a cube weighs each by. */
public float KoralCubeTexelSolidAngle(float2 uv, uint size)
{
    const float2 p = uv * 2.0 - 1.0;
    const float texel = 2.0 / float(size);
    const float d = 1.0 + dot(p, p);
    return texel * texel / (d * sqrt(d));
}
