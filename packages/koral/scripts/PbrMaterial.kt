// The engine's package. What a surface is made of, described physically — as a glTF describes it: what a model file
// that gives a metallic or a roughness is imported as. Colours are as they are picked, not light: a shader makes them so.
// Data and nothing else: a shader takes what it needs of it by annotating its members, `[PbrMaterial("roughness")]`.

import koral.Vec3
import koral.Vec4
import koral.engine.Choices
import koral.engine.Material
import koral.engine.Min
import koral.engine.Range
import koral.engine.Texture

class PbrMaterial : Material() {
    var baseColorMap = Texture.White
    var normalMap = Texture.Normal
    var metallicRoughnessMap = Texture.White    // As glTF has it: roughness in green, metallic in blue.
    var emissiveMap = Texture.White
    var occlusionMap = Texture.White
    var baseColor = Vec4(1f, 1f, 1f, 1f)        // a: how opaque.
    @Range(0f, 1f) var metallic = 0f
    @Range(0f, 1f) var roughness = 0.5f
    @Min(0f) var emission = Vec3(0f, 0f, 0f)
    @Choices("Opaque", "Masked", "Blended") var alphaMode = 0
    @Range(0f, 1f) var alphaCutoff = 0.5f       // Masked: a texel less opaque than this is not drawn.
    var doubleSided = false
}
