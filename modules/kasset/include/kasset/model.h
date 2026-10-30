#pragma once

/**
 * @file model.h
 * @brief What a model file holds besides its meshes: the things in it and where each is, what each is made of, its
 *        lights and its cameras. What an importer reports, and what entities are made of.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace kasset
{
    /**
     * @brief A material as the file reports it: described physically (a glTF, or any file that gives a metallic or a
     *        roughness), whose albedo, roughness and metallic are filled; or the classic way (an OBJ, an FBX's Phong),
     *        whose diffuse, specular and shininess are. What it is imported as is chosen by which (materials.h, ReportOf).
     */
    struct ModelMaterial {
        enum class Model { ePbr, eClassic };
        std::string name;
        Model model = Model::ePbr;
        float albedo[4] { 1.f, 1.f, 1.f, 1.f };     ///< In linear terms, as a file has it; a: how opaque.
        float emission[3] { 0.f, 0.f, 0.f };
        float roughness = 1.f, metallic = 0.f;
        float diffuse[4] { 1.f, 1.f, 1.f, 1.f };    ///< The classic ones, as the file has them; a: how opaque.
        float specular[3] { 0.f, 0.f, 0.f };
        float shininess = 0.f;                      ///< The Phong exponent.
        float alphaCutoff = 0.f;                    ///< Above 0: a texel less opaque than this is not drawn. 0: all of it is.
        bool blended = false;                       ///< Drawn over what is behind it by how opaque it is: glass, a decal.
        bool doubleSided = false;
        /// Its pictures, each by the name it is found by (`@...`); empty for one it has not. roughnessMap is glTF's
        /// metallic and roughness in one (g and b).
        std::string albedoMap, normalMap, roughnessMap, emissiveMap, occlusionMap;
        std::string specularMap, shininessMap, alphaMap;   ///< The classic ones'.
    };

    struct ModelNode {
        std::string name;
        int parent = -1;                            ///< Among Model::nodes; -1 for the first, which is over all.
        float position[3] { 0.f, 0.f, 0.f }, rotation[3] { 0.f, 0.f, 0.f }, scale[3] { 1.f, 1.f, 1.f };   ///< Relative to its parent; rotation in degrees.
        struct Part {
            std::string mesh;                       ///< By the name it is found by: `@model.gltf#3`.
            int material = -1;                      ///< Among Model::materials.
        };
        std::vector<Part> parts;                    ///< What it is drawn as: a mesh each, of one material.
    };

    struct ModelLight {
        enum class Kind { ePoint, eDirectional, eSpot } kind = Kind::ePoint;
        std::string name;
        float position[3] { 0.f, 0.f, 0.f }, direction[3] { 0.f, -1.f, 0.f }, color[3] { 1.f, 1.f, 1.f };
        float intensity = 1.f, range = 0.f, innerAngle = 0.f, outerAngle = 0.f;     ///< Angles in degrees.
        int node = -1;          ///< Among Model::nodes: the one it is on; -1 for none.
        bool atNode = false;    ///< At its node's origin, shining along its -Z: where it is and which way it faces are all its node's.
    };

    /** A camera of the file's, looking along its -Z with +Y up. Position, direction and up are where it is in the file's world. */
    struct ModelCamera {
        std::string name;
        float position[3] { 0.f, 0.f, 0.f }, direction[3] { 0.f, 0.f, -1.f }, up[3] { 0.f, 1.f, 0.f };
        bool orthographic = false;
        float fieldOfView = 60.f;           ///< Perspective: top to bottom, in degrees.
        float height = 2.f;                 ///< Orthographic: how much it sees, top to bottom.
        float aspect = 0.f;                 ///< Width over height; 0 for none given.
        float zNear = 0.1f, zFar = 0.f;     ///< zFar 0: no limit.
        int node = -1;                      ///< As ModelLight's.
        bool atNode = false;
    };

    struct Model {
        std::vector<ModelNode> nodes;               ///< Parents before their children.
        std::vector<ModelMaterial> materials;
        std::vector<ModelLight> lights;
        std::vector<ModelCamera> cameras;
        /// What the file calls each of its meshes, by its index among them: what a mesh is found by again when the file is
        /// read again with its meshes in another order.
        std::vector<std::string> meshNames;
        float boundsMin[3] { 0.f, 0.f, 0.f }, boundsMax[3] { 0.f, 0.f, 0.f };  ///< Of its meshes, as they are in the file.
        /// Every attribute any of its meshes has in the file, by semantic — read or left out (Graph::meshImports) —
        /// with how many numbers of what each is, in the order they are read.
        struct Attribute {
            std::string semantic;
            std::string type;       ///< "float" or "uint".
            std::uint32_t count = 0;
            bool read = true;       ///< Held: not left out.
            bool operator==(const Attribute&) const = default;
        };
        std::vector<Attribute> attributes;
    };
}
