#pragma once

/**
 * @file animation.h
 * @brief A model's clips played on the entities its nodes were made as, and a skinned mesh bent by them.
 *
 * A model is put in a scene as an entity for each of its nodes, under one for the whole (Document::BuildModel). A
 * clip moves nodes: an Animator, on the entity for the whole, plays one of its model's clips on them — each node's
 * entity found under it by name, as it was made — and fades from one to the next. A mesh of the model's that is
 * skinned is drawn bent by its joints, which are some of those entities: its Skin names them, and the runtime works
 * out each joint's matrix every frame, for a shader that marks a buffer `[entity("JOINT_MATRICES")]` and an instance's
 * field `[entity("JOINTS")]` (the animation module's KoralSkinLinear).
 */

#include <kgraph/assets.h>

#include <kecs/world.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kgraph
{
    /** @brief The entities a model's nodes were made as, by node: found under the entity for the whole. */
    struct NodeEntities {
        kecs::Entity root;
        std::vector<kecs::Entity> nodes;        ///< NullEntity for one not found
    };

    /**
     * @brief What bends a drawn entity's mesh: skin `skin` of `model` (`@fox.gltf`), whose joints are nodes of the model
     *        found under `root`, the entity for the whole.
     */
    struct Skin {
        std::string model;
        int skin = 0;
        kecs::Entity root;
        mutable std::shared_ptr<NodeEntities> found;    ///< not saved: what was found, while it still is
    };

    /**
     * @brief Plays `clip` — by its name, `#2` by its number, or empty for the first — of `model`'s on the nodes under its
     *        entity: `time` seconds in, going on at `speed`, again from the start when it ends while `loop`. Given
     *        another clip, it starts it from the beginning and fades from the one before over `fade` seconds. `length`
     *        is how long the clip is, once its model is read.
     */
    struct Animator {
        std::string model;
        std::string clip;
        float time = 0.f;
        float speed = 1.f;
        float fade = 0.2f;
        bool loop = true;
        bool paused = false;
        float length = 0.f;
        // Not saved: what it played last, what it fades from, and the nodes.
        std::string shown;
        bool started = false;
        int from = -1;
        float fromTime = 0.f, fading = 0.f, fadeFor = 0.f;
        std::shared_ptr<NodeEntities> found;
    };

    /** @brief How a model is found by its file's name: what a runtime has read of it, null while it is read. */
    using ModelSource = std::function<std::shared_ptr<const Model>(const std::string& file)>;

    /** @brief The clip @p name names: by its name, `#n` by its number, or empty for the first. -1 for none. */
    [[nodiscard]] int ClipOf(const Model& model, std::string_view name);

    /**
     * @brief The entities @p model 's nodes were made as under @p root, as Document::BuildModel makes them: each the
     *        child of its parent's (or of @p root) of its name. @p known is what was found before, kept while every one
     *        of it is alive and under the same root.
     */
    [[nodiscard]] std::shared_ptr<NodeEntities> FindNodes(const kecs::World& world, kecs::Entity root, const Model& model, std::shared_ptr<NodeEntities> known = {});

    /**
     * @brief Moves every Animator's clip on by @p delta seconds and puts the nodes it moves where the clip has them
     *        then. What is not read yet is left as it is.
     */
    void Animate(kecs::World& world, const ModelSource& models, float delta);

    /**
     * @brief The matrices that bend @p skinned 's mesh, a joint each in the skin's order: each joint where it is now,
     *        after its inverse bind, in @p skinned 's own space (what its MATRIX places). False, and nothing added, when
     *        the model is not read yet or has no such skin.
     */
    bool JointMatrices(const kecs::World& world, kecs::Entity skinned, const Skin& skin, const Model& model, std::vector<kecs::Mat4>& out);
}

template <> struct kecs::Name<kgraph::Skin> { static constexpr std::string_view value = "Skin"; };
template <> struct kecs::Name<kgraph::Animator> { static constexpr std::string_view value = "Animator"; };

template <> struct kecs::Fields<kgraph::Skin> {
    static void Describe(kecs::FieldList<kgraph::Skin>& list)
    {
        list.Field("model", &kgraph::Skin::model).Field("skin", &kgraph::Skin::skin).Field("root", &kgraph::Skin::root);
    }
};
template <> struct kecs::Fields<kgraph::Animator> {
    static void Describe(kecs::FieldList<kgraph::Animator>& list)
    {
        list.Field("model", &kgraph::Animator::model).Field("clip", &kgraph::Animator::clip).Field("time", &kgraph::Animator::time)
            .Field("speed", &kgraph::Animator::speed).Field("fade", &kgraph::Animator::fade).Field("loop", &kgraph::Animator::loop)
            .Field("paused", &kgraph::Animator::paused).Field("length", &kgraph::Animator::length);
    }
};
