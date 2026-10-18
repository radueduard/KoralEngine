package koral.engine

import java.lang.foreign.Arena
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_INT
import java.lang.invoke.MethodHandles
import java.net.URLClassLoader
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import koral.Log
import koral.interop.Native as Koral

/**
 * A project's scripts, running in a scene: every [Script] class among the compiled classes of a folder, registered
 * with the engine as a component, and all of them loaded again when they change.
 *
 * Kotlin is compiled by a build, not by this: [classes] is where the build writes its `.class` files. Given the
 * [sources] and the [compile] command that builds them, [poll] runs the command when a source is saved, and loads
 * what it made. Given neither, it watches [classes] for an IDE or a continuous build to write into.
 *
 * The data is the engine's, so it stays across a reload: each entity keeps the properties whose name and type are
 * the same, a new property starts at zero, and a script that was started is not started again. Scripts that do not
 * compile change nothing: the ones running carry on, and the compiler's errors are logged.
 */
class ScriptHost(
    private val world: World,
    val classes: Path,
    val sources: Path? = null,
    val compile: List<String>? = null,
    private val parent: ClassLoader = ScriptHost::class.java.classLoader,
) : AutoCloseable {
    private var loader: URLClassLoader? = null
    private val registered = LinkedHashMap<String, Registered>()
    private var snapshot = ""
    private var pending = ""
    private var pendingSince = 0L
    private var lastPoll = 0L

    /** The scripts there are now, by name. */
    val scripts: Set<String> get() = registered.keys
    /** How many times the scripts have been loaded. */
    var generation = 0
        private set
    /** What went wrong with the last load that failed; empty once one succeeds. */
    var errors: List<String> = emptyList()
        private set
    /** How long the files must have stopped changing before they are loaded again, in milliseconds. */
    var settleTime = 300L
    /** How often [poll] looks at the files, in milliseconds. */
    var pollInterval = 250L

    /** Builds (when there is a command to) and registers the scripts. False, with [errors], when that fails. */
    fun load(): Boolean {
        snapshot = snapshot()
        return reload()
    }

    /** Between frames: loads the scripts again when the files changed and have settled. True when it did. */
    fun poll(): Boolean {
        val now = System.nanoTime() / 1_000_000
        if (now - lastPoll < pollInterval) return false
        lastPoll = now
        val current = snapshot()
        if (current == snapshot) { pending = ""; return false }
        if (current != pending) { pending = current; pendingSince = now; return false }
        if (now - pendingSince < settleTime) return false
        snapshot = current
        pending = ""
        return reload().also { if (compile != null) snapshot = snapshot() }
    }

    /** Loads the scripts as they are now. False, changing nothing, when they do not build. */
    fun reload(): Boolean {
        if (compile != null) {
            val failed = build(compile)
            if (failed != null) {
                errors = failed
                Log.error("[scripts] they did not compile; what was running carries on.\n" + failed.joinToString("\n"))
                return false
            }
        }
        val next = ScriptLoader(classes, parent)
        val types = try {
            classNames().mapNotNull { name ->
                val type = next.loadClass(name)
                @Suppress("UNCHECKED_CAST")
                if (Script::class.java.isAssignableFrom(type) && !java.lang.reflect.Modifier.isAbstract(type.modifiers)
                    && type.constructors.any { it.parameterCount == 0 }) type as Class<out Script> else null
            }.sortedBy { it.name }
        } catch (e: LinkageError) {
            return refuse(next, "the classes could not be loaded: $e")
        } catch (e: ClassNotFoundException) {
            return refuse(next, "the classes could not be loaded: $e")
        }
        // The layouts first: a property a script cannot keep changes nothing.
        val layouts = try {
            types.map { it to ScriptLayout.of(it) }
        } catch (e: EngineException) {
            return refuse(next, e.message ?: "a script has a property it cannot keep")
        }

        // The code that is going stops being called before it goes. Its data stays where it is.
        val gone = registered.keys - layouts.map { it.second.name }.toSet()
        registered.values.forEach { it.unregister() }
        registered.clear()

        val failed = mutableListOf<String>()
        for ((type, layout) in layouts) {
            val script = Registered(world, type, layout)
            if (script.register()) registered[layout.name] = script else failed += "${layout.name}: ${Native.lastError}"
        }
        loader?.let { ScriptLayout.forget(it); it.close() }
        loader = next
        ++generation
        errors = failed
        failed.forEach { Log.error("[scripts] $it") }
        Log.info("[scripts] loaded: " + (if (registered.isEmpty()) "none" else registered.keys.sorted().joinToString(", "))
                 + (if (gone.isNotEmpty()) " (gone: ${gone.joinToString(", ")})" else ""))
        return true
    }

    private fun refuse(next: URLClassLoader, why: String): Boolean {
        ScriptLayout.forget(next)
        next.close()
        errors = listOf(why)
        Log.error("[scripts] $why")
        return false
    }

    override fun close() {
        registered.values.forEach { it.unregister() }
        registered.clear()
        loader?.let { ScriptLayout.forget(it); it.close() }
        loader = null
    }

    /** Runs the build. Null when it succeeded, and what it printed when it did not. */
    private fun build(command: List<String>): List<String>? {
        val process = try {
            ProcessBuilder(command).directory((sources ?: classes).toFile().let { if (it.isDirectory) it else it.parentFile }).redirectErrorStream(true).start()
        } catch (e: java.io.IOException) {
            return listOf("the build could not be run (${command.joinToString(" ")}): ${e.message}")
        }
        val output = process.inputStream.bufferedReader().readLines()
        if (!process.waitFor(10, TimeUnit.MINUTES)) { process.destroyForcibly(); return listOf("the build did not finish in ten minutes") }
        if (process.exitValue() == 0) return null
        // What matters of what it said: the lines that name an error, or all of it when none does.
        return output.filter { it.startsWith("e: ") || it.contains("error:") }.ifEmpty { output.takeLast(20) }.ifEmpty { listOf("the build failed") }
    }

    private fun classNames(): List<String> {
        if (!Files.isDirectory(classes)) return emptyList()
        return Files.walk(classes).use { files ->
            files.filter { it.toString().endsWith(".class") }
                .map { classes.relativize(it).toString().removeSuffix(".class").replace(java.io.File.separatorChar, '.') }
                .filter { '$' !in it }      // a lambda's, an inner class's: none is a script by itself
                .sorted().toList()
        }
    }

    private fun snapshot(): String {
        val watched = sources?.takeIf { compile != null } ?: classes
        if (!Files.isDirectory(watched)) return ""
        return Files.walk(watched).use { files ->
            files.filter { Files.isRegularFile(it) && (it.toString().endsWith(".kt") || it.toString().endsWith(".class")) }
                .sorted().map { "$it|${Files.getLastModifiedTime(it).toMillis()}|${Files.size(it)}" }.toList().joinToString("\n")
        }
    }

    /**
     * The scripts' classes from [classes] first, and only the rest from [parent]. A project run by its build has that
     * same folder on its own classpath: asked first, the parent would hand every reload the classes it loaded once.
     */
    private class ScriptLoader(classes: Path, parent: ClassLoader) : URLClassLoader(arrayOf(classes.toUri().toURL()), parent) {
        override fun loadClass(name: String, resolve: Boolean): Class<*> = synchronized(getClassLoadingLock(name)) {
            val type = findLoadedClass(name) ?: try { findClass(name) } catch (_: ClassNotFoundException) { super.loadClass(name, false) }
            if (resolve) resolveClass(type)
            type
        }
    }

    // ---- one script type, registered ---------------------------------------------------------------------------

    /** A script class as the engine has it: the functions it calls, and the object each entity's calls are made on. */
    internal class Registered(private val world: World, private val type: Class<out Script>, val layout: ScriptLayout) {
        private val instances = HashMap<Entity, Script>()
        private val reported = HashSet<Hook>()
        private val key = nextKey++
        private var component = Native.NO_COMPONENT

        fun register(): Boolean = Arena.ofConfined().use { a ->
            live[key] = this
            val callbacks = a.allocate(Native.CALLBACKS)
            callbacks.setAtIndex(ADDRESS, 0, MemorySegment.ofAddress(key))
            // Only what the class does: a hook it leaves as Script has it is not called at all — for thousands of entities a
            // frame, a call each into a function that does nothing. Destroyed as well when anything is: it is when an entity's
            // object goes. A script that is only data — fields, and no hook — is called for nothing, and costs nothing a frame.
            val does = Hook.entries.filter { overrides(type, it) }
            Hook.entries.forEachIndexed { i, hook ->
                val called = hook in does || (hook == Hook.Destroyed && does.isNotEmpty())
                callbacks.setAtIndex(ADDRESS, 1L + i, if (called) stubs[i] else MemorySegment.NULL)
            }
            val fields = a.allocate(Native.FIELD, maxOf(layout.properties.size, 1).toLong())
            layout.properties.forEachIndexed { i, p ->
                val at = i * Native.FIELD.byteSize()
                fields.set(ADDRESS, at, a.allocateFrom(p.field.name))
                fields.set(ADDRESS, at + 8, a.allocateFrom(p.type))
                fields.set(JAVA_INT, at + 16, p.offset)
                fields.set(JAVA_INT, at + 20, p.size)
            }
            component = Native.kengine_script_register.invokeExact(world.document, a.allocateFrom(layout.name), layout.size.toLong(), layout.alignment.toLong(),
                                                                   fields, layout.properties.size.toLong(), callbacks) as Int
            if (component == Native.NO_COMPONENT) live.remove(key)
            component != Native.NO_COMPONENT
        }

        fun unregister() {
            Arena.ofConfined().use { a ->
                // A statement, not the lambda's value: a handle that returns nothing must be called as one that does.
                Native.kengine_script_unregister.invokeExact(world.document, a.allocateFrom(layout.name))
                Unit
            }
            live.remove(key)
            instances.clear()
        }

        fun call(hook: Hook, entity: Entity, data: MemorySegment, time: Float) {
            val script = instances.getOrPut(entity) {
                type.getDeclaredConstructor().newInstance().also { it.entity = entity; it.world = world }
            }
            layout.load(script, data.reinterpret(maxOf(layout.size, 1).toLong()))
            try {
                when (hook) {
                    Hook.Created -> script.onCreated()
                    Hook.Enabled -> script.onEnabled()
                    Hook.Started -> script.start()
                    Hook.FixedUpdate -> script.fixedUpdate(time)
                    Hook.Update -> script.update(time)
                    Hook.LateUpdate -> script.lateUpdate(time)
                    Hook.Prepare -> (script as GraphicsScript).prepare()
                    Hook.Disabled -> script.onDisabled()
                    Hook.Destroyed -> script.onDestroyed()
                }
            } catch (e: Throwable) {
                // Said once for each kind of call: a script that throws every frame would bury everything else.
                if (reported.add(hook)) Log.error("[scripts] ${layout.name}.$hook threw on $entity: ${e.stackTraceToString()}")
            }
            // What it did may have moved its data, or taken it away: found again before anything is put back.
            if (hook == Hook.Destroyed) { instances.remove(entity); return }
            Scratch.use { a ->
                val now = Native.kecs_get.invokeExact(world.handle, Native.entity(a, entity), component) as MemorySegment
                if (now == MemorySegment.NULL) instances.remove(entity)
                else layout.store(script, now.reinterpret(maxOf(layout.size, 1).toLong()))
            }
        }
    }

    /** What the engine calls: nine functions for every script type, told apart by the number each registered with. */
    internal object Upcalls {
        private fun dispatch(hook: Hook, user: MemorySegment, entity: MemorySegment, data: MemorySegment, time: Float) {
            // Must not throw: the caller is C.
            try {
                live[user.address()]?.call(hook, Native.entity(entity), data, time)
            } catch (e: Throwable) {
                runCatching { Log.error("[scripts] $hook failed: ${e.stackTraceToString()}") }
            }
        }
        @JvmStatic fun created(user: MemorySegment, entity: MemorySegment, data: MemorySegment) = dispatch(Hook.Created, user, entity, data, 0f)
        @JvmStatic fun enabled(user: MemorySegment, entity: MemorySegment, data: MemorySegment) = dispatch(Hook.Enabled, user, entity, data, 0f)
        @JvmStatic fun started(user: MemorySegment, entity: MemorySegment, data: MemorySegment) = dispatch(Hook.Started, user, entity, data, 0f)
        @JvmStatic fun fixedUpdate(user: MemorySegment, entity: MemorySegment, data: MemorySegment, step: Float) = dispatch(Hook.FixedUpdate, user, entity, data, step)
        @JvmStatic fun update(user: MemorySegment, entity: MemorySegment, data: MemorySegment, delta: Float) = dispatch(Hook.Update, user, entity, data, delta)
        @JvmStatic fun lateUpdate(user: MemorySegment, entity: MemorySegment, data: MemorySegment, delta: Float) = dispatch(Hook.LateUpdate, user, entity, data, delta)
        @JvmStatic fun prepare(user: MemorySegment, entity: MemorySegment, data: MemorySegment) = dispatch(Hook.Prepare, user, entity, data, 0f)
        @JvmStatic fun disabled(user: MemorySegment, entity: MemorySegment, data: MemorySegment) = dispatch(Hook.Disabled, user, entity, data, 0f)
        @JvmStatic fun destroyed(user: MemorySegment, entity: MemorySegment, data: MemorySegment) = dispatch(Hook.Destroyed, user, entity, data, 0f)
    }

    private companion object {
        var nextKey = 1L

        /** Whether [type] does something of its own for [hook]: a function of that hook that is not Script's or GraphicsScript's. */
        fun overrides(type: Class<*>, hook: Hook): Boolean {
            val (name, timed) = when (hook) {
                Hook.Created -> "onCreated" to false
                Hook.Enabled -> "onEnabled" to false
                Hook.Started -> "start" to false
                Hook.FixedUpdate -> "fixedUpdate" to true
                Hook.Update -> "update" to true
                Hook.LateUpdate -> "lateUpdate" to true
                Hook.Prepare -> "prepare" to false
                Hook.Disabled -> "onDisabled" to false
                Hook.Destroyed -> "onDestroyed" to false
            }
            if (hook == Hook.Prepare && !GraphicsScript::class.java.isAssignableFrom(type)) return false
            val method = runCatching { if (timed) type.getMethod(name, Float::class.javaPrimitiveType) else type.getMethod(name) }.getOrNull() ?: return true
            return method.declaringClass != Script::class.java && method.declaringClass != GraphicsScript::class.java
        }
        val live = HashMap<Long, Registered>()

        /** One stub a hook, in the order the C struct has them, for as long as the program runs. */
        val stubs: List<MemorySegment> by lazy {
            val lookup = MethodHandles.lookup()
            listOf("created" to false, "enabled" to false, "started" to false, "fixedUpdate" to true, "update" to true, "lateUpdate" to true,
                   "prepare" to false, "disabled" to false, "destroyed" to false).map { (name, timed) ->
                val descriptor = if (timed) Native.TIMED else Native.PLAIN
                Koral.linker.upcallStub(lookup.findStatic(Upcalls::class.java, name, descriptor.toMethodType()), descriptor, Arena.global())
            }
        }
    }
}
