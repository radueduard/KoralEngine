package koral.engine

import java.nio.file.Files
import java.nio.file.Path
import kotlin.io.path.ExperimentalPathApi
import kotlin.io.path.copyToRecursively
import kotlin.io.path.deleteRecursively
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import koral.App
import koral.AppSettings
import koral.OffscreenSettings
import koral.WindowPlatform

/** Scripts in Kotlin, on a running scene, with no display. One test: an application is one per process. */
@OptIn(ExperimentalPathApi::class)
class ScriptsTest {
    private val classes: Path = Files.createTempDirectory("koral-engine-kotlin-scripts")

    /** Makes the classes folder hold the build named [which], as a compile would leave it. */
    private fun install(which: String) {
        Files.list(classes).use { files -> files.forEach { it.deleteRecursively() } }
        System.getProperty("koral.engine.test.$which").split(java.io.File.pathSeparator).map(Path::of).filter(Files::isDirectory)
            .forEach { it.copyToRecursively(classes, followLinks = false, overwrite = true) }
    }

    @Test
    fun scriptsRunAreEditedAndLoadedAgain() {
        App(AppSettings(platform = WindowPlatform.eNone)).use { app ->
            app.loadLibrary(EngineLibrary.path.toString())
            val scene = app.openOffscreen("Play", OffscreenSettings(title = "play", width = 32, height = 32))
            val world = World.of(scene) ?: error("the Play scene has no world")
            fun frames(count: Int) = repeat(count) { app.frame() }
            fun number(entity: Entity, component: String, field: String) = world.fieldJson(entity, component, field).toFloat()

            // ---- a script is a component, and its methods are called on each entity that has it ----------------
            install("a")
            val host = ScriptHost(world, classes)
            assertTrue(host.load(), "the scripts load: ${host.errors}")
            assertEquals(setOf("Feeder", "Spin", "Watcher"), host.scripts)
            assertTrue(world.hasComponentType("Spin"), "a script is a component type, by its class name")

            val wheel = world.create("Wheel")
            assertTrue(world.add(wheel, "Spin"))
            world.set(wheel, "Spin", "speed", 2)
            world.set(wheel, "Spin", "angle", 10)
            frames(3)
            assertEquals(3f, number(wheel, "Spin", "updates"), "updated once a frame")
            assertEquals(16f, number(wheel, "Spin", "angle"), "its data is the engine's: 10 + 3 * 2")
            assertEquals("true", world.fieldJson(wheel, "Spin", "started"), "started, before its first update")
            assertEquals("[0,1,0]", world.fieldJson(wheel, "Spin", "axis").replace(" ", ""), "a vector property")
            assertEquals("null", world.fieldJson(wheel, "Spin", "mine"), "a private property is not the component's")
            assertTrue("\"Spin\"" in world.toJson(), "and it is in the scene's file")

            // One script reading and writing another's, on another entity.
            val watcher = world.create("Watcher")
            world.add(watcher, "Watcher")
            frames(1)
            assertEquals(18f, number(watcher, "Watcher", "seen"), "a script reads another entity's script")
            assertEquals(118f, number(wheel, "Spin", "angle"), "and writes it")

            // Switched off: it has the script, and nothing runs.
            world.setEnabled(wheel, "Spin", false)
            frames(1)
            val off = number(wheel, "Spin", "updates")
            frames(2)
            assertEquals(off, number(wheel, "Spin", "updates"), "switched off, it is left alone")
            world.setEnabled(wheel, "Spin", true)
            frames(1)
            assertEquals(off + 1, number(wheel, "Spin", "updates"), "and switched on, it carries on")
            assertEquals(listOf(wheel), world.with("Spin"), "found by a query")

            // ---- a graphics script writes what the graph takes ---------------------------------------------------
            world.addNode("Buffer", "numbers")
            world.setInput("numbers", "instanceCount", "8")
            world.setInput("numbers", "usage", "[\"eStorage\"]")
            assertEquals(null, world.nodeProblem("numbers"), "the buffer node builds")
            val source = world.create("Source")
            world.add(source, "Feeder")
            world.set(source, "Feeder", "value", 21)
            frames(2)
            val written = world.buffer("numbers")!!.readFloats(2)
            assertEquals(21f, written[0], "what prepare wrote is in the node's buffer")
            assertEquals(42f, written[1], "all of it")
            assertTrue(number(source, "Feeder", "prepared") >= 2, "once a frame")

            // ---- a property a script cannot keep changes nothing, and is said plainly --------------------------
            install("bad")
            assertFalse(host.reload(), "a property a script cannot keep is refused")
            assertTrue(host.errors.single().let { "Spin.name" in it && "private" in it }, "saying which, and what to do: ${host.errors}")
            assertEquals(1, host.generation, "nothing was loaded")
            val before = number(wheel, "Spin", "updates")
            frames(1)
            assertEquals(before + 1, number(wheel, "Spin", "updates"), "what was running still runs")

            // ---- edited while it runs: the code is new, the data is the entity's -------------------------------
            // The watcher is set to do its work again: its class is not in the edited build, and it must not.
            world.set(watcher, "Watcher", "done", false)
            val angleBefore = number(wheel, "Spin", "angle")
            val updatesBefore = number(wheel, "Spin", "updates")
            install("b")
            assertTrue(host.reload(), "the edited scripts load: ${host.errors}")
            assertEquals(2, host.generation)
            assertEquals(setOf("Feeder", "Spin"), host.scripts, "a script whose class is gone is no longer one")
            assertEquals(angleBefore, number(wheel, "Spin", "angle"), "a property that kept its name and type kept its value")
            assertEquals(updatesBefore, number(wheel, "Spin", "updates"), "and so did this one")
            assertEquals("0", world.fieldJson(wheel, "Spin", "speed"), "one that changed type starts again")
            assertEquals("0", world.fieldJson(wheel, "Spin", "laps"), "and so does a new one")
            assertEquals("null", world.fieldJson(wheel, "Spin", "started"), "one that is gone is gone")
            frames(2)
            assertEquals(2f, number(wheel, "Spin", "laps"), "the new code runs, on the same entity")
            assertEquals(updatesBefore + 2, number(wheel, "Spin", "updates"), "counting on from where it was")
            assertEquals(angleBefore - 2, number(wheel, "Spin", "angle"), "the new update, not the old — and no script that is gone set it")
            assertEquals("false", world.fieldJson(watcher, "Watcher", "done"), "a script whose class is gone is not called")
            assertEquals(21f, world.buffer("numbers")!!.readFloats(1)[0], "a script that was not edited carries on")
            val seen = number(watcher, "Watcher", "seen")
            frames(1)
            assertEquals(seen, number(watcher, "Watcher", "seen"), "the entity keeps the data of a script that is gone")

            // ---- a script that throws is reported, and the rest carry on ---------------------------------------
            val prepared = number(source, "Feeder", "prepared")
            frames(2)       // its third update threw, a frame ago
            assertEquals(5f, number(wheel, "Spin", "laps"), "what it did before throwing is kept, and it is called again")
            assertTrue(number(source, "Feeder", "prepared") >= prepared + 2, "and the other scripts were not stopped by it")

            // ---- with a build command: a saved source is built, then loaded ------------------------------------
            host.close()
            val sources = Files.createTempDirectory("koral-engine-kotlin-sources")
            Files.writeString(sources.resolve("Scripts.kt"), "// as first written")
            install("a")
            val a = System.getProperty("koral.engine.test.a").split(java.io.File.pathSeparator).first { Files.isDirectory(Path.of(it)) }
            val b = System.getProperty("koral.engine.test.b").split(java.io.File.pathSeparator).first { Files.isDirectory(Path.of(it)) }
            // The "compiler": installs build B once the source says so, build A before, and fails when it says to.
            val script = sources.resolve("build.sh")
            Files.writeString(script, """
                if grep -q broken Scripts.kt; then echo "e: Scripts.kt:3:1 this is not Kotlin"; exit 1; fi
                rm -rf "$classes"/*
                if grep -q edited Scripts.kt; then cp -r "$b"/. "$classes"; else cp -r "$a"/. "$classes"; fi
            """.trimIndent())
            val built = ScriptHost(world, classes, sources, listOf("sh", script.toString()))
            built.pollInterval = 10
            built.settleTime = 60
            assertTrue(built.load(), "built and loaded: ${built.errors}")
            assertTrue("Watcher" in built.scripts, "the build as first written")

            fun save(text: String, until: () -> Boolean): Boolean {
                Files.writeString(sources.resolve("Scripts.kt"), text)
                repeat(300) { if (until()) return true; Thread.sleep(10); built.poll() }
                return until()
            }
            assertTrue(save("// broken") { built.errors.isNotEmpty() }, "a save that does not compile is noticed")
            assertTrue("this is not Kotlin" in built.errors.single(), "and the compiler's error is what is said: ${built.errors}")
            assertTrue("Watcher" in built.scripts, "with what was running still running")
            assertTrue(save("// edited, and longer") { "Watcher" !in built.scripts }, "a save that compiles is built and loaded")
            assertTrue(built.errors.isEmpty())
            val laps = number(wheel, "Spin", "laps")
            frames(1)
            assertEquals(laps + 1, number(wheel, "Spin", "laps"), "and runs, with what the entity had")

            built.close()
            frames(2)
            assertEquals(laps + 1, number(wheel, "Spin", "laps"), "with the host gone nothing runs, and the data is still the scene's")
        }
    }
}
