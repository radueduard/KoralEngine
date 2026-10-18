// The engine's package. Light from far away, shining along its entity's forward (-Z): a sun.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

import koral.Vec3
import koral.engine.Script

class DirectionalLight : Script() {
    var color = Vec3(1f, 1f, 1f)
    var intensity = 1f
}
