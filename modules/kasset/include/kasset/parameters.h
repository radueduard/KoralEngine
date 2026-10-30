#pragma once

/**
 * @file parameters.h
 * @brief A producer's parameters as a meta keeps them — a JSON object, each producer's under a name of its own
 *        (`mesh`, `texture`, `materialKinds`) — and the settings the engine's importers take, written and read.
 */

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "mesh.h"
#include "meta.h"
#include "texture.h"

namespace kasset
{
    /** @brief The parameter @p key of @p meta, as its JSON text; empty when it has none. */
    [[nodiscard]] std::string ParameterOf(const Meta& meta, std::string_view key);
    /** @brief Sets parameter @p key of @p meta to @p json, or takes it away for empty. True when that changed it. */
    bool SetParameter(Meta& meta, std::string_view key, std::string_view json);

    /** @brief How a model's meshes are read, as the `mesh` parameter: `{ "leaveOut": [...], "buffers": [[...]] }`. */
    [[nodiscard]] std::string ToJson(const MeshImport& import);
    [[nodiscard]] std::optional<MeshImport> MeshImportFrom(std::string_view json);
    /** @brief How a picture is kept, as the `texture` parameter: `{ "format": "BC7", "quality": "normal" }`. */
    [[nodiscard]] std::string ToJson(const TextureImport& import);
    [[nodiscard]] std::optional<TextureImport> TextureImportFrom(std::string_view json);
    /** @brief What a model's materials are imported as, by what the file reports them as (`pbr`, `classic`) to a kind. */
    [[nodiscard]] std::string ToJson(const std::map<std::string, std::string>& kinds);
    [[nodiscard]] std::optional<std::map<std::string, std::string>> KindsFrom(std::string_view json);
}
