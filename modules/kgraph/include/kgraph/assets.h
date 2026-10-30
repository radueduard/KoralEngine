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

#include <kasset/model.h>

namespace kgraph
{
    using kasset::ModelMaterial;
    using kasset::ModelNode;
    using kasset::ModelLight;
    using kasset::ModelCamera;
    using kasset::Model;
}
