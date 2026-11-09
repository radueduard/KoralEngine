// The engine's base library: lights. The scene's, as the engine's Lights function lays them out — every
// DirectionalLight, then every PointLight, then every SpotLight, one after another in one buffer — and what of a light
// reaches a point, and how much of it its shadow lets through.
//
//     import koral_lights;
//     StructuredBuffer<KoralLight> lights;     // the Lights function's `lights`; its `count` how many there are
//
//     const KoralArrival arrival = KoralArrivalAt(lights[i], position);
//
// A shading lights itself through koral_shading's KoralLit, which takes any IKoralLights and any IKoralShadows.

module koral_lights;

import koral_math;

public static const uint KoralLightDirectional = 0;
public static const uint KoralLightPoint = 1;
public static const uint KoralLightSpot = 2;

/** A light, as the Lights function writes it: 64 bytes in a buffer. */
public struct KoralLight {
    public float3 position;     // where it is: nothing for a directional light
    public float range;         // how far it reaches: nothing for a directional light
    public float3 direction;    // the way it shines: the light's entity's forward
    public uint kind;           // KoralLightDirectional, KoralLightPoint or KoralLightSpot
    public float3 color;        // its colour in linear terms, times its intensity
    public float cosInner;      // a spot's: the cosine of the angle within which it is at its brightest
    public float cosOuter;      // the cosine of the angle past which it gives nothing
    public uint kindIndex;      // which of its kind it is, from 0: the n-th point light — what its shadow is found by
};

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

public KoralArrival KoralArrivalAt(KoralLight light, float3 position)
{
    KoralArrival arrival;
    if (light.kind == KoralLightDirectional) {
        arrival.toLight = KoralSafeNormalize(-light.direction);
        arrival.distance = 1e30;
        arrival.radiance = light.color;
        return arrival;
    }
    const float3 between = light.position - position;
    arrival.distance = max(length(between), 1e-4);
    arrival.toLight = between / arrival.distance;
    arrival.radiance = light.color * KoralDistanceFalloff(arrival.distance, light.range);
    if (light.kind == KoralLightSpot)
        arrival.radiance *= KoralSpotFalloff(dot(-arrival.toLight, KoralSafeNormalize(light.direction)), light.cosInner, light.cosOuter);
    return arrival;
}

// ---- where the lights are kept ----------------------------------------------------------------------------------

/** The lights a point is lit by, wherever they are kept. */
public interface IKoralLights {
    uint Count();
    KoralLight At(uint index);
};

/** As the Lights function gives them: its `lights` and its `count`. */
public struct KoralLightBuffer : IKoralLights {
    StructuredBuffer<KoralLight> lights;
    uint count;
    public __init(StructuredBuffer<KoralLight> lights, uint count) { this.lights = lights; this.count = count; }
    public uint Count() { return count; }
    public KoralLight At(uint index) { return lights[index]; }
};

/** A directional light of a colour shining along a way: what a light of a shader's own is made from. */
public KoralLight KoralDirectionalLight(float3 direction, float3 color)
{
    KoralLight light;
    light.position = 0.0;
    light.range = 0.0;
    light.direction = KoralSafeNormalize(direction);
    light.kind = KoralLightDirectional;
    light.color = color;
    light.cosInner = 0.0;
    light.cosOuter = 0.0;
    light.kindIndex = 0;
    return light;
}

/** The editor's lights for a thing by itself: a key light over the shoulder and a dim one from the other side. */
public struct KoralStudioLights : IKoralLights {
    public __init() {}
    public uint Count() { return 2; }
    public KoralLight At(uint index)
    {
        return index == 0 ? KoralDirectionalLight(float3(-0.45, -0.8, -0.4), float3(3.0, 2.9, 2.75))
                          : KoralDirectionalLight(float3(0.7, 0.1, 0.6), float3(0.5, 0.55, 0.65));
    }
};

/** One light by itself: a sun of a shader's own, or one shining the way the eye looks (what faces the eye is brightest). */
public struct KoralSingleLight : IKoralLights {
    KoralLight light;
    public __init(KoralLight light) { this.light = light; }
    public uint Count() { return 1; }
    public KoralLight At(uint index) { return light; }
};

// ---- shadows ----------------------------------------------------------------------------------------------------

/** How much of a light its shadows let reach a point: 1 all of it, 0 none. koral_shadows has the engine's. */
public interface IKoralShadows {
    float Visibility(KoralLight light, KoralArrival arrival, float3 position, float3 normal);
};

/** No shadows: every light reaches everywhere. */
public struct KoralNoShadows : IKoralShadows {
    public __init() {}
    public float Visibility(KoralLight light, KoralArrival arrival, float3 position, float3 normal) { return 1.0; }
};
