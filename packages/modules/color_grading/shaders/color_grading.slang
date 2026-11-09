// The color-grading module: curves that take light to the display besides the engine's own (koral_display's ACES fit
// and PBR Neutral) — AgX, Reinhard's, Hable's, Lottes's — and what a colourist turns: lift, gamma and gain, white
// balance, saturation and contrast, a 3D table. The Grade function runs them over a picture; AutoExposure works out its
// exposure from how bright it is.
//
//     import color_grading;
//
//     float3 c = KoralWhiteBalance(light, temperature, tint);
//     c = KoralAgX(c * exposure);

module color_grading;

__exported import koral_display;
import koral_math;

// ---- curves ------------------------------------------------------------------------------------------------------

/** Reinhard's, extended: @p white is how bright what is shown as white is. */
public float3 KoralReinhard(float3 x, float white = 4.0)
{
    const float y = KoralLuminance(x);
    const float mapped = y * (1.0 + y / (white * white)) / (1.0 + y);
    return x * (mapped / max(y, 1e-6));
}

float3 HableCurve(float3 x)
{
    const float a = 0.15, b = 0.50, c = 0.10, d = 0.20, e = 0.02, f = 0.30;
    return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}

/** Hable's (Uncharted 2's), filmic: a toe and a shoulder, white at 11.2. */
public float3 KoralHable(float3 x) { return saturate(HableCurve(x * 2.0) / HableCurve(float3(11.2))); }

/** Lottes's: contrast, shoulder and how bright white is said directly. */
public float3 KoralLottes(float3 x, float contrast = 1.6, float shoulder = 0.977, float white = 8.0)
{
    const float midIn = 0.18, midOut = 0.267;
    const float b = (-pow(midIn, contrast) + pow(white, contrast) * midOut) / ((pow(white, contrast * shoulder) - pow(midIn, contrast * shoulder)) * midOut);
    const float c = (pow(white, contrast * shoulder) * pow(midIn, contrast) - pow(white, contrast) * pow(midIn, contrast * shoulder) * midOut)
                  / ((pow(white, contrast * shoulder) - pow(midIn, contrast * shoulder)) * midOut);
    return saturate(pow(x, contrast) / (pow(x, contrast * shoulder) * b + c));
}

/**
 * AgX (Sobotka's), in Benjamin Wrensch's fit: hues kept as they brighten, what is very bright going to white rather
 * than to its channel's colour. Linear in, display-ready out (on the display's curve already).
 */
public float3 KoralAgX(float3 x)
{
    const float3x3 inset = float3x3(0.842479062253094, 0.0784335999999992, 0.0792237451477643,
                                    0.0423282422610123, 0.878468636469772, 0.0791661274605434,
                                    0.0423756549057051, 0.0784336, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0980208811401368, -0.0990297440797205,
                                     -0.0528968517574562, 1.15190312990417, -0.0989611768448433,
                                     -0.0529716355144438, -0.0980434501171241, 1.15107367264116);
    const float low = -12.47393, high = 4.026069;
    float3 v = mul(inset, max(x, 1e-10));
    v = saturate((log2(v) - low) / (high - low));
    const float3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    return saturate(mul(outset, v));
}

/** Which curve Grade's `curve` is: the engine's ACES fit, PBR Neutral, AgX, Reinhard's, Hable's or Lottes's. */
public static const uint KoralCurveAces = 0;
public static const uint KoralCurveNeutral = 1;
public static const uint KoralCurveAgX = 2;
public static const uint KoralCurveReinhard = 3;
public static const uint KoralCurveHable = 4;
public static const uint KoralCurveLottes = 5;

/** Light through curve @p curve, on the display's curve: what a picture of 8 bits a channel holds. */
public float3 KoralCurve(float3 light, uint curve)
{
    switch (curve) {
    case KoralCurveNeutral: return KoralGamma22FromLinear(KoralPbrNeutral(light));
    case KoralCurveAgX: return KoralAgX(light);
    case KoralCurveReinhard: return KoralGamma22FromLinear(saturate(KoralReinhard(light)));
    case KoralCurveHable: return KoralGamma22FromLinear(KoralHable(light));
    case KoralCurveLottes: return KoralGamma22FromLinear(KoralLottes(light));
    default: return KoralDisplay(light);
    }
}

// ---- grading -----------------------------------------------------------------------------------------------------

/** Lift (raises the shadows), gamma (the middle) and gain (scales the highlights), each a colour: 0, 1 and 1 leave it as it is. */
public float3 KoralLiftGammaGain(float3 c, float3 lift, float3 gamma, float3 gain)
{
    const float3 lifted = saturate(c + lift * (1.0 - c)) * gain;
    return pow(max(lifted, 0.0), 1.0 / max(gamma, 1e-3));
}

/** Contrast about a middle grey of 0.18, in log terms: 1 leaves it as it is. */
public float3 KoralContrast(float3 c, float contrast)
{
    const float3 log = log2(max(c, 1e-6) / 0.18);
    return 0.18 * exp2(log * contrast);
}

/** Saturation about its brightness: 0 grey, 1 as it is. */
public float3 KoralSaturation(float3 c, float saturation) { return lerp(float3(KoralLuminance(c)), c, saturation); }

/**
 * White balance: a light of @p temperature (-1 bluer to 1 warmer) and @p tint (-1 greener to 1 more magenta) shown as
 * white — a von Kries adaptation in LMS terms (Bradford's), the illuminant's chromaticity moved along the Planckian locus.
 */
public float3 KoralWhiteBalance(float3 c, float temperature, float tint)
{
    const float t1 = temperature * 10.0 / 6.0, t2 = tint * 10.0 / 6.0;
    // D65's chromaticity, moved.
    const float x = 0.31271 - t1 * (t1 < 0.0 ? 0.1 : 0.05);
    const float standard = 2.87 * x - 3.0 * x * x - 0.27509507;
    const float y = standard + t2 * 0.05;
    const float3 w1 = float3(0.949237, 1.03542, 1.08728);     // D65 in LMS
    const float Y = 1.0, X = Y * x / y, Z = Y * (1.0 - x - y) / y;
    const float3 w2 = float3(0.7328 * X + 0.4296 * Y - 0.1624 * Z, -0.7036 * X + 1.6975 * Y + 0.0061 * Z, 0.0030 * X + 0.0136 * Y + 0.9834 * Z);
    const float3 balance = w1 / w2;
    const float3x3 toLms = float3x3(3.90405e-1, 5.49941e-1, 8.92632e-3, 7.08416e-2, 9.63172e-1, 1.35775e-3, 2.31082e-2, 1.28021e-1, 9.36245e-1);
    const float3x3 fromLms = float3x3(2.85847, -1.62879, -2.48910e-2, -2.10182e-1, 1.15820, 3.24281e-4, -4.18120e-2, -1.18169e-1, 1.06867);
    return mul(fromLms, mul(toLms, c) * balance);
}

/** A colour looked up in a 3D table of @p size texels a side (a .cube's, as a 3D texture): what a grade made elsewhere does. */
public float3 KoralLut3D(Texture3D<float4> table, SamplerState sampler, float3 c, float size)
{
    const float3 at = saturate(c) * ((size - 1.0) / size) + 0.5 / size;
    return table.SampleLevel(sampler, at, 0.0).rgb;
}

// ---- exposure ----------------------------------------------------------------------------------------------------

/** Which of @p bins a brightness falls in, log2 from @p lowest to @p highest. Bin 0 is black. */
public uint KoralHistogramBin(float luminance, float lowest, float highest, uint bins)
{
    if (luminance < 1e-5) return 0;
    const float t = saturate((log2(luminance) - lowest) / (highest - lowest));
    return uint(t * float(bins - 2) + 1.0);
}

/** The middle brightness of a bin: what it stands for. */
public float KoralHistogramLuminance(float bin, float lowest, float highest, uint bins)
{
    return exp2((bin - 1.0) / float(bins - 2) * (highest - lowest) + lowest);
}
