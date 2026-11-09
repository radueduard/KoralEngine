// The engine's base library: how a surface answers light, a part at a time — the light it mirrors off its facets (a
// specular lobe) and the light it takes in and scatters (a diffuse lobe), and how much goes to each (Fresnel). A shading
// is put together from a lobe of each (koral_shadings' KoralPbrOf); another lobe of the same interface makes another.
//
//     import koral_brdf;
//
//     const KoralAngles a = KoralAnglesOf(normal, toEye, toLight);
//     const float3 mirrored = KoralGgxLobe.Reflected(a, f0, roughness);     // times light and a.nl, as a shading adds it up
//
// A lobe's Reflected is the BRDF itself: what it sends to the eye of light arriving square to it, before the cosine.
// Roughness is as a material says it, 0 to 1 (perceptual); a lobe squares it where it needs alpha.

module koral_brdf;

import koral_math;

/** The cosines a lobe is worked out of. */
public struct KoralAngles {
    public float nl;    // the normal and the way to the light
    public float nv;    // the normal and the way to the eye: never quite 0
    public float nh;    // the normal and the halfway between them
    public float vh;    // the eye and the halfway: as much as the light and the halfway
    public float lv;    // the light and the eye: what a rough diffuse lobe (Oren and Nayar's) turns on
};

public KoralAngles KoralAnglesOf(float3 normal, float3 toEye, float3 toLight)
{
    const float3 halfway = KoralSafeNormalize(toEye + toLight, normal);
    KoralAngles a;
    a.nl = saturate(dot(normal, toLight));
    a.nv = max(dot(normal, toEye), 1e-4);
    a.nh = saturate(dot(normal, halfway));
    a.vh = saturate(dot(toEye, halfway));
    a.lv = dot(toLight, toEye);
    return a;
}

// ---- how much is mirrored ----------------------------------------------------------------------------------------

/** Schlick's fit of Fresnel: @p f0 mirrored square on, all of it at a grazing angle. */
public float3 KoralFresnelSchlick(float3 f0, float cosTheta) { return f0 + (1.0 - f0) * KoralPow5(1.0 - saturate(cosTheta)); }
public float KoralFresnelSchlick(float f0, float f90, float cosTheta) { return f0 + (f90 - f0) * KoralPow5(1.0 - saturate(cosTheta)); }

/** What a surface mirrors square on: a little of everything (@p dielectric) for what is not metal, its own colour for what is. */
public float3 KoralReflectanceAtNormal(float3 baseColor, float metallic, float dielectric = 0.04)
{
    return lerp(float3(dielectric), baseColor, metallic);
}

// ---- the parts of a lobe -------------------------------------------------------------------------------------------

/** How many of a rough surface's facets face the halfway: Trowbridge and Reitz's (GGX), of alpha (roughness squared). */
public float KoralGgxDistribution(float nh, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (KoralPi * d * d);
}

/** How much of them is hidden from neither the light nor the eye, over 4 nl nv: Smith's, height-correlated (Heitz). */
public float KoralSmithGgxCorrelated(float nv, float nl, float alpha)
{
    const float a2 = alpha * alpha;
    const float v = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    const float l = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-6);
}

/** Alpha of a perceptual roughness: squared, and never quite 0 — a highlight too small to draw. */
public float KoralAlphaOf(float roughness) { return max(roughness * roughness, 0.002); }

// ---- lobes ----------------------------------------------------------------------------------------------------------

/** Light mirrored off a surface's facets: what is reflected, with how much is mirrored rather than taken in. */
public interface IKoralSpecularLobe {
    static float3 Reflected(KoralAngles a, float3 f0, float roughness);
};

/** Light taken in and scattered back out: as a factor of the surface's colour. */
public interface IKoralDiffuseLobe {
    static float Reflected(KoralAngles a, float roughness);
};

/** GGX facets, Smith's shadowing, Schlick's Fresnel: what most surfaces are drawn with. */
public struct KoralGgxLobe : IKoralSpecularLobe {
    public static float3 Reflected(KoralAngles a, float3 f0, float roughness)
    {
        const float alpha = KoralAlphaOf(roughness);
        return KoralGgxDistribution(a.nh, alpha) * KoralSmithGgxCorrelated(a.nv, a.nl, alpha) * KoralFresnelSchlick(f0, a.vh);
    }
};

/** Blinn and Phong's highlight, normalised so a tighter one is brighter rather than the surface shinier; f0 its colour, no Fresnel. */
public struct KoralBlinnPhongLobe : IKoralSpecularLobe {
    public static float3 Reflected(KoralAngles a, float3 f0, float roughness)
    {
        const float shininess = KoralShininessOf(roughness);
        return f0 * pow(a.nh, shininess) * (shininess + 8.0) / (8.0 * KoralPi);
    }
};

/** As much every way: Lambert's. */
public struct KoralLambertLobe : IKoralDiffuseLobe {
    public static float Reflected(KoralAngles a, float roughness) { return KoralInvPi; }
};

/** Burley's (Disney's): a rough surface brighter at a grazing angle, a smooth one darker. */
public struct KoralBurleyLobe : IKoralDiffuseLobe {
    public static float Reflected(KoralAngles a, float roughness)
    {
        const float f90 = 0.5 + 2.0 * roughness * a.vh * a.vh;
        return KoralFresnelSchlick(1.0, f90, a.nl) * KoralFresnelSchlick(1.0, f90, a.nv) * KoralInvPi;
    }
};

/** Blinn and Phong's exponent of a roughness, and back: the highlight as wide either way. */
public float KoralShininessOf(float roughness) { return max(2.0 / KoralSquare(KoralAlphaOf(roughness)) - 2.0, 1.0); }
public float KoralRoughnessOfShininess(float shininess) { return sqrt(sqrt(2.0 / (shininess + 2.0))); }

// ---- what is around --------------------------------------------------------------------------------------------------

/**
 * How much of what is around a surface mirrors, by how rough it is and how it is seen: a scale of f0 and a bias, the
 * split sum's second half by Karis's fit — no table to read.
 */
public float2 KoralEnvironmentBrdf(float roughness, float nv)
{
    const float4 r = roughness * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);
    const float fall = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return float2(-1.04, 1.04) * fall + r.zw;
}

/** What @p f0 mirrors of what is around: by KoralEnvironmentBrdf. */
public float3 KoralEnvironmentReflectance(float3 f0, float roughness, float nv)
{
    const float2 scaleBias = KoralEnvironmentBrdf(roughness, nv);
    return f0 * scaleBias.x + scaleBias.y;
}

/**
 * What a rough mirror loses to light that bounces between its facets more than once, given back (Fdez-Agüera's fit): a
 * factor for its specular, 1 for a smooth one, more for a rough metal.
 */
public float3 KoralEnergyCompensation(float3 f0, float roughness, float nv)
{
    const float2 scaleBias = KoralEnvironmentBrdf(roughness, nv);
    return 1.0 + f0 * (1.0 / max(scaleBias.x + scaleBias.y, 1e-4) - 1.0);
}
