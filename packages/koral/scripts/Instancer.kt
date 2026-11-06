// The engine's package. Many of its entity's Mesh — up to hundreds of millions — scattered over a square under it, made
// on the GPU and never one by one on the CPU: no entity each. Near, each is its mesh; far, a point of `color`.
// Data and nothing else: the engine's Instances, CullInstances and SplatPoints functions make, cull and draw them.

import koral.Vec2
import koral.Vec3
import koral.engine.Logarithmic
import koral.engine.Min
import koral.engine.Range
import koral.engine.Data

class Instancer : Data() {
    @Min(0f) var count = 100000             // How many. Each is 20 bytes on the GPU: 100 million is 2 GB.
    @Min(0f) var extent = Vec2(100f, 100f)  // The square they are scattered over, along x and z, centred on the entity.
    var seed = 1                            // Another number, another scattering.
    @Min(0f) var scale = Vec2(0.5f, 1.5f)   // How big each is: from, to.
    @Range(0f, 1f) var jitter = 1f          // How far each strays in its cell: 0 a grid, 1 anywhere in it.
    @Min(0f) @Logarithmic var pointDistance = 60f       // Past this, a point rather than its mesh.
    @Min(0f) @Logarithmic var drawDistance = 100000f    // Past this, nothing.
    @Min(0f) var color = Vec3(0.35f, 0.55f, 0.25f)      // What the points are drawn in.
}
