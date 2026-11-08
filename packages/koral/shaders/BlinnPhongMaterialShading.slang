// The classic way of shading, and how a BlinnPhongMaterial is read into it: a diffuse colour, a specular colour and how
// tight a highlight is, normalised so that a tighter highlight is brighter rather than the surface shinier.

module BlinnPhongMaterialShading;

__exported import koral_shading;
__exported import koral_bindless;
__exported import BlinnPhongMaterial;

/** What is at a point, as light. */
public struct KoralBlinnPhong : IKoralShading {
    public float3 normal;
    public float3 diffuse;
    public float3 specular;
    public float shininess;
    public float3 emission;
    public float opacity;

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const float nl = saturate(dot(normal, toLight));
        if (nl <= 0.0) return 0.0;
        const float nh = saturate(dot(normal, normalize(toEye + toLight)));
        return (diffuse / KoralPi + specular * pow(nh, shininess) * (shininess + 8.0) / (8.0 * KoralPi)) * light * nl;
    }

    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        // The roughness whose highlight is as wide. What is around is reflected as a dielectric does, a little face on and
        // all at a grazing angle, and less the rougher it is: a specular colour is no mirror's, or a plastic is chrome.
        const float roughness = sqrt(2.0 / (shininess + 2.0));
        const float grazing = pow(1.0 - saturate(dot(normal, toEye)), 5.0), gloss = 1.0 - roughness;
        const float3 mirrored = specular * (0.16 + 0.84 * grazing) * gloss * gloss;
        return diffuse * environment.Diffuse(normal) + mirrored * environment.Specular(reflect(-toEye, normal), roughness);
    }

    public float3 Emitted() { return emission; }
    public float Opacity() { return opacity; }
};

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
