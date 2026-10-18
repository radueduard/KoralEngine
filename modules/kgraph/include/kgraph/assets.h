#pragma once

/**
 * @file assets.h
 * @brief What a model file holds besides its meshes: the things in it and where each is, what each is made of, and
 *        its lights. What an importer makes entities of.
 *
 * A file's meshes and pictures are resources like any a node makes, found by where they are rather than by a node:
 * `@textures/wall.png` is that picture and `@sponza/Sponza.gltf#3` the fourth mesh of that model, each loaded the
 * first time it is asked for (Runtime::ResourceOf) and saved by that name. A path is the file's own, or one under
 * the runtime's asset directory.
 */

#include <string>
#include <vector>

namespace kgraph
{
    struct ModelMaterial {
        std::string name;
        float albedo[4] { 1.f, 1.f, 1.f, 1.f };     ///< In linear terms, as a file has it; a: how opaque.
        float emission[3] { 0.f, 0.f, 0.f };
        float roughness = 1.f, metallic = 0.f;
        float alphaCutoff = 0.f;                    ///< Above 0: a texel less opaque than this is not drawn. 0: all of it is.
        bool blended = false;                       ///< Drawn over what is behind it by how opaque it is: glass, a decal.
        bool doubleSided = false;
        /// Its pictures, each by the name it is found by (`@...`); empty for one it has not.
        std::string albedoMap, normalMap, roughnessMap, emissiveMap;
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
    };

    struct Model {
        std::vector<ModelNode> nodes;               ///< Parents before their children.
        std::vector<ModelMaterial> materials;
        std::vector<ModelLight> lights;
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
