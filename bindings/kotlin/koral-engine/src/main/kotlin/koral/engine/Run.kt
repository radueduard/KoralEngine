package koral.engine

import java.nio.file.Files
import java.nio.file.Path
import koral.App
import koral.SceneArgs
import koral.WindowSettings

/**
 * Runs a Koral Engine project with Kotlin scripts: on its own, or in the editor. What a project's `main` calls:
 *
 * ```kotlin
 * fun main(args: Array<String>) = koral.engine.run(args)
 * ```
 *
 * Arguments: the project's folder; `--edit` to open it in the editor; `--frames N` to stop after so many frames;
 * `--no-vsync` to draw frames as they are made rather than wait for the display, as `koral-play` and `koral-editor` take it;
 * `--classes <dir>` where the scripts' compiled classes are; and, to have a saved script built and loaded again,
 * `--sources <dir>` with `--compile <command>` (the command's words separated by spaces).
 */
private fun ofThis(directory: Path?, folder: Path): Boolean =
    directory == null || runCatching { Files.isSameFile(directory, folder) }.getOrDefault(directory.toAbsolutePath().normalize() == folder.normalize())

fun run(args: Array<String>) {
    var project: Path? = null
    var edit = false
    var frames = 0L
    var vsync = true
    var classes: Path? = null
    var sources: Path? = null
    var compile: List<String>? = null
    var i = 0
    while (i < args.size) {
        when (val arg = args[i]) {
            "--edit" -> edit = true
            "--frames" -> frames = args[++i].toLong()
            "--no-vsync" -> vsync = false
            "--classes" -> classes = Path.of(args[++i])
            "--sources" -> sources = Path.of(args[++i])
            "--compile" -> compile = args[++i].split(' ').filter { it.isNotEmpty() }
            else -> project = Path.of(arg)
        }
        ++i
    }
    val folder = project?.toAbsolutePath() ?: throw EngineException("which project? Give its folder.")
    if (!Files.exists(folder.resolve("project.json"))) throw EngineException("there is no project in '$folder': no project.json")
    val scriptClasses = classes ?: folder.resolve("kotlin/build/classes/kotlin/main")

    App().use { app ->
        app.loadLibrary(EngineLibrary.path.toString())
        val title = folder.fileName.toString()
        val scene = if (edit) app.open("Editor", WindowSettings(title = "Koral Engine: $title", width = 1600, height = 950, vsync = vsync), SceneArgs().with("project", folder).with("scripts", folder))
                    else app.open("Play", WindowSettings(title = title, vsync = vsync), SceneArgs().with("project", folder))
        var world: World? = null
        var scripts: ScriptHost? = null
        var frame = 0L
        while (app.frame()) {
            // The scene the scripts run in: found again when an editor has opened another.
            if (world == null || !world.isCurrentFor(scene)) {
                scripts?.close()
                world = World.of(scene)
                // This project's scripts, in this project's scenes only: another the editor opened has scripts of its
                // own, or none — and the editor hands one with scripts over to its own runner.
                scripts = world?.takeIf { ofThis(it.projectDirectory, folder) }?.let { ScriptHost(it, scriptClasses, sources, compile) }
                scripts?.load()
            }
            scripts?.poll()
            if (frames > 0 && ++frame >= frames) break
        }
        scripts?.close()
    }
}
