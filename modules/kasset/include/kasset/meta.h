#pragma once

/**
 * @file meta.h
 * @brief What is kept beside an asset's file, in `<file>.meta`: its ID, its kind, what makes it and from what
 *        parameters, the IDs of its parts, and what was changed of them here. The only place an asset's settings are.
 *        A generator's asset (`.kgen`) is a meta of no file.
 */

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "id.h"

namespace kasset
{
    enum class Kind : std::uint8_t {
        eModel,         ///< A file of meshes, materials and pictures, with the things in it and its lights and cameras.
        eMesh,
        eMaterial,
        eTexture,
        eShader,        ///< A shader module. Its entry points are its parts.
        eShaderEntry,   ///< An entry point of a module: only ever a part.
        eLight,         ///< A model's light: only ever a part.
        eCamera,        ///< A model's camera: only ever a part.
        eScript,
        eScene,
        ePrefab,
        eFunction,
        eGenerated,     ///< A generator and its parameters (`.kgen`): what it makes is its kind.
    };
    /** @brief What @p kind is written as in a meta: "model", "mesh", "shaderEntry". */
    [[nodiscard]] std::string_view KindName(Kind kind);
    [[nodiscard]] std::optional<Kind> KindNamed(std::string_view name);
    /**
     * @brief The kind of asset a file is, by its name: a model (`.gltf`, `.obj`, `.fbx`...), a texture, a shader, a
     *        script, a scene (`.scene.json`), a prefab, a function, a material (`.kmat`), a generator's (`.kgen`).
     *        Nothing for a file that is no asset of itself: a glTF's `.bin`, an OBJ's `.mtl`, a meta.
     */
    [[nodiscard]] std::optional<Kind> KindOfFile(const std::filesystem::path& file);

    /** @brief A part of an asset: a model's mesh, material, picture, light or camera, a shader module's entry point. */
    struct Part {
        Id id;
        Kind kind = Kind::eMesh;
        std::string name;       ///< As the file names it: what it is found by again when the file is read again.
        int index = -1;         ///< Where it is among the file's of its kind; found by this when its name is not enough.
        bool operator==(const Part&) const = default;
    };

    struct Meta {
        Id id;
        Kind kind = Kind::eTexture;
        /** What makes it: an importer's name and version (`model@1`), or for a generator's asset its script's reference. */
        std::string producer;
        /** The producer's parameters, as a JSON object's text: kept as written, read by the producer. Empty is none. */
        std::string parameters;
        std::vector<Part> parts;
        /** What was changed of its parts here (a model's materials), as a JSON object's text. Empty is none. */
        std::string overrides;

        /** @brief Its part of @p kind so named, the @p occurrence -th of that name (a file can name two meshes alike). */
        [[nodiscard]] const Part* FindPart(Kind kind, std::string_view name, int occurrence = 0) const;
        [[nodiscard]] const Part* FindPart(Id id) const;
        [[nodiscard]] const Part* PartAt(Kind kind, int index) const;
        bool operator==(const Meta&) const = default;
    };

    /** @brief Where @p file 's meta is: `wall.png` → `wall.png.meta`. A `.kgen` is its own. */
    [[nodiscard]] std::filesystem::path MetaPathOf(const std::filesystem::path& file);
    /** @brief Whether @p file is a meta: `.meta`. */
    [[nodiscard]] bool IsMeta(const std::filesystem::path& file);

    /** @brief @p meta as the JSON a meta file holds, a field a line. */
    [[nodiscard]] std::string Write(const Meta& meta);
    /** @brief The meta @p text writes, or why it is none. */
    [[nodiscard]] std::expected<Meta, std::string> Read(std::string_view text);

    /** @brief Reads the meta file at @p path. */
    [[nodiscard]] std::expected<Meta, std::string> Load(const std::filesystem::path& path);
    /** @brief Writes @p meta to @p path, whole or not at all: written beside it first, then put in its place. */
    [[nodiscard]] std::expected<void, std::string> Save(const std::filesystem::path& path, const Meta& meta);
}
