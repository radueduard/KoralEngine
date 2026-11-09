// The filters module: what a picture is read with besides the base library's Gaussian (GaussianBlur) — bicubic and
// Catmull-Rom sampling, and the weights of the filters its passes run: Kawase's and the dual-filter blur, a bilateral
// and a joint bilateral (kept to edges by depth and normals), à-trous wavelets, and contrast-adaptive sharpening.
//
//     import filters;
//
//     const float4 sharp = KoralSampleCatmullRom(history, smooth, uv, size);     // a history read without blurring it

module filters;

import koral_math;
import koral_color;

// ---- sampling ----------------------------------------------------------------------------------------------------

/** A picture read at @p uv with a Catmull-Rom spline over 4 x 4 texels, in five bilinear reads (Jimenez's): sharper than bilinear. */
public float4 KoralSampleCatmullRom(Texture2D<float4> picture, SamplerState linear, float2 uv, float2 size)
{
    const float2 at = uv * size, centre = floor(at - 0.5) + 0.5;
    const float2 f = at - centre;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2, offset12 = w2 / w12;
    const float2 p0 = (centre - 1.0) / size, p3 = (centre + 2.0) / size, p12 = (centre + offset12) / size;
    float4 sum = 0.0;
    sum += picture.SampleLevel(linear, float2(p12.x, p0.y), 0.0) * w12.x * w0.y;
    sum += picture.SampleLevel(linear, float2(p0.x, p12.y), 0.0) * w0.x * w12.y;
    sum += picture.SampleLevel(linear, float2(p12.x, p12.y), 0.0) * w12.x * w12.y;
    sum += picture.SampleLevel(linear, float2(p3.x, p12.y), 0.0) * w3.x * w12.y;
    sum += picture.SampleLevel(linear, float2(p12.x, p3.y), 0.0) * w12.x * w3.y;
    const float total = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(sum / total, 0.0);
}

/** A picture read at @p uv with a B-spline over 4 x 4 texels, in four bilinear reads: smooth, as a magnified picture should be. */
public float4 KoralSampleBicubic(Texture2D<float4> picture, SamplerState linear, float2 uv, float2 size)
{
    const float2 at = uv * size - 0.5, f = frac(at), base = floor(at);
    const float2 f2 = f * f, f3 = f2 * f;
    const float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    const float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    const float2 g0 = w0 + w1, g1 = w2 + w3;
    const float2 h0 = (base - 0.5 + w1 / g0) / size, h1 = (base + 1.5 + w3 / g1) / size;
    return g0.y * (g0.x * picture.SampleLevel(linear, float2(h0.x, h0.y), 0.0) + g1.x * picture.SampleLevel(linear, float2(h1.x, h0.y), 0.0))
         + g1.y * (g0.x * picture.SampleLevel(linear, float2(h0.x, h1.y), 0.0) + g1.x * picture.SampleLevel(linear, float2(h1.x, h1.y), 0.0));
}

// ---- edge-aware weights ------------------------------------------------------------------------------------------

/** How much a neighbour of colour @p other counts beside @p centre: less the more they differ (a bilateral's range weight). */
public float KoralRangeWeight(float3 centre, float3 other, float sigma)
{
    const float3 d = centre - other;
    return exp(-dot(d, d) / max(2.0 * sigma * sigma, 1e-8));
}

/**
 * How much a neighbour counts by what it is of: as near in depth (relative to how far, @p depthSigma), and facing as
 * the same way (@p normalPower) — a joint bilateral's weight, which keeps a blur off edges the picture does not show.
 */
public float KoralGeometryWeight(float centreDepth, float otherDepth, float3 centreNormal, float3 otherNormal, float depthSigma, float normalPower)
{
    const float depth = exp(-abs(centreDepth - otherDepth) / max(depthSigma * abs(centreDepth), 1e-6));
    const float normal = pow(saturate(dot(centreNormal, otherNormal)), normalPower);
    return depth * normal;
}

/** The B3 spline's weights, the à-trous filter's 5 taps a side: 1/16, 1/4, 3/8, 1/4, 1/16. */
public float KoralAtrousWeight(int offset)
{
    const float w[3] = { 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };
    return w[min(abs(offset), 2)];
}

// ---- sharpening --------------------------------------------------------------------------------------------------

/**
 * Contrast-adaptive sharpening (as AMD's CAS): a pixel and its four neighbours, sharpened by as much as their contrast
 * leaves room for — @p sharpness 0 to 1 — so what is already crisp is not ringed.
 */
public float3 KoralSharpen(float3 centre, float3 up, float3 left, float3 right, float3 down, float sharpness)
{
    const float3 low = min(centre, min(min(up, down), min(left, right)));
    const float3 high = max(centre, max(max(up, down), max(left, right)));
    const float3 room = min(low, 1.0 - high) / max(high, 1e-5);
    const float3 amount = sqrt(saturate(room)) * -lerp(1.0 / 8.0, 1.0 / 5.0, saturate(sharpness));
    return saturate((centre + (up + left + right + down) * amount) / (1.0 + 4.0 * amount));
}
