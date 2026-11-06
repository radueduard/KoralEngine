// A body the physics moves, or that others collide with: how it moves, how heavy it is, and how fast it goes.
// Data and nothing else: Physics reads every RigidBody at once, steps them, and writes where they went.

import koral.Vec3
import koral.engine.Choices
import koral.engine.Data
import koral.engine.Logarithmic
import koral.engine.Min

class RigidBody : Data() {
    // Static never moves; Dynamic is moved by the physics; Kinematic is moved by a script, and pushes what it meets.
    @Choices("Static", "Dynamic", "Kinematic") var motion = 1
    @Min(0.001f) @Logarithmic var mass = 1f             // In kilograms. A static or kinematic body's is not used.
    @Min(0f) var linearDamping = 0.01f                  // How much of its velocity it loses a second.
    @Min(0f) var angularDamping = 0.05f                 // How much of its spin it loses a second.
    var velocity = Vec3(0f, 0f, 0f)                     // In metres a second: what it starts with, and what it has.
    var angularVelocity = Vec3(0f, 0f, 0f)              // In radians a second, about each axis.
}
