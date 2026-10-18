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

    // ---- vertices of any make -----------------------------------------------------------------------------

    /** @brief What the numbers of an attribute are: each four bytes. */
    enum class AttributeType : std::uint8_t { eFloat, eInt, eUInt };

    /**
     * @brief One attribute of every vertex: what it is (its semantic, as a shader asks for it: `POSITION`, `UV1`,
     *        `BONE_IDS`, or anything of a script's: `WIND`), and its numbers — @ref count of them a vertex, one
     *        vertex after another, each a word of four bytes (a float's bits, for a float).
     */
    struct VertexAttribute {
        std::string semantic;
        AttributeType type = AttributeType::eFloat;
        std::uint32_t count = 0;            ///< Numbers a vertex: 1 to 4.
        std::vector<std::uint32_t> words;   ///< count × the vertices.
        bool operator==(const VertexAttribute&) const = default;
    };

    /**
     * @brief A mesh as numbers, of any attributes: what a model's mesh is read as, and a script makes. On the device
     *        each of @ref buffers is a vertex buffer of the attributes it names, side by side in each vertex; what
     *        none names is in one more after them. An attribute that is not here is not anywhere: nothing of it is
     *        held.
     */
    struct VertexData {
        std::uint32_t vertexCount = 0;
        std::vector<VertexAttribute> attributes;
        std::vector<std::uint32_t> indices;             ///< Empty: drawn in the order of its vertices.
        std::vector<std::vector<std::string>> buffers;  ///< By semantic. Empty: one buffer of them all.

        [[nodiscard]] const VertexAttribute* Find(std::string_view semantic) const;
        [[nodiscard]] VertexAttribute* Find(std::string_view semantic);
        /** @brief Why it is no mesh: an attribute's numbers that are not count × vertexCount, an index past the last vertex. Empty when it is one. */
        [[nodiscard]] std::string Problem() const;
    };
    /** @brief A default mesh's numbers, as attributes: POSITION, NORMAL and UV, in one buffer. */
    [[nodiscard]] VertexData ToVertexData(const MeshData& data);

    /**
     * @brief How a model file's meshes are read: which of their attributes are left out — not read, not held — and
     *        how the rest are put in vertex buffers. By default every attribute a mesh has is read, all in one buffer.
     *        @see Graph::meshImports
     */
    struct MeshImport {
        std::vector<std::string> leaveOut;              ///< Semantics: `BONE_IDS`, `TANGENT`. POSITION is always read.
        std::vector<std::vector<std::string>> buffers;  ///< As VertexData::buffers.
        bool operator==(const MeshImport&) const = default;
        [[nodiscard]] bool Default() const { return leaveOut.empty() && buffers.empty(); }
    };
}
