// koral_engine.h, as Kotlin calls it: one downcall a function, through java.lang.foreign.

package koral.engine

import java.lang.foreign.Arena
import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.MemoryLayout
import java.lang.foreign.MemorySegment
import java.lang.foreign.SegmentAllocator
import java.lang.foreign.StructLayout
import java.lang.foreign.SymbolLookup
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_BOOLEAN
import java.lang.foreign.ValueLayout.JAVA_DOUBLE
import java.lang.foreign.ValueLayout.JAVA_FLOAT
import java.lang.foreign.ValueLayout.JAVA_INT
import java.lang.foreign.ValueLayout.JAVA_LONG
import java.lang.invoke.MethodHandle
import java.nio.file.Files
import java.nio.file.Path
import koral.interop.Native as Koral

/** Something the engine refused, with why. */
class EngineException(message: String) : RuntimeException(message)

/** Where the engine's native library is: what an application loads for the engine's scenes. */
object EngineLibrary {
    private val os = System.getProperty("os.name").lowercase()
    val fileName: String = when {
        os.startsWith("windows") -> "koral-engine.dll"
        os.startsWith("mac") -> "libkoral-engine.dylib"
        else -> "libkoral-engine.so"
    }

    /**
     * The library's path: the system property `koral.engine.library` or `KORAL_ENGINE_LIBRARY` when one names it,
     * and otherwise in `KORAL_ENGINE`'s `lib`, or the working directory. Throws when it is in none of them.
     */
    val path: Path by lazy {
        val candidates = listOfNotNull(
            System.getProperty("koral.engine.library")?.takeIf { it.isNotEmpty() }?.let { Path.of(it) },
            System.getenv("KORAL_ENGINE_LIBRARY")?.takeIf { it.isNotEmpty() }?.let { Path.of(it) },
            System.getenv("KORAL_ENGINE")?.takeIf { it.isNotEmpty() }?.let { Path.of(it, "lib", fileName) },
            Path.of(fileName),
        )
        candidates.firstOrNull { Files.exists(it) }?.toAbsolutePath()
            ?: throw EngineException("Koral Engine's native library ($fileName) was not found. Set KORAL_ENGINE_LIBRARY to the library itself.")
    }

    /**
     * Loads the library, before [koral.App.loadLibrary] is given it. Told first, Koral loads a copy on Windows, and what
     * this side calls is then another library than the one the scenes are in, which knows of none of them.
     */
    fun load() = Native.load()

    /**
     * The engine's C interface is the running program's own: one of the engine's programs (`koral-editor`, `koral-play`)
     * that runs the scripts inside it ([Embedded]). [resolve] is a C function of it that gives a symbol's address by
     * its name. Before anything is called.
     */
    fun useLoaded(resolve: MemorySegment) { embedded = resolve }

    /** The running program's symbol resolver, when the engine is in it: kept here, as [Native] looks everything up as it is first used. */
    @Volatile internal var embedded: MemorySegment? = null
}

internal object Native {
    const val NO_COMPONENT = -1     // 0xFFFFFFFF
    val ENTITY: StructLayout = MemoryLayout.structLayout(JAVA_INT.withName("index"), JAVA_INT.withName("generation"))
    /** KecsField: two strings, an offset and a size. */
    val FIELD: StructLayout = MemoryLayout.structLayout(ADDRESS.withName("name"), ADDRESS.withName("type"), JAVA_INT.withName("offset"), JAVA_INT.withName("size"))
    /** KengineScriptCallbacks: user, then nine functions. */
    val CALLBACKS: StructLayout = MemoryLayout.structLayout(*Array(10) { ADDRESS })


    private val library: SymbolLookup by lazy {
        EngineLibrary.embedded?.let { resolver ->
            val resolve = Koral.linker.downcallHandle(resolver, FunctionDescriptor.of(ADDRESS, ADDRESS))
            return@lazy SymbolLookup { name ->
                Arena.ofConfined().use { a ->
                    val found = resolve.invokeExact(a.allocateFrom(name)) as MemorySegment
                    if (found.address() == 0L) java.util.Optional.empty() else java.util.Optional.of(found)
                }
            }
        }
        if (!System.getProperty("os.name").lowercase().startsWith("windows")) return@lazy SymbolLookup.libraryLookup(EngineLibrary.path, Arena.global())
        // What it imports is beside it, where Windows does not look on its own, as Koral has it look for its libraries.
        val setDirectory = java.lang.foreign.Linker.nativeLinker().downcallHandle(
            SymbolLookup.libraryLookup("kernel32", Arena.global()).find("SetDllDirectoryW").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS))
        Arena.ofConfined().use { a ->
            val ignored = setDirectory.invokeWithArguments(a.allocateFrom(EngineLibrary.path.parent.toString(), Charsets.UTF_16LE)) as Int
            try { SymbolLookup.libraryLookup(EngineLibrary.path, Arena.global()) }
            finally { setDirectory.invokeWithArguments(MemorySegment.NULL) }
        }
    }
    fun load() { library }

    private fun call(name: String, result: MemoryLayout?, vararg arguments: MemoryLayout): MethodHandle {
        val symbol = library.find(name).orElseThrow { EngineException("the engine's library has no $name: it is another version's") }
        return Koral.linker.downcallHandle(symbol, if (result == null) FunctionDescriptor.ofVoid(*arguments) else FunctionDescriptor.of(result, *arguments))
    }

    fun text(segment: MemorySegment): String = if (segment == MemorySegment.NULL) "" else segment.reinterpret(Long.MAX_VALUE).getString(0)
    fun entity(arena: SegmentAllocator, entity: Entity): MemorySegment =
        arena.allocate(ENTITY).also { it.set(JAVA_INT, 0, entity.index); it.set(JAVA_INT, 4, entity.generation) }
    fun entity(segment: MemorySegment) = Entity(segment.get(JAVA_INT, 0), segment.get(JAVA_INT, 4))

    val lastError: String get() = text(kengine_last_error.invokeExact() as MemorySegment)

    val kengine_last_error = call("kengine_last_error", ADDRESS)
    val kengine_document = call("kengine_document", ADDRESS, ADDRESS)
    val kengine_document_world = call("kengine_document_world", ADDRESS, ADDRESS)
    val kengine_document_json = call("kengine_document_json", ADDRESS, ADDRESS)
    val kengine_project_directory = call("kengine_project_directory", ADDRESS, ADDRESS)
    val kengine_module_scripts = call("kengine_module_scripts", ADDRESS, ADDRESS)
    val kengine_document_save = call("kengine_document_save", JAVA_BOOLEAN, ADDRESS, ADDRESS)
    val kengine_playing = call("kengine_playing", JAVA_BOOLEAN, ADDRESS)
    val kengine_set_playing = call("kengine_set_playing", JAVA_BOOLEAN, ADDRESS, JAVA_BOOLEAN)
    val kengine_script_input = call("kengine_script_input", ADDRESS, ADDRESS)
    val kengine_debug = call("kengine_debug", ADDRESS, ADDRESS)

    val kengine_node_add = call("kengine_node_add", ADDRESS, ADDRESS, ADDRESS, ADDRESS, JAVA_DOUBLE, JAVA_DOUBLE)
    val kengine_node_remove = call("kengine_node_remove", JAVA_BOOLEAN, ADDRESS, ADDRESS)
    val kengine_node_connect = call("kengine_node_connect", JAVA_BOOLEAN, ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS)
    val kengine_node_resource = call("kengine_node_resource", ADDRESS, ADDRESS, ADDRESS, ADDRESS)
    val kengine_node_set_input_json = call("kengine_node_set_input_json", JAVA_BOOLEAN, ADDRESS, ADDRESS, ADDRESS, ADDRESS)
    val kengine_node_ok = call("kengine_node_ok", JAVA_BOOLEAN, ADDRESS, ADDRESS)

    val kecs_destroy = call("kecs_destroy", JAVA_BOOLEAN, ADDRESS, ENTITY)
    val kecs_alive = call("kecs_alive", JAVA_BOOLEAN, ADDRESS, ENTITY)
    val kecs_entity_count = call("kecs_entity_count", JAVA_LONG, ADDRESS)
    val kecs_set_parent = call("kecs_set_parent", JAVA_BOOLEAN, ADDRESS, ENTITY, ENTITY)
    val kecs_parent = call("kecs_parent", ENTITY, ADDRESS, ENTITY)
    val kecs_set_parent_at = call("kecs_set_parent_at", JAVA_BOOLEAN, ADDRESS, ENTITY, ENTITY, JAVA_INT, JAVA_BOOLEAN)
    val kecs_child_count = call("kecs_child_count", JAVA_LONG, ADDRESS, ENTITY)
    val kecs_child = call("kecs_child", ENTITY, ADDRESS, ENTITY, JAVA_LONG)
    val kecs_find = call("kecs_find", ENTITY, ADDRESS, ADDRESS)
    val kecs_position = call("kecs_position", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_rotation = call("kecs_rotation", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_scale = call("kecs_scale", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_set_position = call("kecs_set_position", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_set_rotation = call("kecs_set_rotation", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_set_scale = call("kecs_set_scale", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_local_matrix = call("kecs_local_matrix", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_world_matrix = call("kecs_world_matrix", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_set_world_matrix = call("kecs_set_world_matrix", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_positions = call("kecs_positions", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_rotations = call("kecs_rotations", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_scales = call("kecs_scales", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_set_positions = call("kecs_set_positions", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_set_rotations = call("kecs_set_rotations", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_set_scales = call("kecs_set_scales", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_orientation = call("kecs_orientation", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_set_orientation = call("kecs_set_orientation", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)
    val kecs_orientations = call("kecs_orientations", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_set_orientations = call("kecs_set_orientations", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS)
    val kecs_field_width = call("kecs_field_width", JAVA_INT, ADDRESS, JAVA_INT, ADDRESS)
    val kecs_field_floats = call("kecs_field_floats", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, ADDRESS, ADDRESS)
    val kecs_set_field_floats = call("kecs_set_field_floats", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, ADDRESS, ADDRESS)
    val kecs_field_ints = call("kecs_field_ints", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, ADDRESS, ADDRESS)
    val kecs_set_field_ints = call("kecs_set_field_ints", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, ADDRESS, ADDRESS)
    val kengine_entity_create = call("kengine_entity_create", ENTITY, ADDRESS, ADDRESS)
    val kengine_entity_name = call("kengine_entity_name", ADDRESS, ADDRESS, ENTITY)
    val kengine_entity_set_name = call("kengine_entity_set_name", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS)

    val kecs_component_find = call("kecs_component_find", JAVA_INT, ADDRESS, ADDRESS)
    val kecs_register_tag = call("kecs_register_tag", JAVA_INT, ADDRESS, ADDRESS)
    val kecs_add = call("kecs_add", JAVA_BOOLEAN, ADDRESS, ENTITY, JAVA_INT, ADDRESS)
    val kecs_remove = call("kecs_remove", JAVA_BOOLEAN, ADDRESS, ENTITY, JAVA_INT)
    val kecs_has = call("kecs_has", JAVA_BOOLEAN, ADDRESS, ENTITY, JAVA_INT)
    val kecs_get = call("kecs_get", ADDRESS, ADDRESS, ENTITY, JAVA_INT)
    val kecs_set_enabled = call("kecs_set_enabled", JAVA_BOOLEAN, ADDRESS, ENTITY, JAVA_INT, JAVA_BOOLEAN)
    val kecs_is_enabled = call("kecs_is_enabled", JAVA_BOOLEAN, ADDRESS, ENTITY, JAVA_INT)
    val kecs_mark_changed = call("kecs_mark_changed", JAVA_BOOLEAN, ADDRESS, ENTITY, JAVA_INT)
    val kecs_query = call("kecs_query", JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_LONG, JAVA_BOOLEAN, ADDRESS, JAVA_LONG)

    val kengine_field_json = call("kengine_field_json", ADDRESS, ADDRESS, ENTITY, ADDRESS, ADDRESS)
    val kengine_field_set_json = call("kengine_field_set_json", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS, ADDRESS, ADDRESS)

    val kengine_script_register = call("kengine_script_register", JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, JAVA_LONG, ADDRESS, JAVA_LONG, ADDRESS)
    val kengine_script_unregister = call("kengine_script_unregister", null, ADDRESS, ADDRESS)
    /** KengineResourceScriptCallbacks: user, then fill. */
    val RESOURCE_CALLBACKS: StructLayout = MemoryLayout.structLayout(ADDRESS, ADDRESS)
    /** KengineScriptResource: name, kind, settings. */
    val SCRIPT_RESOURCE: StructLayout = MemoryLayout.structLayout(ADDRESS.withName("name"), ADDRESS.withName("kind"), ADDRESS.withName("settings"))
    /** KengineContents: name, node, kind, format, extent[3], layers, texel_bytes, bytes, size. */
    val CONTENTS: StructLayout = MemoryLayout.structLayout(ADDRESS.withName("name"), ADDRESS.withName("node"), ADDRESS.withName("kind"), ADDRESS.withName("format"),
        JAVA_INT.withName("x"), JAVA_INT.withName("y"), JAVA_INT.withName("z"), JAVA_INT.withName("layers"), JAVA_INT.withName("texelBytes"),
        MemoryLayout.paddingLayout(4), ADDRESS.withName("bytes"), JAVA_LONG.withName("size"))
    val FILL: FunctionDescriptor = FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_LONG)
    val kengine_resource_script_register = call("kengine_resource_script_register", JAVA_BOOLEAN, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_LONG, ADDRESS, JAVA_LONG, ADDRESS)
    val kengine_resource_script_unregister = call("kengine_resource_script_unregister", null, ADDRESS, ADDRESS)

    /** KengineAttribute: the semantic, the type and count of its numbers, and the numbers. */
    val ATTRIBUTE: StructLayout = MemoryLayout.structLayout(ADDRESS.withName("semantic"), JAVA_INT.withName("type"), JAVA_INT.withName("count"), ADDRESS.withName("data"))
    val kengine_image_create = call("kengine_image_create", ADDRESS, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS)
    val kengine_entity_give_picture = call("kengine_entity_give_picture", JAVA_BOOLEAN, ADDRESS, ENTITY, ADDRESS, ADDRESS)
    val kengine_mesh_create = call("kengine_mesh_create", ADDRESS, ADDRESS, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS)
    val kengine_mesh_with_attributes = call("kengine_mesh_with_attributes", ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS, JAVA_INT, ADDRESS)
    val kengine_mesh_without_attributes = call("kengine_mesh_without_attributes", ADDRESS, ADDRESS, ADDRESS, ADDRESS, ADDRESS)
    val kengine_mesh_vertex_count = call("kengine_mesh_vertex_count", JAVA_INT, ADDRESS, ADDRESS)
    val kengine_mesh_attribute_count = call("kengine_mesh_attribute_count", JAVA_INT, ADDRESS, ADDRESS)
    val kengine_mesh_attribute = call("kengine_mesh_attribute", JAVA_BOOLEAN, ADDRESS, ADDRESS, JAVA_INT, ADDRESS)
    val kengine_mesh_indices = call("kengine_mesh_indices", JAVA_LONG, ADDRESS, ADDRESS, ADDRESS, JAVA_LONG)

    /** What the engine calls back with: (user, entity, data) and (user, entity, data, seconds). */
    val PLAIN: FunctionDescriptor = FunctionDescriptor.ofVoid(ADDRESS, ENTITY, ADDRESS)
    val TIMED: FunctionDescriptor = FunctionDescriptor.ofVoid(ADDRESS, ENTITY, ADDRESS, JAVA_FLOAT)
}

/**
 * Memory for one call into the engine — what it is given, and what it gives back — there only while the call is. One
 * block a thread, handed out from its start and given back as each call returns, so that a call made inside another
 * nests: what an Arena opened and closed for every call costs is more than most of the calls do.
 */
@PublishedApi internal object Scratch {
    private const val SIZE = 64 * 1024L

    @PublishedApi internal class Block : SegmentAllocator {
        private val memory: MemorySegment = Arena.ofAuto().allocate(SIZE, 16)
        @PublishedApi internal var used = 0L
        override fun allocate(byteSize: Long, byteAlignment: Long): MemorySegment {
            val at = (used + byteAlignment - 1) / byteAlignment * byteAlignment
            // More than the block holds: memory of its own, let go of when nothing refers to it.
            if (at + byteSize > SIZE) return Arena.ofAuto().allocate(byteSize, byteAlignment)
            used = at + byteSize
            return memory.asSlice(at, byteSize).fill(0)     // as an Arena's is: nothing left of the call before
        }
    }

    @PublishedApi internal val blocks: ThreadLocal<Block> = ThreadLocal.withInitial { Block() }

    inline fun <R> use(body: (SegmentAllocator) -> R): R {
        val block = blocks.get()
        val mark = block.used
        try { return body(block) } finally { block.used = mark }
    }
}
