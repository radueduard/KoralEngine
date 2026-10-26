//
// kgraph: how picture files are kept on the device — the names of formats, what a picture is for as its field and name
// tell, the packs a set of files suggests — and cooking one into the project's cache.
//

#include <kgraph/textures.h>

#include "cook.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <ranges>

#include <koralImageCompress.h>

namespace kgraph
{
    namespace {
        std::string Lower(const std::string_view text)
        {
            std::string lower(text);
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lower;
        }

        /** The file's own name, without its folder or extension: `wall_Normal` of `@textures/wall_Normal.png`. */
        std::string_view Stem(std::string_view id)
        {
            if (const auto slash = id.find_last_of("/\\"); slash != std::string_view::npos) id.remove_prefix(slash + 1);
            if (id.starts_with('@')) id.remove_prefix(1);
            if (const auto dot = id.rfind('.'); dot != std::string_view::npos) id = id.substr(0, dot);
            return id;
        }

        bool Any(const std::string_view text, std::initializer_list<std::string_view> words)
        {
            return std::ranges::any_of(words, [&](const std::string_view word) { return text.find(word) != std::string_view::npos; });
        }

        /** What a material's occlusion, roughness or metalness file's name ends in, and what is before it. */
        enum class Part : std::uint8_t { eNone, eOcclusion, eRoughness, eMetalness };
        std::pair<Part, std::string> PartOf(const std::string_view id)
        {
            const std::string_view stem = Stem(id);
            const std::string lower = Lower(stem);
            // Longest first: `_ambientocclusion` before `_occlusion`, `_metallic` and `_metalness` before nothing.
            static constexpr std::array<std::pair<std::string_view, Part>, 9> Endings { {
                { "ambientocclusion", Part::eOcclusion }, { "occlusion", Part::eOcclusion }, { "ao", Part::eOcclusion },
                { "roughness", Part::eRoughness }, { "rough", Part::eRoughness },
                { "metalness", Part::eMetalness }, { "metallic", Part::eMetalness }, { "metalic", Part::eMetalness }, { "metal", Part::eMetalness },
            } };
            for (const auto& [ending, part] : Endings) {
                if (!lower.ends_with(ending) || lower.size() == ending.size()) continue;
                const char before = lower[lower.size() - ending.size() - 1];
                if (before != '_' && before != '-' && before != ' ' && before != '.') continue;
                return { part, std::string(stem.substr(0, stem.size() - ending.size() - 1)) };
            }
            return { Part::eNone, {} };
        }
    }

    std::string_view TextureFormatName(const TextureFormat format)
    {
        switch (format) {
        case TextureFormat::eBC1: return "BC1";
        case TextureFormat::eBC3: return "BC3";
        case TextureFormat::eBC4: return "BC4";
        case TextureFormat::eBC5: return "BC5";
        case TextureFormat::eBC7: return "BC7";
        default: return "asRead";
        }
    }

    std::optional<TextureFormat> TextureFormatNamed(const std::string_view name)
    {
        for (const TextureFormat format : TextureFormats()) if (TextureFormatName(format) == name) return format;
        return std::nullopt;
    }

    const std::vector<TextureFormat>& TextureFormats()
    {
        static const std::vector<TextureFormat> all { TextureFormat::eAsRead, TextureFormat::eBC1, TextureFormat::eBC3, TextureFormat::eBC4, TextureFormat::eBC5, TextureFormat::eBC7 };
        return all;
    }

    std::string_view TextureQualityName(const TextureQuality quality)
    {
        return quality == TextureQuality::eFast ? "fast" : quality == TextureQuality::eBest ? "best" : "normal";
    }

    std::optional<TextureQuality> TextureQualityNamed(const std::string_view name)
    {
        for (const TextureQuality quality : { TextureQuality::eFast, TextureQuality::eNormal, TextureQuality::eBest })
            if (TextureQualityName(quality) == name) return quality;
        return std::nullopt;
    }

    std::string_view TextureKindName(const TextureKind kind)
    {
        switch (kind) {
        case TextureKind::eNormal: return "normal";
        case TextureKind::eMask: return "mask";
        case TextureKind::eData: return "data";
        default: return "color";
        }
    }

    std::optional<TextureKind> TextureKindNamed(const std::string_view name)
    {
        for (const TextureKind kind : { TextureKind::eColor, TextureKind::eNormal, TextureKind::eMask, TextureKind::eData })
            if (TextureKindName(kind) == name) return kind;
        return std::nullopt;
    }

    TextureKind TextureKindOf(const std::string_view field, const std::string_view id)
    {
        // The file's name first: a field called `roughnessMap` can hold a metalness and roughness together.
        const std::string name = Lower(Stem(id));
        const auto numbers = [](const std::string& text) {
            int found = 0;
            for (const auto words : { std::initializer_list<std::string_view> { "rough" }, { "metal" }, { "occlusion", "_ao" } }) found += Any(text, words);
            return found;
        };
        if (Any(name, { "normal", "_nrm", "_nor" })) return TextureKind::eNormal;
        if (numbers(name) >= 2 || Any(name, { "orm", "arm", "_mr" })) return TextureKind::eData;
        if (numbers(name) == 1 || Any(name, { "opacity", "alpha", "mask", "height", "displacement", "specular", "gloss" })) return TextureKind::eMask;
        if (Any(name, { "basecolor", "base_color", "albedo", "diffuse", "color", "colour", "emissive", "emission" })) return TextureKind::eColor;
        const std::string held = Lower(field);
        if (held.find("normal") != std::string::npos) return TextureKind::eNormal;
        if (Any(held, { "rough", "metal", "occlusion", "ao" })) return TextureKind::eData;
        if (Any(held, { "opacity", "mask", "height" })) return TextureKind::eMask;
        return TextureKind::eColor;
    }

    TextureFormat DefaultTextureFormat(const TextureKind kind)
    {
        switch (kind) {
        case TextureKind::eNormal: return TextureFormat::eBC5;
        case TextureKind::eMask: return TextureFormat::eBC4;
        default: return TextureFormat::eBC7;
        }
    }

    bool IsTexturePack(const std::string_view id) { return id.ends_with(".pack"); }

    bool TextureCompressible(const std::string_view id)
    {
        if (IsTexturePack(id)) return true;
        const auto dot = id.rfind('.');
        const std::string extension = dot == std::string_view::npos ? std::string() : Lower(id.substr(dot));
        return extension != ".hdr" && extension != ".exr" && extension != ".ktx" && extension != ".ktx2" && extension != ".dds";
    }

    std::map<std::string, TexturePack> SuggestTexturePacks(const std::vector<std::string>& ids)
    {
        // By material: the folder and what comes before the ending, each part's file.
        std::map<std::pair<std::string, std::string>, std::array<std::string, 4>> materials;
        for (const auto& id : ids) {
            if (!id.starts_with('@') || IsTexturePack(id)) continue;
            const auto [part, base] = PartOf(id);
            if (part == Part::eNone) continue;
            const auto slash = id.find_last_of('/');
            const std::string folder = slash == std::string::npos ? std::string("@") : id.substr(0, slash + 1);
            materials[{ folder, base }][static_cast<std::size_t>(part)] = id;
        }
        std::map<std::string, TexturePack> packs;
        for (const auto& [where, parts] : materials) {
            const auto& [folder, base] = where;
            const int found = !parts[1].empty() + !parts[2].empty() + !parts[3].empty();
            if (found < 2) continue;
            TexturePack pack;
            const auto channel = [&](const Part part, const float otherwise) {
                const std::string& from = parts[static_cast<std::size_t>(part)];
                return from.empty() ? TexturePack::Channel { {}, 0, otherwise } : TexturePack::Channel { from, 0, 0.f };
            };
            pack.channels = { channel(Part::eOcclusion, 1.f), channel(Part::eRoughness, 1.f), channel(Part::eMetalness, 0.f), TexturePack::Channel { {}, 0, 1.f } };
            packs.emplace(std::format("{}{}_ORM.pack", folder, base), std::move(pack));
        }
        return packs;
    }

    // ---- cooking ------------------------------------------------------------------------------------------------

    namespace cook {
        namespace {
            /** Any picture a file decodes to, as eight bits a channel and four channels: what is missing, 0 — alpha 1. */
            std::expected<kimg::CpuImage, std::string> Rgba8(kimg::CpuImage image)
            {
                using F = kor::Image::Format;
                int channels = 0, bytes = 0;
                switch (image.format) {
                case F::eR8_UNORM: channels = 1; bytes = 1; break;
                case F::eRG8_UNORM: channels = 2; bytes = 1; break;
                case F::eRGBA8_UNORM: case F::eRGBA8_SRGB: channels = 4; bytes = 1; break;
                case F::eR16_UNORM: channels = 1; bytes = 2; break;
                case F::eRG16_UNORM: channels = 2; bytes = 2; break;
                case F::eRGBA16_UNORM: channels = 4; bytes = 2; break;
                default: return std::unexpected("its pixels are not of eight or sixteen bits a channel");
                }
                if (channels == 4 && bytes == 1) { image.format = F::eRGBA8_UNORM; return image; }
                const std::size_t texels = static_cast<std::size_t>(image.extent.x) * image.extent.y;
                std::vector<unsigned char> out(texels * 4);
                for (std::size_t t = 0; t < texels; ++t)
                    for (int c = 0; c < 4; ++c) {
                        unsigned char value = c == 3 ? 255 : 0;
                        if (c < channels) {
                            // Sixteen bits: the high byte, as little-endian words have it last.
                            const std::size_t at = (t * channels + c) * bytes;
                            value = bytes == 1 ? image.pixels[at] : image.pixels[at + 1];
                        }
                        out[t * 4 + c] = value;
                    }
                image.pixels = std::move(out);
                image.format = F::eRGBA8_UNORM;
                return image;
            }

            std::expected<kimg::CpuImage, std::string> Read(const std::filesystem::path& file)
            {
                auto decoded = kimg::DecodeImageFile(file);
                if (!decoded) return std::unexpected(std::string(decoded.error().message));
                auto rgba = Rgba8(std::move(*decoded));
                if (!rgba) return std::unexpected(std::format("'{}': {}", file.generic_string(), rgba.error()));
                return rgba;
            }

            /** The settings as one line, for the key: anything in it changed is another cooked file. */
            std::string Settings(const Recipe& recipe)
            {
                std::string line = std::format("v1 {} {}", TextureFormatName(recipe.format), TextureQualityName(recipe.quality));
                if (recipe.pack)
                    for (const auto& channel : recipe.pack->channels) line += std::format(" [{} {} {}]", channel.from, channel.channel, channel.constant);
                return line;
            }

            std::uint64_t Fnv(const std::string_view text, std::uint64_t hash = 14695981039346656037ull)
            {
                for (const char c : text) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
                return hash;
            }

            /**
             * What a cooked file of @p id is called, but for its key: the file's own name, without what could not be in
             * one, and a short hash of its whole id — two files of one name in two folders are two pictures.
             */
            std::string Named(const std::string_view id)
            {
                std::string name(Stem(id));
                for (char& c : name) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
                return std::format("{}-{:08x}", name, Fnv(id) & 0xffffffffu);
            }

            std::vector<std::string> SourcesOf(const Recipe& recipe)
            {
                if (!recipe.pack) return { recipe.id };
                std::vector<std::string> sources;
                for (const auto& channel : recipe.pack->channels)
                    if (!channel.from.empty() && std::ranges::find(sources, channel.from) == sources.end()) sources.push_back(channel.from);
                return sources;
            }
        }

        std::optional<std::filesystem::path> CookedPath(const std::filesystem::path& cache, const Recipe& recipe, const FileOf& fileOf)
        {
            if (cache.empty()) return std::nullopt;
            std::uint64_t key = Fnv(Settings(recipe));
            key = Fnv(recipe.id, key);
            for (const auto& source : SourcesOf(recipe)) {
                const std::filesystem::path file = fileOf(source);
                std::error_code ec;
                const auto size = std::filesystem::file_size(file, ec);
                if (ec) return std::nullopt;
                const auto time = std::filesystem::last_write_time(file, ec);
                if (ec) return std::nullopt;
                key = Fnv(std::format("{} {} {}", source, size, time.time_since_epoch().count()), key);
            }
            return cache / std::format("{}-{:016x}.ktx2", Named(recipe.id), key);
        }

        std::expected<kimg::CpuImage, std::string> Pixels(const Recipe& recipe, const FileOf& fileOf)
        {
            if (!recipe.pack) return Read(fileOf(recipe.id));
            // Each file once, however many channels are taken of it.
            std::map<std::string, kimg::CpuImage> read;
            kor::UVec3 extent { 1, 1, 1 };
            for (const auto& source : SourcesOf(recipe)) {
                auto image = Read(fileOf(source));
                if (!image) return std::unexpected(image.error());
                extent.x = std::max(extent.x, image->extent.x);
                extent.y = std::max(extent.y, image->extent.y);
                read.emplace(source, std::move(*image));
            }
            kimg::CpuImage packed;
            packed.format = kor::Image::Format::eRGBA8_UNORM;
            packed.extent = extent;
            packed.pixels.resize(static_cast<std::size_t>(extent.x) * extent.y * 4);
            for (int c = 0; c < 4; ++c) {
                const auto& channel = recipe.pack->channels[static_cast<std::size_t>(c)];
                if (channel.from.empty()) {
                    const auto value = static_cast<unsigned char>(std::clamp(channel.constant, 0.f, 1.f) * 255.f + 0.5f);
                    for (std::size_t t = 0; t < packed.pixels.size() / 4; ++t) packed.pixels[t * 4 + c] = value;
                    continue;
                }
                // A smaller picture is stretched over the biggest: the texel nearest.
                const kimg::CpuImage& from = read.at(channel.from);
                const int taken = std::clamp(channel.channel, 0, 3);
                for (kor::u32 y = 0; y < extent.y; ++y) {
                    const kor::u32 fy = static_cast<kor::u32>(static_cast<std::uint64_t>(y) * from.extent.y / extent.y);
                    for (kor::u32 x = 0; x < extent.x; ++x) {
                        const kor::u32 fx = static_cast<kor::u32>(static_cast<std::uint64_t>(x) * from.extent.x / extent.x);
                        packed.pixels[(static_cast<std::size_t>(y) * extent.x + x) * 4 + c] = from.pixels[(static_cast<std::size_t>(fy) * from.extent.x + fx) * 4 + taken];
                    }
                }
            }
            return packed;
        }

        std::expected<std::filesystem::path, std::string> Cook(const std::filesystem::path& cache, const Recipe& recipe, const FileOf& fileOf)
        {
            if (recipe.format == TextureFormat::eAsRead) return std::unexpected("it is kept as it is read: there is nothing to cook");
            const auto target = CookedPath(cache, recipe, fileOf);
            if (!target) return std::unexpected(std::format("'{}': a file it is made of is not there", recipe.id));
            auto pixels = Pixels(recipe, fileOf);
            if (!pixels) return std::unexpected(pixels.error());
            // Laid out as the transcoder reads each format: BC4 takes green, BC5 green and alpha.
            if (recipe.format == TextureFormat::eBC4 || recipe.format == TextureFormat::eBC5)
                for (std::size_t t = 0; t < pixels->pixels.size(); t += 4) {
                    unsigned char* texel = &pixels->pixels[t];
                    const unsigned char x = texel[0], y = texel[1];
                    texel[0] = texel[1] = texel[2] = x;
                    texel[3] = recipe.format == TextureFormat::eBC5 ? y : 255;
                }
            kimg::CompressOptions options;
            options.codec = kimg::Codec::eUASTC;
            options.quality = recipe.quality == TextureQuality::eFast ? 0 : recipe.quality == TextureQuality::eBest ? 255 : 128;
            options.generateMipmaps = true;
            options.srgb = false;   // read as the file is: a shader that takes a colour out of its gamma does that itself
            options.supercompress = true;
            options.transcodeTo = recipe.format == TextureFormat::eBC1 ? kimg::BlockFormat::eBC1
                                : recipe.format == TextureFormat::eBC3 ? kimg::BlockFormat::eBC3
                                : recipe.format == TextureFormat::eBC4 ? kimg::BlockFormat::eBC4
                                : recipe.format == TextureFormat::eBC5 ? kimg::BlockFormat::eBC5
                                : kimg::BlockFormat::eBC7;
            const std::string stem = target->stem().string();
            auto written = kimg::CompressToKtx(*pixels, target->parent_path(), stem, options);
            if (!written) return std::unexpected(std::string(written.error().message));
            // What was cooked of it before — of other settings, or of the files as they were — is let go of.
            std::error_code ec;
            const std::string prefix = Named(recipe.id) + "-";
            for (const auto& entry : std::filesystem::directory_iterator(target->parent_path(), ec)) {
                const std::string name = entry.path().filename().string();
                if (entry.path() != *written && name.starts_with(prefix) && name.size() == prefix.size() + 16 + 5 && name.ends_with(".ktx2"))
                    std::filesystem::remove(entry.path(), ec);
            }
            return *written;
        }
    }
}
