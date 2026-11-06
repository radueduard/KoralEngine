// The engine's package. Light from its entity's position along its forward (-Z), in a cone: full inside innerAngle, none past outerAngle.
// Data and nothing else: what a light does is the graph's, which reads it through a node of its name.

import koral.engine.Range

class SpotLight : LocalLight() {
    @Range(0f, 90f) var innerAngle = 20f    // From the middle of the cone to where it starts fading, in degrees.
    @Range(0f, 90f) var outerAngle = 30f    // From the middle of the cone to its edge, in degrees.
}
