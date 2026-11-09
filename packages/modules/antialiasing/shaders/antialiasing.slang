// The antialiasing module: edges smoothed after a picture is drawn, for a project without the base library's TAA or
// beside it — FXAA (Lottes's, its quality form: the edge's ends searched for along it) — and a picture drawn smaller
// made the screen's size by a Lanczos filter. The FXAA and Upscale functions run them (antialiasing_passes).
//
//     import antialiasing;
//
//     const float3 smooth = KoralFxaa(picture, linear, uv, 1.0 / size, KoralFxaaSettings());

module antialiasing;

import koral_math;
import koral_color;

/** How FXAA finds edges and how far it looks along them. */
public struct KoralFxaaSettings {
    public float threshold;          // the least contrast, relative to the brightest round, that is an edge: 0.125 is high quality
    public float thresholdMin;       // the least contrast at all: dark edges are left
    public float subpixel;           // how much of what is thinner than a pixel is smoothed, 0 to 1
    public uint steps;               // how far along an edge its ends are looked for
    public __init() { threshold = 0.125; thresholdMin = 0.0625; subpixel = 0.75; steps = 12; }
};

float Luma(Texture2D<float4> picture, SamplerState linear, float2 uv) { return KoralLuminance(picture.SampleLevel(linear, uv, 0.0).rgb); }

/** A pixel of @p picture at @p uv (@p texel one over its size) with its edge smoothed: FXAA. */
public float3 KoralFxaa(Texture2D<float4> picture, SamplerState linear, float2 uv, float2 texel, KoralFxaaSettings settings)
{
    const float3 centre = picture.SampleLevel(linear, uv, 0.0).rgb;
    const float m = KoralLuminance(centre);
    const float n = Luma(picture, linear, uv + float2(0.0, -texel.y)), s = Luma(picture, linear, uv + float2(0.0, texel.y));
    const float e = Luma(picture, linear, uv + float2(texel.x, 0.0)), w = Luma(picture, linear, uv + float2(-texel.x, 0.0));
    const float high = max(max(max(n, s), max(e, w)), m), low = min(min(min(n, s), min(e, w)), m);
    const float contrast = high - low;
    if (contrast < max(settings.thresholdMin, high * settings.threshold)) return centre;

    const float ne = Luma(picture, linear, uv + float2(texel.x, -texel.y)), nw = Luma(picture, linear, uv - texel);
    const float se = Luma(picture, linear, uv + texel), sw = Luma(picture, linear, uv + float2(-texel.x, texel.y));
    // How much is thinner than a pixel: the centre against the average round it.
    const float average = (2.0 * (n + s + e + w) + ne + nw + se + sw) / 12.0;
    const float thin = smoothstep(0.0, 1.0, saturate(abs(average - m) / contrast));
    const float blend = thin * thin * settings.subpixel;

    // Whether the edge runs across or along, and which side of it the other side is.
    const float horizontal = abs(n + s - 2.0 * m) * 2.0 + abs(ne + se - 2.0 * e) + abs(nw + sw - 2.0 * w);
    const float vertical = abs(e + w - 2.0 * m) * 2.0 + abs(ne + nw - 2.0 * n) + abs(se + sw - 2.0 * s);
    const bool across = horizontal >= vertical;
    const float positive = across ? s : e, negative = across ? n : w;
    const float gradientPositive = abs(positive - m), gradientNegative = abs(negative - m);
    float step = across ? texel.y : texel.x;
    float opposite;
    float gradient;
    if (gradientPositive >= gradientNegative) { opposite = positive; gradient = gradientPositive; }
    else { opposite = negative; gradient = gradientNegative; step = -step; }

    // Along the edge, each way, until its ends: where the contrast across it falls away.
    float2 along = across ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 at = uv + (across ? float2(0.0, step * 0.5) : float2(step * 0.5, 0.0));
    const float edge = (m + opposite) * 0.5, quarter = gradient * 0.25;
    float2 forward = at + along, backward = at - along;
    float lumaForward = Luma(picture, linear, forward) - edge, lumaBackward = Luma(picture, linear, backward) - edge;
    bool doneForward = abs(lumaForward) >= quarter, doneBackward = abs(lumaBackward) >= quarter;
    for (uint i = 1; i < settings.steps && !(doneForward && doneBackward); ++i) {
        const float scale = i < 4 ? 1.0 : i < 8 ? 2.0 : 4.0;
        if (!doneForward) { forward += along * scale; lumaForward = Luma(picture, linear, forward) - edge; doneForward = abs(lumaForward) >= quarter; }
        if (!doneBackward) { backward -= along * scale; lumaBackward = Luma(picture, linear, backward) - edge; doneBackward = abs(lumaBackward) >= quarter; }
    }
    const float toForward = across ? forward.x - uv.x : forward.y - uv.y, toBackward = across ? uv.x - backward.x : uv.y - backward.y;
    const bool nearerForward = toForward < toBackward;
    const float nearest = min(toForward, toBackward), length = toForward + toBackward;
    // Only on the side of the edge where the end that is nearer turns the other way.
    const bool turns = ((nearerForward ? lumaForward : lumaBackward) < 0.0) != (m - edge < 0.0);
    const float offset = turns ? 0.5 - nearest / max(length, 1e-6) : 0.0;
    const float moved = max(offset, blend);
    const float2 to = uv + (across ? float2(0.0, moved * step) : float2(moved * step, 0.0));
    return picture.SampleLevel(linear, to, 0.0).rgb;
}

/** Lanczos's window of two lobes at @p x texels. */
public float KoralLanczos2(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    const float px = KoralPi * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

/** A smaller picture read at @p uv with a Lanczos filter of 4 x 4 texels, kept within its neighbours so it does not ring. */
public float3 KoralLanczosUpscale(Texture2D<float4> picture, float2 uv, uint2 size)
{
    const float2 at = uv * float2(size) - 0.5, base = floor(at), f = at - base;
    float3 sum = 0.0, low = 1e30, high = -1e30;
    float total = 0.0;
    for (int y = -1; y <= 2; ++y)
        for (int x = -1; x <= 2; ++x) {
            const int2 texel = clamp(int2(base) + int2(x, y), int2(0, 0), int2(size) - 1);
            const float3 c = picture.Load(int3(texel, 0)).rgb;
            const float w = KoralLanczos2(float(x) - f.x) * KoralLanczos2(float(y) - f.y);
            sum += c * w;
            total += w;
            if (x >= 0 && x <= 1 && y >= 0 && y <= 1) { low = min(low, c); high = max(high, c); }
        }
    return clamp(sum / max(total, 1e-6), low, high);
}
