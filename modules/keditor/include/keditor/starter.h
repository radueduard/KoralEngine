#pragma once

/**
 * @file starter.h
 * @brief A scene to start from: a graph that draws meshes lit, under a sky, and a few
 *        things for it to draw.
 *
 * The graph is ordinary nodes, there to be changed: nothing in the engine knows of it. Its shaders are four files
 * (engineCommon, lit, sky) the project has to have among its own, which WriteStandardShaders puts there.
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
     * @brief Gives a project what its Kotlin scripts are built and run with: a Gradle build at its root whose sources
     *        are its `scripts` folder, with `play` and `edit` tasks, and a `gradle.properties` saying where this
     *        machine's SDK, engine and JDK are. A file that is there is left as it is, but for gradle.properties,
     *        which is this machine's and is written again. What was written, by name.
     */
    std::vector<std::string> WriteKotlinBuild(const std::filesystem::path& project, const std::string& name);

    /**
     * @brief Gives a project what an editor of code needs to know it — Visual Studio Code, by what it reads: where a
     *        shader's `import` is looked for, which extensions its files want, and for C# scripts a project file
     *        naming the engine's own assemblies, so that a script is written with the engine's names offered and
     *        checked. (Kotlin scripts have that of the Gradle build, WriteKotlinBuild.) What says where this
     *        machine's engine is is written again each time; the rest only where there is none. What was written.
     */
    std::vector<std::string> WriteEditorSetup(const std::filesystem::path& project, const std::string& name);

    /**
     * @brief Adds the standard graph to @p document, a pass a page:
     *        - `frame`: the scene's camera, sky, sun and lights as values, and the two buffers of them shaders read;
     *        - `scene`: a picture the size of the window with its depth and an image of which entity is where, and
     *          a pipeline `lit` that draws every entity with a Mesh and its component — what its shader asks of
     *          each: an albedo, how metallic and how rough, what light it gives off;
     *        - `sky` and `show`. Nothing of it is an editor's: picking, the line round what is picked and the editor's
     *          shapes are the editor's own passes (keditor::Overlay), whatever the graph.
     *        Each pass brings in what it uses of the passes before it. False, with @p error, when a node could not
     *        be added.
     */
    bool AddStandardGraph(Document& document, std::string& error);
    /** @brief Adds a sky, a camera, a sun and a sphere: something to see, and to see it by. The sphere, to pick. */
    kecs::Entity AddStandardScene(Document& document);
}
