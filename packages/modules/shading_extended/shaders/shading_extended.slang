// The shading-extended module: surfaces the engine's KoralPbr does not cover — lobes for koral_shadings' KoralPbrOf,
// and shadings of their own (IKoralShading) for what has more to it than one layer.
//
//     import shading_extended;
//
//     KoralPbrOf<KoralGgxLobe, KoralOrenNayarLobe> plaster;      // a rough matte surface, flatter than Lambert's
//     KoralClearCoatPbr paint;                                    // a base under a clear varnish
//     KoralAnisotropicPbr brushed;                                // brushed metal: its highlight drawn out along a way
//     KoralClothPbr velvet;                                       // a sheen at grazing angles
//     KoralTranslucentPbr leaf;                                   // light through a thin surface, and scattered in it

module shading_extended;

__exported import koral_shadings;
import koral_math;

// ---- lobes ---------------------------------------------------------------------------------------------------------

/** Oren and Nayar's diffuse, in Fujii's improved form: a rough matte surface flatter than Lambert's, as clay and plaster are. */
public struct KoralOrenNayarLobe : IKoralDiffuseLobe {
    public static float Reflected(KoralAngles a, float roughness)
    {
        const float s = a.lv - a.nl * a.nv;
        const float t = s > 0.0 ? max(a.nl, a.nv) : 1.0;
        const float sigma = roughness * roughness;
        const float A = 1.0 / (KoralPi + (KoralPi / 2.0 - 2.0 / 3.0) * sigma), B = sigma * A;
        return A + B * s / t;
    }
};

/** Charlie's sheen distribution (Estevez and Kulla): fibres standing up off a cloth, bright at grazing angles. */
public float KoralCharlieDistribution(float nh, float roughness)
{
    const float inverse = 1.0 / max(roughness * roughness, 1e-3);
    const float sin2 = max(1.0 - nh * nh, 0.0078125);
    return (2.0 + inverse) * pow(sin2, inverse * 0.5) / (2.0 * KoralPi);
}

/** Neubelt's visibility for a sheen: simpler than Smith's, as cloth needs. */
public float KoralNeubeltVisibility(float nv, float nl) { return 1.0 / max(4.0 * (nl + nv - nl * nv), 1e-4); }

// ---- a base under a clear coat -------------------------------------------------------------------------------------

/**
 * A metal-rough base (GGX over Lambert) under a clear coat — a varnish, a car's paint, wet stone: what the coat mirrors
 * (a dielectric's 0.04, of its own roughness) is taken from what reaches the base.
 */
public struct KoralClearCoatPbr : IKoralShading {
    public KoralPbr base;
    public float coat;              // how much of a coat there is, 0 to 1
    public float coatRoughness;
    public float3 coatNormal;       // the coat's own: often the surface's before a normal map

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const KoralAngles a = KoralAnglesOf(coatNormal, toEye, toLight);
        const float fresnel = KoralFresnelSchlick(0.04, 1.0, a.vh) * coat;
        const float3 mirrored = a.nl > 0.0 ? KoralGgxLobe.Reflected(a, float3(0.04), coatRoughness) * coat * light * a.nl : float3(0.0);
        return base.Reflected(toEye, toLight, light) * (1.0 - fresnel) + mirrored;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        const float nv = saturate(dot(coatNormal, toEye));
        const float fresnel = KoralFresnelSchlick(0.04, 1.0, nv) * coat;
        const float3 shine = KoralEnvironmentReflectance(float3(0.04), coatRoughness, nv) * coat;
        return base.Ambient(environment, toEye) * (1.0 - fresnel) + shine * environment.Specular(reflect(-toEye, coatNormal), coatRoughness);
    }

    public float3 Emitted() { return base.Emitted(); }
    public float Opacity() { return base.Opacity(); }
};

// ---- brushed --------------------------------------------------------------------------------------------------------

/**
 * A metal-rough surface whose highlight is drawn out along @p tangent (brushed metal, hair, a vinyl record's grooves):
 * anisotropic GGX (Burley's), @p anisotropy -1 to 1 — 0 is KoralPbr's.
 */
public struct KoralAnisotropicPbr : IKoralShading {
    public KoralPbr base;
    public float3 tangent;
    public float3 bitangent;
    public float anisotropy;

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const float3 n = base.normal;
        const KoralAngles a = KoralAnglesOf(n, toEye, toLight);
        if (a.nl <= 0.0) return 0.0;
        const float3 h = KoralSafeNormalize(toEye + toLight, n);
        const float alpha = KoralAlphaOf(base.roughness);
        const float at = max(alpha * (1.0 + anisotropy), 0.002), ab = max(alpha * (1.0 - anisotropy), 0.002);
        const float th = dot(tangent, h), bh = dot(bitangent, h);
        const float d = th * th / (at * at) + bh * bh / (ab * ab) + a.nh * a.nh;
        const float distribution = 1.0 / (KoralPi * at * ab * d * d);
        const float tv = dot(tangent, toEye), bv = dot(bitangent, toEye), tl = dot(tangent, toLight), bl = dot(bitangent, toLight);
        const float lambdaV = a.nl * length(float3(at * tv, ab * bv, a.nv)), lambdaL = a.nv * length(float3(at * tl, ab * bl, a.nl));
        const float visibility = 0.5 / max(lambdaV + lambdaL, 1e-6);
        const float3 f0 = KoralReflectanceAtNormal(base.baseColor, base.metallic);
        const float3 fresnel = KoralFresnelSchlick(f0, a.vh);
        const float3 taken = (1.0 - fresnel) * (1.0 - base.metallic) * base.baseColor * KoralInvPi;
        return (taken + distribution * visibility * fresnel) * light * a.nl;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        // What is around mirrored along the normal bent towards the brushing (McAuley's), as rough as it is across.
        const float3 across = cross(anisotropy >= 0.0 ? bitangent : tangent, toEye);
        const float3 bent = KoralSafeNormalize(lerp(base.normal, cross(across, anisotropy >= 0.0 ? bitangent : tangent), abs(anisotropy) * saturate(4.0 * base.roughness)), base.normal);
        KoralPbr bentBase = base;
        bentBase.normal = bent;
        return bentBase.Ambient(environment, toEye);
    }

    public float3 Emitted() { return base.Emitted(); }
    public float Opacity() { return base.Opacity(); }
};

// ---- cloth ----------------------------------------------------------------------------------------------------------

/** Cloth: a diffuse colour, and a sheen of its own colour at grazing angles (Charlie's, Neubelt's visibility) — velvet, wool. */
public struct KoralClothPbr : IKoralShading {
    public float3 normal;
    public float3 baseColor;
    public float3 sheenColor;
    public float roughness;
    public float occlusion;
    public float opacity;

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const KoralAngles a = KoralAnglesOf(normal, toEye, toLight);
        if (a.nl <= 0.0) return 0.0;
        const float3 sheen = sheenColor * KoralCharlieDistribution(a.nh, roughness) * KoralNeubeltVisibility(a.nv, a.nl);
        return (baseColor * KoralInvPi + sheen) * light * a.nl;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        const float nv = saturate(dot(normal, toEye));
        const float3 sheen = sheenColor * (1.0 - nv) * (1.0 - nv) * 0.25;     // a grazing glow of what is around
        return (baseColor * environment.Diffuse(normal) + sheen * environment.Specular(reflect(-toEye, normal), roughness)) * occlusion;
    }

    public float3 Emitted() { return 0.0; }
    public float Opacity() { return opacity; }
};

// ---- thin and translucent ---------------------------------------------------------------------------------------

/**
 * A thin surface light passes through — a leaf, paper, a lampshade — and that light scatters in (wrapped light, its
 * colour @p scatter): what is lit from behind glows through, and what is lit from the side is softer than Lambert's.
 */
public struct KoralTranslucentPbr : IKoralShading {
    public KoralPbr base;
    public float3 transmission;     // what of the light from behind comes through: its colour, 0 for none
    public float3 scatter;          // the colour light takes on scattering in the surface
    public float wrap;              // how far round the light wraps, 0 (Lambert's) to 1

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const float facing = dot(base.normal, toLight);
        // From behind: through it, as from a matte surface facing the other way.
        const float3 through = transmission * base.baseColor * KoralInvPi * saturate(-facing) * light;
        // Wrapped round the edge of what is lit, tinted as it scatters.
        const float wrapped = saturate((facing + wrap) / ((1.0 + wrap) * (1.0 + wrap))) - saturate(facing);
        const float3 scattered = scatter * base.baseColor * KoralInvPi * max(wrapped, 0.0) * light;
        return base.Reflected(toEye, toLight, light) + through + scattered;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        return base.Ambient(environment, toEye) + transmission * base.baseColor * environment.Diffuse(-base.normal);
    }

    public float3 Emitted() { return base.Emitted(); }
    public float Opacity() { return base.Opacity(); }
};

// ---- iridescence -------------------------------------------------------------------------------------------------

/**
 * What a thin film on a surface mirrors — a soap bubble, oil on water, a beetle's shell: the two faces' reflections
 * interfering by how far apart they are (@p thickness nanometres, a film of index @p filmIor over a base of @p baseIor),
 * worked out at three wavelengths for red, green and blue (an Airy sum of Schlick's Fresnel at each face).
 */
public float3 KoralThinFilmFresnel(float cosTheta, float thickness, float filmIor = 1.33, float baseIor = 1.5)
{
    const float sinTheta2 = 1.0 - cosTheta * cosTheta;
    const float cosFilm = sqrt(saturate(1.0 - sinTheta2 / (filmIor * filmIor)));
    const float r01 = KoralSquare((1.0 - filmIor) / (1.0 + filmIor)), r12 = KoralSquare((filmIor - baseIor) / (filmIor + baseIor));
    const float f01 = KoralFresnelSchlick(r01, 1.0, cosTheta), f12 = KoralFresnelSchlick(r12, 1.0, cosFilm);
    const float3 wavelengths = float3(650.0, 532.0, 450.0);
    const float3 phase = 4.0 * KoralPi * filmIor * thickness * cosFilm / wavelengths;
    const float a = sqrt(f01 * f12);
    const float3 numerator = f01 + f12 + 2.0 * a * cos(phase);
    const float3 denominator = 1.0 + f01 * f12 + 2.0 * a * cos(phase);
    return saturate(numerator / denominator);
}
