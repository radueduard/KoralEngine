// The engine's base library: light made the picture a display shows. Exposed, put through a curve that rolls what is
// brighter than white off rather than clipping it, and encoded for the display (the power 2.2: koral_color).
//
//     import koral_display;
//
//     return float4(KoralDisplay(lit), 1.0);                           // the engine's: ACES's fit, then the display's curve
//     const float3 neutral = KoralPbrNeutral(light * exposure);        // colours kept as they are, up to white
//
// A project that wants other curves (AgX, a 3D table, Reinhard) imports the color-grading module.

module koral_display;

__exported import koral_color;

/** Narkowicz's fit of ACES's curve: contrasty, hues pulled towards white as they brighten. */
public float3 KoralAcesFitted(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

/** Khronos's PBR Neutral: a base colour shown as it is under white light, and only what is past white rolled off. */
public float3 KoralPbrNeutral(float3 color)
{
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, float3(newPeak), g);
}

/** What a camera's exposure value at ISO 100 lets through: the scale light is multiplied by before its curve. */
public float KoralExposureFromEv100(float ev100) { return 1.0 / (1.2 * exp2(ev100)); }

/** Light as the engine shows it: ACES's fit, on the display's curve. */
public float3 KoralDisplay(float3 light) { return KoralGamma22FromLinear(KoralAcesFitted(light)); }
