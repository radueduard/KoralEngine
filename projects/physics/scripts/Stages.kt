// What a step does, a stage a function, in the order Physics calls them. Each reads and writes the buffers it is given
// and nothing else: no world, no entities, nothing made. TODO(you): every body below.

import koral.Vec3

/**
 * Once, when the bodies are gathered: each dynamic body's [Bodies.inverseInertia] from its shape, its size and its
 * mass ([Bodies.inverseMass] is set already). A body that nothing moves keeps zeros.
 */
fun computeInertia(bodies: Bodies, shapes: Shapes) {
}

/** Speeds each dynamic body up by [gravity] for [dt] seconds, and slows it by its damping. */
fun applyForces(bodies: Bodies, gravity: Vec3, dt: Float) {
}

/**
 * Finds where bodies touch, into [contacts] (cleared first). A pair of shape tables at a time — plane and cube, plane
 * and sphere, ... sphere and sphere, ... mesh and mesh — each pair of rows tested by the test of those two shapes. Two
 * bodies that nothing moves need no test.
 */
fun findContacts(bodies: Bodies, shapes: Shapes, contacts: Contacts) {
    contacts.clear()
}

/** Changes the velocities of the bodies of each contact so that they stop going into each other, and bounce and slide as their colliders say. */
fun solveContacts(bodies: Bodies, contacts: Contacts, dt: Float) {
}

/** Moves each dynamic body along its velocity, and turns it by its angular velocity, for [dt] seconds. */
fun integrate(bodies: Bodies, dt: Float) {
}
