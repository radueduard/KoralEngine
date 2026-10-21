import koral.Vec3
import koral.engine.ResourceScript
import koral.engine.Entity
import koral.engine.GraphicsScript
import koral.engine.Script

class Spin : Script() {
    var angle = 0f
    var speed = 99f             // an initialiser is the class's, not the component's: a new component starts at zero
    var updates = 0
    var started = false
    var axis = Vec3(0f, 0f, 0f)
    var target = Entity.Null
    private var mine = 0        // this run's only

    override fun start() { started = true; axis = Vec3(0f, 1f, 0f) }
    override fun update(delta: Float) { angle += speed; ++updates; ++mine }
}

/** Reads the Spin of the first entity that has one, and sets it turning from 118: once. */
class Watcher : Script() {
    var seen = 0f
    var done = false
    override fun lateUpdate(delta: Float) {
        if (done) return
        for (entity in world.with("Spin")) {
            val spin = world.read<Spin>(entity)!!
            seen = spin.angle
            spin.angle = 118f
            world.write(entity, spin)
            done = true
        }
    }
}

/** The graphics half: what a pass reads, written from an entity's data before anything is drawn. */
class Feeder : GraphicsScript() {
    var value = 0f
    var prepared = 0
    override fun prepare() {
        ++prepared
        world.buffer("numbers")?.write(floatArrayOf(value, value * 2))
    }
}

/**
 * Makes a buffer of a number each, a step apart, an image a step brighter a row, a view of it and a sampler. A Script
 * node of the graph runs it, with its settings, and gives what it makes.
 */
class Ramp : ResourceScript() {
    var step = 1f
    val table = Buffer(instanceCount = 32, usage = "eStorage")
    val rows = Image(format = "eR32_SFLOAT", width = 4, height = 8, usage = "eSampled | eTransferSrc | eTransferDst")
    val rowsView = ImageView(image = ::rows.name)
    val nearest = Sampler(minFilter = "eNearest", magFilter = "eNearest")
    override fun init() {
        for (i in 0 until table.bytes.capacity() / 4) table.bytes.putFloat(i * 4, i * step)
        for (y in 0 until rows.height) for (x in 0 until rows.width) rows.bytes.putFloat((y * rows.width + x) * 4, y * step)
    }
}
