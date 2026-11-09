// The engine's base library: what a material is, and what a shading has to answer — the two interfaces every shader,
// the engine's material previews and the small pictures of materials go through — and the lit colour of either.
//
//     import PbrMaterialShading;      // a kind's shading module: `<Kind>Shading`
//
//     const KoralSurface surface = KoralSurfaceAt(world, normal, uv, smooth);
//     const float3 lit = KoralLit(material, surface, camera, KoralLightBuffers(directional, points, spots, counts), ambient, shadows);
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
//         public ToonLit Read(KoralSurface at) { ToonLit lit; lit.normal = at.normal; lit.colour = KoralLinearFromGamma22(tint.rgb); return lit; }
//     };
//
// A kind can give back one of the engine's shadings instead (Shading = KoralPbr: koral_shadings) and need no function
// of its own.

module koral_shading;

__exported import koral_lights;
__exported import koral_camera;
__exported import koral_math;
__exported import koral_color;

// ---- a point of a surface -------------------------------------------------------------------------------------------

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
    surface.normal = KoralSafeNormalize(normal);
    surface.uv = uv;
    surface.sampler = sampler;
    const float3 dx = ddx(position), dy = ddy(position);
    const float2 du = ddx(uv), dv = ddy(uv);
    float3 tangent = dx * dv.y - dy * du.y;
    if (dot(tangent, tangent) < 1e-20) tangent = abs(surface.normal.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    surface.tangent = KoralSafeNormalize(tangent - surface.normal * dot(surface.normal, tangent));
    surface.bitangent = cross(surface.normal, surface.tangent) * (du.x * dv.y - dv.x * du.y < 0.0 ? -1.0 : 1.0);
    return surface;
}

/**
 * A surface's point from its mesh's own tangent (xyz, and w the way the bitangent goes, ±1): where there is no fragment
 * to work it out from — a ray's hit, a compute pass.
 */
public KoralSurface KoralSurfaceOf(float3 position, float3 normal, float4 tangent, float2 uv, SamplerState sampler)
{
    KoralSurface surface;
    surface.position = position;
    surface.normal = KoralSafeNormalize(normal);
    surface.uv = uv;
    surface.sampler = sampler;
    surface.tangent = KoralSafeNormalize(tangent.xyz - surface.normal * dot(surface.normal, tangent.xyz),
                                         abs(surface.normal.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0));
    surface.bitangent = cross(surface.normal, surface.tangent) * (tangent.w < 0.0 ? -1.0 : 1.0);
    return surface;
}

/** Which way a surface faces with its normal map's texel, as a normal map has it: 0.5, 0.5, 1 is straight out. */
public float3 KoralMappedNormal(KoralSurface surface, float3 texel)
{
    const float3 turned = texel * 2.0 - 1.0;
    return KoralSafeNormalize(surface.tangent * turned.x + surface.bitangent * turned.y + surface.normal * turned.z, surface.normal);
}

/** A normal map's texel of two channels (BC5, RG): the third worked out, as a texel KoralMappedNormal takes. */
public float3 KoralNormalTexelOf(float2 rg)
{
    const float2 xy = rg * 2.0 - 1.0;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy)))) * 0.5 + 0.5;
}

/** One normal map's turn over another's (both as texels): reoriented, the detail following the base (Barré-Brisebois and Hill). */
public float3 KoralBlendNormalTexels(float3 base, float3 detail)
{
    const float3 t = base * float3(2.0, 2.0, 2.0) + float3(-1.0, -1.0, 0.0);
    const float3 u = detail * float3(-2.0, -2.0, 2.0) + float3(1.0, 1.0, -1.0);
    return KoralSafeNormalize(t * dot(t, u) - u * t.z) * 0.5 + 0.5;
}

// ---- what is around --------------------------------------------------------------------------------------------------
// IKoralEnvironment, nothing round, the same every way and the sky's KoralAmbientLight are koral_lights'.

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

/**
 * The light a shading at @p position, facing @p normal (its surface's, for its shadows), sends to the camera: of each
 * light of each kind, as much as its shadows let through, of what is around, and its own. Not tone mapped.
 */
public float3 KoralLit<S : IKoralShading, L : IKoralLights, E : IKoralEnvironment, H : IKoralShadows>(S shading, float3 position, float3 normal, KoralCamera camera, L lights, E environment, H shadows)
{
    const float3 toEye = camera.ToEye(position);
    float3 lit = shading.Emitted() + shading.Ambient(environment, toEye);
    for (uint i = 0; i < lights.DirectionalCount(); ++i) {
        const KoralDirectionalLight light = lights.Directional(i);
        const KoralArrival arrival = light.ArrivalAt(position);
        const float visible = shadows.DirectionalVisibility(i, light, arrival, position, normal);
        if (visible > 0.0) lit += shading.Reflected(toEye, arrival.toLight, arrival.radiance * visible);
    }
    for (uint i = 0; i < lights.PointCount(); ++i) {
        const KoralPointLight light = lights.Point(i);
        if (!light.Reaches(position)) continue;
        const KoralArrival arrival = light.ArrivalAt(position);
        const float visible = shadows.PointVisibility(i, light, arrival, position, normal);
        if (visible > 0.0) lit += shading.Reflected(toEye, arrival.toLight, arrival.radiance * visible);
    }
    for (uint i = 0; i < lights.SpotCount(); ++i) {
        const KoralSpotLight light = lights.Spot(i);
        if (!light.Reaches(position)) continue;
        const KoralArrival arrival = light.ArrivalAt(position);
        const float visible = shadows.SpotVisibility(i, light, arrival, position, normal);
        if (visible > 0.0) lit += shading.Reflected(toEye, arrival.toLight, arrival.radiance * visible);
    }
    return lit;
}

public float3 KoralLit<S : IKoralShading, L : IKoralLights, E : IKoralEnvironment>(S shading, float3 position, float3 normal, KoralCamera camera, L lights, E environment)
{
    return KoralLit(shading, position, normal, camera, lights, environment, KoralNoShadows());
}

/** The same at a point of a surface. */
public float3 KoralLit<S : IKoralShading, L : IKoralLights, E : IKoralEnvironment, H : IKoralShadows>(S shading, KoralSurface surface, KoralCamera camera, L lights, E environment, H shadows)
{
    return KoralLit(shading, surface.position, surface.normal, camera, lights, environment, shadows);
}

/** The same, with no shadows. */
public float3 KoralLit<S : IKoralShading, L : IKoralLights, E : IKoralEnvironment>(S shading, KoralSurface surface, KoralCamera camera, L lights, E environment)
{
    return KoralLit(shading, surface.position, surface.normal, camera, lights, environment, KoralNoShadows());
}

/** The same of a material, read where it is. */
public float3 KoralLit<M : IKoralMaterial, L : IKoralLights, E : IKoralEnvironment, H : IKoralShadows>(M material, KoralSurface surface, KoralCamera camera, L lights, E environment, H shadows)
{
    return KoralLit(material.Read(surface), surface, camera, lights, environment, shadows);
}

public float3 KoralLit<M : IKoralMaterial, L : IKoralLights, E : IKoralEnvironment>(M material, KoralSurface surface, KoralCamera camera, L lights, E environment)
{
    return KoralLit(material.Read(surface), surface, camera, lights, environment, KoralNoShadows());
}

/** The same with nothing around it. */
public float3 KoralLit<M : IKoralMaterial, L : IKoralLights>(M material, KoralSurface surface, KoralCamera camera, L lights)
{
    return KoralLit(material.Read(surface), surface, camera, lights, KoralNoEnvironment(), KoralNoShadows());
}
