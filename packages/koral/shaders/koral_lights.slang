// The engine's base library: lights. Each kind its own struct, as the engine's Lights function lays them out — a buffer
// of each: its DirectionalLights, its PointLights, its SpotLights — and each knows what of it reaches a point. The light
// from all round, the sky's, is a KoralAmbientLight: the scene's Sky, as a ConstantBuffer holds it.
//
//     import koral_lights;
//     StructuredBuffer<KoralPointLight> pointLights;      // the Lights function's `points`; `counts`.y how many
//
//     const KoralArrival arrival = pointLights[i].ArrivalAt(position);
//
// A light's index in its buffer is which of its kind it is: what its shadow is found by. A shading lights itself through
// koral_shading's KoralLit, which takes any IKoralLights, any IKoralEnvironment and any IKoralShadows.

module koral_lights;

import koral_math;

// ---- how much reaches -------------------------------------------------------------------------------------------

/** How much of a light is left @p distance from it, out to @p range: less by the square of the way, and none at its range. */
public float KoralDistanceFalloff(float distance, float range)
{
    const float fade = saturate(1.0 - pow(distance / max(range, 1e-4), 4.0));
    return fade * fade / (distance * distance + 1.0);
}

/** How much of a spot reaches a point @p cosAngle off its middle: all of it within its inner cone, none past its outer. */
public float KoralSpotFalloff(float cosAngle, float cosInner, float cosOuter) { return smoothstep(cosOuter, cosInner, cosAngle); }

/** What of a light reaches a point: from where, how far it came, and how much of it. */
public struct KoralArrival {
    public float3 toLight;
    public float distance;      // to the light: very far for a directional one
    public float3 radiance;
};

/** Whether a sphere touches a box: its nearest point within the radius. */
bool SphereTouchesBox(float3 centre, float radius, float3 low, float3 high)
{
    const float3 away = clamp(centre, low, high) - centre;
    return dot(away, away) < radius * radius;
}

// ---- the kinds ----------------------------------------------------------------------------------------------------

/** A light from far away, the same everywhere: the sun. 32 bytes in the Lights function's `directional`. */
public struct KoralDirectionalLight {
    public float3 direction;    // the way it shines: the light's entity's forward
    public float unused0;
    public float3 color;        // in linear terms, times its intensity
    public float unused1;

    public __init(float3 direction, float3 color)
    {
        this.direction = KoralSafeNormalize(direction);
        this.color = color;
        unused0 = 0.0;
        unused1 = 0.0;
    }
    /** The way to it, from anywhere. */
    public float3 ToLight() { return -KoralSafeNormalize(direction); }
    public KoralArrival ArrivalAt(float3 position)
    {
        KoralArrival arrival;
        arrival.toLight = ToLight();
        arrival.distance = 1e30;
        arrival.radiance = color;
        return arrival;
    }
};

/** A light at a point, shining every way, out to its range. 32 bytes in the Lights function's `points`. */
public struct KoralPointLight {
    public float3 position;
    public float range;         // how far it reaches
    public float3 color;        // in linear terms, times its intensity
    public float unused;

    public __init(float3 position, float range, float3 color)
    {
        this.position = position;
        this.range = range;
        this.color = color;
        unused = 0.0;
    }
    /** Whether it reaches @p point at all. */
    public bool Reaches(float3 point) { const float3 d = point - position; return dot(d, d) < range * range; }
    /** Whether it reaches into the box from @p low to @p high: a cluster's, a tile's. */
    public bool Touches(float3 low, float3 high) { return SphereTouchesBox(position, range, low, high); }
    public KoralArrival ArrivalAt(float3 point)
    {
        KoralArrival arrival;
        const float3 between = position - point;
        arrival.distance = max(length(between), 1e-4);
        arrival.toLight = between / arrival.distance;
        arrival.radiance = color * KoralDistanceFalloff(arrival.distance, range);
        return arrival;
    }
};

/** A light at a point, shining one way within a cone, out to its range. 48 bytes in the Lights function's `spots`. */
public struct KoralSpotLight {
    public float3 position;
    public float range;         // how far it reaches
    public float3 direction;    // the way it shines: the light's entity's forward
    public float cosInner;      // the cosine of the angle within which it is at its brightest
    public float3 color;        // in linear terms, times its intensity
    public float cosOuter;      // the cosine of the angle past which it gives nothing

    public __init(float3 position, float range, float3 direction, float3 color, float cosInner, float cosOuter)
    {
        this.position = position;
        this.range = range;
        this.direction = KoralSafeNormalize(direction);
        this.color = color;
        this.cosInner = cosInner;
        this.cosOuter = cosOuter;
    }
    /** How much of it goes the way @p fromLight: all within its inner cone, none past its outer. */
    public float Cone(float3 fromLight) { return KoralSpotFalloff(dot(fromLight, KoralSafeNormalize(direction)), cosInner, cosOuter); }
    /** Whether it reaches @p point at all: near enough, and within its cone. */
    public bool Reaches(float3 point)
    {
        const float3 d = point - position;
        const float far = dot(d, d);
        return far < range * range && (far < 1e-8 || dot(d * rsqrt(far), KoralSafeNormalize(direction)) > cosOuter);
    }
    /** Whether it reaches into the box from @p low to @p high: as much as the sphere its range is, which holds its cone. */
    public bool Touches(float3 low, float3 high) { return SphereTouchesBox(position, range, low, high); }
    /** How wide its shadow's map is, in radians: twice its outer angle, and two degrees more, as a SpotShadows node makes it. */
    public float MapAngle() { return 2.0 * acos(clamp(cosOuter, -1.0, 1.0)) + 2.0 * 0.0174532925; }
    public KoralArrival ArrivalAt(float3 point)
    {
        KoralArrival arrival;
        const float3 between = position - point;
        arrival.distance = max(length(between), 1e-4);
        arrival.toLight = between / arrival.distance;
        arrival.radiance = color * KoralDistanceFalloff(arrival.distance, range) * Cone(-arrival.toLight);
        return arrival;
    }
};

// ---- the light from all round -------------------------------------------------------------------------------------

/** The light that comes from all round, for a shading to take what it needs of. */
public interface IKoralEnvironment {
    /** What a surface facing @p normal is given of it, spread over the half of everything it sees. */
    float3 Diffuse(float3 normal);
    /** What is seen along @p direction, as blurred as a surface so rough sees it. */
    float3 Specular(float3 direction, float roughness);
};

/** Nothing round it: only the lights light it. */
public struct KoralNoEnvironment : IKoralEnvironment {
    public __init() {}
    public float3 Diffuse(float3 normal) { return 0.0; }
    public float3 Specular(float3 direction, float roughness) { return 0.0; }
};

/** The same light every way. */
public struct KoralUniformEnvironment : IKoralEnvironment {
    float3 light;
    public __init(float3 light) { this.light = light; }
    public float3 Diffuse(float3 normal) { return light; }
    public float3 Specular(float3 direction, float roughness) { return light; }
};

/**
 * The sky's light from all round: three colours — the zenith's above, the horizon's round it, the ground's below — times
 * how much light it gives. 48 bytes, as a ConstantBuffer holds it of the scene's Sky.
 */
public struct KoralAmbientLight : IKoralEnvironment {
    public float3 zenith;
    public float strength;      // how much light it gives all round: the Sky's `ambient`
    public float3 horizon;
    public float unused0;
    public float3 ground;
    public float unused1;

    public __init(float3 zenith, float3 horizon, float3 ground, float strength)
    {
        this.zenith = zenith;
        this.horizon = horizon;
        this.ground = ground;
        this.strength = strength;
        unused0 = 0.0;
        unused1 = 0.0;
    }
    /** What is seen along @p direction, sharp: the ground below the horizon, the zenith above it. Not times its strength. */
    public float3 Along(float3 direction)
    {
        const float y = direction.y;
        return y >= 0.0 ? lerp(horizon, zenith, pow(saturate(y), 0.45)) : lerp(horizon, ground, pow(saturate(-y * 4.0), 0.6));
    }
    public float3 Diffuse(float3 normal)
    {
        const float3 sky = lerp(horizon, zenith, saturate(normal.y));
        return lerp(ground, sky, smoothstep(0.0, 1.0, normal.y * 0.5 + 0.5)) * strength;
    }
    public float3 Specular(float3 direction, float roughness) { return lerp(Along(direction) * strength, Diffuse(direction), saturate(roughness)); }
};

// ---- where the lights are kept ------------------------------------------------------------------------------------

/** The lights a point is lit by, wherever they are kept: each kind by itself. */
public interface IKoralLights {
    uint DirectionalCount();
    KoralDirectionalLight Directional(uint index);
    uint PointCount();
    KoralPointLight Point(uint index);
    uint SpotCount();
    KoralSpotLight Spot(uint index);
};

/** As the Lights function gives them: its three buffers, and its `counts` (x directional, y point, z spot lights). */
public struct KoralLightBuffers : IKoralLights {
    StructuredBuffer<KoralDirectionalLight> directional;
    StructuredBuffer<KoralPointLight> points;
    StructuredBuffer<KoralSpotLight> spots;
    uint3 counts;
    public __init(StructuredBuffer<KoralDirectionalLight> directional, StructuredBuffer<KoralPointLight> points, StructuredBuffer<KoralSpotLight> spots, uint3 counts)
    {
        this.directional = directional;
        this.points = points;
        this.spots = spots;
        this.counts = counts;
    }
    public uint DirectionalCount() { return counts.x; }
    public KoralDirectionalLight Directional(uint index) { return directional[index]; }
    public uint PointCount() { return counts.y; }
    public KoralPointLight Point(uint index) { return points[index]; }
    public uint SpotCount() { return counts.z; }
    public KoralSpotLight Spot(uint index) { return spots[index]; }
};

/** None: only what is around lights it. */
public struct KoralNoLights : IKoralLights {
    public __init() {}
    public uint DirectionalCount() { return 0; }
    public KoralDirectionalLight Directional(uint index) { return KoralDirectionalLight(float3(0.0, -1.0, 0.0), 0.0); }
    public uint PointCount() { return 0; }
    public KoralPointLight Point(uint index) { return KoralPointLight(0.0, 0.0, 0.0); }
    public uint SpotCount() { return 0; }
    public KoralSpotLight Spot(uint index) { return KoralSpotLight(0.0, 0.0, float3(0.0, -1.0, 0.0), 0.0, 1.0, 1.0); }
};

/** One directional light by itself: a sun of a shader's own, or one shining the way the eye looks (what faces it is brightest). */
public struct KoralSingleLight : IKoralLights {
    KoralDirectionalLight light;
    public __init(KoralDirectionalLight light) { this.light = light; }
    public uint DirectionalCount() { return 1; }
    public KoralDirectionalLight Directional(uint index) { return light; }
    public uint PointCount() { return 0; }
    public KoralPointLight Point(uint index) { return KoralPointLight(0.0, 0.0, 0.0); }
    public uint SpotCount() { return 0; }
    public KoralSpotLight Spot(uint index) { return KoralSpotLight(0.0, 0.0, float3(0.0, -1.0, 0.0), 0.0, 1.0, 1.0); }
};

/** The editor's lights for a thing by itself: a key light over the shoulder and a dim one from the other side. */
public struct KoralStudioLights : IKoralLights {
    public __init() {}
    public uint DirectionalCount() { return 2; }
    public KoralDirectionalLight Directional(uint index)
    {
        return index == 0 ? KoralDirectionalLight(float3(-0.45, -0.8, -0.4), float3(3.0, 2.9, 2.75))
                          : KoralDirectionalLight(float3(0.7, 0.1, 0.6), float3(0.5, 0.55, 0.65));
    }
    public uint PointCount() { return 0; }
    public KoralPointLight Point(uint index) { return KoralPointLight(0.0, 0.0, 0.0); }
    public uint SpotCount() { return 0; }
    public KoralSpotLight Spot(uint index) { return KoralSpotLight(0.0, 0.0, float3(0.0, -1.0, 0.0), 0.0, 1.0, 1.0); }
};

// ---- shadows ----------------------------------------------------------------------------------------------------

/**
 * How much of a light its shadows let reach a point: 1 all of it, 0 none. Each kind by itself, @p index which of its kind
 * the light is. koral_shadows has the engine's.
 */
public interface IKoralShadows {
    float DirectionalVisibility(uint index, KoralDirectionalLight light, KoralArrival arrival, float3 position, float3 normal);
    float PointVisibility(uint index, KoralPointLight light, KoralArrival arrival, float3 position, float3 normal);
    float SpotVisibility(uint index, KoralSpotLight light, KoralArrival arrival, float3 position, float3 normal);
};

/** No shadows: every light reaches everywhere. */
public struct KoralNoShadows : IKoralShadows {
    public __init() {}
    public float DirectionalVisibility(uint index, KoralDirectionalLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
    public float PointVisibility(uint index, KoralPointLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
    public float SpotVisibility(uint index, KoralSpotLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
};
