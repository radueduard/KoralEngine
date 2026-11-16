#include <kgraph/animation.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <cmath>
#include <optional>

namespace kgraph
{
    int ClipOf(const Model& model, const std::string_view name)
    {
        if (model.animations.empty()) return -1;
        if (name.empty()) return 0;
        for (std::size_t i = 0; i < model.animations.size(); ++i)
            if (model.animations[i].name == name) return static_cast<int>(i);
        if (name.starts_with('#')) {
            int n = -1;
            const auto [end, failed] = std::from_chars(name.data() + 1, name.data() + name.size(), n);
            if (failed == std::errc {} && end == name.data() + name.size() && n >= 0 && n < static_cast<int>(model.animations.size())) return n;
        }
        return -1;
    }

    std::shared_ptr<NodeEntities> FindNodes(const kecs::World& world, const kecs::Entity root, const Model& model, std::shared_ptr<NodeEntities> known)
    {
        const auto still = [&](const NodeEntities& found) {
            if (found.root != root || found.nodes.size() != model.nodes.size()) return false;
            for (std::size_t i = 0; i < found.nodes.size(); ++i) {
                const kecs::Entity entity = found.nodes[i];
                if (entity.IsNull()) continue;
                if (!world.Alive(entity)) return false;
                const int parent = model.nodes[i].parent;
                const kecs::Entity above = parent >= 0 ? found.nodes[static_cast<std::size_t>(parent)] : root;
                if (world.ParentOf(entity) != above) return false;
            }
            return true;
        };
        if (known && still(*known)) return known;
        auto found = std::make_shared<NodeEntities>();
        found->root = root;
        found->nodes.assign(model.nodes.size(), kecs::NullEntity);
        // Parents come before their children: each is the child of its parent's entity so called — the first not taken
        // already, as two of one name are made one after the other.
        std::vector<kecs::Entity> taken;
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const ModelNode& node = model.nodes[i];
            const kecs::Entity above = node.parent >= 0 ? found->nodes[static_cast<std::size_t>(node.parent)] : root;
            if (above.IsNull()) continue;
            const std::string name = node.name.empty() ? std::format("Node {}", i) : node.name;
            for (const kecs::Entity child : world.ChildrenOf(above))
                if (world.NameOf(child) == name && !std::ranges::contains(taken, child)) {
                    found->nodes[i] = child;
                    taken.push_back(child);
                    break;
                }
        }
        return found;
    }

    namespace {
        struct Pose {
            kecs::Vec3 position;
            kecs::Quat rotation;
            kecs::Vec3 scale { 1.f, 1.f, 1.f };
            bool moved = false;     // a clip keys some of it
        };

        /** Where in @p times @p t falls: the key before it, the one after, and how far between. */
        struct Between { std::size_t a = 0, b = 0; float f = 0.f; };
        Between Find(const std::vector<float>& times, const float t)
        {
            if (times.size() < 2 || t <= times.front()) return {};
            if (t >= times.back()) return { times.size() - 1, times.size() - 1, 0.f };
            const auto after = static_cast<std::size_t>(std::ranges::upper_bound(times, t) - times.begin());
            const std::size_t before = after - 1;
            const float span = times[after] - times[before];
            return { before, after, span > 0.f ? (t - times[before]) / span : 0.f };
        }

        kecs::Vec3 Lerp(const kecs::Vec3& a, const kecs::Vec3& b, const float f) { return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f }; }

        kecs::Quat Slerp(const kecs::Quat& a, kecs::Quat b, const float f)
        {
            float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
            if (cosine < 0.f) { cosine = -cosine; b = { -b.x, -b.y, -b.z, -b.w }; }    // the shorter way round
            float wa = 1.f - f, wb = f;
            if (cosine < 0.9995f) {
                const float angle = std::acos(cosine), sine = std::sin(angle);
                wa = std::sin((1.f - f) * angle) / sine;
                wb = std::sin(f * angle) / sine;
            }
            return kecs::Normalize({ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb });
        }

        kecs::Vec3 Three(const std::vector<float>& values, const std::size_t key) { return { values[key * 3], values[key * 3 + 1], values[key * 3 + 2] }; }
        kecs::Quat Four(const std::vector<float>& values, const std::size_t key) { return { values[key * 4], values[key * 4 + 1], values[key * 4 + 2], values[key * 4 + 3] }; }

        /** Each node as it stands before any clip: as the model has it. */
        std::vector<Pose> Rest(const Model& model)
        {
            std::vector<Pose> rest(model.nodes.size());
            for (std::size_t i = 0; i < model.nodes.size(); ++i) {
                const ModelNode& node = model.nodes[i];
                rest[i].position = { node.position[0], node.position[1], node.position[2] };
                rest[i].rotation = kecs::ToQuat({ node.rotation[0], node.rotation[1], node.rotation[2] });
                rest[i].scale = { node.scale[0], node.scale[1], node.scale[2] };
            }
            return rest;
        }

        /** @p clip at @p t seconds, over @p pose: what it keys of each node, the rest as it was. */
        void Sample(const ModelAnimation& clip, const float t, std::vector<Pose>& pose)
        {
            for (const auto& channel : clip.channels) {
                if (channel.node < 0 || static_cast<std::size_t>(channel.node) >= pose.size()) continue;
                Pose& node = pose[static_cast<std::size_t>(channel.node)];
                node.moved = true;
                if (!channel.positionTimes.empty() && channel.positions.size() >= channel.positionTimes.size() * 3) {
                    const auto [a, b, f] = Find(channel.positionTimes, t);
                    node.position = Lerp(Three(channel.positions, a), Three(channel.positions, b), f);
                }
                if (!channel.rotationTimes.empty() && channel.rotations.size() >= channel.rotationTimes.size() * 4) {
                    const auto [a, b, f] = Find(channel.rotationTimes, t);
                    node.rotation = Slerp(Four(channel.rotations, a), Four(channel.rotations, b), f);
                }
                if (!channel.scaleTimes.empty() && channel.scales.size() >= channel.scaleTimes.size() * 3) {
                    const auto [a, b, f] = Find(channel.scaleTimes, t);
                    node.scale = Lerp(Three(channel.scales, a), Three(channel.scales, b), f);
                }
            }
        }

        /** Where a clip of @p length is @p t seconds in, played on by @p step: round again, or held at its end. */
        float Advance(const float t, const float step, const float length, const bool loop)
        {
            const float next = t + step;
            if (length <= 0.f) return 0.f;
            if (loop) { const float in = std::fmod(next, length); return in < 0.f ? in + length : in; }
            return std::clamp(next, 0.f, length);
        }
    }

    void Animate(kecs::World& world, const ModelSource& models, const float delta)
    {
        world.Query<kecs::Mut<Animator>>().Each([&](const kecs::Entity entity, const kecs::Mut<Animator> mut) {
            Animator& animator = mut.Write();
            if (animator.model.empty()) return;
            const auto model = models(animator.model);
            if (!model) return;
            const int clip = ClipOf(*model, animator.clip);
            if (clip < 0) return;
            const ModelAnimation& playing = model->animations[static_cast<std::size_t>(clip)];
            animator.length = playing.duration;
            // Another clip than the one shown: from its start, faded into from the one before.
            if (!animator.started || animator.shown != animator.clip) {
                const int before = animator.started ? ClipOf(*model, animator.shown) : -1;
                animator.from = before != clip ? before : -1;
                animator.fromTime = animator.time;
                animator.fadeFor = animator.fading = animator.from >= 0 ? std::max(animator.fade, 0.f) : 0.f;
                if (animator.started) animator.time = 0.f;
                animator.shown = animator.clip;
                animator.started = true;
            }
            const float step = animator.paused ? 0.f : delta * animator.speed;
            animator.time = Advance(animator.time, step, playing.duration, animator.loop);

            animator.found = FindNodes(world, entity, *model, std::move(animator.found));
            std::vector<Pose> pose = Rest(*model);
            Sample(playing, animator.time, pose);
            // A fade goes on with time, played or paused: how far into it this frame is, once it has gone on.
            if (animator.fading > 0.f) animator.fading = std::max(animator.fading - std::max(std::abs(step), delta), 0.f);
            if (animator.fading > 0.f && animator.from >= 0 && static_cast<std::size_t>(animator.from) < model->animations.size()) {
                const ModelAnimation& before = model->animations[static_cast<std::size_t>(animator.from)];
                animator.fromTime = Advance(animator.fromTime, step, before.duration, animator.loop);
                std::vector<Pose> was = Rest(*model);
                Sample(before, animator.fromTime, was);
                const float into = animator.fadeFor > 0.f ? 1.f - animator.fading / animator.fadeFor : 1.f;
                for (std::size_t i = 0; i < pose.size(); ++i) {
                    if (!pose[i].moved && !was[i].moved) continue;
                    pose[i].position = Lerp(was[i].position, pose[i].position, into);
                    pose[i].rotation = Slerp(was[i].rotation, pose[i].rotation, into);
                    pose[i].scale = Lerp(was[i].scale, pose[i].scale, into);
                    pose[i].moved = true;
                }
            }
            for (std::size_t i = 0; i < pose.size(); ++i) {
                const kecs::Entity node = animator.found->nodes[i];
                if (!pose[i].moved || node.IsNull()) continue;
                world.SetPosition(node, pose[i].position);
                world.SetOrientation(node, pose[i].rotation);
                world.SetScale(node, pose[i].scale);
            }
        });
    }

    bool JointMatrices(const kecs::World& world, const kecs::Entity skinned, const Skin& skin, const Model& model, std::vector<kecs::Mat4>& out)
    {
        if (skin.skin < 0 || static_cast<std::size_t>(skin.skin) >= model.skins.size() || skin.root.IsNull() || !world.Alive(skin.root)) return false;
        skin.found = FindNodes(world, skin.root, model, std::move(skin.found));
        const ModelSkin& of = model.skins[static_cast<std::size_t>(skin.skin)];
        const kecs::Mat4 into = kecs::Inverse(world.WorldMatrix(skinned));
        for (const auto& joint : of.joints) {
            kecs::Mat4 bind;
            std::copy(std::begin(joint.inverseBind), std::end(joint.inverseBind), bind.m.begin());
            const kecs::Entity node = joint.node >= 0 && static_cast<std::size_t>(joint.node) < skin.found->nodes.size() ? skin.found->nodes[static_cast<std::size_t>(joint.node)] : kecs::NullEntity;
            // A joint not found is where it was bound: the mesh as it is.
            out.push_back(node.IsNull() ? kecs::Mat4 {} : into * world.WorldMatrix(node) * bind);
        }
        return true;
    }
}
