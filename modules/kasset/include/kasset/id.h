#pragma once

/**
 * @file id.h
 * @brief What an asset is known by: an ID that stays when its file is renamed or moved. A part of an asset (a model's
 *        mesh) is known by its asset's ID and one of its own: a Reference.
 */

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace kasset
{
    /** @brief 128 bits, written as 32 lowercase hex digits. The zero ID is none. */
    struct Id {
        std::uint64_t high = 0, low = 0;

        /** @brief A new one, at random: no other asset has it. */
        [[nodiscard]] static Id New();
        /**
         * @brief The engine's own asset called @p name (`mesh/cube`, `material/Gold`): the same ID in every project
         *        and every run, worked out from the name.
         */
        [[nodiscard]] static Id Named(std::string_view name);
        /** @brief The ID @p text writes: 32 hex digits. Nothing for anything else. */
        [[nodiscard]] static std::optional<Id> Parse(std::string_view text);

        [[nodiscard]] std::string Text() const;
        [[nodiscard]] bool Valid() const { return high != 0 || low != 0; }
        explicit operator bool() const { return Valid(); }
        auto operator<=>(const Id&) const = default;
    };

    /**
     * @brief What a field holds to name an asset, or a part of one: `<asset>` or `<asset>/<part>`, each an Id's text.
     */
    struct Reference {
        Id asset;
        Id part;    ///< None: the asset itself.

        /** @brief The reference @p text writes. Nothing for anything else: an old `@path`, a `$cube`. */
        [[nodiscard]] static std::optional<Reference> Parse(std::string_view text);
        [[nodiscard]] std::string Text() const;
        [[nodiscard]] bool Valid() const { return asset.Valid(); }
        auto operator<=>(const Reference&) const = default;
    };
}

template <>
struct std::hash<kasset::Id> {
    std::size_t operator()(const kasset::Id& id) const noexcept { return static_cast<std::size_t>(id.high ^ (id.low * 0x9e3779b97f4a7c15ull)); }
};
