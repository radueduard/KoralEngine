#pragma once

// kgraph: cooking a picture — a file, or a pack of others' channels — into a block-compressed KTX2 in the project's
// cache, and what is read of a picture when it has not been. Touches nothing of the device: run on any thread.

#include <kgraph/textures.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <koralImageImport.h>

namespace kgraph::cook
{
    /** @brief A picture file of an asset id: where it is on disk. */
    using FileOf = std::function<std::filesystem::path(std::string_view id)>;

    /** @brief What a picture is cooked of and how: a file of its own, or a pack's channels. */
    struct Recipe {
        std::string id;                         ///< `@wall.png`, `@wall_ORM.pack`
        TextureFormat format = TextureFormat::eAsRead;
        TextureQuality quality = TextureQuality::eNormal;
        std::optional<TexturePack> pack;        ///< A pack's channels; nothing for a file.
        bool operator==(const Recipe&) const = default;
    };

    /**
     * @brief Where @p recipe's cooked file is in @p cache: its name, and a key of everything it was cooked of — each
     *        file's size and time, and the settings — so a file or a setting changed is another name, and the old one
     *        is cooked again rather than read. Nothing when a file it is of is not there, or there is no cache.
     */
    [[nodiscard]] std::optional<std::filesystem::path> CookedPath(const std::filesystem::path& cache, const Recipe& recipe, const FileOf& fileOf);

    /** @brief The pixels of @p recipe as eight bits a channel, four channels: its file's, or its pack's put together. */
    [[nodiscard]] std::expected<kimg::CpuImage, std::string> Pixels(const Recipe& recipe, const FileOf& fileOf);

    /**
     * @brief Cooks @p recipe into @p cache, at CookedPath: the pixels laid out as its format's encoder reads them, then
     *        encoded, with their mips. Other files cooked of the same picture before — of other settings, or other
     *        files — are taken away. Seconds of work: off the frame.
     */
    [[nodiscard]] std::expected<std::filesystem::path, std::string> Cook(const std::filesystem::path& cache, const Recipe& recipe, const FileOf& fileOf);
}
