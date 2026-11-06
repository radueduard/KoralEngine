package koral.engine

import java.lang.foreign.MemorySegment
import java.nio.file.Path
import koral.Log

/**
 * The engine's programs (`koral-editor`, `koral-play`) running a project's Kotlin scripts in their own process: a JVM
 * is started inside them (JNI), and these are what they call, between frames. The engine is the program's own copy:
 * its C interface is found through the program ([start]'s resolver), not in a library loaded beside it.
 *
 * The scripts are compiled in this process too, by Kotlin's compiler (`koral.engine.compiler` names the folder of its
 * jars), on a thread of its own: into the project's `.koral/cache/kotlin`, where a build of the sources as they are
 * is loaded at once the next time the project is opened.
 */
object Embedded {
    private var scripts: ScriptHost? = null
    private var compiler: InProcessKotlinCompiler? = null
    private var compilerFor: List<Path> = emptyList()

    /** Once, before anything else: [resolver] is the program's C function that gives a symbol by its name. Null when done, or why not. */
    @JvmStatic
    fun start(resolver: Long): String? = try {
        EngineLibrary.useLoaded(MemorySegment.ofAddress(resolver))
        null
    } catch (e: Throwable) {
        said(e)
    }

    /**
     * The scripts of [project] (its folder) run on the entities of [document] from now, with the engine's own of
     * [enginePackage] (its folder): compiled first, unless they were already as they are. Null when they are under way,
     * or why they cannot be run.
     */
    @JvmStatic
    fun attach(document: Long, project: String, enginePackage: String): String? = try {
        stop()
        val folder = Path.of(project)
        // The project's, its modules' — what they have is the project's too — and the engine's own.
        val modules = Native.text(Native.kengine_module_scripts.invokeExact(MemorySegment.ofAddress(document)) as MemorySegment)
            .lines().filter { it.isNotBlank() }.map { Path.of(it) }
        val sources = listOf(folder.resolve("scripts")) + modules + listOf(Path.of(enginePackage).resolve("scripts"))
        val compilerJars = System.getProperty("koral.engine.compiler")?.let { InProcessKotlinCompiler.jarsOf(Path.of(it)) }.orEmpty()
        if (compilerJars.isEmpty()) throw EngineException("Kotlin's compiler is not beside the engine: koral.engine.compiler names no folder of its jars")
        // One compiler, and its classes loaded once, for as long as the sources are the same folders: a project opened again.
        val kotlinc = compiler?.takeIf { compilerFor == sources }
            ?: InProcessKotlinCompiler(sources, System.getProperty("java.class.path"), compilerJars).also { compiler = it; compilerFor = sources }
        val host = ScriptHost(World(MemorySegment.ofAddress(document)), folder.resolve(".koral/cache/kotlin"), folder.resolve("scripts"), compiler = kotlinc)
        scripts = host
        host.load()
        null
    } catch (e: Throwable) {
        Log.error("[scripts] the project's Kotlin scripts could not be run: ${said(e)}")
        said(e)
    }

    /** Between frames: what was compiled, loaded; a saved script, compiled again. */
    @JvmStatic
    fun poll() {
        try { scripts?.poll() } catch (e: Throwable) { Log.error("[scripts] $e") }
    }

    /** Whether the scripts are being compiled now. */
    @JvmStatic
    fun compiling(): Boolean = scripts?.isCompiling == true

    /** Waits for the scripts being compiled, and loads them: a program that runs so many frames, and no more. */
    @JvmStatic
    fun settle() {
        try { scripts?.awaitCompile() } catch (e: Throwable) { Log.error("[scripts] $e") }
    }

    /** The scripts stop: their components are the engine's, and stay; nothing is called on them. */
    @JvmStatic
    fun detach() {
        try { stop() } catch (e: Throwable) { Log.error("[scripts] $e") }
    }

    /** What was thrown, and what it was thrown for: each cause in turn. */
    private fun said(e: Throwable): String = generateSequence(e) { it.cause }.take(5).joinToString(": caused by ")

    private fun stop() {
        scripts?.close()
        scripts = null
    }
}
