#pragma once

/**
 * @file mesh.h
 * @brief A mesh as numbers, of any attributes: what an importer reads a model's meshes as and a generator makes,
 *        before it is anything a device holds; and how a model's meshes are to be read.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kasset
{
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
        bool operator==(const VertexData&) const = default;
    };
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
