#pragma once

/**
 * @file rewrite.h
 * @brief A file's references, written as IDs and read back as what the runtime names them by. A scene, a prefab, a
 *        material keeps an asset by its ID, so that its file can be renamed and moved: in memory, it is named by where
 *        it is now. Each string value of a JSON text is looked at — never a key — and the rest is left as it is.
 */

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace kasset
{
    class Database;

    /** @brief @p json with each string value @p replace gives another for replaced by that: keys, and the rest, as they are. */
    [[nodiscard]] std::string RewriteStrings(std::string_view json, const std::function<std::optional<std::string>(std::string_view)>& replace);

    /**
     * @brief @p json as a file keeps it: each file of @p assets named by where it is (`@textures/wall.png`,
     *        `@ship.gltf#3`) named by its ID instead, its part's too. What names no file of the project's is as it is,
     *        and so are the engine's own (`$cube`, `material:Gold`): they never move.
     */
    [[nodiscard]] std::string ToIds(std::string_view json, Database& assets);
    /**
     * @brief @p json as it is worked on: each ID named by where its asset is now. An ID of no asset there is now is left
     *        as it is, and counted in @p missing.
     */
    [[nodiscard]] std::string FromIds(std::string_view json, const Database& assets, std::size_t* missing = nullptr);
}
