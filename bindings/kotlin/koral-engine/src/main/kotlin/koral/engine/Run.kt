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
 * `--classes <dir>` where the scripts' compiled classes are; and, to have a saved script built and loaded again,
 * `--sources <dir>` with `--compile <command>` (the command's words separated by spaces).
 */
fun run(args: Array<String>) {
    var project: Path? = null
    var edit = false
    var frames = 0L
    var classes: Path? = null
    var sources: Path? = null
    var compile: List<String>? = null
    var i = 0
    while (i < args.size) {
        when (val arg = args[i]) {
            "--edit" -> edit = true
            "--frames" -> frames = args[++i].toLong()
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
        val scene = if (edit) app.open("Editor", WindowSettings(title = "Koral Engine: $title", width = 1600, height = 950), SceneArgs().with("project", folder))
                    else app.open("Play", WindowSettings(title = title), SceneArgs().with("project", folder))
        var world: World? = null
        var scripts: ScriptHost? = null
        var frame = 0L
        while (app.frame()) {
            // The scene the scripts run in: found again when an editor has opened another.
            if (world == null || !world.isCurrentFor(scene)) {
                scripts?.close()
                world = World.of(scene)
                scripts = world?.let { ScriptHost(it, scriptClasses, sources, compile) }
                scripts?.load()
            }
            scripts?.poll()
            if (frames > 0 && ++frame >= frames) break
        }
        scripts?.close()
    }
}
