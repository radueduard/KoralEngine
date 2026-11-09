// The ibl-precise module: light from all round as exactly as a table makes it — the split sum's second half read from a
// table baked once (the BrdfLut function) rather than the base library's fit, what a rough metal loses to light that
// bounces between its facets given back of it, and reflection probes placed in a room: what is seen in one moved to
// where the room's walls are (box projected), and probes blended where they meet.
//
//     import ibl_precise;
//
//     const float2 dfg = KoralBrdfFromTable(lut, smooth, roughness, nv);
//     const float3 shine = (f0 * dfg.x + dfg.y) * KoralEnergyFromTable(f0, dfg);

module ibl_precise;

import koral_math;
import koral_sampling;
import koral_brdf;
import koral_random;

/** The table's scale and bias of f0 for a surface @p roughness rough seen at @p nv: BrdfLut's picture, roughness down and nv across. */
public float2 KoralBrdfFromTable(Texture2D<float4> table, SamplerState sampler, float roughness, float nv)
{
    return table.SampleLevel(sampler, float2(saturate(nv), saturate(roughness)), 0.0).rg;
}

/** What a rough mirror loses to light bouncing between its facets, given back, of the table's scale and bias (Fdez-Agüera). */
public float3 KoralEnergyFromTable(float3 f0, float2 dfg) { return 1.0 + f0 * (1.0 / max(dfg.x + dfg.y, 1e-4) - 1.0); }

/**
 * The split sum's second half at @p roughness and @p nv, integrated over GGX's facets (@p samples of them): what a
 * table's texel holds. BrdfLut bakes a picture of it.
 */
public float2 KoralIntegrateBrdf(float roughness, float nv, uint samples)
{
    const float3 v = float3(sqrt(max(1.0 - nv * nv, 0.0)), 0.0, nv);
    const float alpha = KoralAlphaOf(roughness);
    float a = 0.0, b = 0.0;
    for (uint i = 0; i < samples; ++i) {
        const float3 h = KoralGgxVisibleNormal(v, alpha, KoralR2(i));
        const float3 l = reflect(-v, h);
        if (l.z <= 0.0) continue;
        // Sampled by the visible normals, a sample's weight is G2 / G1 (Heitz).
        const float g1 = 2.0 * nv / (nv + sqrt(alpha * alpha + (1.0 - alpha * alpha) * nv * nv));
        const float g2 = KoralSmithGgxCorrelated(nv, l.z, alpha) * 4.0 * nv * l.z;
        const float weight = g2 / max(g1, 1e-6);
        const float fc = KoralPow5(1.0 - saturate(dot(v, h)));
        a += (1.0 - fc) * weight;
        b += fc * weight;
    }
    return float2(a, b) / float(max(samples, 1u));
}

/**
 * The way to look up a probe of a room so that what it saw is where the room's walls are: @p direction from @p position
 * met with the box from @p boxMin to @p boxMax, made a way from the probe's own place (Lagarde's box projection).
 */
public float3 KoralBoxProject(float3 direction, float3 position, float3 boxMin, float3 boxMax, float3 probe)
{
    const float3 toMax = (boxMax - position) / direction, toMin = (boxMin - position) / direction;
    const float3 far = max(toMax, toMin);
    const float distance = min(far.x, min(far.y, far.z));
    return KoralSafeNormalize(position + direction * distance - probe, direction);
}

/** How much a probe whose box is @p boxMin to @p boxMax counts at @p position: 1 well inside, falling to 0 over @p blend at its faces. */
public float KoralProbeWeight(float3 position, float3 boxMin, float3 boxMax, float blend)
{
    const float3 inside = min(position - boxMin, boxMax - position);
    return saturate(min(inside.x, min(inside.y, inside.z)) / max(blend, 1e-4));
}
