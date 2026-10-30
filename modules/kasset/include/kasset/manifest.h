#pragma once

/**
 * @file manifest.h
 * @brief What a game is made of: every asset its scenes reach, by what names what — and what is found by its name rather
 *        than its file (its shaders, its scripts, its functions) — so that a build has that and nothing else.
 */

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "id.h"

namespace kasset
{
    class Database;

    struct Manifest {
        std::vector<Id> scenes;                     ///< What it was worked out from.
        std::vector<Id> assets;                     ///< Every asset reached, the scenes among them, each once.
        /// Every file it needs, from the project's folder, each once and in order: each asset's, its meta, what a model
        /// names beside it, and the project's own (`project.json`, its shaders, scripts, functions and objects).
        std::vector<std::filesystem::path> files;
    };

    /**
     * @brief What the scenes @p scenes reach of @p assets, as their files name it (Database::IndexReferences is read
     *        again first), and what any game of the project has.
     */
    [[nodiscard]] Manifest Reach(Database& assets, const std::vector<Id>& scenes);

    /** @brief @p manifest as JSON: `{ "koral-manifest": 1, "scenes": [...], "assets": [...], "files": [...] }`. */
    [[nodiscard]] std::string Write(const Manifest& manifest);
    [[nodiscard]] std::expected<Manifest, std::string> ReadManifest(std::string_view json);
}
