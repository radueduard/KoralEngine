#pragma once

/**
 * @file textures.h
 * @brief How a scene's picture files are kept on the device: as they are read, or block-compressed — cooked once into
 *        the project's `cache/textures` and read from there from then on — and pictures made of the channels of others.
 *
 * Cooking changes nothing a scene names: an entity's `@wall_Roughness.png` is still that, and the runtime reads its
 * cooked file in its place while one is there that was cooked of the file as it is now, with the settings as they are
 * now (Graph::textureImports). A pack (Graph::texturePacks) is a picture of its own made of others' channels — the
 * metalness, roughness and occlusion of a material as one — and each picture it is made of is, to whoever holds it,
 * the pack's channel it went into, in every channel: what reads the file reads that, from the one picture.
 */

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kgraph
{
    /**
     * @brief What a picture is kept on the device as. A block format is what its cooked file holds — what the GPU
     *        samples — so what it reads of the picture is fixed by it:
     *
     *   eAsRead  as the file is read: four bytes a texel, its mips made on the device
     *   eBC1     red, green and blue, 4 bits a texel; no alpha
     *   eBC3     red, green, blue and a smooth alpha, 8 bits a texel
     *   eBC4     one channel, 4 bits a texel: the picture's red, read back in red, green and blue
     *   eBC5     two channels, 8 bits a texel: the picture's red and green — a normal map's x and y; its blue reads 1,
     *            and a shader works out z from x and y
     *   eBC7     red, green, blue and alpha, 8 bits a texel: the best of them
     */
    enum class TextureFormat : std::uint8_t { eAsRead, eBC1, eBC3, eBC4, eBC5, eBC7 };

    /** @brief A format's name as a graph saves it: `asRead`, `BC1`... */
    [[nodiscard]] std::string_view TextureFormatName(TextureFormat format);
    /** @brief The format of a name TextureFormatName gives; nothing for any other. */
    [[nodiscard]] std::optional<TextureFormat> TextureFormatNamed(std::string_view name);
    /** @brief Every format, eAsRead first. */
    [[nodiscard]] const std::vector<TextureFormat>& TextureFormats();

    /** @brief How hard the encoder works: what cooking takes, against how near to the picture what it makes is. */
    enum class TextureQuality : std::uint8_t { eFast, eNormal, eBest };
    [[nodiscard]] std::string_view TextureQualityName(TextureQuality quality);
    [[nodiscard]] std::optional<TextureQuality> TextureQualityNamed(std::string_view name);

    /** @brief How one picture file is kept. @see Graph::textureImports */
    struct TextureImport {
        TextureFormat format = TextureFormat::eAsRead;
        TextureQuality quality = TextureQuality::eNormal;
        bool operator==(const TextureImport&) const = default;
        [[nodiscard]] bool Default() const { return format == TextureFormat::eAsRead; }
    };

    /**
     * @brief A picture made of the channels of others: its red, green, blue and alpha each one channel of a picture
     *        file, or a number. Of pictures of different sizes, it is as big as the biggest, the others stretched.
     *        @see Graph::texturePacks
     */
    struct TexturePack {
        struct Channel {
            std::string from;           ///< A picture file, by its asset id (`@wall_Roughness.png`); empty: `constant`.
            int channel = 0;            ///< Which of its channels: 0 red, 1 green, 2 blue, 3 alpha.
            float constant = 0.f;       ///< What it is, 0 to 1, when it is no file's.
            bool operator==(const Channel&) const = default;
        };
        std::array<Channel, 4> channels;    ///< What its red, green, blue and alpha are.
        TextureFormat format = TextureFormat::eBC7;
        TextureQuality quality = TextureQuality::eNormal;
        bool operator==(const TexturePack&) const = default;
    };

    /**
     * @brief What a picture is for, as far as it can be told from the field that holds it (`albedoMap`) and the file's
     *        name (`wall_Normal.png`): what decides the format it is offered by default.
     *
     *   eColor   a colour, or one the eye sees as such: albedo, base colour, emission
     *   eNormal  a normal map
     *   eMask    one number a texel: roughness, metalness, occlusion, opacity, height — each in a file of its own
     *   eData    numbers that are no colour, several to a texel: a metalness and roughness together
     */
    enum class TextureKind : std::uint8_t { eColor, eNormal, eMask, eData };
    [[nodiscard]] std::string_view TextureKindName(TextureKind kind);
    [[nodiscard]] std::optional<TextureKind> TextureKindNamed(std::string_view name);
    /** @brief What a picture held by @p field (may be empty) and found by @p id is for. */
    [[nodiscard]] TextureKind TextureKindOf(std::string_view field, std::string_view id);
    /** @brief What a picture of @p kind is offered as: BC7 for a colour or numbers together, BC5 for a normal map, BC4 for a mask. */
    [[nodiscard]] TextureFormat DefaultTextureFormat(TextureKind kind);

    /**
     * @brief The packs @p ids suggest, by their pack ids: of each material whose occlusion, roughness and metalness come
     *        as files of their own — `wall_AO.png`, `wall_Roughness.png`, `wall_Metalness.png` — at least two of them,
     *        one `@wall_ORM.pack` of occlusion in red, roughness in green and metalness in blue, as glTF lays them out;
     *        what is missing is 1 occlusion or 0 metalness. Alpha is 1.
     */
    [[nodiscard]] std::map<std::string, TexturePack> SuggestTexturePacks(const std::vector<std::string>& ids);

    /**
     * @brief Whether picture file @p id can be kept block-compressed: not a picture of more than eight bits a channel's
     *        range (`.hdr`, `.exr`), which these formats cannot hold, nor one compressed already (`.ktx`, `.ktx2`, `.dds`).
     *        One that cannot is kept as it is read, whatever the graph says.
     */
    [[nodiscard]] bool TextureCompressible(std::string_view id);

    /** @brief Whether @p id names a pack rather than a file: it ends in `.pack`. */
    [[nodiscard]] bool IsTexturePack(std::string_view id);
}
