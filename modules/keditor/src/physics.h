// The engine's physics scripts: PhysicsSimulation, RigidBody, Collider and Joint, in C++ over kphysics, registered on every
// scene that plays as FlyCamera is. Each PhysicsSimulation steps a simulation of the world's bodies on the fixed step.
#pragma once

#include <kecs/component.h>

#include <debugDraw.h>

namespace keditor
{
    class Document;
    class PlayScene;

    /** @brief Registers the four physics script types on @p scene's document. */
    void RegisterPhysics(PlayScene& scene);

    /** @brief Whether @p entity has a Collider of the physics' own, in @p document. */
    [[nodiscard]] bool HasCollider(const Document& document, kecs::Entity entity);
    /** @brief Whether @p entity's Collider is sized by fields a gizmo can drag: a sphere, a box, a capsule or a cone. */
    [[nodiscard]] bool ColliderResizable(const Document& document, kecs::Entity entity);
    /**
     * @brief Draws @p entity's Collider where and as the physics will have it when the scene plays: from the entity's
     *        position, rotation and scale, as RigidBody's kind colours it (grey static, green dynamic, blue kinematic),
     *        seen through whatever is in front of it. A mesh's by its sharp edges.
     */
    void DrawCollider(kor::DebugDraw& debug, Document& document, kecs::Entity entity, bool editing = false);
    /**
     * @brief A scaling gizmo on @p entity's Collider: each handle dragged changes the field that sizes it that way —
     *        a box's halfExtents; a capsule's or a cone's radius across and height along; a sphere's radius, any way.
     *        Each frame of the drag is a move (Document::Moved), one change to take back when it is let go; while
     *        it plays the bodies are built again with it. Nothing for a collider of no such fields.
     * @return Whether it was dragged this frame.
     */
    bool EditCollider(kor::DebugDraw& debug, Document& document, kecs::Entity entity, const kor::Mat4& viewProjection, const kor::GizmoPointer& pointer);
}
