// The engine's package. A lens on its entity, looking along its forward (-Z): what the scene is seen through.
// A kind: no entity has a Camera itself, but a PerspectiveCamera or an OrthographicCamera — and the graph finds either by
// this name, with the near and far every camera has. The first entity that is a Camera is the scene's.

import koral.engine.Data
import koral.engine.Logarithmic
import koral.engine.Min

abstract class Camera : Data() {
    @Min(0.001f) @Logarithmic var near = 0.1f
    @Min(0f) var far = 100f                     // A perspective's 0: as far as there is — no far plane.
}
