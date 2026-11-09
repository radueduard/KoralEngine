// The atmosphere module: the sky as the air makes it (after Hillaire, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique", 2020) — a planet with 100 km of air round it: Rayleigh's scattering, that makes it blue, Mie's
// of the haze, that makes the glow round the sun, and the ozone's absorption — worked out each frame into three small
// tables:
//
//   transmittance     how much of the light gets through the air, by height and the angle up, to the top of it
//   multiScattering   the light scattered more than once, as a single number per height and sun angle (its Psi)
//   skyView           the sky seen from the eye, all round, for the sun as it is now: what the sky pass looks up
//
// and from them what lights a scene: the sky's colours over the zenith, round the horizon and off the ground, and the
// sun's colour once the air has had it (KoralSkyLight, a buffer of one). The tables are for a sun of 1: its colour and
// strength above the air scales them. Lengths in the air are kilometres; in the scene, metres. The Atmosphere function
// makes the tables and the light (atmosphere_passes); Sky draws the sky and its clouds (atmosphere_clouds); Fog the light
// in the air near the eye (atmosphere_fog).
//
//     import atmosphere;
//
//     const float3 sky = KoralSkyViewAt(skyView, linear, direction, view, air) * view.sunColor;

module atmosphere;

import koral_math;
import koral_depth;

/** What a frame's sky is seen as: from where, the sun's way and colour, how big its disc is, and when (what clouds move by). */
public struct KoralSkyView {
    public float4x4 inverseViewProjection;  // the eye's: the way it looks through a pixel
    public float3 eye;                      // in the scene, metres
    public float time;                      // seconds
    public float3 sunDirection;             // the way its light travels
    public float sunSize;                   // degrees across
    public float3 sunColor;                 // linear, times its intensity, above the air
    public uint frame;                      // which frame, from 0: what what is marched starts a little further along by
    public float3 groundColor;              // the ground under the sky: its albedo
    public float unused;
};

/** The air: how much the ground gives back, how much haze it has (times Earth's), how high the scene's ground is in it. */
public struct KoralAir {
    public float groundAlbedo;
    public float haze;
    public float altitude;                  // km
    public float unused;
    public __init() { groundAlbedo = 0.25; haze = 1.0; altitude = 0.2; unused = 0.0; }
};

/** What lights a scene of the sky, made each frame of the tables (the Atmosphere function's `light`): rgb each, a unused. */
public struct KoralSkyLight {
    public float4 zenith;
    public float4 horizon;
    public float4 ground;
    public float4 sun;                      // the sun's light once the air has had it
};

public static const float KoralPlanetBottom = 6360.0, KoralPlanetTop = 6460.0;
static const float3 RayleighScattering = float3(5.802, 13.558, 33.1) * 1e-3;
static const float RayleighHeight = 8.0;
static const float MieScattering = 3.996e-3, MieAbsorption = 0.444e-3, MieHeight = 1.2, MieG = 0.8;
static const float3 OzoneAbsorption = float3(0.650, 1.881, 0.085) * 1e-3;

public static const uint2 KoralTransmittanceSize = uint2(256, 64);
public static const uint KoralMultiSize = 32;
public static const uint2 KoralSkyViewSize = uint2(192, 108);

/** What the air is at a height above the ground: its scattering by Rayleigh and by Mie, and how much it dims, all of it. */
public struct KoralMedium { public float3 rayleigh; public float mie; public float3 extinction; };

public KoralMedium KoralMediumAt(float height, KoralAir air)
{
    KoralMedium m;
    const float r = exp(-height / RayleighHeight), k = exp(-height / MieHeight) * air.haze;
    const float ozone = max(0.0, 1.0 - abs(height - 25.0) / 15.0);
    m.rayleigh = RayleighScattering * r;
    m.mie = MieScattering * k;
    m.extinction = m.rayleigh + (MieScattering + MieAbsorption) * k + OzoneAbsorption * ozone;
    return m;
}

/** How far along @p d from @p o, at the middle of the planet, a sphere of @p radius is first met going out; -1 when it is not. */
public float KoralToSphere(float3 o, float3 d, float radius)
{
    const float b = dot(o, d), c = dot(o, o) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    const float s = sqrt(disc);
    const float near = -b - s, far = -b + s;
    return near >= 0.0 ? near : far >= 0.0 ? far : -1.0;
}

public float KoralRayleighPhase(float c) { return 3.0 / (16.0 * KoralPi) * (1.0 + c * c); }

/** Cornette and Shanks's phase of the haze: forward, round the sun. */
public float KoralMiePhase(float c)
{
    const float g = MieG, g2 = g * g;
    return 3.0 / (8.0 * KoralPi) * (1.0 - g2) * (1.0 + c * c) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

/** Henyey and Greenstein's: how much of light travelling one way is scattered at @p cosine from it. */
public float KoralHenyeyGreenstein(float cosine, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * KoralPi * pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-4), 1.5));
}

// ---- the transmittance table: by height and the cosine up (Bruneton's mapping) ------------------------------------------

public float2 KoralTransmittanceUv(float r, float mu)
{
    const float h = sqrt(KoralPlanetTop * KoralPlanetTop - KoralPlanetBottom * KoralPlanetBottom);
    const float rho = sqrt(max(r * r - KoralPlanetBottom * KoralPlanetBottom, 0.0));
    const float d = max(0.0, -r * mu + sqrt(max(r * r * (mu * mu - 1.0) + KoralPlanetTop * KoralPlanetTop, 0.0)));
    const float dMin = KoralPlanetTop - r, dMax = rho + h;
    return float2((d - dMin) / max(dMax - dMin, 1e-4), rho / h);
}

/** How much of the light gets from the top of the air to a point @p r from the planet's middle, the sun @p mu up. */
public float3 KoralTransmittanceAt(Texture2D<float4> table, SamplerState s, float r, float mu)
{
    return table.SampleLevel(s, KoralTransmittanceUv(r, mu), 0.0).rgb;
}

public float3 KoralMultiAt(Texture2D<float4> table, SamplerState s, float r, float muSun)
{
    return table.SampleLevel(s, float2(muSun * 0.5 + 0.5, saturate((r - KoralPlanetBottom) / (KoralPlanetTop - KoralPlanetBottom))), 0.0).rgb;
}

// ---- the sky seen from the eye: by the angle round from the sun, across, and up, finer near the horizon ------------------

/** Where the eye is, from the middle of the planet: the scene's ground at the air's altitude, the eye its height over it. */
public float KoralEyeRadius(KoralSkyView view, KoralAir air) { return KoralPlanetBottom + max(air.altitude + view.eye.y * 0.001, 0.001); }

public float2 KoralSkyViewUv(float r, float3 d, float3 toSun)
{
    const float horizon = sqrt(max(r * r - KoralPlanetBottom * KoralPlanetBottom, 0.0));
    const float beta = acos(clamp(horizon / r, -1.0, 1.0)), zenithHorizon = KoralPi - beta;
    const float theta = acos(clamp(d.y, -1.0, 1.0));
    const float v = theta < zenithHorizon ? (1.0 - sqrt(saturate(1.0 - theta / zenithHorizon))) * 0.5
                                          : 0.5 + sqrt(saturate((theta - zenithHorizon) / beta)) * 0.5;
    const float2 a = d.xz, b = toSun.xz;
    const float la = length(a), lb = length(b);
    const float cosPhi = la > 1e-5 && lb > 1e-5 ? dot(a / la, b / lb) : 1.0;
    const float u = sqrt(acos(clamp(cosPhi, -1.0, 1.0)) / KoralPi);
    return float2(u, v);
}

/** The sky seen along @p d, for a sun of 1: what the eye sees of the air, the sun's disc left out. */
public float3 KoralSkyViewAt(Texture2D<float4> table, SamplerState s, float3 d, KoralSkyView view, KoralAir air)
{
    return table.SampleLevel(s, KoralSkyViewUv(KoralEyeRadius(view, air), d, -view.sunDirection), 0.0).rgb;
}

/**
 * The ground under the sky seen along @p d, where it looks down: its colour lit as @p light says the ground is — less
 * under a cloud (@p shadow) — dimmed by the air between, far off into the haze. Nothing, looking up.
 */
public float3 KoralGroundSeen(float3 d, KoralSkyView view, KoralSkyLight light, KoralAir air, float shadow = 1.0)
{
    if (d.y >= 0.0) return 0.0;
    const float distance = min(max(view.eye.y, 0.5) * 0.001 / max(-d.y, 1e-3), 400.0);
    const float3 thickest = RayleighScattering + (MieScattering + MieAbsorption) * air.haze;
    return light.ground.rgb * lerp(0.35, 1.0, shadow) * exp(-thickest * distance * 0.6) * smoothstep(0.0, -0.02, d.y);
}

/** Where the scene's ground (y = 0) is along @p d from the eye, in metres. */
public float3 KoralGroundPoint(float3 d, KoralSkyView view) { return view.eye + d * (max(view.eye.y, 0.5) / max(-d.y, 1e-3)); }

/** The sun's disc seen along @p d: its light once the air has had it, over the disc view.sunSize across, edges soft. */
public float3 KoralSunDisc(float3 d, KoralSkyView view, KoralSkyLight light)
{
    const float edge = cos(radians(view.sunSize * 0.5));
    return light.sun.rgb * smoothstep(edge - 0.00002, edge + 0.00002, dot(d, -view.sunDirection)) * 8.0;
}

/** The light the sky gives a surface facing @p normal: the ground's from below, the zenith's from above, the horizon's between. */
public float3 KoralSkyLightFacing(float3 normal, KoralSkyLight light)
{
    const float up = normal.y;
    return up >= 0.0 ? lerp(light.horizon.rgb, light.zenith.rgb, up) : lerp(light.horizon.rgb, light.ground.rgb, -up);
}

/** The way the eye looks through pixel @p uv of the view's picture. */
public float3 KoralSkyRay(float2 uv, KoralSkyView view) { return KoralRayThrough(KoralClipFromUv(uv), view.inverseViewProjection).direction; }
