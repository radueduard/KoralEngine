package koral.engine

import java.nio.file.Files
import java.nio.file.Path
import kotlin.io.path.ExperimentalPathApi
import kotlin.io.path.copyToRecursively
import kotlin.io.path.deleteRecursively
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import org.junit.jupiter.api.Assumptions.assumeTrue
import koral.App
import koral.AppSettings
import koral.Buffer
import koral.BufferType
import koral.BufferUsage
import koral.CommandBuffer
import koral.OffscreenSettings
import koral.SceneArgs
import koral.WindowPlatform

/**
 * E6's gate, in Kotlin: shadow mapping in a project the engine's own test made and saved. A Script moves the light, a
 * GraphicsScript turns it into the camera the shadow pass draws from, and both are swapped for edited builds while
 * the scene runs. What is checked is the picture: where the shadow falls.
 */
@OptIn(ExperimentalPathApi::class)
class ShadowGateTest {
    private val classes: Path = Files.createTempDirectory("koral-engine-kotlin-shadow")

    private fun install(build: String) {
        Files.list(classes).use { files -> files.forEach { it.deleteRecursively() } }
        System.getProperty("koral.engine.test.shadow$build").split(java.io.File.pathSeparator).map(Path::of).filter(Files::isDirectory)
            .forEach { it.copyToRecursively(classes, followLinks = false, overwrite = true) }
    }

    @Test
    fun aScriptMovesTheLightAndAGraphicsScriptMakesItsCamera() {
        val project = System.getProperty("koral.engine.test.shadowProject")
        assumeTrue(project != null && Files.exists(Path.of(project, "project.json")), "the engine's own gate makes the project first")
        App(AppSettings(platform = WindowPlatform.eNone)).use { app ->
            EngineLibrary.load()
            app.loadLibrary(EngineLibrary.path.toString())
            val scene = app.openOffscreen("Play", OffscreenSettings(title = "shadow", width = 32, height = 32), SceneArgs().with("project", project))
            val world = World.of(scene) ?: error("the Play scene has no world")
            fun frames(count: Int) = repeat(count) { app.frame() }
            fun slant(entity: Entity) = world.fieldJson(entity, "Light", "slant").replace(" ", "")

            for (node in listOf("shadow", "lit", "cast", "draw", "show")) assertEquals(null, world.nodeProblem(node), "the project's graph is built: $node")
            val light = world.query(emptyList()).single { world.nameOf(it) == "Light" }

            // The picture, read back: the lit pass's image, a byte a channel.
            val readback = Buffer.Builder().setSize(32L * 32 * 4).setUsage(BufferUsage.eTransferDst).setType(BufferType.eReadback).build()
            fun red(x: Int, y: Int): Int {
                val canvas = world.image("canvas") ?: error("there is no canvas")
                CommandBuffer.singleTimeCommand { it.copyImageToBuffer(canvas, readback) }.waitBlocking()
                canvas.close()
                return readback.read()[(y * 32 + x) * 4].toInt() and 0xFF
            }
            fun dark(x: Int, y: Int) = red(x, y) in 31..79
            fun lit(x: Int, y: Int) = red(x, y) > 200

            // Every entity has a name, a place in a hierarchy and a transform: the world's own, and no components.
            run {
                val car = world.create("Car")
                val wheel = world.create("Wheel")
                assertTrue(world.setParent(wheel, car), "an entity goes under another")
                assertEquals(car, world.parentOf(wheel))
                assertEquals(listOf(wheel), world.childrenOf(car))
                assertTrue(car in world.roots && wheel !in world.roots)
                assertEquals(wheel, world.find("Wheel"))
                world.setPosition(car, koral.Vec3(10f, 0f, 0f))
                world.setPosition(wheel, koral.Vec3(1f, 2f, 0f))
                world.setRotation(wheel, koral.Vec3(0f, 30f, 0f))
                assertEquals(koral.Vec3(1f, 2f, 0f), world.positionOf(wheel))
                assertEquals(koral.Vec3(0f, 30f, 0f), world.rotationOf(wheel))
                assertEquals(koral.Vec3(1f, 1f, 1f), world.scaleOf(wheel))
                assertEquals(11f, world.worldMatrix(wheel)[3, 0], 1e-4f, "where its parent is, and then where it is")
                assertEquals(1f, world.localMatrix(wheel)[3, 0], 1e-4f)
                assertTrue(world.setParent(wheel, Entity.Null))
                assertEquals(11f, world.positionOf(wheel).x, 1e-4f, "it stays where it was in the world")
                assertTrue(world.setWorldMatrix(wheel, koral.Mat4.Identity))
                assertEquals(0f, world.positionOf(wheel).x, 1e-4f)
                world.destroy(car)
                world.destroy(wheel)
            }

            install("A")
            val host = ScriptHost(world, classes)
            assertTrue(host.load(), "the scripts load: ${host.errors}")
            assertEquals(setOf("Light", "LightCamera"), host.scripts)
            world.add(light, "Light")
            world.add(light, "LightCamera")
            world.set(light, "LightCamera", "reach", 1)

            // The light straight overhead: the shadow is under the occluder, in the middle of the ground.
            frames(2)
            assertTrue(dark(16, 16), "the middle is in shadow (${red(16, 16)})")
            assertTrue(lit(4, 4) && lit(24, 16), "and the rest of the ground is lit")

            // Leaning: the shadow of something half a unit up falls half the lean away. A lean of 1 is 8 pixels.
            world.setVector(light, "Light", "slant", 1f, 0f)
            frames(2)
            assertTrue(dark(24, 16) && lit(16, 16), "the shadow follows the light (${red(24, 16)}, ${red(16, 16)})")

            // The script moving it, a step a frame.
            world.set(light, "Light", "speed", -0.25)
            frames(8)
            world.set(light, "Light", "speed", 0)
            assertEquals("[-1,0]", slant(light), "the script moved the light")
            frames(2)
            assertTrue(dark(8, 16) && lit(16, 16) && lit(24, 16), "and the shadow went with it (${red(8, 16)})")

            // ---- edited while it runs -------------------------------------------------------------------------

            // What a method does: the light moves the other way now. It carries on from where it was.
            install("B")
            assertTrue(host.reload(), "the edited script loads: ${host.errors}")
            assertEquals("[-1,0]", slant(light), "the light is where it was")
            world.set(light, "Light", "speed", 0.25)
            frames(4)
            world.set(light, "Light", "speed", 0)
            frames(2)
            assertTrue(dark(8, 24) && lit(8, 16), "the new code moves the shadow down, from where it was (${red(8, 24)}, ${red(8, 16)})")

            // The graphics script, edited: the camera sees from the other side.
            install("C")
            assertTrue(host.reload(), "the edited graphics script loads: ${host.errors}")
            assertEquals("1", world.fieldJson(light, "LightCamera", "reach"), "it kept its reach")
            frames(2)
            assertTrue(dark(24, 24) && lit(8, 24), "and the shadow is on the other side (${red(24, 24)}, ${red(8, 24)})")

            // A script's shape: a property renamed and one added. The light keeps what kept its name; the rest is new.
            install("D")
            assertTrue(host.reload(), "the reshaped script loads: ${host.errors}")
            assertEquals("[-1,1]", slant(light), "where it leans is kept")
            assertEquals("0", world.fieldJson(light, "Light", "rate"), "a new property starts at nothing")
            assertEquals("false", world.fieldJson(light, "Light", "paused"))
            assertEquals("null", world.fieldJson(light, "Light", "speed"), "and the old one is gone")
            world.set(light, "Light", "rate", -0.25)
            frames(4)
            world.set(light, "Light", "paused", true)
            frames(3)
            assertEquals("[-1,0]", slant(light), "the new properties are what the new code reads")
            assertTrue(dark(24, 16) && lit(24, 24), "and the shadow shows it (${red(24, 16)})")

            // And rebuilt under it, as a build that watches the sources leaves it: noticed and loaded without being asked.
            host.pollInterval = 10
            host.settleTime = 60
            val generation = host.generation
            install("E")
            repeat(300) { if (host.generation == generation) { Thread.sleep(10); host.poll() } }
            frames(2)
            assertEquals(generation + 1, host.generation, "a rebuilt script is loaded")
            assertTrue(dark(8, 16) && lit(24, 16), "and the shadow is back on its side (${red(8, 16)})")

            host.close()
            readback.close()
        }
    }
}
