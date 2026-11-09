// The engine's shadings: what a material's fields are read into, of any kind — the engine's PbrMaterial and
// BlinnPhongMaterial, and a project's kind that describes the same things (its Shading = KoralPbr).
//
//     import koral_shadings;
//
//     KoralPbr pbr;                                       // GGX and Lambert: KoralPbrOf<KoralGgxLobe, KoralLambertLobe>
//     KoralPbrOf<KoralGgxLobe, KoralBurleyLobe> cloth;    // or any lobes of koral_brdf's interfaces
//
// Each is an IKoralShading (koral_shading), lit by KoralLit.

module koral_shadings;

__exported import koral_shading;
__exported import koral_brdf;

/**
 * A metal-rough surface, of a specular lobe @p S and a diffuse lobe @p D: a colour, how metal, how rough. What a rough
 * metal loses to light bouncing between its facets is given back.
 */
public struct KoralPbrOf<S : IKoralSpecularLobe, D : IKoralDiffuseLobe> : IKoralShading {
    public float3 normal;
    public float3 baseColor;
    public float metallic;
    public float roughness;
    public float occlusion;
    public float3 emission;
    public float opacity;

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const KoralAngles a = KoralAnglesOf(normal, toEye, toLight);
        if (a.nl <= 0.0) return 0.0;
        const float3 f0 = KoralReflectanceAtNormal(baseColor, metallic);
        const float3 mirrored = S.Reflected(a, f0, roughness) * KoralEnergyCompensation(f0, roughness, a.nv);
        const float3 taken = (1.0 - KoralFresnelSchlick(f0, a.vh)) * (1.0 - metallic) * baseColor * D.Reflected(a, roughness);
        return (taken + mirrored) * light * a.nl;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        const float nv = saturate(dot(normal, toEye));
        const float3 f0 = KoralReflectanceAtNormal(baseColor, metallic);
        const float3 shine = KoralEnvironmentReflectance(f0, roughness, nv) * KoralEnergyCompensation(f0, roughness, nv);
        const float3 taken = (1.0 - metallic) * baseColor * environment.Diffuse(normal);
        return (taken + shine * environment.Specular(reflect(-toEye, normal), roughness)) * occlusion;
    }

    public float3 Emitted() { return emission; }
    public float Opacity() { return opacity; }
};

/** The engine's physically based shading: GGX facets over Lambert. */
public typealias KoralPbr = KoralPbrOf<KoralGgxLobe, KoralLambertLobe>;

/** A matte surface of a colour: what a shape by itself is shown as. */
public KoralPbr KoralMatte(float3 normal, float3 color)
{
    KoralPbr matte;
    matte.normal = normal;
    matte.baseColor = color;
    matte.metallic = 0.0;
    matte.roughness = 1.0;
    matte.occlusion = 1.0;
    matte.emission = 0.0;
    matte.opacity = 1.0;
    return matte;
}

/**
 * The classic way: a diffuse colour, a specular colour and how tight a highlight is (Blinn and Phong's, normalised so a
 * tighter highlight is brighter rather than the surface shinier).
 */
public struct KoralBlinnPhong : IKoralShading {
    public float3 normal;
    public float3 diffuse;
    public float3 specular;
    public float shininess;
    public float3 emission;
    public float opacity;

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const KoralAngles a = KoralAnglesOf(normal, toEye, toLight);
        if (a.nl <= 0.0) return 0.0;
        const float roughness = KoralRoughnessOfShininess(shininess);
        return (diffuse * KoralLambertLobe.Reflected(a, roughness) + KoralBlinnPhongLobe.Reflected(a, specular, roughness)) * light * a.nl;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        // The roughness whose highlight is as wide. What is around is mirrored as a dielectric does, a little face on and
        // all at a grazing angle, and less the rougher it is: a specular colour is no mirror's, or a plastic is chrome.
        const float roughness = KoralRoughnessOfShininess(shininess);
        const float gloss = 1.0 - roughness * roughness;
        const float3 mirrored = specular * KoralFresnelSchlick(0.16, 1.0, saturate(dot(normal, toEye))) * gloss * gloss;
        return diffuse * environment.Diffuse(normal) + mirrored * environment.Specular(reflect(-toEye, normal), roughness);
    }

    public float3 Emitted() { return emission; }
    public float Opacity() { return opacity; }
};

/** Light of its own and nothing reflected: a sky, a screen, what is shown as it is. */
public struct KoralUnlit : IKoralShading {
    public float3 color;
    public float opacity;
    public __init(float3 color, float opacity = 1.0) { this.color = color; this.opacity = opacity; }
    public float3 Reflected(float3 toEye, float3 toLight, float3 light) { return 0.0; }
    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye) { return 0.0; }
    public float3 Emitted() { return color; }
    public float Opacity() { return opacity; }
};
