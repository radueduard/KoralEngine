// What a material is, and what a shading function has to answer: the two interfaces a shader, the engine's material
// preview and the small pictures of materials all go through.
//
//     import PbrMaterialShading;      // a kind's shading module: `<Kind>Shading`
//
//     const KoralSurface surface = KoralSurfaceAt(world, normal, uv, smooth);
//     const float3 lit = KoralLit(material, surface, camera, KoralLightBuffer(lights, count));
//
// A material kind is generated as a struct of its fields (materials.h). Its shading module extends that struct with
// IKoralMaterial, which says how the fields are read at a point of a surface, and gives back an IKoralShading: what is
// there, pictures read, and how it answers light. A kind `Toon` of a project's is shaded by a module `ToonShading` of the
// project's shaders; with that, its materials are drawn in the editor's preview as the engine's own are:
//
//     module ToonShading;
//     __exported import koral_shading;
//     __exported import Toon;
//
//     public struct ToonLit : IKoralShading {
//         public float3 normal, colour;
//         public float3 Reflected(float3 toEye, float3 toLight, float3 light) { return colour * light * step(0.5, dot(normal, toLight)); }
//         public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye) { return colour * environment.Diffuse(normal); }
//         public float3 Emitted() { return 0.0; }
//         public float Opacity() { return 1.0; }
//     };
//     public extension Toon : IKoralMaterial {
//         public typealias Shading = ToonLit;
//         public ToonLit Read(KoralSurface at) { ToonLit lit; lit.normal = at.normal; lit.colour = KoralLinear(tint.rgb); return lit; }
//     };
//
// A kind can give back an engine's shading instead (Shading = KoralPbr) and need no function of its own.

module koral_shading;

__exported import koral_lights;

public static const float KoralPi = 3.14159265358979;

/** A point of a surface, seen from nowhere in particular: where it is, which way it faces, and how to read a picture there. */
public struct KoralSurface {
    public float3 position;     // in the world
    public float3 normal;       // a unit long, facing out
    public float3 tangent;      // the way u grows
    public float3 bitangent;    // the way v grows
    public float2 uv;
    public SamplerState sampler;
};

/** A surface's point from what a fragment has: its directions along the surface worked out from how its uv changes. */
public KoralSurface KoralSurfaceAt(float3 position, float3 normal, float2 uv, SamplerState sampler)
{
    KoralSurface surface;
    surface.position = position;
    surface.normal = normalize(normal);
    surface.uv = uv;
    surface.sampler = sampler;
    const float3 dx = ddx(position), dy = ddy(position);
    const float2 du = ddx(uv), dv = ddy(uv);
    float3 tangent = dx * dv.y - dy * du.y;
    if (dot(tangent, tangent) < 1e-20) tangent = abs(surface.normal.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    surface.tangent = normalize(tangent - surface.normal * dot(surface.normal, tangent));
    surface.bitangent = cross(surface.normal, surface.tangent) * (du.x * dv.y - dv.x * du.y < 0.0 ? -1.0 : 1.0);
    return surface;
}

/** Which way a surface faces with its normal map's texel, as a normal map has it: 0.5, 0.5, 1 is straight out. */
public float3 KoralMappedNormal(KoralSurface surface, float3 texel)
{
    const float3 turned = texel * 2.0 - 1.0;
    return normalize(surface.tangent * turned.x + surface.bitangent * turned.y + surface.normal * turned.z);
}

/** A colour as it is picked, as light: a material's fields and pictures are the former, a shading function works in the latter. */
public float3 KoralLinear(float3 picked) { return pow(max(picked, 0.0), 2.2); }

/** Where the camera is. */
public struct KoralCamera {
    public float3 position;
    public __init(float3 position) { this.position = position; }
};

// ---- lights ---------------------------------------------------------------------------------------------------------

/** What of a light reaches a point: from where, and how much of it. */
public struct KoralArrival {
    public float3 toLight;
    public float3 radiance;
};

public KoralArrival KoralArrivalAt(KoralLight light, float3 position)
{
    KoralArrival arrival;
    if (light.kind == KoralLightDirectional) {
        arrival.toLight = normalize(-light.direction);
        arrival.radiance = light.color;
        return arrival;
    }
    const float3 between = light.position - position;
    const float distance = max(length(between), 1e-4);
    arrival.toLight = between / distance;
    // Less by the square of the way, and nothing at its range.
    const float fade = saturate(1.0 - pow(distance / max(light.range, 1e-4), 4.0));
    arrival.radiance = light.color * fade * fade / (distance * distance + 1.0);
    if (light.kind == KoralLightSpot)
        arrival.radiance *= smoothstep(light.cosOuter, light.cosInner, dot(-arrival.toLight, normalize(light.direction)));
    return arrival;
}

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

// ---- what is around ---------------------------------------------------------------------------------------------------

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

/** A cube map, its mip levels blurred more and more. */
public struct KoralCubeEnvironment : IKoralEnvironment {
    TextureCube<float4> cube;
    SamplerState sampler;
    float mips;
    public __init(TextureCube<float4> cube, SamplerState sampler, float mips) { this.cube = cube; this.sampler = sampler; this.mips = mips; }
    public float3 Diffuse(float3 normal) { return cube.SampleLevel(sampler, normal, max(mips - 1.0, 0.0)).rgb; }
    public float3 Specular(float3 direction, float roughness) { return cube.SampleLevel(sampler, direction, sqrt(saturate(roughness)) * max(mips - 1.0, 0.0)).rgb; }
};

// ---- the interfaces ---------------------------------------------------------------------------------------------------

/**
 * What a shading function is: what is at a point, read, and how it answers light. What it needs to do that it holds,
 * given by the material that made it; what is outside it comes in by these.
 */
public interface IKoralShading {
    /** The light it sends towards the eye of @p light (radiance) arriving from @p toLight: its surface's angle included. */
    float3 Reflected(float3 toEye, float3 toLight, float3 light);
    /** The light it sends towards the eye of what is around it. */
    float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye);
    /** The light it gives off itself. */
    float3 Emitted();
    /** How much of what is behind it it hides: 1 opaque, 0 not there at all. */
    float Opacity();
};

/** What a material is: fields that, read at a point of a surface, are a shading. */
public interface IKoralMaterial {
    associatedtype Shading : IKoralShading;
    Shading Read(KoralSurface at);
};

// ---- the lit colour ---------------------------------------------------------------------------------------------------

/** The light a shading sends to the camera: of each light, of what is around, and its own. Not tone mapped. */
public float3 KoralLit<S : IKoralShading, L : IKoralLights, E : IKoralEnvironment>(S shading, KoralSurface surface, KoralCamera camera, L lights, E environment)
{
    const float3 toEye = normalize(camera.position - surface.position);
    float3 lit = shading.Emitted() + shading.Ambient(environment, toEye);
    for (uint i = 0; i < lights.Count(); ++i) {
        const KoralArrival arrival = KoralArrivalAt(lights.At(i), surface.position);
        lit += shading.Reflected(toEye, arrival.toLight, arrival.radiance);
    }
    return lit;
}

/** The same of a material, read where it is. */
public float3 KoralLit<M : IKoralMaterial, L : IKoralLights, E : IKoralEnvironment>(M material, KoralSurface surface, KoralCamera camera, L lights, E environment)
{
    return KoralLit(material.Read(surface), surface, camera, lights, environment);
}

/** The same with nothing around it. */
public float3 KoralLit<M : IKoralMaterial, L : IKoralLights>(M material, KoralSurface surface, KoralCamera camera, L lights)
{
    return KoralLit(material.Read(surface), surface, camera, lights, KoralNoEnvironment());
}
