package koral.engine

import java.nio.file.Files
import java.nio.file.Path
import kotlin.io.path.ExperimentalPathApi
import kotlin.io.path.copyToRecursively
import kotlin.io.path.deleteRecursively
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertTrue
import koral.App
import koral.AppSettings
import koral.OffscreenSettings
import koral.Resource
import koral.Vec3
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
            // Set as whatever the field is: a vector, an entity, none.
            world.set(wheel, "Spin", "axis", Vec3(1f, 0.5f, 0f))
            assertEquals("[1,0.5,0]", world.fieldJson(wheel, "Spin", "axis").replace(" ", ""), "a vector, set whole")
            val hub = world.create("Hub")
            world.set(wheel, "Spin", "target", hub)
            assertTrue("\"entity\":${hub.index}" in world.fieldJson(wheel, "Spin", "target").replace(" ", ""), "an entity: ${world.fieldJson(wheel, "Spin", "target")}")
            world.set(wheel, "Spin", "target", null)
            assertEquals("null", world.fieldJson(wheel, "Spin", "target").replace(" ", ""), "and none")
            world.destroy(hub)

            // Many at once: three floats an entity, in two calls whatever the number.
            val many = Entities.of(List(3) { world.create("Many $it") })
            assertEquals(3, world.setRotations(many, floatArrayOf(0f, 10f, 0f, 0f, 20f, 0f, 0f, 30f, 0f)), "every one of them set")
            assertEquals(20f, world.rotationOf(many[1]).y, "each its own")
            val turned = world.rotationsOf(many)
            assertEquals(listOf(10f, 20f, 30f), listOf(turned[1], turned[4], turned[7]), "and read back in order")
            assertEquals(1f, world.scalesOf(many)[0], "a scale that was never set is one")
            world.destroy(many[2])
            assertEquals(2, world.setPositions(many, FloatArray(9) { 1f }), "one that is gone is left out")
            assertEquals(0f, world.positionsOf(many, turned)[6], "and reads as nothing")
            assertTrue(world.positionsOf(many, turned) === turned, "the array given back is the one passed in")
            // Turned by quaternions, four floats an entity: kept as given, made length 1, the angles worked out of them.
            val h = kotlin.math.sqrt(0.5f)
            assertEquals(2, world.setOrientations(many, floatArrayOf(0f, 1f, 0f, 1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f)), "the two alive")
            val quats = world.orientationsOf(many)
            assertEquals(h, quats[1], 1e-6f, "made length 1")
            assertEquals(1f, quats[11], "the identity, for one that is gone")
            assertEquals(90f, world.rotationOf(many[0]).y, 0.05f, "and the angles follow")
            assertTrue(world.setOrientation(many[1], koral.Quat(h, 0f, 0f, h)))
            assertEquals(h, world.orientationOf(many[1]).x, 1e-6f)
            val drawn = world.debug.lineCount
            world.debug.line(koral.Vec3(0f, 0f, 0f), koral.Vec3(1f, 0f, 0f))
            assertTrue(world.debug.lineCount > drawn, "a script draws with the scene's debug lines")
            many.take(2).forEach { world.destroy(it) }

            // A field of many at once, as numbers: a Vec3 is three an entity, true or false one.
            val two = Entities.of(wheel, world.create("No Spin"))
            assertEquals(3, world.fieldWidth("Spin", "axis"))
            assertEquals(0, world.fieldWidth("Spin", "nothing"), "a field there is not")
            val axisBefore = world.floatsOf(two, "Spin", "axis")
            assertEquals(1, world.setFloats(two, "Spin", "axis", floatArrayOf(1f, 2f, 3f, 4f, 5f, 6f)), "only the one with a Spin")
            assertEquals("[1,2,3]", world.fieldJson(wheel, "Spin", "axis").replace(" ", ""), "written where it is")
            assertEquals(listOf(1f, 2f, 3f, 0f, 0f, 0f), world.floatsOf(two, "Spin", "axis").toList(), "and nothing for one without it")
            assertEquals(listOf(1, 0), world.intsOf(two, "Spin", "started").toList(), "true or false as 1 and 0")
            world.setFloats(two, "Spin", "axis", axisBefore)
            world.destroy(two[1])
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

            // ---- a script fills a resource as it is made --------------------------------------------------------
            assertEquals(setOf("Ramp"), host.resourceScripts, "a script that makes resources is one")
            assertFalse(world.hasComponentType("Ramp"), "and no component")
            world.addNode("Script", "ramp")
            world.setInput("ramp", "script", "\"Ramp\"")
            world.setInput("ramp", "field.step", "0.25")
            assertEquals(null, world.nodeProblem("ramp"), "what it makes is made")
            frames(1)
            assertEquals(0.75f, world.buffer("ramp", "table")!!.readFloats(8)[3], "with what the script filled it with, given the node's setting")
            assertNotNull(world.image("ramp", "rows"), "its image, an output by its name")
            assertNotNull(world.resource<Resource>("ramp", "rowsView"), "and its view")
            assertNotNull(world.resource<Resource>("ramp", "nearest"), "and its sampler")
            world.setInput("ramp", "field.step", "2")
            frames(2)
            assertEquals(6f, world.buffer("ramp", "table")!!.readFloats(8)[3], "a setting changed: made and filled again")

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
            // Run by its build, a project has the classes folder on its own classpath too, which keeps what it loaded once.
            val classpath = java.net.URLClassLoader(arrayOf(classes.toUri().toURL()), ScriptHost::class.java.classLoader)
            val built = ScriptHost(world, classes, sources, listOf("sh", script.toString()), classpath)
            built.pollInterval = 10
            built.settleTime = 60
            assertTrue(built.load(), "built and loaded: ${built.errors}")
            classpath.loadClass("Spin")     // build A's, from now on the classpath's
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
            assertEquals(laps + 1, number(wheel, "Spin", "laps"), "and runs, with what the entity had: the new build's, not the classpath's")

            built.close()
            frames(2)
            assertEquals(laps + 1, number(wheel, "Spin", "laps"), "with the host gone nothing runs, and the data is still the scene's")

            // ---- a script's meshes, of attributes of its own -------------------------------------------------------
            val blade = world.meshes.create("blade", 3, listOf(
                MeshAttribute.floats("POSITION", 3, floatArrayOf(0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f, 0f)),
                MeshAttribute.floats("WIND", 1, floatArrayOf(0f, 0.5f, 1f)),
            ), intArrayOf(0, 1, 2), "POSITION; WIND")
            assertEquals("\$script/blade", blade, "a script's mesh is named for it")
            val grass = world.create("Grass")
            world.meshes.give(grass, blade)
            assertEquals("\"\$script/blade\"", world.fieldJson(grass, "Mesh", "mesh"), "and given to an entity")
            val read = world.meshes.read(blade)
            assertEquals(3, read.vertexCount)
            assertEquals(1f, read.find("WIND")!!.asFloats()[2], "its own attribute, read back")
            assertEquals(listOf(0, 1, 2), read.indices.toList())
            val cube = world.meshes.read("\$cube")
            val coloured = world.meshes.withAttributes("\$cube", "coloured cube", listOf(MeshAttribute.floats("COLOR", 3, FloatArray(cube.vertexCount * 3))))
            assertEquals(5, world.meshes.read(coloured).attributes.size, "a default mesh (position, normal, UV, tangent), with an attribute more")
            assertEquals(3, world.meshes.read(world.meshes.withoutAttributes(coloured, "bare cube", "COLOR", "UV")).attributes.size, "and fewer")
            assertTrue(runCatching { world.meshes.create("wrong", 3, listOf(MeshAttribute.floats("WIND", 1, floatArrayOf(0f)))) }.isFailure, "no mesh is refused")
            frames(2)
        }
    }
}
