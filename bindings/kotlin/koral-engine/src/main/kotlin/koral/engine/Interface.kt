// What a script shows over the game: an interface in Compose, drawn wherever the game is seen — the player's window, the
// editor's Game panel — and given that place's keys and mouse. While the pointer is over it, `input.interfaceWantsMouse`
// says so, and a script that steers by the mouse leaves it be.
//
//     class Hud : Setup(), PerFrame {
//         private var shown: GameInterface? = null
//         override fun initialize() { shown = world.setContent { Text("Hello") } }
//         override fun onDestroyed() { shown?.close() }
//         override fun update(delta: Float) {}
//     }

package koral.engine

import androidx.compose.runtime.Composable
import java.lang.foreign.Arena
import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.invoke.MethodHandles
import koral.compose.ComposeUi
import koral.compose.KoralDarkTheme
import koral.compose.Theme
import koral.interop.Native as Koral

/**
 * An interface shown over the game ([World.setContent]): close it — a script's `onDestroyed` — to take it away. Its
 * [ui] is the Compose interface itself: its theme, and what it says of the pointer and keyboard.
 */
class GameInterface internal constructor(private val world: World, val ui: ComposeUi) : AutoCloseable {
    private val key = synchronized(shown) { ++next }
    private var closed = false

    init {
        synchronized(shown) { shown[key] = this }
        Native.kengine_interface_show.invokeExact(world.document, ui.handle, compose, MemorySegment.ofAddress(key))
    }

    /** Whether the pointer is over it, or text is being typed into it: what a script that steers by the mouse leaves be. */
    val wantsPointer: Boolean get() = !closed && ui.wantsPointer

    override fun close() {
        if (closed) return
        closed = true
        // Shown no more before it goes: nothing reaches it after.
        Native.kengine_interface_hide.invokeExact(world.document, ui.handle)
        synchronized(shown) { shown.remove(key) }
        ui.close()
    }

    internal companion object {
        private val shown = HashMap<Long, GameInterface>()
        private var next = 0L

        /** Called by the engine each frame, before it gives the interface its input: what changed, composed again. */
        @JvmStatic fun composeAgain(user: MemorySegment) {
            val shown = synchronized(shown) { shown[user.address()] } ?: return
            try { shown.ui.compose() } catch (e: Throwable) { koral.Log.error("[interface] composing failed: $e") }
        }

        private val compose: MemorySegment by lazy {
            val descriptor = FunctionDescriptor.ofVoid(ADDRESS)
            Koral.linker.upcallStub(MethodHandles.lookup().findStatic(Companion::class.java.declaringClass, "composeAgain", descriptor.toMethodType()),
                                    descriptor, Arena.global())
        }
    }
}

/**
 * Shows [content] over the game, wherever it is seen, until the [GameInterface] made is closed: as Android's
 * `setContent` does, for a scene of the engine's. In [theme], [scale] times as big as it is written.
 */
fun World.setContent(theme: Theme = KoralDarkTheme, scale: Float = 1f, content: @Composable () -> Unit): GameInterface =
    GameInterface(this, ComposeUi(theme, scale, content))
