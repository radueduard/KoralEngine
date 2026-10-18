// The engine's package. The sky: what is behind everything, and the light that comes from all of it — zenith from above, ground from below — in ambient measure.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

import koral.Vec3
import koral.engine.Script

class Sky : Script() {
    var zenith = Vec3(0.22f, 0.42f, 0.78f)
    var horizon = Vec3(0.72f, 0.8f, 0.9f)
    var ground = Vec3(0.23f, 0.21f, 0.19f)
    var ambient = 0.35f
    var sunSize = 1.5f    // How wide the sun's disc is, in degrees.
}
