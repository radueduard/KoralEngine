// The engine's package. Light: how much of it, and of what colour. A kind — no entity has a Light itself, but one of
// what derives from it — so that a query for Light finds every light, the colour and intensity read from whichever it is.

import koral.Vec3
import koral.engine.Data
import koral.engine.Logarithmic
import koral.engine.Min

abstract class Light : Data() {
    @Min(0f) var color = Vec3(1f, 1f, 1f)
    @Min(0f) @Logarithmic var intensity = 1f
}
