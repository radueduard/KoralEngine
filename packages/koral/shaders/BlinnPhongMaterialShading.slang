// How a BlinnPhongMaterial is read into the engine's classic shading (koral_shadings' KoralBlinnPhong): a diffuse colour,
// a specular colour and how tight a highlight is.

module BlinnPhongMaterialShading;

__exported import koral_shadings;
__exported import koral_bindless;
__exported import BlinnPhongMaterial;

public extension BlinnPhongMaterial : IKoralMaterial {
    public typealias Shading = KoralBlinnPhong;

    public KoralBlinnPhong Read(KoralSurface at)
    {
        const float4 base = kor_Texture2D[NonUniformResourceIndex(diffuseMap)].Sample(at.sampler, at.uv);
        KoralBlinnPhong phong;
        phong.normal = KoralMappedNormal(at, kor_Texture2D[NonUniformResourceIndex(normalMap)].Sample(at.sampler, at.uv).xyz);
        phong.diffuse = KoralLinearFromGamma22(diffuse.rgb * base.rgb);
        phong.specular = KoralLinearFromGamma22(specular * kor_Texture2D[NonUniformResourceIndex(specularMap)].Sample(at.sampler, at.uv).rgb);
        phong.shininess = max(shininess, 1.0);
        phong.emission = emission * KoralLinearFromGamma22(kor_Texture2D[NonUniformResourceIndex(emissiveMap)].Sample(at.sampler, at.uv).rgb);
        const float alpha = diffuse.a * base.a;
        phong.opacity = alphaMode == 0 ? 1.0 : alphaMode == 1 ? (alpha < alphaCutoff ? 0.0 : 1.0) : alpha;
        return phong;
    }
};
