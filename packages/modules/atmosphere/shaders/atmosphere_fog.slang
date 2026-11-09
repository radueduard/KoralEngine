// The atmosphere module's light in the air near the eye: the view cut into 160 x 90 x 64 froxels — a cell every few
// pixels, exponential in depth out to `fog.distance` — each lit by the sun (through the clouds, and its shadow maps or
// rays where it has them), the sky, and every light of the scene (koral_lights'); blended with what the cell held last
// frame, gathered front to back along each pixel's ray, and laid over the picture by its depth. The Fog function runs it.
//
//   inject...   a cell: how much light it scatters toward the eye, and how much it dims what is behind it
//   settle      the same cell last frame, found where it was, blended in: the cells' jitter averages out
//   gather      a thread a column: light and transmittance from the eye to each slice
//   apply       a pixel: what is in front of it, over it

import atmosphere;
import atmosphere_clouds;
import koral_math;
import koral_random;
import koral_depth;
import koral_lights;
import koral_shadows;

/** What the haze is, and how its grid is laid. */
public struct KoralFog {
    public float density;       // extinction a metre, at the floor
    public float anisotropy;    // Henyey and Greenstein's g: forward scattering, toward the light
    public float strength;      // the light it scatters, times this
    public float distance;      // how far from the eye the grid reaches, metres
    public float ambient;       // the sky's share, as a fraction of what lights a surface
    public float history;       // how much of last frame's cell is kept
    public float height;        // its density falls to a third every this many metres up
    public float floor;         // where it is thickest, and the same below
    public float near;          // the view's near plane
    public uint samples;        // places in a cell lit each frame
    public uint lightCount;     // how many of `lights` there are
    public uint hasHistory;     // 1 when last frame's cells are of this view
    public float4x4 inverseView;
    public float4x4 previousViewProjection;
};

ConstantBuffer<KoralSkyView> view;
ConstantBuffer<KoralFog> fog;
ConstantBuffer<KoralClouds> clouds;
StructuredBuffer<KoralSkyLight> skyLight;
StructuredBuffer<KoralLight> lights;

public static const uint3 KoralFogCells = uint3(160, 90, 64);

float DensityAt(float3 p) { return fog.density * exp(-max(p.y - fog.floor, 0.0) / max(fog.height, 0.01)); }
float DepthAt(float w) { return fog.near * pow(fog.distance / fog.near, w); }
float SliceAt(float depth) { return log(max(depth, fog.near) / fog.near) / log(fog.distance / fog.near); }
float3 Forward() { return -float3(fog.inverseView[2][0], fog.inverseView[2][1], fog.inverseView[2][2]); }

float3 WorldOf(float2 uv, float w)
{
    const float3 ray = KoralSkyRay(uv, view);
    return view.eye + ray * (DepthAt(w) / max(dot(ray, Forward()), 1e-3));
}

/** How a point of the air knows what reaches it from the sun and from a light: an interface, each entry its own way. */
interface ISight {
    float SunSeen(float3 p, float along);
    float LightSeen(KoralLight light, float3 p, float distance);
};

/** Nothing in the way but the clouds. */
struct OpenSight : ISight {
    float SunSeen(float3 p, float along) { return 1.0; }
    float LightSeen(KoralLight light, float3 p, float distance) { return 1.0; }
};

// By the sun's shadow maps (SunShadows'), one tap a place: the cells blur it.
ConstantBuffer<KoralSunCascades> cascades;
Texture2D<float> sunAtlas;
struct SunSight : ISight {
    float SunSeen(float3 p, float along) { return KoralSunVisibilityTap(sunAtlas, cascades, p, along); }
    float LightSeen(KoralLight light, float3 p, float distance) { return 1.0; }
};

// By rays into the scene.
RaytracingAccelerationStructure scene;
struct RaySight : ISight {
    float Clear(float3 p, float3 direction, float far)
    {
        RayDesc ray = { p, 0.02, direction, far };
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
    }
    float SunSeen(float3 p, float along) { return Clear(p, -view.sunDirection, 2000.0); }
    float LightSeen(KoralLight light, float3 p, float distance) { return Clear(p, (light.position - p) / max(distance, 1e-4), distance - 0.05); }
};

/** The light a point of the grid scatters toward the eye, a metre of a haze of density 1: the sun's, the sky's, the lights'. */
float3 ScatteredAt<S : ISight>(S sight, uint3 id, float3 jitter, out float density)
{
    const float2 uv = (float2(id.xy) + jitter.xy) / float2(KoralFogCells.xy);
    const float w = (float(id.z) + jitter.z) / float(KoralFogCells.z);
    const float3 p = WorldOf(uv, w);
    density = DensityAt(p);
    const float3 ray = normalize(p - view.eye);
    const KoralSkyLight s = skyLight[0];
    // The sky's light comes from all round: over it, round the horizon, off the ground.
    const float3 sky = KoralSkyLightFacing(float3(0, 1, 0), s) * 0.5 + KoralSkyLightFacing(float3(1, 0, 0), s) * 0.35 + KoralSkyLightFacing(float3(0, -1, 0), s) * 0.15;
    float3 lit = sky * fog.ambient / (4.0 * KoralPi);
    if (any(s.sun.rgb > 0.0))
        lit += s.sun.rgb * KoralHenyeyGreenstein(dot(view.sunDirection, -ray), fog.anisotropy) * sight.SunSeen(p, DepthAt(w)) * KoralCloudShadowAt(p, view, clouds);
    for (uint i = 0; i < fog.lightCount; ++i) {
        const KoralLight light = lights[i];
        if (light.kind == KoralLightDirectional) continue;      // the sun is the sky's
        const KoralArrival arrival = KoralArrivalAt(light, p);
        if (all(arrival.radiance <= 0.0)) continue;
        lit += arrival.radiance * KoralHenyeyGreenstein(dot(-arrival.toLight, -ray), fog.anisotropy) * sight.LightSeen(light, p, arrival.distance);
    }
    return lit;
}

[[vk::image_format("rgba16f")]] RWTexture3D<float4> injected;   // rgb in-scattered light a metre, a extinction

/** What a cell scatters: a few places in it each frame, others the next (Roberts's R3), as @p sight sees them. */
void InjectCell<S : ISight>(S sight, uint3 id)
{
    if (any(id >= KoralFogCells)) return;
    const uint frameNumber = view.frame & 1023u;
    const float3 start = float3(KoralRandom(id.x + id.y * 160u, id.z, 0), KoralRandom(id.x + id.y * 160u, id.z, 1), KoralRandom(id.x + id.y * 160u, id.z, 2));
    float3 lit = 0.0;
    float extinction = 0.0;
    const uint samples = max(fog.samples, 1u);
    for (uint k = 0; k < samples; ++k) {
        float density;
        const float3 scattered = ScatteredAt(sight, id, frac(start + float(frameNumber * samples + k) * float3(0.8191725134, 0.6710436067, 0.5497004779)), density);
        lit += scattered * density;
        extinction += density;
    }
    injected[id] = float4(lit / float(samples) * fog.strength, extinction / float(samples));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void inject(uint3 id : SV_DispatchThreadID) { OpenSight sight; InjectCell(sight, id); }

[shader("compute")]
[numthreads(8, 8, 1)]
void injectSunShadowed(uint3 id : SV_DispatchThreadID) { SunSight sight; InjectCell(sight, id); }

[shader("compute")]
[numthreads(8, 8, 1)]
void injectTraced(uint3 id : SV_DispatchThreadID) { RaySight sight; InjectCell(sight, id); }

// ---- settle -------------------------------------------------------------------------------------------------------------

Texture3D<float4> fresh;                    // `injected`, read
Texture3D<float4> before;                   // `settled` as last frame left it
SamplerState clamped;
[[vk::image_format("rgba16f")]] RWTexture3D<float4> settled;

[shader("compute")]
[numthreads(8, 8, 1)]
void settle(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= KoralFogCells)) return;
    const float4 now = fresh.Load(int4(id, 0));
    float4 result = now;
    if (fog.hasHistory != 0) {
        // Where the cell's middle was in last frame's grid: its place on last frame's picture, and its depth then.
        const float3 p = WorldOf((float2(id.xy) + 0.5) / float2(KoralFogCells.xy), (float(id.z) + 0.5) / float(KoralFogCells.z));
        const float4 then = mul(float4(p, 1.0), fog.previousViewProjection);
        if (then.w > 0.0) {
            const float3 at = float3(KoralUvFromClip(then.xy / then.w), SliceAt(then.w));
            if (all(at >= 0.0) && all(at <= 1.0)) {
                const float4 old = before.SampleLevel(clamped, at, 0.0);
                if (all(isfinite(old))) result = lerp(now, old, fog.history);
            }
        }
    }
    settled[id] = result;
}

// ---- gather -------------------------------------------------------------------------------------------------------------

Texture3D<float4> cells;                    // `settled`, read
[[vk::image_format("rgba16f")]] RWTexture3D<float4> gathered;  // rgb light from the eye to the end of the slice, a what gets through

[shader("compute")]
[numthreads(8, 8, 1)]
void gather(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= KoralFogCells.xy)) return;
    const float3 ray = KoralSkyRay((float2(id.xy) + 0.5) / float2(KoralFogCells.xy), view);
    const float stretch = 1.0 / max(dot(ray, Forward()), 1e-3);
    float3 lit = 0.0;
    float through = 1.0;
    float near = fog.near;
    for (uint z = 0; z < KoralFogCells.z; ++z) {
        const float far = DepthAt(float(z + 1) / float(KoralFogCells.z));
        const float span = (far - near) * stretch;
        const float4 cell = cells.Load(int4(id.xy, z, 0));
        const float extinction = max(cell.a, 1e-6);
        const float passes = exp(-extinction * span);
        // What the slice scatters toward the eye, integrated over its length as it dims itself (Hillaire).
        lit += through * cell.rgb * (1.0 - passes) / extinction;
        through *= passes;
        gathered[uint3(id.xy, z)] = float4(lit, through);
        near = far;
    }
}

// ---- apply --------------------------------------------------------------------------------------------------------------

Texture2D<float> depth;
Texture3D<float4> fogCells;                 // `gathered`, read
[[vk::image_format("rgba16f")]] RWTexture2D<float4> picture;

[shader("compute")]
[numthreads(8, 8, 1)]
void apply(uint3 id : SV_DispatchThreadID, uniform uint reversed)
{
    uint width, height;
    picture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float2 uv = KoralPixelUv(id.xy, uint2(width, height));
    const float d = depth.Load(int3(id.xy, 0));
    const bool far = reversed != 0 ? d <= 0.0 : d >= 1.0;
    // How far along the view: the point back through the view, onto its forward.
    const float distance = far ? fog.distance : dot(KoralWorldFromDepth(uv, d, view.inverseViewProjection) - view.eye, Forward());
    const float w = saturate(SliceAt(distance) - 0.5 / float(KoralFogCells.z));
    const float4 inFront = fogCells.SampleLevel(clamped, float3(uv, w), 0.0);
    const float4 color = picture[id.xy];
    picture[id.xy] = float4(color.rgb * inFront.a + inFront.rgb, color.a);
}
