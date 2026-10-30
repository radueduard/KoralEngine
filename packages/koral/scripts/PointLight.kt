// The engine's package. Light from its entity's position, every way, fading out to nothing at its range.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

import koral.Vec3
import koral.engine.Logarithmic
import koral.engine.Min
import koral.engine.Range
import koral.engine.Script

class PointLight : Setup() {
    @Min(0f) var color = Vec3(1f, 0.85f, 0.6f)
    @Min(0f) @Logarithmic var intensity = 2f
    @Min(0f) @Logarithmic var range = 5f
}
