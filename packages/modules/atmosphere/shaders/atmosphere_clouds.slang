// The atmosphere module's clouds: cumulus in a layer over everything, made of noise (the noise module's) and carried on
// the wind, and the shadows they cast. A function of where and when, worked out wherever it is needed — the sky, the sun
// on a surface, the sun in the air — so that what is seen of the clouds and where their shadows fall cannot disagree, and
// nothing is kept from frame to frame. Each octave drifts a little faster than the one under it, so the clouds change
// shape as they go.
//
// A weather map over the ground says where clouds stand and how tall they grow; in the layer over it, a density in three
// dimensions makes each a heap — a flat base, billows, a narrowing top — that the sky marches through (KoralMarchClouds),
// each step lit by marching on toward the sun: a cloud shadows itself, its tops lit and its bases grey. What only needs
// the clouds roughly — the light the sky gives a scene — reads them as a flat layer of the same weather (KoralWithClouds).
//
//     import atmosphere_clouds;
//
//     const KoralClouds clouds = KoralClouds();                 // fair-weather cumulus; set what differs
//     const float sun = KoralCloudShadowAt(position, view, clouds);

module atmosphere_clouds;

import koral_math;
import koral_color;
import noise;
__exported import atmosphere;

/** What the clouds are: how much of the sky they cover, how high and thick, the wind, how they are marched. */
public struct KoralClouds {
    public float coverage;      // 0 none to 1
    public float height;        // the layer's base, metres
    public float scale;         // how wide a cloud is, about, metres
    public float windSpeed;     // metres a second
    public float windAngle;     // radians from +X toward +Z
    public float shadow;        // the most of the sun a cloud hides, however thick
    public float opacity;       // how thick they look against the sky, as a flat layer
    public float distance;      // how far away they thin out into the haze, metres
    public float sunBlock;      // the flat layer's: optical depth a unit of density
    public float thickness;     // from the bases to the tallest tops, metres
    public float extinction;    // how much a metre of the densest cloud dims
    public uint steps;          // the most steps a view ray takes through the layer
    public uint lightSteps;     // steps toward the sun from each, longer each time
    public float shaftHaze;     // haze under them, extinction a metre at the ground: what the sun's rays through the gaps are seen in
    public uint shaftSteps;
    public float reach;         // how far along a view ray they are marched, metres
    public __init()
    {
        coverage = 0.45; height = 600.0; scale = 800.0; windSpeed = 25.0; windAngle = 0.6; shadow = 1.0; opacity = 3.0;
        distance = 12000.0; sunBlock = 10.0; thickness = 1600.0; extinction = 0.045; steps = 192; lightSteps = 5;
        shaftHaze = 0.00008; shaftSteps = 12; reach = 6000.0;
    }
};

static const uint Octaves = 6;

float2 Wind(KoralClouds c, float time) { return float2(cos(c.windAngle), sin(c.windAngle)) * c.windSpeed * time; }

/** The weather over a point of the ground: x how much it is cloud country, 0 to 1; y its finest octave. */
float2 WeatherAt(float2 xz, float time, KoralClouds c, uint octaves = Octaves)
{
    const float2 wind = Wind(c, time);
    float2 p = (xz - wind) / c.scale;
    // Warped first, by a coarse noise of its own: outlines that curl and pinch, not round blobs.
    p += (float2(KoralValueNoise(p * 0.35 + 3.1), KoralValueNoise(p * 0.35 + 8.7)) - 0.5) * 1.4;
    float sum = 0.0, weight = 0.5, total = 0.0, fine = 0.0;
    for (uint o = 0; o < octaves; ++o) {
        // Each finer octave drifts a third faster than the one before, and is turned a little, against grid lines.
        const float octave = KoralValueNoise(p - wind / c.scale * (0.33 * float(o)));
        sum += octave * weight;
        total += weight;
        if (o == octaves - 1) fine = octave;
        p = float2(p.x * 1.6 - p.y * 1.2, p.x * 1.2 + p.y * 1.6) + 17.0;
        weight *= 0.5;
    }
    // Octaves added up bunch about a half: spread over 0 to 1, so that a coverage covers about as much as it says.
    return float2(saturate((sum / total - 0.25) / 0.5), fine);
}

/** How much cloud there is over a point of the ground, 0 to 1, as a flat layer. */
public float KoralCloudDensity(float2 xz, float time, KoralClouds c)
{
    if (c.coverage <= 0.0) return 0.0;
    const float2 w = WeatherAt(xz, time, c);
    const float body = smoothstep(1.0 - c.coverage, 1.0 - c.coverage + 0.18, w.x);
    return saturate(body - (1.0 - body) * w.y * 0.8);
}

/** Puffs: round lumps with creases between them, a cumulus' cauliflower. 0 to about 1. */
float Billow(float3 p) { return saturate(abs(KoralPerlin(p)) * 1.1); }

/** The heap's density at @p p, without its wisps; and @p q, where it is in the noise. */
float Shape(float3 p, float time, KoralClouds c, out float3 q)
{
    q = 0.0;
    const float h = (p.y - c.height) / c.thickness;
    if (h <= 0.0 || h >= 1.0 || c.coverage <= 0.0) return 0.0;
    const float2 w = WeatherAt(p.xz, time, c, 4);
    const float local = smoothstep(1.0 - c.coverage, 1.0 - c.coverage + 0.35, w.x);
    if (local <= 0.0) return 0.0;
    // Taller where there is more of it; flat at the base, rounding at once, narrowing all the way up.
    const float top = 0.2 + 0.8 * local;
    const float profile = smoothstep(0.0, 0.05, h) * (1.0 - smoothstep(0.3, 1.0, h / top));
    if (profile <= 0.0) return 0.0;
    const float2 wind = Wind(c, time);
    q = (p - float3(wind.x, -time * 0.6, wind.y)) / (c.scale * 0.4);
    const float shape = Billow(q) * 0.5 + Billow(q * 2.03 + 5.1) * 0.3 + Billow(q * 4.07 + 9.7) * 0.2;
    const float fill = local * profile * lerp(1.0, 0.85, smoothstep(0.0, 0.25, h));
    return saturate((shape - (1.0 - fill)) / max(fill, 1e-3));
}

/** A heap's edges eaten into wisps by a finer noise; its middle left. */
float Eroded(float density, float3 q)
{
    if (density <= 0.0 || density >= 0.6) return density;
    const float fine = Billow(q * 11.3 + 2.7) * 0.6 + Billow(q * 23.1 + 7.3) * 0.4;
    return saturate((density - fine * 0.25 * (1.0 - density)) / 0.75);
}

/** How dense the cloud is at @p p, 0 to 1; with @p detail, its edges eaten into wisps. */
public float KoralCloudAt(float3 p, float time, KoralClouds c, bool detail)
{
    float3 q;
    const float density = Shape(p, time, c, q);
    return detail ? Eroded(density, q) : density;
}

float StepAt(float t) { return clamp(t * 0.006, 12.0, 80.0); }

float DepthTowardSun(float3 p, float3 l, float time, KoralClouds c)
{
    float tau = 0.0, at = 0.0, step = 40.0;
    for (uint k = 0; k < c.lightSteps; ++k) {
        tau += KoralCloudAt(p + l * (at + step * 0.5), time, c, false) * step;
        at += step;
        step *= 1.8;
    }
    return tau * c.extinction;
}

/** How much of the clouds along @p d is seen, not the haze in front of them: far off toward the horizon, they fade into the sky. */
public float KoralCloudFade(float3 from, float3 d, KoralClouds c)
{
    if (d.y <= 0.01) return 0.0;
    return exp(-(c.height - from.y) / d.y / c.distance) * smoothstep(0.01, 0.08, d.y);
}

/**
 * The clouds along a view ray: rgb the light they send back along it, a how much of what is behind them gets through.
 * Each step lit by the sun through what is between it and the sun — single scattering and, in octaves each dimmer, less
 * dimmed and less forward, the light scattered many times that keeps a thick cloud white (after Wrenninge) — and by the
 * sky. @p jitter, 0 to 1, where in its first step it starts.
 */
public float4 KoralMarchClouds(float3 from, float3 d, KoralSkyView view, KoralSkyLight light, KoralClouds c, float jitter)
{
    if (c.coverage <= 0.0 || d.y <= 0.01 || from.y >= c.height) return float4(0.0, 0.0, 0.0, 1.0);
    const float enter = (c.height - from.y) / d.y, leave = (c.height + c.thickness - from.y) / d.y;
    const float far = min(leave, enter + c.reach);
    const float3 l = -view.sunDirection;
    const float cosine = dot(d, l);
    const float3 sun = light.sun.rgb * smoothstep(-0.05, 0.05, l.y);
    const float3 above = light.zenith.rgb * 1.6, below = light.horizon.rgb * 0.35 + light.ground.rgb * 0.5;
    float3 lit = 0.0;
    float through = 1.0;
    // Through clear air in strides of four, by the heaps alone; at the first cloud, a stride back, and on in steps.
    float t = enter + StepAt(enter) * jitter;
    bool striding = true;
    uint clear = 0;
    for (uint i = 0; i < c.steps && t < far && through > 0.01; ++i) {
        const float step = StepAt(t);
        const float3 p = from + d * t;
        float3 q;
        const float shape = Shape(p, view.time, c, q);
        if (striding) {
            if (shape <= 0.001) { t += step * 4.0; continue; }
            striding = false;
            clear = 0;
            t = max(enter, t - step * 4.0);
            continue;
        }
        t += step;
        const float density = Eroded(shape, q);
        if (density <= 0.001) { if (++clear >= 6) striding = true; continue; }
        clear = 0;
        const float sigma = c.extinction * density;
        const float tau = DepthTowardSun(p, l, view.time, c);
        float3 sunLit = 0.0;
        float a = 1.0, b = 1.0, g = 1.0;
        for (uint o = 0; o < 3; ++o) {
            sunLit += sun * a * exp(-b * tau) * lerp(KoralHenyeyGreenstein(cosine, 0.8 * g), KoralHenyeyGreenstein(cosine, -0.25 * g), 0.3);
            a *= 0.5; b *= 0.35; g *= 0.5;
        }
        // Powder: deep inside, the side toward the sun is darker than its edges.
        sunLit *= 1.0 - exp(-tau * 2.0 - density * 1.5) * 0.6;
        const float h = saturate((p.y - c.height) / c.thickness);
        const float passes = exp(-sigma * step);
        lit += through * (sunLit + lerp(below, above, sqrt(h))) * (1.0 - passes);
        through *= passes;
    }
    through = saturate((through - 0.01) / 0.99);
    return float4(lit * KoralCloudFade(from, d, c), through);
}

/** How much of the sun reaches @p p past the clouds, 0 to 1: marched across the layer along the way to the sun. */
public float KoralCloudShadowAt(float3 p, KoralSkyView view, KoralClouds c, uint steps = 4)
{
    const float3 l = -view.sunDirection;
    if (c.coverage <= 0.0 || l.y <= 0.02 || p.y >= c.height) return 1.0;
    const float enter = (c.height - p.y) / l.y, across = c.thickness / l.y;
    float tau = 0.0;
    for (uint k = 0; k < steps; ++k) tau += KoralCloudAt(p + l * (enter + across * (float(k) + 0.5) / float(steps)), view.time, c, false);
    tau *= c.extinction * across / float(steps);
    return 1.0 - c.shadow * (1.0 - exp(-tau)) * smoothstep(0.02, 0.15, l.y);
}

/**
 * The haze between the eye and the clouds along @p d, lit by the sun as far as the clouds let it through: rgb the light
 * it sends to the eye, a what gets through. Bright in the gaps, dark in the clouds' shadows: the sun's rays through them.
 */
public float4 KoralMarchShafts(float3 from, float3 d, KoralSkyView view, KoralSkyLight light, KoralClouds c, float jitter)
{
    const float3 l = -view.sunDirection;
    if (c.shaftHaze <= 0.0 || l.y <= 0.0 || from.y >= c.height) return float4(0.0, 0.0, 0.0, 1.0);
    const float reach = d.y > 0.01 ? min((c.height - from.y) / d.y, c.reach) : c.reach;
    const float step = reach / float(max(c.shaftSteps, 1u));
    const float cosine = dot(d, l);
    const float phase = lerp(KoralHenyeyGreenstein(cosine, 0.0), KoralHenyeyGreenstein(cosine, 0.75), 0.7);
    const float3 sun = light.sun.rgb * smoothstep(0.0, 0.05, l.y);
    const bool beams = cosine > 0.2;
    float3 lit = 0.0;
    float through = 1.0;
    for (uint i = 0; i < c.shaftSteps; ++i) {
        const float3 p = from + d * (step * (float(i) + jitter));
        const float sigma = c.shaftHaze * exp(-max(p.y, 0.0) / 1500.0);
        const float passes = exp(-sigma * step);
        lit += through * sun * phase * (beams ? KoralCloudShadowAt(p, view, c, 2) : 1.0) * (1.0 - passes);
        through *= passes;
    }
    return float4(lit, through);
}

/**
 * The sky along @p d with the clouds over it as a flat layer: @p sky behind, the clouds lit by the sun through what of
 * them is between, and by the sky, thinning into the haze toward the horizon. What the light the sky gives is made of.
 */
public float3 KoralWithClouds(float3 sky, float3 from, float3 d, KoralSkyView view, KoralSkyLight light, KoralClouds c)
{
    if (c.coverage <= 0.0 || d.y <= 0.0 || from.y >= c.height) return sky;
    const float t = (c.height - from.y) / d.y;
    if (t <= 0.0) return sky;
    const float2 at = from.xz + d.xz * t;
    const float density = KoralCloudDensity(at, view.time, c);
    if (density <= 0.0) return sky;
    const float3 l = -view.sunDirection;
    const float toward = KoralCloudDensity(at + l.xz / max(l.y, 0.15) * c.scale * 0.12, view.time, c);
    const float sunThrough = (exp(-3.0 * toward) + 0.45 * exp(-0.5 * toward)) * (1.0 - exp(-6.0 * density));
    const float g = 0.6, cosine = dot(d, l);
    const float glow = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-4), 1.5) * 0.25 + 0.6;
    const float3 over = light.zenith.rgb;
    const float3 tint = KoralLuminance(over) > 1e-5 ? over / KoralLuminance(over) : 1.0;
    const float3 ambient = max(KoralLuminance(sky), 0.03) * lerp(float3(1.0, 1.0, 1.03), tint, 0.5) * (0.7 + 0.3 * (1.0 - density));
    const float3 lit = ambient + light.sun.rgb * sunThrough * glow * smoothstep(-0.05, 0.1, l.y) * 0.1;
    const float alpha = (1.0 - exp(-c.opacity * density)) * smoothstep(0.0, 0.12, d.y) * exp(-t / c.distance);
    return lerp(sky, lit, alpha);
}
