// The atmosphere module's passes, made each frame (see atmosphere.slang for what the tables are): transmittance, then the
// multiple scattering of it, then the sky seen from the eye of both, then what lights the scene of that (`light`); and
// the sky drawn where nothing was, its clouds marched at a quarter of the picture's size. `view` is what the frame is
// seen as (KoralSkyView), `air` and `clouds` what they are: blocks a ShaderBuffer fills, as the Atmosphere and Sky
// functions do.

import atmosphere;
import atmosphere_clouds;
import koral_math;
import koral_random;

ConstantBuffer<KoralSkyView> view;
ConstantBuffer<KoralAir> air;
ConstantBuffer<KoralClouds> clouds;
SamplerState clampedLinear;

[[vk::image_format("rgba16f")]] RWTexture2D<float4> transmittanceOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void transmittance(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= KoralTransmittanceSize)) return;
    const float2 uv = KoralPixelUv(id.xy, KoralTransmittanceSize);
    const float h = sqrt(KoralPlanetTop * KoralPlanetTop - KoralPlanetBottom * KoralPlanetBottom), rho = h * uv.y;
    const float r = sqrt(rho * rho + KoralPlanetBottom * KoralPlanetBottom);
    const float dMin = KoralPlanetTop - r, dMax = rho + h, d = dMin + uv.x * (dMax - dMin);
    const float mu = d == 0.0 ? 1.0 : clamp((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
    const float3 o = float3(0.0, r, 0.0), dir = float3(sqrt(1.0 - mu * mu), mu, 0.0);
    const float length = KoralToSphere(o, dir, KoralPlanetTop);
    float3 depth = 0.0;
    const uint Steps = 40;
    for (uint i = 0; i < Steps; ++i) {
        const float3 p = o + dir * (length * (float(i) + 0.5) / float(Steps));
        depth += KoralMediumAt(sqrt(dot(p, p)) - KoralPlanetBottom, air).extinction * (length / float(Steps));
    }
    transmittanceOut[id.xy] = float4(exp(-depth), 1.0);
}

Texture2D<float4> transmittanceTable;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> multiOut;

/** Psi, by the sun's cosine across and height up: light scattered once more toward the eye from every way, and on again. */
[shader("compute")]
[numthreads(8, 8, 1)]
void multiScattering(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= KoralMultiSize)) return;
    const float2 uv = KoralPixelUv(id.xy, uint2(KoralMultiSize, KoralMultiSize));
    const float muSun = uv.x * 2.0 - 1.0;
    const float r = KoralPlanetBottom + uv.y * (KoralPlanetTop - KoralPlanetBottom - 0.01) + 0.005;
    const float3 o = float3(0.0, r, 0.0), sunDir = float3(sqrt(1.0 - muSun * muSun), muSun, 0.0);
    float3 second = 0.0, onward = 0.0;
    const uint Square = 8, Steps = 20;
    for (uint a = 0; a < Square; ++a)
        for (uint b = 0; b < Square; ++b) {
            const float cosTheta = 1.0 - 2.0 * (float(a) + 0.5) / float(Square), phi = KoralTau * (float(b) + 0.5) / float(Square);
            const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
            const float3 dir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
            const float ground = KoralToSphere(o, dir, KoralPlanetBottom), top = KoralToSphere(o, dir, KoralPlanetTop);
            const float length = ground > 0.0 ? ground : top;
            float3 through = 1.0, light = 0.0, sent = 0.0;
            for (uint i = 0; i < Steps; ++i) {
                const float dt = length / float(Steps);
                const float3 p = o + dir * (dt * (float(i) + 0.5));
                const float rp = sqrt(dot(p, p));
                const KoralMedium m = KoralMediumAt(rp - KoralPlanetBottom, air);
                const float3 scattering = m.rayleigh + m.mie;
                const float3 step = exp(-m.extinction * dt);
                const float3 up = p / rp;
                const float3 sun = KoralToSphere(p, sunDir, KoralPlanetBottom) > 0.0 ? 0.0 : KoralTransmittanceAt(transmittanceTable, clampedLinear, rp, dot(up, sunDir));
                const float3 e = max(m.extinction, 1e-6);
                light += through * (scattering * sun / (4.0 * KoralPi) - scattering * sun / (4.0 * KoralPi) * step) / e;
                sent += through * (scattering - scattering * step) / e;
                through *= step;
            }
            if (ground > 0.0) {     // and what the ground under it gives back
                const float3 p = o + dir * ground, up = normalize(p);
                light += through * KoralTransmittanceAt(transmittanceTable, clampedLinear, KoralPlanetBottom, dot(up, sunDir)) * saturate(dot(up, sunDir)) * air.groundAlbedo / KoralPi;
            }
            second += light;
            onward += sent;
        }
    const float n = float(Square * Square);
    multiOut[id.xy] = float4((second / n) / max(1.0 - onward / n, 1e-3), 1.0);
}

Texture2D<float4> multiTable;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> skyViewOut;

/** The sky along @p dir from @p o, for a sun of 1 along @p sunDir: single scattering, with the multiple as Psi gives it. */
float3 SkyAlong(float3 o, float3 dir, float3 sunDir)
{
    const float ground = KoralToSphere(o, dir, KoralPlanetBottom), top = KoralToSphere(o, dir, KoralPlanetTop);
    const float length = ground > 0.0 ? ground : top;
    if (length <= 0.0) return 0.0;
    const float c = dot(dir, sunDir);
    const float pr = KoralRayleighPhase(c), pm = KoralMiePhase(c);
    float3 through = 1.0, light = 0.0;
    const uint Steps = 32;
    float before = 0.0;
    for (uint i = 0; i < Steps; ++i) {
        // Closer together near the eye, where the air is thickest.
        const float t0 = (float(i) + 0.3) / float(Steps), t = length * t0 * t0;
        const float dt = t - before;
        before = t;
        const float3 p = o + dir * t;
        const float rp = sqrt(dot(p, p));
        const float muSun = dot(p / rp, sunDir);
        const KoralMedium m = KoralMediumAt(rp - KoralPlanetBottom, air);
        const float3 sun = KoralToSphere(p, sunDir, KoralPlanetBottom) > 0.0 ? 0.0 : KoralTransmittanceAt(transmittanceTable, clampedLinear, rp, muSun);
        const float3 scattered = (m.rayleigh * pr + m.mie * pm) * sun + (m.rayleigh + m.mie) * KoralMultiAt(multiTable, clampedLinear, rp, muSun);
        const float3 step = exp(-m.extinction * dt);
        const float3 e = max(m.extinction, 1e-6);
        light += through * (scattered - scattered * step) / e;
        through *= step;
    }
    return light;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void skyView(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= KoralSkyViewSize)) return;
    const float2 uv = KoralPixelUv(id.xy, KoralSkyViewSize);
    const float r = KoralEyeRadius(view, air);
    // The angle up from the table's v, the same way KoralSkyViewUv goes: back.
    const float horizon = sqrt(max(r * r - KoralPlanetBottom * KoralPlanetBottom, 0.0));
    const float beta = acos(clamp(horizon / r, -1.0, 1.0)), zenithHorizon = KoralPi - beta;
    float theta;
    if (uv.y < 0.5) { const float k = 1.0 - 2.0 * uv.y; theta = zenithHorizon * (1.0 - k * k); }
    else { const float k = 2.0 * uv.y - 1.0; theta = zenithHorizon + beta * k * k; }
    const float phi = uv.x * uv.x * KoralPi;
    // In a frame where the sun is round at phi 0, as high as it is.
    const float3 toSun = -view.sunDirection;
    const float sunUp = toSun.y, sunAcross = sqrt(max(1.0 - sunUp * sunUp, 0.0));
    const float3 dir = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
    skyViewOut[id.xy] = float4(SkyAlong(float3(0.0, r, 0.0), dir, float3(sunAcross, sunUp, 0.0)), 1.0);
}

// ---- what lights the scene ------------------------------------------------------------------------------------------

Texture2D<float4> skyViewTable;
RWStructuredBuffer<KoralSkyLight> light;

/** The sky seen from the eye along @p d with the clouds over it as a flat layer, lit by @p s 's sun. */
float3 Clouded(float3 d, KoralSkyLight s) { return KoralWithClouds(view.sunColor * KoralSkyViewAt(skyViewTable, clampedLinear, d, view, air), view.eye, d, view, s, clouds); }

[shader("compute")]
[numthreads(1, 1, 1)]
void lighting()
{
    KoralSkyLight s = light[0];
    const float3 toSun = -view.sunDirection;
    const float3 sun = view.sunColor * KoralTransmittanceAt(transmittanceTable, clampedLinear, KoralEyeRadius(view, air), toSun.y);
    s.sun = float4(sun, 0.0);     // first: the clouds below are lit by this frame's sun
    // The sky as it is drawn, the clouds over it, as a surface facing up sees it: three rings up to near the zenith, and
    // round the horizon, averaged over the way round.
    float3 zenith = 0.0, horizon = 0.0;
    const uint Round = 16;
    const float rings[3] = { 0.97, 0.866, 0.6 };
    for (uint i = 0; i < Round; ++i) {
        const float a = KoralTau * (float(i) + 0.5) / float(Round);
        for (uint k = 0; k < 3; ++k) {
            const float up = rings[k], across = sqrt(1.0 - up * up), b = a + float(k) * 0.4;
            zenith += Clouded(float3(cos(b) * across, up, sin(b) * across), s);
        }
        horizon += Clouded(normalize(float3(cos(a), 0.08, sin(a))), s);
    }
    zenith /= float(Round * 3);
    horizon /= float(Round);
    s.zenith = float4(zenith, 0.0);
    s.horizon = float4(horizon, 0.0);
    // The ground: what falls on it, sun and sky, given back by its colour.
    s.ground = float4(view.groundColor / KoralPi * (sun * saturate(toSun.y) + KoralPi * (zenith * 0.6 + horizon * 0.4)), 0.0);
    light[0] = s;
}

// ---- the sky ----------------------------------------------------------------------------------------------------------

StructuredBuffer<KoralSkyLight> skyLight;
Texture2D<float> depth;
Texture2D<float4> skyViewSeen;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> cloudsMarched;      // rgb the clouds' light, a what of the sky gets through
Texture2D<float4> cloudsSeen;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> picture;

/** Whether depth @p d is where nothing was drawn: 1 (0 for depth reversed). */
bool Far(float d, uint reversed) { return reversed != 0 ? d <= 0.0 : d >= 1.0; }

/**
 * The clouds a pixel of a quarter of the picture's size sees, where any of those under it shows the sky: started each
 * frame a little further along, so that what the frames before average is smooth.
 */
[shader("compute")]
[numthreads(8, 8, 1)]
void marchSky(uint3 id : SV_DispatchThreadID, uniform uint reversed)
{
    uint width, height, depthWidth, depthHeight;
    cloudsMarched.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    depth.GetDimensions(depthWidth, depthHeight);
    const int2 cell = max(int2(ceil(float2(depthWidth, depthHeight) / float2(width, height))), 1);
    bool sky = false;
    for (int y = 0; y < cell.y && !sky; ++y)
        for (int x = 0; x < cell.x && !sky; ++x)
            sky = Far(depth.Load(int3(min(int2(id.xy) * cell + int2(x, y), int2(depthWidth, depthHeight) - 1), 0)), reversed);
    if (!sky) { cloudsMarched[id.xy] = float4(0.0, 0.0, 0.0, 1.0); return; }
    const float3 d = KoralSkyRay(KoralPixelUv(id.xy, uint2(width, height)), view);
    const float jitter = KoralInterleavedGradientNoise(float2(id.xy) + float(view.frame % 64u) * 5.588238);
    const KoralSkyLight s = skyLight[0];
    const float4 shafts = KoralMarchShafts(view.eye, d, view, s, clouds, jitter);
    const float4 marched = KoralMarchClouds(view.eye, d, view, s, clouds, jitter);
    cloudsMarched[id.xy] = float4(shafts.rgb + shafts.a * marched.rgb, shafts.a * marched.a);
}

/** The sky where nothing was drawn: the air, the ground under it, the sun's disc, and the clouds over them. */
[shader("compute")]
[numthreads(8, 8, 1)]
void sky(uint3 id : SV_DispatchThreadID, uniform uint reversed, uniform float intensity)
{
    uint width, height;
    picture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    if (!Far(depth.Load(int3(id.xy, 0)), reversed)) return;
    const float2 uv = KoralPixelUv(id.xy, uint2(width, height));
    const float3 d = KoralSkyRay(uv, view);
    const KoralSkyLight s = skyLight[0];
    const float3 seen = KoralSkyViewAt(skyViewSeen, clampedLinear, d, view, air) * view.sunColor * intensity
                      + KoralGroundSeen(d, view, s, air, d.y < 0.0 ? KoralCloudShadowAt(KoralGroundPoint(d, view), view, clouds) : 1.0);
    const float4 marched = cloudsSeen.SampleLevel(clampedLinear, uv, 0.0);
    picture[id.xy] = float4(seen * lerp(1.0, marched.a, KoralCloudFade(view.eye, d, clouds)) + KoralSunDisc(d, view, s) * marched.a + marched.rgb, 1.0);
}
