#pragma once

/**
 * @file scene.h
 * @brief What a scene is made of besides meshes: a camera, lights, a sky. Components every world a graph runs for
 *        has, which the SceneView node reads each frame and hands to shaders as two buffers.
 *
 * A component here is data and nothing else: what a thing does is a script's. Where a camera or a light is, and which way it looks or shines, is its
 * entity's transform: forward is -Z.
 */

#include <optional>

#include <kmath/matrix.h>
#include <kmath/vector.h>

#include <kecs/kecs.h>

#include "entities.h"
#include "view.h"

namespace kgraph
{
    /** @brief A lens on its entity, looking along its forward: the scene is seen through the first there is. */
    struct Camera {
        float fieldOfView = 60.f;   ///< Top to bottom, in degrees.
        float near = 0.1f;
        float far = 100.f;
    };
    /** @brief Light from far away, shining along its entity's forward. The first one is the sky's sun. */
    struct DirectionalLight {
        kor::Vec3 color { 1.f, 1.f, 1.f };
        float intensity = 1.f;
    };
    /** @brief Light from its entity's position, every way, fading out to nothing at its range. */
    struct PointLight {
        kor::Vec3 color { 1.f, 0.85f, 0.6f };
        float intensity = 2.f;
        float range = 5.f;
    };
    /** @brief Light from its entity's position along its forward, in a cone: full inside innerAngle, none past outerAngle. */
    struct SpotLight {
        kor::Vec3 color { 1.f, 1.f, 1.f };
        float intensity = 3.f;
        float range = 8.f;
        float innerAngle = 20.f;    ///< From the middle of the cone to where it starts fading, in degrees.
        float outerAngle = 30.f;    ///< From the middle of the cone to its edge, in degrees.
    };
    /**
     * @brief The sky: what is behind everything, and the light that comes from all of it — zenith from above, ground
     *        from below — in ambient measure. The sun in it is the first DirectionalLight. Without one, these defaults.
     */
    struct Sky {
        kor::Vec3 zenith { 0.22f, 0.42f, 0.78f };
        kor::Vec3 horizon { 0.72f, 0.80f, 0.90f };
        kor::Vec3 ground { 0.23f, 0.21f, 0.19f };
        float ambient = 0.35f;
        float sunSize = 1.5f;       ///< How wide the sun's disc is, in degrees.
    };
    /** @brief Makes the components above known to @p world, so that an entity can be given one and a file can name one. */
    void RegisterSceneComponents(kecs::World& world);
    /** @brief What @p entity sees, as a camera: from where it is, turned as it is. Its scale is no part of a view. */
    [[nodiscard]] View ViewOf(const kecs::World& world, kecs::Entity entity);
    /** @brief The first entity with a Camera, parents before their children; NullEntity when there is none. */
    [[nodiscard]] kecs::Entity FirstCamera(kecs::World& world);
    /** @brief A kecs matrix as the framework's, and back: the same sixteen floats. */
    [[nodiscard]] kor::Mat4 ToKor(const kecs::Mat4& matrix);
    [[nodiscard]] kecs::Mat4 ToKecs(const kor::Mat4& matrix);
}

template <> struct kecs::Name<kgraph::Camera> { static constexpr std::string_view value = "Camera"; };
template <> struct kecs::Name<kgraph::DirectionalLight> { static constexpr std::string_view value = "DirectionalLight"; };
template <> struct kecs::Name<kgraph::PointLight> { static constexpr std::string_view value = "PointLight"; };
template <> struct kecs::Name<kgraph::SpotLight> { static constexpr std::string_view value = "SpotLight"; };
template <> struct kecs::Name<kgraph::Sky> { static constexpr std::string_view value = "Sky"; };
KECS_FIELDS(kgraph::Camera, fieldOfView, near, far)
KECS_FIELDS(kgraph::DirectionalLight, color, intensity)
KECS_FIELDS(kgraph::PointLight, color, intensity, range)
KECS_FIELDS(kgraph::SpotLight, color, intensity, range, innerAngle, outerAngle)
KECS_FIELDS(kgraph::Sky, zenith, horizon, ground, ambient, sunSize)
