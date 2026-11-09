// How a PbrMaterial is read into the engine's physically based shading (koral_shadings' KoralPbr: GGX over Lambert). A
// kind of a project's that describes the same things can give back a KoralPbr too.

module PbrMaterialShading;

__exported import koral_shadings;
__exported import koral_bindless;
__exported import PbrMaterial;

public extension PbrMaterial : IKoralMaterial {
    public typealias Shading = KoralPbr;

    public KoralPbr Read(KoralSurface at)
    {
        const float4 base = kor_Texture2D[NonUniformResourceIndex(baseColorMap)].Sample(at.sampler, at.uv);
        const float4 surface = kor_Texture2D[NonUniformResourceIndex(metallicRoughnessMap)].Sample(at.sampler, at.uv);
        KoralPbr pbr;
        pbr.normal = KoralMappedNormal(at, kor_Texture2D[NonUniformResourceIndex(normalMap)].Sample(at.sampler, at.uv).xyz);
        pbr.baseColor = KoralLinearFromGamma22(baseColor.rgb * base.rgb);
        pbr.metallic = saturate(metallic * surface.b);
        pbr.roughness = clamp(roughness * surface.g, 0.03, 1.0);
        pbr.occlusion = kor_Texture2D[NonUniformResourceIndex(occlusionMap)].Sample(at.sampler, at.uv).r;
        pbr.emission = emission * KoralLinearFromGamma22(kor_Texture2D[NonUniformResourceIndex(emissiveMap)].Sample(at.sampler, at.uv).rgb);
        const float alpha = baseColor.a * base.a;
        pbr.opacity = alphaMode == 0 ? 1.0 : alphaMode == 1 ? (alpha < alphaCutoff ? 0.0 : 1.0) : alpha;
        return pbr;
    }
};
