// The engine's package. Light from its entity's position along its forward (-Z), in a cone: full inside innerAngle, none past outerAngle.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

import koral.Vec3
import koral.engine.Logarithmic
import koral.engine.Min
import koral.engine.Range
import koral.engine.Script

class SpotLight : Setup() {
    @Min(0f) var color = Vec3(1f, 1f, 1f)
    @Min(0f) @Logarithmic var intensity = 3f
    @Min(0f) @Logarithmic var range = 8f
    @Range(0f, 90f) var innerAngle = 20f    // From the middle of the cone to where it starts fading, in degrees.
    @Range(0f, 90f) var outerAngle = 30f    // From the middle of the cone to its edge, in degrees.
}
