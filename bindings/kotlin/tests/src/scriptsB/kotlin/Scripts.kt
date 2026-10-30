import koral.engine.BeforeDraw
import koral.engine.Setup
import koral.engine.Script

// Spin, edited. Watcher is gone.
class Spin : Script() {
    var angle = 0f              // as it was: kept
    var speed = 0.0             // a Double now: reset
    var updates = 0             // as it was: kept
    var laps = 0                // new: zero
    // started, axis and target are gone

    override fun update(delta: Float) {
        angle -= 1f; ++updates; ++laps
        if (laps == 3) throw IllegalStateException("on purpose")    // what it did before throwing is kept
    }
}

class Feeder : Setup(), BeforeDraw {
    var value = 0f
    var prepared = 0
    override fun prepare() {
        ++prepared
        world.buffer("numbers")?.write(floatArrayOf(value, value * 2))
    }
}
