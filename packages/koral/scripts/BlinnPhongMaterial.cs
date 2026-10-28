// The engine's package. What a surface is made of, described the classic way — as an OBJ, or an FBX's Phong or Lambert,
// describes it: a diffuse colour, a specular colour and how tight its highlight is. Colours are as they are picked.
// Data and nothing else: a shader takes what it needs of it by annotating its members, `[BlinnPhongMaterial("shininess")]`.

public sealed class BlinnPhongMaterial : Material
{
    public Texture diffuseMap = Texture.White;
    public Texture specularMap = Texture.White;
    public Texture normalMap = Texture.Normal;
    public Texture emissiveMap = Texture.White;
    public Vec4 diffuse = new(0.8f, 0.8f, 0.8f, 1f);        // a: how opaque.
    [Min(0f)] public Vec3 specular = new(0.2f, 0.2f, 0.2f);
    [Min(1f), Logarithmic] public float shininess = 32f;    // The Phong exponent: higher, a smaller and sharper highlight.
    [Min(0f)] public Vec3 emission = new(0f, 0f, 0f);
    [Choices("Opaque", "Masked", "Blended")] public int alphaMode = 0;
    [Range(0f, 1f)] public float alphaCutoff = 0.5f;        // Masked: a texel less opaque than this is not drawn.
    public bool doubleSided = false;
}
