// The engine's package. A lens on its entity, looking along its forward (-Z): what the scene is seen through.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

import koral.engine.Logarithmic
import koral.engine.Min
import koral.engine.Range
import koral.engine.Script

class Camera : Script() {
    @Range(1f, 179f) var fieldOfView = 60f    // Top to bottom, in degrees.
    @Min(0.001f) @Logarithmic var near = 0.1f
    @Min(0.01f) @Logarithmic var far = 100f
}
