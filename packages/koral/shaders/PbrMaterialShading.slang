// The engine's physically based shading, and how a PbrMaterial is read into it: a metal-rough surface under GGX. A kind of
// a project's that describes the same things can give back a KoralPbr too (see koral_shading).

module PbrMaterialShading;

__exported import koral_shading;
__exported import koral_bindless;
__exported import PbrMaterial;

/** What is at a point, as light: a colour, how metal, how rough. */
public struct KoralPbr : IKoralShading {
    public float3 normal;
    public float3 baseColor;
    public float metallic;
    public float roughness;
    public float occlusion;
    public float3 emission;
    public float opacity;

    float3 Reflectance() { return lerp(float3(0.04), baseColor, metallic); }

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const float3 halfway = normalize(toEye + toLight);
        const float nl = saturate(dot(normal, toLight)), nv = max(dot(normal, toEye), 1e-4), nh = saturate(dot(normal, halfway)), vh = saturate(dot(toEye, halfway));
        if (nl <= 0.0) return 0.0;
        const float a = max(roughness * roughness, 0.002), a2 = a * a;
        const float d = nh * nh * (a2 - 1.0) + 1.0;
        const float distribution = a2 / (KoralPi * d * d);
        const float k = a * 0.5;
        const float visibility = 1.0 / (4.0 * (nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));
        const float3 fresnel = lerp(Reflectance(), float3(1.0), pow(1.0 - vh, 5.0));
        const float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * baseColor / KoralPi;
        return (diffuse + fresnel * distribution * visibility) * light * nl;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        const float nv = saturate(dot(normal, toEye));
        // What a rough surface keeps of the reflection it would have, by how it is seen (Karis's fit of it).
        const float4 r = roughness * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);
        const float fall = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
        const float2 scale = float2(-1.04, 1.04) * fall + r.zw;
        const float3 shine = Reflectance() * scale.x + scale.y;
        const float3 diffuse = (1.0 - metallic) * baseColor * environment.Diffuse(normal);
        return (diffuse + shine * environment.Specular(reflect(-toEye, normal), roughness)) * occlusion;
    }

    public float3 Emitted() { return emission; }
    public float Opacity() { return opacity; }
};

public extension PbrMaterial : IKoralMaterial {
    public typealias Shading = KoralPbr;

    public KoralPbr Read(KoralSurface at)
    {
        const float4 base = kor_Texture2D[NonUniformResourceIndex(baseColorMap)].Sample(at.sampler, at.uv);
        const float4 surface = kor_Texture2D[NonUniformResourceIndex(metallicRoughnessMap)].Sample(at.sampler, at.uv);
        KoralPbr pbr;
        pbr.normal = KoralMappedNormal(at, kor_Texture2D[NonUniformResourceIndex(normalMap)].Sample(at.sampler, at.uv).xyz);
        pbr.baseColor = KoralLinear(baseColor.rgb * base.rgb);
        pbr.metallic = saturate(metallic * surface.b);
        pbr.roughness = clamp(roughness * surface.g, 0.03, 1.0);
        pbr.occlusion = kor_Texture2D[NonUniformResourceIndex(occlusionMap)].Sample(at.sampler, at.uv).r;
        pbr.emission = emission * KoralLinear(kor_Texture2D[NonUniformResourceIndex(emissiveMap)].Sample(at.sampler, at.uv).rgb);
        const float alpha = baseColor.a * base.a;
        pbr.opacity = alphaMode == 0 ? 1.0 : alphaMode == 1 ? (alpha < alphaCutoff ? 0.0 : 1.0) : alpha;
        return pbr;
    }
};
