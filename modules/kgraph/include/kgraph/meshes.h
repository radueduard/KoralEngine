#pragma once

/**
 * @file meshes.h
 * @brief The procedural meshes: a cube, a sphere, a plane, a cylinder, a cone, a capsule, a torus and more, each made
 *        from parameters of its own.
 *
 * At its initial parameters each is centred on the origin and about a unit across, its triangles counter-clockwise seen from outside. A
 * vertex is a position, a normal and a texture coordinate; on the device, with the directions of its picture's u and v
 * along the surface too, as the graph asks (Tangents), all in one buffer: POSITION, NORMAL, TANGENT, UV.
 */

#include <kasset/mesh.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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
     * @brief The procedural meshes' shapes, by name. Each, as it starts, about a unit across and centred on the origin
     *        (but for `axes`, from it):
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
    /** @brief One number a shape is made from. Each entity tunes its own: see ShapeName. */
    struct MeshParameter {
        std::string name;
        float initial, least, most;
        bool whole;             ///< A count: rings, segments.
        std::string says;       ///< What it does, for a tooltip.
    };
    /**
     * @brief The name of one mesh of a shape, by @p values: a value of each of its ShapeParameters. Those at their
     *        initial value are left out, in their order, so that one set of numbers has one name, and what has the same
     *        name is the same mesh: `sphere`, `sphere?radius=0.8&rings=24`. Empty for what is no shape.
     */
    [[nodiscard]] std::string ShapeName(std::string_view shape, std::span<const float> values);
    /** @brief The shape a name is of (`sphere`, of `sphere?rings=24`). Empty for what is none. */
    [[nodiscard]] std::string_view ShapeOf(std::string_view name);
    /** @brief What a shape is made from, in order. @p name may carry values; none for what is no shape. */
    [[nodiscard]] const std::vector<MeshParameter>& ShapeParameters(std::string_view name);
    /** @brief The value of each of ShapeParameters that a name says, the rest initial, each within its limits. */
    [[nodiscard]] std::vector<float> ShapeValues(std::string_view name);
    /**
     * @brief The mesh so named: as ShapeName has it, or one of DefaultMeshNames, at its initial values. Nothing for a
     *        name of no shape.
     */
    [[nodiscard]] std::optional<MeshData> ShapeData(std::string_view name);
    /**
     * @brief What a procedural mesh is called where a resource is named by the node that made it: `$cube`,
     *        `$sphere?rings=24`. An entity's Mesh holds one without the graph having a node for it. @see Runtime::ResourceOf
     */
    [[nodiscard]] std::string DefaultMeshId(std::string_view name);

    // ---- vertices of any make -----------------------------------------------------------------------------

    using kasset::AttributeType;
    using kasset::VertexAttribute;
    using kasset::VertexData;

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

    using kasset::MeshImport;
}
