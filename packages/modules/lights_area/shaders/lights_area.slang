// The lights-area module: lights with a size — a sphere (a bulb), a tube (a fluorescent tube, a neon sign), a rectangle
// (a window, a panel) — lit from their representative point (Karis's): the point of the light nearest the way the
// surface mirrors, its highlight widened by as much as the light is wide and the energy kept. And a light's IES profile
// read from a picture of it. What each gives is an arrival: shading.Reflected(toEye, arrival.toLight, arrival.radiance).
//
//     import lights_area;
//
//     const KoralArrival a = KoralSphereLightArrival(bulb, position, normal, toEye, roughness);
//     lit += shading.Reflected(toEye, a.toLight, a.radiance);

module lights_area;

__exported import koral_lights;
import koral_math;
import koral_brdf;

/** A light with a size: a sphere of `radius`, made a tube `length` long along `axis`, or a rectangle `size` wide along `axis` and `up`. */
public struct KoralAreaLight {
    public float3 position;     // its middle
    public float radius;        // a sphere's or a tube's
    public float3 axis;         // a tube's way, or a rectangle's width's; of unit length
    public float length;        // a tube's
    public float3 up;           // a rectangle's height's way; of unit length
    public float2 size;         // a rectangle's width and height
    public float3 color;        // linear, times its intensity
    public float range;         // where it ends, as a point light's
};

/** How much brighter a highlight must be made, widened from @p roughness by a light @p radius wide @p distance away (Karis). */
public float KoralSphereNormalization(float roughness, float radius, float distance)
{
    const float alpha = KoralAlphaOf(roughness);
    const float widened = saturate(alpha + radius / (2.0 * max(distance, 1e-4)));
    return KoralSquare(alpha / widened);
}

KoralArrival Arrive(float3 toPoint, float3 color, float range, float normalization)
{
    KoralArrival arrival;
    arrival.distance = max(length(toPoint), 1e-4);
    arrival.toLight = toPoint / arrival.distance;
    arrival.radiance = color * KoralDistanceFalloff(arrival.distance, range) * normalization;
    return arrival;
}

/** A sphere light reaching a point: from the point of it nearest the way the surface mirrors the eye. */
public KoralArrival KoralSphereLightArrival(KoralAreaLight light, float3 position, float3 normal, float3 toEye, float roughness)
{
    const float3 mirrored = reflect(-toEye, normal);
    const float3 toCentre = light.position - position;
    const float3 nearestOnRay = dot(toCentre, mirrored) * mirrored - toCentre;
    const float3 toPoint = toCentre + nearestOnRay * saturate(light.radius / max(length(nearestOnRay), 1e-6));
    return Arrive(toPoint, light.color, light.range, KoralSphereNormalization(roughness, light.radius, length(toCentre)));
}

/** A tube light: the nearest point of its line to the mirrored way, then of the sphere about that point. */
public KoralArrival KoralTubeLightArrival(KoralAreaLight light, float3 position, float3 normal, float3 toEye, float roughness)
{
    const float3 mirrored = reflect(-toEye, normal);
    const float3 l0 = light.position - light.axis * light.length * 0.5 - position, l1 = light.position + light.axis * light.length * 0.5 - position;
    const float3 ld = l1 - l0;
    const float rl = dot(mirrored, ld);
    const float t = saturate((dot(mirrored, l0) * rl - dot(l0, ld)) / max(dot(ld, ld) - rl * rl, 1e-6));
    KoralAreaLight sphere = light;
    sphere.position = position + l0 + ld * t;
    KoralArrival arrival = KoralSphereLightArrival(sphere, position, normal, toEye, roughness);
    // A line is brighter than its sphere: as much more as it is long beside it.
    arrival.radiance *= 1.0 + light.length / max(2.0 * light.radius + light.length, 1e-4);
    return arrival;
}

/** A rectangle light: the point of its plane the mirrored way meets, kept within it; facing one way, as a window does. */
public KoralArrival KoralRectLightArrival(KoralAreaLight light, float3 position, float3 normal, float3 toEye, float roughness)
{
    const float3 facing = cross(light.axis, light.up);
    const float3 mirrored = reflect(-toEye, normal);
    const float3 toCentre = light.position - position;
    const float along = dot(mirrored, facing);
    float3 onPlane = abs(along) > 1e-4 ? mirrored * (dot(toCentre, facing) / along) - toCentre : -toCentre;
    const float2 within = clamp(float2(dot(onPlane, light.axis), dot(onPlane, light.up)), -light.size * 0.5, light.size * 0.5);
    const float3 toPoint = toCentre + light.axis * within.x + light.up * within.y;
    KoralArrival arrival = Arrive(toPoint, light.color, light.range, KoralSphereNormalization(roughness, length(light.size) * 0.5, length(toCentre)));
    arrival.radiance *= saturate(dot(-arrival.toLight, facing));     // lit from its face, not its back
    return arrival;
}

/**
 * How much of a light's intensity goes towards @p fromLight (in the light's own space: -z down its middle) by its IES
 * profile, as a picture of it: vertical angle (0 straight down to π straight up) across, horizontal angle (0 to 2π) down.
 */
public float KoralIesProfile(Texture2D<float> profile, SamplerState sampler, float3 fromLight)
{
    const float3 d = KoralSafeNormalize(fromLight);
    const float vertical = acos(clamp(-d.z, -1.0, 1.0)) * KoralInvPi;
    const float horizontal = (atan2(d.y, d.x) + KoralPi) / KoralTau;
    return profile.SampleLevel(sampler, float2(vertical, horizontal), 0.0);
}
