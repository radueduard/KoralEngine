// What a body is shaped like, about its entity's position and turned as it is. Each is a Collider: a kind, so that
// `world.with("RigidBody", "Collider")` finds every body whatever its shape, and friction and restitution are read
// from whichever it has. Sizes are before the entity's scale.

import koral.Vec3
import koral.engine.Data
import koral.engine.Logarithmic
import koral.engine.Min
import koral.engine.Range

abstract class Collider : Data() {
    @Range(0f, 2f) var friction = 0.5f                  // How much it holds against sliding: 0 ice, 1 rubber.
    @Range(0f, 1f) var restitution = 0.2f               // How much of its speed it bounces back with.
}

/** An endless flat ground: through its entity's position, facing its up (+Y). Never moves, whatever its body says. */
class PlaneCollider : Collider() {
    // A plane is its place and its facing, both its entity's: nothing more to say.
}

class CubeCollider : Collider() {
    @Min(0.001f) var halfExtents = Vec3(0.5f, 0.5f, 0.5f)       // Half its size along each of its axes.
}

class SphereCollider : Collider() {
    @Min(0.001f) var radius = 0.5f
}

/** Round about its Y axis, flat at both ends. */
class CylinderCollider : Collider() {
    @Min(0.001f) var radius = 0.5f
    @Min(0.001f) var height = 1f                        // From one flat end to the other.
}

/** A cylinder along its Y axis with half a sphere on each end. */
class CapsuleCollider : Collider() {
    @Min(0.001f) var radius = 0.5f
    @Min(0f) var height = 1f                            // Of the cylinder between the two ends: 0 is a sphere.
}

/** The triangles of its entity's Mesh. */
class MeshCollider : Collider() {
    // The shape is the entity's Mesh: nothing more to say here.
}
