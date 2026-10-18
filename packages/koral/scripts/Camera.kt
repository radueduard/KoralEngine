// The engine's package. A lens on its entity, looking along its forward (-Z): what the scene is seen through.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

import koral.engine.Script

class Camera : Script() {
    var fieldOfView = 60f    // Top to bottom, in degrees.
    var near = 0.1f
    var far = 100f
}
