#pragma once

/**
 * @file meshes.h
 * @brief The meshes there are without making any: a cube, a sphere, a plane, a cylinder, a cone, a capsule, a torus.
 *
 * Each is centred on the origin and about a unit across, its triangles counter-clockwise seen from outside. A
 * vertex is a position, a normal and a texture coordinate; on the device, with the directions of its picture's u and v
 * along the surface too, as the graph asks (Tangents), all in one buffer: POSITION, NORMAL, TANGENT, UV.
 */

#include <array>
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
        /// A skinned one's, a vertex each (or none): which four bones move it, and by how much each.
        std::vector<std::array<std::uint32_t, 4>> boneIds;
        std::vector<std::array<float, 4>> boneWeights;
    };

    /**
     * @brief The default meshes, by name. Each about a unit across and centred on the origin (but for `axes`, from it):
     *
     *   cube, sphere, plane (facing up), cylinder, cone, capsule, torus, quad (facing +z), triangle
     *   icosphere        a sphere of even triangles
     *   grid             the plane cut 64 by 64: for what is moved vertex by vertex (water, ground, cloth)
     *   insideCube       the cube and the sphere seen from inside: a sky box, a sky dome, a room
     *   insideSphere
     *   roundedBox       a cube with its edges rounded
     *   wedge            a ramp, its back at -z
     *   pyramid, hemisphere (its flat side down), pipe (a tube), arch, stairs (climbing towards -z)
     *   teapot, knot     to try shading on: curved every way
     *   arrow            up +y; axes: arrows along +x, +y, +z from the origin
     *   skinnedCylinder  of three bones, with BONE_IDS and BONE_WEIGHTS: laid out apart from the rest
     */
    [[nodiscard]] const std::vector<std::string>& DefaultMeshNames();
    /**
     * @brief The mesh so named: one of DefaultMeshNames. Nothing for a name that is none of them.
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
    /**
     * @brief Which way a mesh's picture runs along its surface, besides its normal: what a normal map is turned by.
     *        The tangent is the way u grows, the bitangent the way v does, each at right angles to the normal.
     *
     *   eFour            TANGENT of four numbers: the tangent, and in w which side of it the bitangent is (1 or -1):
     *                    bitangent = cross(normal, tangent.xyz) * tangent.w — as glTF and MikkTSpace have it. The least
     *                    held, and what most shaders are written for.
     *   eWithBitangent   TANGENT and BITANGENT, three numbers each: as a model file's meshes are read, so that the
     *                    default meshes are laid out as those are (and are packed with them, of the same attributes).
     *   eNone            neither: POSITION, NORMAL and UV.
     */
    enum class Tangents : std::uint8_t { eFour, eWithBitangent, eNone };
    /** @brief What a Tangents is saved as: `tangent`, `tangentBitangent`, `none`. */
    [[nodiscard]] std::string_view TangentsName(Tangents tangents);
    /** @brief A Tangents by what it is saved as; nothing for what is none. */
    [[nodiscard]] std::optional<Tangents> TangentsNamed(std::string_view name);
    /**
     * @brief Gives a mesh tangents (and bitangents) as @p form says, worked out from its POSITION, NORMAL and UV and
     *        its triangles, after its NORMAL: in place of any it had. One without a NORMAL or a UV is left as it is, and
     *        so is any with eNone (but for tangents it had, taken out).
     */
    void AddTangents(VertexData& data, Tangents form);
    /** @brief A default mesh's numbers, as attributes: POSITION, NORMAL, the tangents asked for and UV, in one buffer. */
    [[nodiscard]] VertexData ToVertexData(const MeshData& data, Tangents tangents = Tangents::eNone);

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
