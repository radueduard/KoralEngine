// The shadows-soft module: shadows softer than the base library's filtering, as large lights cast them — percentage-closer
// soft shadows (Fernando's: the penumbra as wide as the occluder is far from what it shades), filtering over a Poisson
// disc, variance shadow maps (exponential, Lauritzen's EVSM: a map that can be blurred and read with one tap), and
// contact shadows traced through the depth of the picture for what is too small for a map.
//
//     import shadows_soft;
//
//     const float lit = KoralPcss(maps, compare, depths, layer, place, lightSize, filter);

module shadows_soft;

import koral_math;
import koral_sampling;
__exported import koral_shadows;

/** Sixteen points of a disc as evenly apart as random points can be (a Poisson disc): taps that do not line up. */
public static const float2 KoralPoissonDisc[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464), float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790),
};

/** A map's taps over a Poisson disc @p radius (in its uv) wide, turned by @p turn: each a comparison of four texels. */
public float KoralPoissonVisibility(Texture2DArray<float> maps, SamplerComparisonState compare, uint layer, float2 uv, float reference, float radius, float turn)
{
    const float2x2 turned = float2x2(cos(turn), -sin(turn), sin(turn), cos(turn));
    float sum = 0.0;
    [unroll] for (uint i = 0; i < 16; ++i)
        sum += maps.SampleCmpLevelZero(compare, float3(uv + mul(turned, KoralPoissonDisc[i]) * radius, float(layer)), reference);
    return sum / 16.0;
}

/**
 * Percentage-closer soft shadows on a map of depth: what is nearer the light than the point is looked for over a disc as
 * wide as the light (@p lightSize, in the map's uv at depth 1) — the blockers' average depth — and the point filtered
 * over a penumbra as wide as the blockers are far from it. @p depths is the map read as numbers, @p maps for comparing.
 */
public float KoralPcss(Texture2DArray<float> depths, Texture2DArray<float> maps, SamplerComparisonState compare, SamplerState point,
                       uint layer, KoralMapPlace place, float lightSize, float turn)
{
    if (!place.inside) return 1.0;
    const float searchRadius = lightSize * saturate(place.reference);
    float blockers = 0.0, count = 0.0;
    [unroll] for (uint i = 0; i < 16; ++i) {
        const float d = depths.SampleLevel(point, float3(place.uv + KoralPoissonDisc[i] * searchRadius, float(layer)), 0.0);
        if (d < place.reference) { blockers += d; count += 1.0; }
    }
    if (count == 0.0) return 1.0;
    const float blocker = blockers / count;
    const float penumbra = (place.reference - blocker) / max(blocker, 1e-4) * lightSize;
    return KoralPoissonVisibility(maps, compare, layer, place.uv, place.reference, max(penumbra, 1e-4), turn);
}

// ---- variance shadow maps -------------------------------------------------------------------------------------------

/** EVSM's warps of a depth (0 to 1): a positive and a negative exponential, each with its square — the four a moments map holds. */
public float4 KoralEvsmMoments(float depth, float2 exponents = float2(40.0, 5.0))
{
    const float d = depth * 2.0 - 1.0;
    const float positive = exp(exponents.x * d), negative = -exp(-exponents.y * d);
    return float4(positive, positive * positive, negative, negative * negative);
}

/** Chebyshev's bound of how much is lit past a depth, of a mean and a mean square, @p bleed of the light that leaks cut. */
public float KoralChebyshev(float2 moments, float depth, float minVariance, float bleed)
{
    if (depth <= moments.x) return 1.0;
    const float variance = max(moments.y - moments.x * moments.x, minVariance);
    const float d = depth - moments.x;
    const float upper = variance / (variance + d * d);
    return saturate((upper - bleed) / (1.0 - bleed));
}

/** How lit a point is by an EVSM moments map (blurred as wide as the shadow should be soft): one tap, any width of blur. */
public float KoralEvsmVisibility(float4 moments, float depth, float2 exponents = float2(40.0, 5.0), float bleed = 0.2)
{
    const float4 warped = KoralEvsmMoments(depth, exponents);
    const float positive = KoralChebyshev(moments.xy, warped.x, 1e-4 * exponents.x * exponents.x, bleed);
    const float negative = KoralChebyshev(moments.zw, warped.z, 1e-4 * exponents.y * exponents.y, bleed);
    return min(positive, negative);
}

// ---- contact shadows --------------------------------------------------------------------------------------------

/**
 * Whether what is seen at @p position is hidden from a light along @p toLight by something in the picture's own depth
 * within @p reach of it: @p steps along the way, each compared with what the picture has drawn there (thicker than
 * @p thickness is taken to be hollow). For what is too small for a shadow map: a cup on a table, a face's creases.
 */
public float KoralContactShadow(Texture2D<float> depth, float4x4 viewProjection, float3 position, float3 toLight, float reach, uint steps, float thickness,
                                float jitter, bool reversed)
{
    uint width, height;
    depth.GetDimensions(width, height);
    for (uint i = 1; i <= steps; ++i) {
        const float3 p = position + toLight * reach * (float(i) - jitter) / float(steps);
        const float4 clip = mul(float4(p, 1.0), viewProjection);
        if (clip.w <= 0.0) break;
        const float3 ndc = clip.xyz / clip.w;
        if (any(abs(ndc.xy) > 1.0)) break;
        const float2 uv = ndc.xy * 0.5 + 0.5;
        const float drawn = depth.Load(int3(int2(uv * float2(width, height)), 0));
        const float behind = reversed ? drawn - ndc.z : ndc.z - drawn;
        if (behind > 0.0 && behind < thickness) return 0.0;
    }
    return 1.0;
}
