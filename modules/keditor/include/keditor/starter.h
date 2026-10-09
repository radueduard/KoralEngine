#pragma once

/**
 * @file starter.h
 * @brief A scene to start from: a graph that draws meshes lit, under a sky, with what is picked outlined — and a few
 *        things for it to draw.
 *
 * The graph is ordinary nodes, there to be changed: nothing in the engine knows of it. Its shaders are four files
 * (engineCommon, lit, sky, outline) the project has to have among its own, which WriteStandardShaders puts there.
 */

#include <filesystem>
#include <string>
#include <vector>

#include "document.h"

namespace keditor
{
    /** @brief The standard shaders, by file name. */
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& StandardShaders();
    /**
     * @brief Writes the standard shaders into @p directory, each only where there is no file of its name: one that
     *        was edited is left as it is. What was written, by name.
     */
    std::vector<std::string> WriteStandardShaders(const std::filesystem::path& directory);

    /** @brief The standard objects — a cube, a sphere, a camera, the lights, a sky — by file name: what Create makes. */
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& StandardObjects();
    /** @brief Writes the standard objects into @p directory, each only where there is no file of its name. */
    std::vector<std::string> WriteStandardObjects(const std::filesystem::path& directory);

    /**
     * @brief Adds the standard graph to @p document: a picture the size of the window with its depth and an image of
     *        which entity is where; a pipeline `lit` that draws every entity with a Mesh and its component — what its
     *        shader asks of each: an albedo, how metallic and how rough, what light it gives off; the sky behind; an outline round what is picked; the editor's shapes;
     *        picking; and the picture shown. False, with @p error, when a node could not be added.
     */
    bool AddStandardGraph(Document& document, std::string& error);
    /** @brief Adds a sky, a camera, a sun and a sphere: something to see, and to see it by. The sphere, to pick. */
    kecs::Entity AddStandardScene(Document& document);
}
