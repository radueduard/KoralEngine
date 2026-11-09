// The engine's package. What a surface is made of, described physically — as a glTF describes it: what a model file
// that gives a metallic or a roughness is imported as. Colours are as they are picked, not light: a shader makes them so.
// Data and nothing else: a shader takes what it needs of it by annotating its members, `[PbrMaterial("roughness")]`.

public sealed class PbrMaterial : Material
{
    [Shown] public Texture baseColorMap = Texture.White;
    public Texture normalMap = Texture.Normal;
    public Texture metallicRoughnessMap = Texture.White;    // As glTF has it: roughness in green, metallic in blue.
    public Texture emissiveMap = Texture.White;
    public Texture occlusionMap = Texture.White;
    [Shown] public Vec4 baseColor = new(1f, 1f, 1f, 1f);   // a: how opaque.
    [Shown, Range(0f, 1f)] public float metallic = 0f;
    [Shown, Range(0f, 1f)] public float roughness = 0.5f;
    [Min(0f)] public Vec3 emission = new(0f, 0f, 0f);
    [Choices("Opaque", "Masked", "Blended")] public int alphaMode = 0;
    [Range(0f, 1f)] public float alphaCutoff = 0.5f;        // Masked: a texel less opaque than this is not drawn.
    public bool doubleSided = false;
}
