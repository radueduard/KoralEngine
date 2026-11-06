package koral.engine

import java.io.ByteArrayOutputStream
import java.io.PrintStream
import java.net.URLClassLoader
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.Executors

/** What compiles a project's Kotlin scripts into a folder of classes, for a [ScriptHost]. */
fun interface ScriptCompiler {
    /** Compiles into [into], an empty folder. Null when that worked; what was wrong when it did not. */
    fun compile(into: Path): List<String>?
}

/** Where a [ScriptHost]'s [ScriptCompiler] runs: one thread, apart from the frames, which carry on as it does. */
internal val compileThread = Executors.newSingleThreadExecutor { work -> Thread(work, "koral-scripts-compiler").apply { isDaemon = true } }

/**
 * Kotlin's own compiler, in this process: `kotlin-compiler-embeddable` and what it needs are the jars of [compilerJars],
 * loaded by a class loader of their own so that nothing of them is seen by the scripts. The scripts are compiled
 * against [classpath] — the engine's jars, Koral's, Kotlin's standard library — from every folder of [sources] there is.
 */
class InProcessKotlinCompiler(
    private val sources: List<Path>,
    private val classpath: String,
    compilerJars: List<Path>,
    private val jvmTarget: String = "25",
) : ScriptCompiler {
    private val loader = URLClassLoader("kotlin-compiler", compilerJars.map { it.toUri().toURL() }.toTypedArray(), ClassLoader.getPlatformClassLoader())

    override fun compile(into: Path): List<String>? {
        val roots = sources.filter { Files.isDirectory(it) }.map { it.toString() }
        if (roots.isEmpty()) return null
        val arguments = listOf("-d", into.toString(), "-classpath", classpath, "-no-stdlib", "-no-reflect", "-jvm-target", jvmTarget,
                               "-module-name", "scripts", "-nowarn") + roots
        val said = ByteArrayOutputStream()
        val thread = Thread.currentThread()
        val before = thread.contextClassLoader
        thread.contextClassLoader = loader
        val code = try {
            val type = loader.loadClass("org.jetbrains.kotlin.cli.jvm.K2JVMCompiler")
            val compiler = type.getDeclaredConstructor().newInstance()
            val exec = type.getMethod("exec", PrintStream::class.java, Array<String>::class.java)
            PrintStream(said, true, Charsets.UTF_8).use { exec.invoke(compiler, it, arguments.toTypedArray()) }.toString()
        } catch (e: ReflectiveOperationException) {
            return listOf("Kotlin's compiler could not be run: ${(e as? java.lang.reflect.InvocationTargetException)?.targetException ?: e}")
        } finally {
            thread.contextClassLoader = before
        }
        if (code == "OK") return null
        // What matters of what it said: the lines that name an error, or all of it when none does.
        val lines = said.toString(Charsets.UTF_8).lines().filter { it.isNotBlank() }
        return lines.filter { it.startsWith("error:") || it.startsWith("e: ") || it.contains(": error:") }.ifEmpty { lines.takeLast(20) }
            .ifEmpty { listOf("Kotlin's compiler stopped ($code)") }
    }

    companion object {
        /** The jars of [folder]: what `koral.engine.compiler` names, in the engine's programs. */
        fun jarsOf(folder: Path): List<Path> =
            if (!Files.isDirectory(folder)) emptyList()
            else Files.list(folder).use { files -> files.filter { it.toString().endsWith(".jar") }.sorted().toList() }
    }
}
