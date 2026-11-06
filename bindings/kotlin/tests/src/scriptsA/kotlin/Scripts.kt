import koral.Vec3
import koral.engine.ResourceScript
import koral.engine.Entity
import koral.engine.BeforeDraw
import koral.engine.PerFrame
import koral.engine.Reinitialize
import koral.engine.Setup
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

/** Starts over when its size is changed, and not for what else changes. */
class Restarter : Setup(), PerFrame {
    @Reinitialize var size = 0
    var other = 0
    var made = 0                // how many times initialize has run
    var gone = 0
    var madeOf = 0
    var kept = 0                // what this object has seen: new when it starts over
    private var frames = 0

    override fun initialize() { ++made; madeOf = size }
    override fun onDestroyed() { world.set(entity, "Restarter", "gone", gone + 1) }      // what is held is not stored back after this
    override fun update(delta: Float) { kept = ++frames; if (other == 7) size = 99 }
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
class Feeder : Setup(), BeforeDraw {
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

/** A mesh made of its fields: made again when they change, and shared by every entity of the same ones. */
class Pyramid : koral.engine.Setup(), koral.engine.MeshGenerator {
    var height = 1f
    override fun initialize(mesh: koral.engine.MeshWriter) {
        mesh.positions(floatArrayOf(-0.5f, 0f, -0.5f, 0.5f, 0f, -0.5f, 0.5f, 0f, 0.5f, -0.5f, 0f, 0.5f, 0f, height, 0f))
        mesh.indices(intArrayOf(0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0, 0, 1, 2, 0, 2, 3))
    }
}

/** A picture made of its fields, in its entity's material: made again when they change. */
class Checker : koral.engine.Setup(), koral.engine.ImageGenerator {
    var squares = 2
    override fun initialize(image: koral.engine.ImageWriter) {
        image.size(8, 8)
        for (y in 0 until image.height) for (x in 0 until image.width) {
            val white = (x * squares / image.width + y * squares / image.height) % 2 == 0
            image.set(x, y, if (white) 1f else 0f, if (white) 1f else 0f, if (white) 1f else 0f)
        }
    }
}
