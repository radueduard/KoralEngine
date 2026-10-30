#pragma once

/**
 * @file import.h
 * @brief The importers: what reads a file of another program's — a model, a picture — and makes the engine's of it.
 *        Each is a producer: what it makes is kept in a cache by what it was made of, and read from there after, so a
 *        file is read by its importer once for each content and settings it has.
 *
 * Touches nothing of the device: run anywhere, off the frame, or by a build's tools.
 */

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <kasset/artifact.h>
#include <kasset/beside.h>
#include <kasset/cache.h>
#include <kasset/mesh.h>

namespace kasset
{
    /** @brief The model importer, as a producer: its name and version. Up whenever what it makes changes. */
    inline constexpr std::string_view ModelProducer = "model@2";
    /** @brief What a model's artifact is kept by: the file, what it names beside it that is not a picture, and @p import. */
    [[nodiscard]] Key ModelKey(const std::filesystem::path& file, const MeshImport& import);
    /** @brief Reads model @p file with its importer, as @p import says: what it holds, or why it could not be. */
    [[nodiscard]] std::expected<ModelArtifact, std::string> ReadModel(const std::filesystem::path& file, const MeshImport& import);
    /**
     * @brief What model @p file holds, as @p import says: from @p cache when it was read so before — of the file as it
     *        is now — or read with its importer and kept there. With no cache, read each time.
     */
    [[nodiscard]] std::expected<ModelArtifact, std::string> ImportModel(const std::filesystem::path& file, const MeshImport& import, const Cache* cache);
}
