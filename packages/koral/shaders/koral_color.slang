// The engine's base library: colour. Light is added up in linear terms; a colour as it is picked, and as the display
// shows it, is on a curve. The engine's own curve is the power 2.2 (KoralLinearFromGamma22): what a material's colours
// and pictures are read with and what the display is given, as the Lights function's ColorConvert has it. sRGB's exact
// curve is here for what is stored as sRGB.
//
//     import koral_color;
//
//     const float3 albedo = KoralLinearFromGamma22(material.baseColor.rgb);
//     const float y = KoralLuminance(light);

module koral_color;

import koral_random;

// ---- curves --------------------------------------------------------------------------------------------------------

/** A colour as it is picked, in the linear terms light adds up in: the power 2.2. */
public float3 KoralLinearFromGamma22(float3 picked) { return pow(max(picked, 0.0), 2.2); }

/** Linear light back on the power-2.2 curve: what a display is given. */
public float3 KoralGamma22FromLinear(float3 linear) { return pow(max(linear, 0.0), 1.0 / 2.2); }

/** sRGB's exact curve, undone: a straight part near black, a power 2.4 past it. */
public float3 KoralLinearFromSrgb(float3 srgb)
{
    const float3 c = max(srgb, 0.0);
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

/** Linear light on sRGB's exact curve. */
public float3 KoralSrgbFromLinear(float3 linear)
{
    const float3 c = max(linear, 0.0);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// ---- how bright --------------------------------------------------------------------------------------------------

/** How bright a linear colour looks: Rec. 709's weights. */
public float KoralLuminance(float3 linear) { return dot(linear, float3(0.2126, 0.7152, 0.0722)); }

// ---- other terms -------------------------------------------------------------------------------------------------

/** Brightness and two colour differences apart (YCoCg): where a box round a colour's neighbours is tightest. */
public float3 KoralToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
public float3 KoralFromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

/** Oklab, of linear sRGB: lightness, then green to red and blue to yellow, a step the same size seen the same everywhere. */
public float3 KoralOklabFromLinear(float3 c)
{
    const float3 lms = float3(dot(c, float3(0.4122214708, 0.5363325363, 0.0514459929)),
                              dot(c, float3(0.2119034982, 0.6806995451, 0.1073969566)),
                              dot(c, float3(0.0883024619, 0.2817188376, 0.6299787005)));
    const float3 r = sign(lms) * pow(abs(lms), 1.0 / 3.0);
    return float3(dot(r, float3(0.2104542553, 0.7936177850, -0.0040720468)),
                  dot(r, float3(1.9779984951, -2.4285922050, 0.4505937099)),
                  dot(r, float3(0.0259040371, 0.7827717662, -0.8086757660)));
}

public float3 KoralLinearFromOklab(float3 lab)
{
    const float3 r = float3(dot(lab, float3(1.0, 0.3963377774, 0.2158037573)),
                            dot(lab, float3(1.0, -0.1055613458, -0.0638541728)),
                            dot(lab, float3(1.0, -0.0894841775, -1.2914855480)));
    const float3 lms = r * r * r;
    return float3(dot(lms, float3(4.0767416621, -3.3077115913, 0.2309699292)),
                  dot(lms, float3(-1.2684380046, 2.6097574011, -0.3413193965)),
                  dot(lms, float3(-0.0041960863, -0.7034186147, 1.7076147010)));
}

/** Hue (0 to 1 round the wheel), saturation and value, to linear terms as they are written. */
public float3 KoralRgbFromHsv(float3 hsv)
{
    const float3 k = frac(hsv.x + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0;
    return hsv.z * lerp(float3(1.0), saturate(abs(k - 3.0) - 1.0), hsv.y);
}

// ---- what is wrong -----------------------------------------------------------------------------------------------

/** Whether any part of @p v is not a number, or is infinite. */
public bool KoralIsBad(float3 v) { return any(isnan(v)) || any(isinf(v)); }

/** @p v, or black where it is not a number or is infinite: one bad pixel spreads through a blur, a history, a sum. */
public float3 KoralSane(float3 v) { return KoralIsBad(v) ? float3(0.0) : v; }

// ---- telling things apart ----------------------------------------------------------------------------------------

/** A colour of @p index's own, light enough to be seen on dark and on light: an entity's, a cluster's, a meshlet's. */
public float3 KoralColorOfIndex(uint index)
{
    const uint h = KoralPcgHash(index);
    return 0.25 + 0.75 * float3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0;
}
