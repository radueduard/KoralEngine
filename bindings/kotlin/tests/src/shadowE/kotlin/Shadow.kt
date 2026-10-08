import koral.Vec2
import koral.engine.GraphicsScript
import koral.engine.Script

class Light : Script() {
    var slant = Vec2(0f, 0f)    // as it was: every light keeps where it leans
    var rate = 0f               // was speed: new, and zero
    var paused = false          // new
    override fun update(delta: Float) { if (!paused) slant = Vec2(slant.x, slant.y + rate) }
}

// The light, as a camera: the matrix that takes the world to where the light sees it, a column after another.
class LightCamera : GraphicsScript() {
    var reach = 0f
    override fun prepare() {
        val light = world.read<Light>(entity) ?: return
        val x = light.slant.x * reach
        val y = light.slant.y * reach
        world.buffer("light camera")?.write(floatArrayOf(1f, 0f, 0f, 0f,   0f, 1f, 0f, 0f,   x, y, -1f, 0f,   0f, 0f, 1f, 1f))
    }
}
