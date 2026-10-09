#pragma once

/**
 * @file meshes.h
 * @brief The meshes there are without making any: a cube, a sphere, a plane, a cylinder, a cone, a capsule, a torus.
 *
 * Each is centred on the origin and about a unit across, its triangles counter-clockwise seen from outside. A
 * vertex is a position, a normal and a texture coordinate, in one buffer: POSITION, NORMAL, UV.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kgraph
{
    struct MeshVertex {
        float position[3];
        float normal[3];
        float uv[2];
    };

    /** @brief A mesh as numbers, before it is anything a device holds. */
    struct MeshData {
        std::vector<MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    /** @brief The default meshes, by name: cube, sphere, plane, cylinder, cone, capsule, torus. */
    [[nodiscard]] const std::vector<std::string>& DefaultMeshNames();
    /**
     * @brief The mesh so named: one of DefaultMeshNames, or `quad` (a unit square facing +z) or `triangle`. Nothing
     *        for a name that is none of them.
     */
    [[nodiscard]] std::optional<MeshData> ShapeData(std::string_view name);
    /**
     * @brief What a default mesh is called where a resource is named by the node that made it: `$cube`. An
     *        entity's Mesh holds one without the graph having a node for it. @see Runtime::ResourceOf
     */
    [[nodiscard]] std::string DefaultMeshId(std::string_view name);
}
