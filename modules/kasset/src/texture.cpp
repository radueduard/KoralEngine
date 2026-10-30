//
// kasset: how picture files are kept on the device — the names of formats, what a picture is for as its field and name
// tell, the packs a set of files suggests — and what a picture is for.
//

#include <kasset/texture.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <functional>
#include <initializer_list>
#include <ranges>


namespace kasset
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
}
