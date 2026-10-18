// The engine's package. Light from its entity's position along its forward (-Z), in a cone: full inside innerAngle, none past outerAngle.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

import koral.Vec3
import koral.engine.Script

class SpotLight : Script() {
    var color = Vec3(1f, 1f, 1f)
    var intensity = 3f
    var range = 8f
    var innerAngle = 20f    // From the middle of the cone to where it starts fading, in degrees.
    var outerAngle = 30f    // From the middle of the cone to its edge, in degrees.
}
