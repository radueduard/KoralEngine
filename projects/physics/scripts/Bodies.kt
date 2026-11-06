// Every body the physics steps, as one float buffer a property: body `i`'s position is `position[3 * i]`, `[3 * i + 1]`
// and `[3 * i + 2]`; its inverse mass is `inverseMass[i]`. Vectors are three floats a body and quaternions four (x, y,
// z, w), one body's after another — the layout `world.positionsOf` and `world.floatsOf` fill, so the buffers are read
// from the entities and written back to them as they are, with nothing in between.
//
// Made once, the size of the bodies there are; nothing is made while it steps.

class Bodies {
    var count = 0
        private set

    // Where it is, how it is turned, and how it moves.
    var position = FloatArray(0)            // 3 a body: its centre, in the world.
    var orientation = FloatArray(0)         // 4 a body: x, y, z, w.
    var velocity = FloatArray(0)            // 3 a body: metres a second.
    var angularVelocity = FloatArray(0)     // 3 a body: radians a second, about the world's axes.

    // How it answers a push. 0 for a body that nothing moves: a static or a kinematic one.
    var inverseMass = FloatArray(0)         // 1 a body.
    var inverseInertia = FloatArray(0)      // 3 a body: the diagonal of its inverse inertia, about its own axes.

    // How it loses speed, a second.
    var linearDamping = FloatArray(0)       // 1 a body.
    var angularDamping = FloatArray(0)      // 1 a body.

    // What its collider is made of.
    var friction = FloatArray(0)            // 1 a body.
    var restitution = FloatArray(0)         // 1 a body.

    /** Room for [bodies] bodies, every property zero. */
    fun resize(bodies: Int) {
        count = bodies
        position = FloatArray(bodies * 3)
        orientation = FloatArray(bodies * 4)
        velocity = FloatArray(bodies * 3)
        angularVelocity = FloatArray(bodies * 3)
        inverseMass = FloatArray(bodies)
        inverseInertia = FloatArray(bodies * 3)
        linearDamping = FloatArray(bodies)
        angularDamping = FloatArray(bodies)
        friction = FloatArray(bodies)
        restitution = FloatArray(bodies)
    }

    /** Whether body [i] is moved by the physics: it has a mass to push. */
    fun isDynamic(i: Int) = inverseMass[i] > 0f
}
