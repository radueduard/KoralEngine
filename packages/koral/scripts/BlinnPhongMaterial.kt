// The engine's package. What a surface is made of, described the classic way — as an OBJ, or an FBX's Phong or Lambert,
// describes it: a diffuse colour, a specular colour and how tight its highlight is. Colours are as they are picked.
// Data and nothing else: a shader takes what it needs of it by annotating its members, `[BlinnPhongMaterial("shininess")]`.

import koral.Vec3
import koral.Vec4
import koral.engine.Choices
import koral.engine.Logarithmic
import koral.engine.Material
import koral.engine.Min
import koral.engine.Range
import koral.engine.Texture

class BlinnPhongMaterial : Material() {
    var diffuseMap = Texture.White
    var specularMap = Texture.White
    var normalMap = Texture.Normal
    var emissiveMap = Texture.White
    var diffuse = Vec4(0.8f, 0.8f, 0.8f, 1f)        // a: how opaque.
    @Min(0f) var specular = Vec3(0.2f, 0.2f, 0.2f)
    @Min(1f) @Logarithmic var shininess = 32f       // The Phong exponent: higher, a smaller and sharper highlight.
    @Min(0f) var emission = Vec3(0f, 0f, 0f)
    @Choices("Opaque", "Masked", "Blended") var alphaMode = 0
    @Range(0f, 1f) var alphaCutoff = 0.5f           // Masked: a texel less opaque than this is not drawn.
    var doubleSided = false
}
