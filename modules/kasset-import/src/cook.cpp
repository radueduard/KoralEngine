// kasset: cooking a picture — a file, or a pack of others' channels — into a block-compressed KTX2 in the project's cache.

#include <kasset/cook.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <map>

#include <koralImageCompress.h>

namespace kasset
{
    namespace cook {
        namespace {
            /** The file's own name, without its folder or extension: `wall_Normal` of `@textures/wall_Normal.png`. */
            std::string_view Stem(std::string_view id)
            {
                if (const auto slash = id.find_last_of("/\\"); slash != std::string_view::npos) id.remove_prefix(slash + 1);
                if (id.starts_with('@')) id.remove_prefix(1);
                if (const auto dot = id.rfind('.'); dot != std::string_view::npos) id = id.substr(0, dot);
                return id;
            }

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
            // Of each file's content, not its time: a checkout or a copy is the same picture, and needs no cooking again.
            Key key(TextureProducer);
            key.Add(Settings(recipe)).Add(recipe.id);
            for (const auto& source : SourcesOf(recipe)) {
                key.Add(source);
                if (!key.AddFile(fileOf(source))) return std::nullopt;
            }
            return cache / std::format("{}-{}.ktx2", Named(recipe.id), key.Text());
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
                if (entry.path() != *written && name.starts_with(prefix) && name.size() == prefix.size() + 32 + 5 && name.ends_with(".ktx2"))
                    std::filesystem::remove(entry.path(), ec);
            }
            return *written;
        }
    }
}
