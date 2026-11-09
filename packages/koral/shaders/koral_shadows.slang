// The engine's base library: how much of a light reaches a point past what hides it, read from the shadow maps the
// engine's functions draw — the sun's four cascades in one atlas (SunShadows), a cube of six faces for each point light
// (PointLightShadows), a map for each spot (SpotLightShadows).
//
//     import koral_shadows;
//
//     const KoralSunShadows sun = KoralSunShadows(atlas, cascades, viewDepth, KoralShadowFilter());
//     const KoralPointShadows lamps = KoralPointShadows(cubes, faces, compare, KoralShadowFilter());
//     const float3 lit = KoralLit(material, surface, camera, lights, environment, KoralCombineShadows(sun, lamps));
//
// Every map is depth as its light sees it, 0 near and 1 far. A point is moved off its surface, and toward its light,
// by as much of the world as a texel of the map covers there — a bias the same near the light and far, as a bias of
// the map's depth (not linear in distance) is not — before it is compared.

module koral_shadows;

import koral_math;
import koral_sampling;
__exported import koral_lights;

/** How shadows are filtered: how soft, how many taps, how far a point is moved before it is compared. */
public struct KoralShadowFilter {
    /** How far a point is moved toward its light, in its map's texels there. */
    public float bias;
    /** How far the filter reaches, in texels (the sun's: in its first cascade's, as wide in the world in every cascade). */
    public float softness;
    /** How many filtered taps a point light's or a spot's shadow takes. */
    public uint taps;
    /** The last part of each of the sun's cascades fades into the next. */
    public float cascadeBlend;
    /** Another turn of the taps' disc for each pixel and frame, 0 to 1: for TAA to average into a soft edge. */
    public float noise;

    public __init() { bias = 1.0; softness = 1.5; taps = 8; cascadeBlend = 0.2; noise = 0.0; }
};

// ---- the sun --------------------------------------------------------------------------------------------------------

/** The sun's cascades, as ShadowCascades fits them and a buffer holds them: their matrices, how far each reaches, its texel. */
public struct KoralSunCascades {
    public float4x4 cascades[4];
    public float4 splits;           // how far from the eye each cascade reaches
    public float4 worldTexel;       // how wide a texel of each is, in the world
};

/** Which of the sun's cascades holds a point @p depth along the view: 4 past the last. */
public uint KoralCascadeOf(KoralSunCascades sun, float depth)
{
    uint c = 0;
    while (c < 4 && depth > sun.splits[c]) ++c;
    return c;
}

/** How lit a point is in cascade @p c of the sun's atlas (two by two): 5 x 5 taps, as wide in the world in every cascade. */
public float KoralCascadeVisibility(Texture2D<float> atlas, KoralSunCascades sun, uint c, float3 position, float3 normal, float nl, KoralShadowFilter filter)
{
    const float3 offset = normal * sun.worldTexel[c] * (2.0 + 4.0 * (1.0 - nl));
    const float4 lit = mul(float4(position + offset, 1.0), sun.cascades[c]);
    const float3 ndc = lit.xyz / lit.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z > 1.0 || ndc.z < 0.0) return 1.0;
    uint width, height;
    atlas.GetDimensions(width, height);
    const float tile = float(width) * 0.5;
    const float2 corner = float2(float(c & 1), float(c >> 1));
    const float2 at = (ndc.xy * 0.5 + 0.5 + corner) * tile;
    const float reach = max(filter.softness * sun.worldTexel[0] / sun.worldTexel[c], 1.0);
    const float2 low = corner * tile, high = low + tile - 1.0;
    const float bias = filter.bias * 0.0015;
    float sum = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) {
            const float2 tap = clamp(at + float2(x, y) * reach * 0.5, low, high);
            sum += atlas.Load(int3(int2(tap), 0)) + bias >= ndc.z ? 1.0 : 0.0;
        }
    return sum / 25.0;
}

/** How lit a point @p depth along the view is by the sun: the first cascade that reaches it, faded into the next over its last part. */
public float KoralSunVisibility(Texture2D<float> atlas, KoralSunCascades sun, float3 position, float3 normal, float3 toLight, float depth, KoralShadowFilter filter)
{
    const uint c = KoralCascadeOf(sun, depth);
    if (c >= 4) return 1.0;
    const float nl = saturate(dot(normal, toLight));
    float light = KoralCascadeVisibility(atlas, sun, c, position, normal, nl, filter);
    const float start = c == 0 ? 0.0 : sun.splits[c - 1];
    const float through = (depth - start) / max(sun.splits[c] - start, 1e-4);
    if (c < 3 && through > 1.0 - filter.cascadeBlend)
        light = lerp(light, KoralCascadeVisibility(atlas, sun, c + 1, position, normal, nl, filter), smoothstep(1.0 - filter.cascadeBlend, 1.0, through));
    return light;
}

/** Whether a point in the air sees the sun: one tap of the first cascade that holds it; lit past the last. For fog. */
public float KoralSunVisibilityTap(Texture2D<float> atlas, KoralSunCascades sun, float3 position, float depth)
{
    const uint c = KoralCascadeOf(sun, depth);
    if (c >= 4) return 1.0;
    const float4 lit = mul(float4(position, 1.0), sun.cascades[c]);
    const float3 ndc = lit.xyz / lit.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z > 1.0 || ndc.z < 0.0) return 1.0;
    uint width, height;
    atlas.GetDimensions(width, height);
    const float2 at = (ndc.xy * 0.5 + 0.5 + float2(float(c & 1), float(c >> 1))) * float(width) * 0.5;
    return atlas.Load(int3(int2(at), 0)) + 0.0015 >= ndc.z ? 1.0 : 0.0;
}

// ---- a map of a point light's face or a spot ----------------------------------------------------------------------

/** Where a point is on a light's map: where (0 to 1), the depth it is compared at, and whether the map has it. */
public struct KoralMapPlace {
    public bool inside;
    public float2 uv;
    public float reference;
};

/**
 * Where a point is on the map @p matrix makes, @p width texels across over @p across radians: moved off its surface
 * (along @p normal, more the more it slopes from the light) and toward the light at @p lightAt.
 */
public KoralMapPlace KoralMapPlaceOf(float4x4 matrix, uint width, float across, float3 lightAt, float3 position, float3 normal, float3 toLight, float bias)
{
    const float distance = length(position - lightAt);
    // At this distance, a texel of the map is as much of the world across.
    const float texel = 2.0 * tan(across * 0.5) * distance / float(width);
    float3 p = position + normal * texel * (1.5 + 3.0 * (1.0 - saturate(dot(normal, toLight))));
    const float3 from = p - lightAt;
    const float apart = length(from);
    p = lightAt + from * max(apart - texel * bias, 0.0) / max(apart, 1e-6);
    const float4 seen = mul(float4(p, 1.0), matrix);
    KoralMapPlace place;
    place.inside = seen.w > 0.0;
    const float3 ndc = seen.xyz / max(seen.w, 1e-12);
    place.inside = place.inside && all(abs(ndc.xy) <= 1.0) && ndc.z >= 0.0 && ndc.z <= 1.0;
    place.uv = ndc.xy * 0.5 + 0.5;
    place.reference = ndc.z;
    return place;
}

/** Filtered taps over a disc (Vogel's spiral, turned by the filter's noise) of layer @p layer, each a comparison of four texels. */
public float KoralMapVisibility(Texture2DArray<float> maps, SamplerComparisonState compare, uint layer, KoralMapPlace place, uint width, KoralShadowFilter filter)
{
    if (!place.inside) return 1.0;
    const float radius = filter.softness / float(width);
    const float edge = 0.5 / float(width);
    float sum = 0.0;
    const uint taps = max(filter.taps, 1u);
    for (uint i = 0; i < taps; ++i) {
        const float2 uv = clamp(place.uv + KoralVogelDisk(i, taps, filter.noise * KoralTau) * radius, edge, 1.0 - edge);
        sum += maps.SampleCmpLevelZero(compare, float3(uv, float(layer)), place.reference);
    }
    return sum / float(taps);
}

/** Which face of a point light's cube a point @p fromLight away is on: +x, -x, +y, -y, +z, -z — as PointShadows orders them. */
public uint KoralCubeFaceOf(float3 fromLight)
{
    const float3 a = abs(fromLight);
    return a.x >= a.y && a.x >= a.z ? (fromLight.x > 0.0 ? 0u : 1u) : a.y >= a.z ? (fromLight.y > 0.0 ? 2u : 3u) : (fromLight.z > 0.0 ? 4u : 5u);
}

/** How lit a point is by point light @p lamp (of PointLightShadows' first `most`): the face it is on, @p faces its matrices. */
public float KoralPointVisibility(Texture2DArray<float> cubes, SamplerComparisonState compare, StructuredBuffer<float4x4> faces, uint lamp,
                                  float3 lampAt, float3 position, float3 normal, float3 toLight, KoralShadowFilter filter)
{
    uint width, height, layers;
    cubes.GetDimensions(width, height, layers);
    const uint layer = lamp * 6 + KoralCubeFaceOf(position - lampAt);
    if (layer >= layers) return 1.0;
    const KoralMapPlace place = KoralMapPlaceOf(faces[layer], width, KoralHalfPi, lampAt, position, normal, toLight, filter.bias);
    return KoralMapVisibility(cubes, compare, layer, place, width, filter);
}

/** How lit a point is by spot @p spot (of SpotLightShadows' first `most`), @p matrices its maps', @p across its map's width in radians. */
public float KoralSpotVisibility(Texture2DArray<float> maps, SamplerComparisonState compare, StructuredBuffer<float4x4> matrices, uint spot, float across,
                                 float3 spotAt, float3 position, float3 normal, float3 toLight, KoralShadowFilter filter)
{
    uint width, height, layers;
    maps.GetDimensions(width, height, layers);
    if (spot >= layers) return 1.0;
    const KoralMapPlace place = KoralMapPlaceOf(matrices[spot], width, across, spotAt, position, normal, toLight, filter.bias);
    return KoralMapVisibility(maps, compare, spot, place, width, filter);
}

// ---- as KoralLit takes them ---------------------------------------------------------------------------------------

/** The sun's shadows: for the first directional light, its cascades; every other light lets through. */
public struct KoralSunShadows : IKoralShadows {
    public Texture2D<float> atlas;
    public KoralSunCascades sun;
    public float depth;             // how far along the view the point is: which cascade holds it
    public KoralShadowFilter filter;
    public __init(Texture2D<float> atlas, KoralSunCascades sun, float depth, KoralShadowFilter filter) { this.atlas = atlas; this.sun = sun; this.depth = depth; this.filter = filter; }
    public float DirectionalVisibility(uint index, KoralDirectionalLight light, KoralArrival arrival, float3 position, float3 normal)
    {
        return index == 0 ? KoralSunVisibility(atlas, sun, position, normal, arrival.toLight, depth, filter) : 1.0;
    }
    public float PointVisibility(uint index, KoralPointLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
    public float SpotVisibility(uint index, KoralSpotLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
};

/** The point lights' shadows: a cube for each of the first, found by which point light it is; every other light lets through. */
public struct KoralPointShadows : IKoralShadows {
    public Texture2DArray<float> maps;
    public StructuredBuffer<float4x4> faces;
    public SamplerComparisonState compare;
    public KoralShadowFilter filter;
    public __init(Texture2DArray<float> maps, StructuredBuffer<float4x4> faces, SamplerComparisonState compare, KoralShadowFilter filter) { this.maps = maps; this.faces = faces; this.compare = compare; this.filter = filter; }
    public float DirectionalVisibility(uint index, KoralDirectionalLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
    public float PointVisibility(uint index, KoralPointLight light, KoralArrival arrival, float3 position, float3 normal)
    {
        return KoralPointVisibility(maps, compare, faces, index, light.position, position, normal, arrival.toLight, filter);
    }
    public float SpotVisibility(uint index, KoralSpotLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
};

/** The spots' shadows: a map for each of the first, found by which spot it is; every other light lets through. */
public struct KoralSpotShadows : IKoralShadows {
    public Texture2DArray<float> maps;
    public StructuredBuffer<float4x4> matrices;
    public SamplerComparisonState compare;
    public KoralShadowFilter filter;
    public __init(Texture2DArray<float> maps, StructuredBuffer<float4x4> matrices, SamplerComparisonState compare, KoralShadowFilter filter) { this.maps = maps; this.matrices = matrices; this.compare = compare; this.filter = filter; }
    public float DirectionalVisibility(uint index, KoralDirectionalLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
    public float PointVisibility(uint index, KoralPointLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
    public float SpotVisibility(uint index, KoralSpotLight light, KoralArrival arrival, float3 position, float3 normal)
    {
        return KoralSpotVisibility(maps, compare, matrices, index, light.MapAngle(), light.position, position, normal, arrival.toLight, filter);
    }
};

/** Two kinds of shadows at once: what both let through. Made by KoralCombineShadows. */
public struct KoralBothShadows<A : IKoralShadows, B : IKoralShadows> : IKoralShadows {
    public A first;
    public B second;
    public __init(A first, B second) { this.first = first; this.second = second; }
    public float DirectionalVisibility(uint index, KoralDirectionalLight light, KoralArrival arrival, float3 position, float3 normal)
    {
        return first.DirectionalVisibility(index, light, arrival, position, normal) * second.DirectionalVisibility(index, light, arrival, position, normal);
    }
    public float PointVisibility(uint index, KoralPointLight light, KoralArrival arrival, float3 position, float3 normal)
    {
        return first.PointVisibility(index, light, arrival, position, normal) * second.PointVisibility(index, light, arrival, position, normal);
    }
    public float SpotVisibility(uint index, KoralSpotLight light, KoralArrival arrival, float3 position, float3 normal)
    {
        return first.SpotVisibility(index, light, arrival, position, normal) * second.SpotVisibility(index, light, arrival, position, normal);
    }
};

/** Two kinds of shadows as one: KoralCombineShadows(sun, KoralCombineShadows(points, spots)) for all three. */
public KoralBothShadows<A, B> KoralCombineShadows<A : IKoralShadows, B : IKoralShadows>(A first, B second) { return KoralBothShadows<A, B>(first, second); }
