//
// kgraph: materials. A material kind is the shape of what a drawn thing is made of — the engine's package has
// PbrMaterial and BlinnPhongMaterial, and a project may declare more — as its script declares it: data and nothing
// else. Each is a component of the world, its pictures shared references to images at the front of it, and has a form a
// shader can import, a Slang module of the kind's name with a struct of it and the attribute a shader takes a field of
// it by:
//
// @code
// import PbrMaterial;
// struct Instance {
//     [entity("MATRIX")] float4x4 model;
//     [PbrMaterial("baseColor")] float4 albedo;      // the entity's PbrMaterial's baseColor
//     [PbrMaterial("baseColorMap")] uint albedoMap;  // a picture: its bindless index
// };
// @endcode
//
// `koral_materials` imports every kind there is.
//

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <kecs/kecs.h>

#include "assets.h"
#include "value.h"

namespace kgraph
{
    /** @brief A kind of what a drawn thing is made of, as its script declares it (kscript::ScriptType of a Material). */
    struct MaterialKind {
        std::string name;                           ///< Its component's, its Slang module's and its shader attribute's.
        std::vector<kecs::FieldDescription> fields; ///< Its pictures ("Image", a shared reference each) first, then the rest.
        std::size_t size = 0, alignment = 8;
        /// What a new one's fields other than its pictures start as, `size` bytes (the pictures' are no matter). Empty: zeros.
        std::vector<std::byte> initial;
        /// Its pictures, each by field: what it starts as, by the name it is found by (`$white`, `@textures/ramp.png`).
        std::vector<std::pair<std::string, std::string>> textures;
        std::string declaredIn;                     ///< The script it is declared in, for what is generated of it to say.
    };

    struct Resource;

    /**
     * @brief A material as a model file's importer reports it, by named values: what a material kind is chosen by — its
     *        model — and filled from, a field by name.
     *
     * Described physically: `baseColor` (vec4), `metallic`, `roughness`, `emission`, and the pictures `baseColorMap`,
     * `normalMap`, `metallicRoughnessMap`, `emissiveMap`, `occlusionMap`. Classic: `diffuse` (vec4), `specular`,
     * `shininess`, `emission`, and `diffuseMap`, `specularMap`, `normalMap`, `emissiveMap`, `shininessMap`, `alphaMap`.
     * Either: `alphaMode` (0 opaque, 1 masked, 2 blended), `alphaCutoff`, `doubleSided`. Colours are as they are picked,
     * not light; a picture is by the name it is found by, `$white` (`$normal` for a normal map) for one it has not.
     */
    struct ReportedMaterial {
        ModelMaterial::Model model = ModelMaterial::Model::ePbr;
        std::string name;
        std::vector<std::pair<std::string, Value>> values;

        /** @brief The value so named; null for none. */
        [[nodiscard]] const Value* Find(std::string_view value) const;
    };
    [[nodiscard]] ReportedMaterial ReportOf(const ModelMaterial& material);

    /**
     * @brief The world's component of @p kind: its pictures shared references, each a new one starts holding what
     *        @p starting has for it (in the order of `kind.textures`), and its other fields as `kind.initial` has them.
     */
    [[nodiscard]] kecs::ComponentDescription MaterialComponentOf(const MaterialKind& kind, std::vector<std::shared_ptr<Resource>> starting);

    /**
     * @brief What a field of a kind is in a shader: `float`, `float3`, `float4x4`, `uint` — a picture is its bindless
     *        index, and a true or false a uint. Empty for one that has no form on the GPU: an entity.
     */
    [[nodiscard]] std::string_view GpuTypeOf(const kecs::FieldDescription& field);

    /**
     * @brief Why a shader's member, of type @p member (as kgraph names one: "f32", "vec4", "u32", "mat4"), cannot hold
     *        field @p field of a kind; empty when it can. It can when they are the same type; and a colour's three
     *        numbers fill a vec4 (its w 1), a vec4's first three a vec3, a quaternion a vec4, true or false and a smaller whole number a 32-bit one,
     *        and a picture a u32 — its bindless index, which the engine writes.
     */
    [[nodiscard]] std::string MaterialFieldMisfit(const kecs::FieldDescription& field, std::string_view member);
    /**
     * @brief Writes field @p field 's value — at @p from, a kind's component's bytes at the field — as a member of type
     *        @p member holds it, into @p to: @p size bytes. For a field MaterialFieldMisfit says fits; not a picture.
     */
    void WriteMaterialField(const kecs::FieldDescription& field, const std::byte* from, std::string_view member, std::byte* to, std::size_t size);

    /** @brief A kind's Slang module, and the fields it leaves out: those of no GPU form, with why. */
    struct MaterialModule {
        std::string source;
        std::vector<std::string> left;
    };
    [[nodiscard]] MaterialModule MaterialModuleOf(const MaterialKind& kind);
    /** @brief `koral_materials`: every kind's module, imported and exported, for a shader that wants them all. */
    [[nodiscard]] std::string MaterialsModuleOf(std::span<const MaterialKind> kinds);
    /**
     * @brief Writes every kind's module, and `koral_materials`, into @p directory — a file only when what it holds
     *        changed, so that nothing that imports it is built again for nothing — and takes away a kind's that is no
     *        longer. What could not be written, and the fields left out, are returned. With @p whole false, @p kinds are
     *        some of those of a folder another keeps: theirs alone are written, and nothing else is touched.
     */
    std::vector<std::string> WriteMaterialModules(const std::filesystem::path& directory, std::span<const MaterialKind> kinds, bool whole = true);
    /** @brief Writes @p text to @p file when it does not hold it already: what imports it is not built again for nothing. */
    bool WriteIfChanged(const std::filesystem::path& file, const std::string& text);
}
