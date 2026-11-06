package koral.engine

import java.lang.foreign.Arena
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_INT
import java.lang.foreign.ValueLayout.JAVA_LONG
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
 * Given the [sources] and a [compiler] — Kotlin's own, in this process, as the engine's programs have it — [poll]
 * compiles them on a thread of its own when a source is saved, each build into a folder of its own under [classes],
 * and loads it once it is done. Given a [compile] command instead, it runs that and loads what it wrote into
 * [classes]. Given neither, it watches [classes] for an IDE or a continuous build to write into.
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
    /**
     * What compiles [sources] in this process instead, on a thread of its own, so that frames carry on as it does: each
     * build into a folder of its own under [classes], loaded once it is done. A build of the sources as they are is
     * not made again: the one from before is loaded at once.
     */
    val compiler: ScriptCompiler? = null,
) : AutoCloseable {
    private var loader: URLClassLoader? = null
    /** Where the classes being run are: [classes], or with a [compiler], its last build that succeeded. */
    private var current: Path = classes
    private var compiling: java.util.concurrent.Future<Built>? = null
    private var compileAgain = false
    private var closed = false

    /** A build of the [compiler]'s: where it went, of the sources as they were, and what was wrong when it failed. */
    private class Built(val into: Path, val sources: String, val errors: List<String>?)
    private val registered = LinkedHashMap<String, Registered>()
    private val resources = LinkedHashMap<String, RegisteredResource>()
    private var snapshot = ""
    private var pending = ""
    private var pendingSince = 0L
    private var lastPoll = 0L

    /** The scripts there are now, by name. */
    val scripts: Set<String> get() = registered.keys
    /** The scripts that make resources there are now, by name: what the graph's Script nodes can name. */
    val resourceScripts: Set<String> get() = resources.keys
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
        if (compiler != null) {
            // Built already, of the sources as they are: loaded now. Otherwise built, and loaded once it is.
            val stamp = classes.resolve("built")
            val kept = runCatching { Files.readString(stamp) }.getOrNull()?.split('\n', limit = 2)
            if (kept != null && kept.size == 2 && kept[1] == snapshot && Files.isDirectory(classes.resolve(kept[0]))) {
                current = classes.resolve(kept[0])
                return reload()
            }
            startCompile()
            return true
        }
        return reload()
    }

    /** Whether the [compiler] is building the scripts now. */
    val isCompiling: Boolean get() = compiling != null

    /** Waits for the build the [compiler] is making, and loads it: what a test, or a program on its last frame, does. */
    fun awaitCompile(): Boolean {
        val running = compiling ?: return false
        compiling = null
        return applyBuild(running.get())
    }

    private fun startCompile() {
        val compiler = compiler ?: return
        if (compiling != null) { compileAgain = true; return }
        val sources = snapshot()
        val into = classes.resolve("b" + java.lang.Long.toString(System.currentTimeMillis(), 36))
        Log.info("[scripts] compiling...")
        compiling = compileThread.submit<Built> {
            val errors = try {
                Files.createDirectories(into)
                compiler.compile(into)
            } catch (e: Throwable) {
                listOf("the compiler failed: $e")
            }
            Built(into, sources, errors)
        }
    }

    private fun applyBuild(built: Built): Boolean {
        if (closed) return false
        val ok = if (built.errors != null) {
            errors = built.errors
            Log.error("[scripts] they did not compile; what was running carries on.\n" + built.errors.joinToString("\n"))
            deleteBuild(built.into)
            false
        } else {
            val before = current
            current = built.into
            if (reload()) {
                runCatching { Files.writeString(classes.resolve("built"), built.into.fileName.toString() + "\n" + built.sources) }
                // What earlier builds left: gone, now that nothing loads from them.
                runCatching {
                    Files.list(classes).use { all -> all.filter { Files.isDirectory(it) && it != current }.toList() }.forEach { deleteBuild(it) }
                }
                true
            } else {
                current = before
                deleteBuild(built.into)
                false
            }
        }
        if (compileAgain) { compileAgain = false; startCompile() }
        return ok
    }

    private fun deleteBuild(folder: Path) {
        runCatching {
            Files.walk(folder).use { files -> files.sorted(Comparator.reverseOrder()).toList() }.forEach { Files.deleteIfExists(it) }
        }
    }

    /** Between frames: loads the scripts again when the files changed and have settled. True when it did. */
    fun poll(): Boolean {
        compiling?.let { running ->
            if (!running.isDone) return@let
            compiling = null
            return applyBuild(running.get())
        }
        val now = System.nanoTime() / 1_000_000
        if (now - lastPoll < pollInterval) return false
        lastPoll = now
        val seen = snapshot()
        if (seen == snapshot) { pending = ""; return false }
        if (seen != pending) { pending = seen; pendingSince = now; return false }
        if (now - pendingSince < settleTime) return false
        snapshot = seen
        pending = ""
        if (compiler != null) { startCompile(); return false }
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
        val next = ScriptLoader(current, parent)
        val types = try {
            classNames().mapNotNull { name ->
                val type = next.loadClass(name)
                if ((Setup::class.java.isAssignableFrom(type) || ResourceScript::class.java.isAssignableFrom(type)) && !java.lang.reflect.Modifier.isAbstract(type.modifiers)
                    && type.constructors.any { it.parameterCount == 0 }) type else null
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
        val gone = (registered.keys + resources.keys) - layouts.map { it.second.name }.toSet()
        registered.values.forEach { it.unregister() }
        registered.clear()
        resources.values.forEach { it.unregister() }
        resources.clear()

        val failed = mutableListOf<String>()
        for ((type, layout) in layouts) {
            // One that makes resources is a Script node's to run, and no component.
            if (ResourceScript::class.java.isAssignableFrom(type)) {
                val resource = RegisteredResource(world, type, layout)
                val why = try {
                    if (resource.register()) null else Native.lastError
                } catch (e: java.lang.reflect.InvocationTargetException) {
                    "making one threw ${e.targetException.javaClass.simpleName}: ${e.targetException.message}"
                }
                if (why == null) resources[layout.name] = resource else failed += "${layout.name}: $why"
                continue
            }
            @Suppress("UNCHECKED_CAST")
            val script = Registered(world, type as Class<out Setup>, layout)
            if (script.register()) registered[layout.name] = script else failed += "${layout.name}: ${Native.lastError}"
        }
        loader?.let { ScriptLayout.forget(it); it.close() }
        loader = next
        ++generation
        errors = failed
        failed.forEach { Log.error("[scripts] $it") }
        Log.info("[scripts] loaded: " + (if (registered.isEmpty() && resources.isEmpty()) "none" else (registered.keys + resources.keys).sorted().joinToString(", "))
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
        closed = true
        registered.values.forEach { it.unregister() }
        registered.clear()
        resources.values.forEach { it.unregister() }
        resources.clear()
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
        if (!Files.isDirectory(current)) return emptyList()
        return Files.walk(current).use { files ->
            files.filter { it.toString().endsWith(".class") }
                .map { current.relativize(it).toString().removeSuffix(".class").replace(java.io.File.separatorChar, '.') }
                .filter { '$' !in it }      // a lambda's, an inner class's: none is a script by itself
                .sorted().toList()
        }
    }

    private fun snapshot(): String {
        val watched = sources?.takeIf { compile != null || compiler != null } ?: classes
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
    internal class Registered(private val world: World, private val type: Class<out Setup>, val layout: ScriptLayout) {
        private val instances = HashMap<Entity, Setup>()
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

        private val generated = HashMap<Entity, Long>()     // each entity's fields when its mesh was made of them

        /**
         * Makes [entity]'s mesh of the generator's fields as they are, when they are not what it was made of: named for
         * the script and the fields, so that every entity of the same fields has the same one, made once.
         */
        private fun generate(script: Setup, entity: Entity, data: MemorySegment) {
            val bytes = data.reinterpret(maxOf(layout.size, 1).toLong())
            var hash = -3750763034362895579L
            for (i in 0 until layout.size) hash = (hash xor (bytes.get(java.lang.foreign.ValueLayout.JAVA_BYTE, i.toLong()).toLong() and 0xff)) * 1099511628211L
            if (generated[entity] == hash) return
            generated[entity] = hash
            val name = "gen/${layout.name}/${java.lang.Long.toHexString(hash).padStart(16, '0')}"
            if (script is MeshGenerator) {
                val made = world.generated.getOrPut(name) {
                    val mesh = MeshWriter()
                    script.initialize(mesh)
                    if (mesh.vertexCount == 0) throw EngineException("${layout.name} wrote no positions: a mesh is made of where its vertices are")
                    world.meshes.create(name, mesh.vertexCount, mesh.attributes, mesh.indexList, mesh.bufferLayout)
                }
                world.meshes.give(entity, made)
            }
            if (script is ImageGenerator) {
                val picture = "$name/picture"
                val made = world.generated.getOrPut(picture) {
                    val image = ImageWriter()
                    script.initialize(image)
                    java.lang.foreign.Arena.ofConfined().use { a ->
                        val texels = a.allocateFrom(java.lang.foreign.ValueLayout.JAVA_BYTE, *image.texels)
                        val created = Native.kengine_image_create.invokeExact(world.document, a.allocateFrom(picture), image.width, image.height, texels) as MemorySegment
                        if (created == MemorySegment.NULL) throw EngineException(Native.lastError)
                        Native.text(created)
                    }
                }
                val given = java.lang.foreign.Arena.ofConfined().use { a ->
                    Native.kengine_entity_give_picture.invokeExact(world.document, Native.entity(a, entity), a.allocateFrom(script.into), a.allocateFrom(made)) as Boolean
                }
                if (!given) throw EngineException("${layout.name} makes its entity's ${script.into}, and no material of it has a picture so named")
            }
        }

        /** The bytes of what it starts over for, and each entity's as the script last left them. */
        private val restarting = layout.properties.filter { it.reinitializes }
        private val seen = HashMap<Entity, ByteArray>()

        private fun watched(data: MemorySegment): ByteArray {
            val bytes = data.reinterpret(maxOf(layout.size, 1).toLong())
            val out = ByteArray(restarting.sumOf { it.size })
            var at = 0
            for (p in restarting) {
                MemorySegment.copy(bytes, java.lang.foreign.ValueLayout.JAVA_BYTE, p.offset.toLong(), out, at, p.size)
                at += p.size
            }
            return out
        }

        private fun dataOf(entity: Entity): MemorySegment? = Scratch.use { a ->
            val now = Native.kecs_get.invokeExact(world.handle, Native.entity(a, entity), component) as MemorySegment
            if (now == MemorySegment.NULL) null else now
        }

        /** Something else changed what it starts over for: it goes, and is added again. */
        private fun startOverIfChanged(entity: Entity, data: MemorySegment) {
            val last = seen[entity] ?: return
            if (last.contentEquals(watched(data))) return
            for (hook in listOf(Hook.Disabled, Hook.Destroyed, Hook.Created, Hook.Enabled, Hook.Started)) call(hook, entity, dataOf(entity) ?: return, 0f)
        }

        fun call(hook: Hook, entity: Entity, given: MemorySegment, time: Float) {
            var data = given
            if (restarting.isNotEmpty() && (hook == Hook.Update || hook == Hook.FixedUpdate)) {
                startOverIfChanged(entity, data)
                data = dataOf(entity) ?: return
            }
            val script = instances.getOrPut(entity) {
                type.getDeclaredConstructor().newInstance().also { it.entity = entity; it.world = world }
            }
            layout.load(script, data.reinterpret(maxOf(layout.size, 1).toLong()))
            try {
                when (hook) {
                    Hook.Created -> script.createdCore()
                    Hook.Enabled -> script.onEnabled()
                    Hook.Started -> script.initializeCore()
                    Hook.FixedUpdate -> (script as? FixedStep)?.fixedUpdate(time)
                    Hook.Update -> (script as? PerFrame)?.update(time)
                    Hook.LateUpdate -> (script as? PerFrame)?.lateUpdate(time)
                    Hook.Prepare -> {
                        (script as? BeforeDraw)?.prepare()
                        if (script is MeshGenerator || script is ImageGenerator) generate(script, entity, data)
                    }
                    Hook.Disabled -> script.onDisabled()
                    Hook.Destroyed -> script.onDestroyed()
                }
            } catch (e: Throwable) {
                // Said once for each kind of call: a script that throws every frame would bury everything else.
                if (reported.add(hook)) Log.error("[scripts] ${layout.name}.$hook threw on $entity: ${e.stackTraceToString()}")
            }
            // What it did may have moved its data, or taken it away: found again before anything is put back.
            if (hook == Hook.Destroyed) { instances.remove(entity); generated.remove(entity); seen.remove(entity); return }
            Scratch.use { a ->
                val now = Native.kecs_get.invokeExact(world.handle, Native.entity(a, entity), component) as MemorySegment
                if (now == MemorySegment.NULL) { instances.remove(entity); seen.remove(entity) }
                else {
                    layout.store(script, now.reinterpret(maxOf(layout.size, 1).toLong()))
                    if (restarting.isNotEmpty()) seen[entity] = watched(now)
                }
            }
        }
    }

    // ---- one script that makes resources, registered -----------------------------------------------------------

    /**
     * A script class that makes resources, as the engine has it: an object made each time it fills what it makes, given
     * the Script node's settings, so that nothing one node's were filled with is left over for the next.
     */
    internal class RegisteredResource(private val world: World, private val type: Class<*>, val layout: ScriptLayout) {
        private val key = nextKey++
        private var said: Arena? = null     // why the last fill could not be: the engine reads it before the next

        /** The fields of what it makes, in the order they are written. */
        private val made: List<java.lang.reflect.Field> = generateSequence<Class<*>>(type) { it.superclass }.takeWhile { it != ResourceScript::class.java }
            .toList().reversed().flatMap { c -> c.declaredFields.filter { ResourceScript.Resource::class.java.isAssignableFrom(it.type) && !java.lang.reflect.Modifier.isStatic(it.modifiers) } }
            .onEach { it.isAccessible = true }

        /** What a resource is made with, as JSON: flags given as `"eSampled | eStorage"` a list of them. */
        private fun json(resource: ResourceScript.Resource): String {
            fun text(t: String) = "\"" + t.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
            return resource.settings.joinToString(", ", "{", "}") { (input, value) ->
                "${text(input)}: " + when (value) {
                    is String -> if ('|' in value) value.split('|').map { it.trim() }.filter { it.isNotEmpty() }.joinToString(", ", "[", "]") { text(it) } else text(value)
                    is Boolean, is Number -> value.toString()
                    else -> text(value.toString())
                }
            }
        }

        fun register(): Boolean = Arena.ofConfined().use { a ->
            val callbacks = a.allocate(Native.RESOURCE_CALLBACKS)
            callbacks.setAtIndex(ADDRESS, 0, MemorySegment.ofAddress(key))
            callbacks.setAtIndex(ADDRESS, 1, fillStub)
            val fields = a.allocate(Native.FIELD, maxOf(layout.properties.size, 1).toLong())
            layout.properties.forEachIndexed { i, p ->
                val at = i * Native.FIELD.byteSize()
                fields.set(ADDRESS, at, a.allocateFrom(p.field.name))
                fields.set(ADDRESS, at + 8, a.allocateFrom(p.type))
                fields.set(JAVA_INT, at + 16, p.offset)
                fields.set(JAVA_INT, at + 20, p.size)
            }
            // What it makes, as an object of it makes them: each as its property's initialiser says.
            val sample = type.getDeclaredConstructor().newInstance()
            val resources = a.allocate(Native.SCRIPT_RESOURCE, maxOf(made.size, 1).toLong())
            made.forEachIndexed { i, field ->
                val resource = field.get(sample) as ResourceScript.Resource
                val at = i * Native.SCRIPT_RESOURCE.byteSize()
                resources.set(ADDRESS, at, a.allocateFrom(field.name))
                resources.set(ADDRESS, at + 8, a.allocateFrom(resource.kind))
                resources.set(ADDRESS, at + 16, a.allocateFrom(json(resource)))
            }
            resourcesLive[key] = this
            val ok = Native.kengine_resource_script_register.invokeExact(world.document, a.allocateFrom(layout.name), layout.size.toLong(),
                                                                         fields, layout.properties.size.toLong(), resources, made.size.toLong(), callbacks) as Boolean
            if (!ok) resourcesLive.remove(key)
            ok
        }

        fun unregister() {
            Arena.ofConfined().use { a ->
                Native.kengine_resource_script_unregister.invokeExact(world.document, a.allocateFrom(layout.name))
                Unit
            }
            resourcesLive.remove(key)
            said?.close()
            said = null
        }

        fun fill(contents: MemorySegment, count: Long, settings: MemorySegment, size: Long): MemorySegment {
            said?.close()
            said = null
            val all = contents.reinterpret(Native.CONTENTS.byteSize() * count)
            val node = if (count > 0) Native.text(all.get(ADDRESS, 8)) else ""
            val filled = mutableListOf<ResourceScript.Filled>()
            return try {
                val script = type.getDeclaredConstructor().newInstance() as ResourceScript
                script.world = world
                if (size >= layout.size) layout.load(script, settings.reinterpret(maxOf(layout.size, 1).toLong()))
                for (field in made) (field.get(script) as ResourceScript.Resource).name = field.name
                // Each of its images and buffers, as it was made, to be written.
                for (i in 0 until count) {
                    val c = all.asSlice(i * Native.CONTENTS.byteSize(), Native.CONTENTS.byteSize())
                    val name = Native.text(c.get(ADDRESS, 0))
                    val resource = made.firstOrNull { it.name == name }?.get(script) as? ResourceScript.Filled ?: continue
                    resource.data = c.get(ADDRESS, 56).reinterpret(c.get(JAVA_LONG, 64)).asByteBuffer().order(java.nio.ByteOrder.LITTLE_ENDIAN)
                    if (resource is ResourceScript.Image) {
                        resource.format = Native.text(c.get(ADDRESS, 24))
                        resource.width = c.get(JAVA_INT, 32)
                        resource.height = c.get(JAVA_INT, 36)
                        resource.depth = c.get(JAVA_INT, 40)
                        resource.layers = c.get(JAVA_INT, 44)
                        resource.texelBytes = c.get(JAVA_INT, 48)
                    }
                    filled += resource
                }
                script.init()
                MemorySegment.NULL
            } catch (e: Throwable) {
                val thrown = (e as? java.lang.reflect.InvocationTargetException)?.targetException ?: e
                Log.error("[scripts] ${layout.name}.init threw on $node: ${thrown.stackTraceToString()}")
                val arena = Arena.ofShared().also { said = it }
                arena.allocateFrom("it threw ${thrown.javaClass.simpleName}: ${thrown.message}")
            } finally {
                // What it was given is the engine's again: written to after this, it says so.
                filled.forEach { it.data = null }
            }
        }
    }

    /** What the engine calls to fill what a script makes: one function for every such script, told apart by the number each registered with. */
    internal object ResourceUpcalls {
        @JvmStatic fun fill(user: MemorySegment, contents: MemorySegment, count: Long, settings: MemorySegment, size: Long): MemorySegment =
            // Must not throw: the caller is C.
            try {
                resourcesLive[user.address()]?.fill(contents, count, settings, size) ?: MemorySegment.NULL
            } catch (e: Throwable) {
                runCatching { Log.error("[scripts] a fill failed: ${e.stackTraceToString()}") }
                MemorySegment.NULL
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

        /**
         * Whether [type] does something of its own for [hook]. A script as written before its pieces: a function of that
         * hook that is not Script's or GraphicsScript's. One now: the piece it takes, and a function of its own where the
         * piece has one that does nothing.
         */
        fun overrides(type: Class<*>, hook: Hook): Boolean {
            val legacy = Script::class.java.isAssignableFrom(type)
            val bases = setOf<Class<*>>(Setup::class.java, Script::class.java, GraphicsScript::class.java)
            fun own(name: String, timed: Boolean): Boolean {
                val method = runCatching { if (timed) type.getMethod(name, Float::class.javaPrimitiveType) else type.getMethod(name) }.getOrNull() ?: return false
                return method.declaringClass !in bases && !method.declaringClass.isInterface
            }
            fun takes(piece: Class<*>) = piece.isAssignableFrom(type)
            return when (hook) {
                Hook.Created -> legacy && own("onCreated", false)
                Hook.Enabled -> own("onEnabled", false)
                Hook.Started -> own(if (legacy) "start" else "initialize", false)
                Hook.FixedUpdate -> if (legacy) own("fixedUpdate", true) else takes(FixedStep::class.java)
                Hook.Update -> if (legacy) own("update", true) else takes(PerFrame::class.java)
                Hook.LateUpdate -> if (legacy) own("lateUpdate", true) else takes(PerFrame::class.java) && own("lateUpdate", true)
                // A generator is looked at before each frame is drawn too: made again when its fields have changed.
                Hook.Prepare -> if (legacy) GraphicsScript::class.java.isAssignableFrom(type) && own("prepare", false) else takes(BeforeDraw::class.java) || takes(MeshGenerator::class.java) || takes(ImageGenerator::class.java)
                Hook.Disabled -> own("onDisabled", false)
                Hook.Destroyed -> own("onDestroyed", false)
            }
        }
        val live = HashMap<Long, Registered>()
        val resourcesLive = HashMap<Long, RegisteredResource>()
        val fillStub: MemorySegment by lazy {
            Koral.linker.upcallStub(MethodHandles.lookup().findStatic(ResourceUpcalls::class.java, "fill", Native.FILL.toMethodType()), Native.FILL, Arena.global())
        }

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
