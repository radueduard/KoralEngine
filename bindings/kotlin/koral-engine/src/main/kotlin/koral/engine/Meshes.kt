// Meshes a script makes: of any attributes — the usual ones, or its own (WIND) — in the buffers it says.

package koral.engine

import java.lang.foreign.Arena
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_FLOAT
import java.lang.foreign.ValueLayout.JAVA_INT

/** What an attribute's numbers are. Each is four bytes. */
enum class AttributeType(val value: Int) { Float(0), Int(1), UInt(2) }

/**
 * One attribute of every vertex of a mesh: what it is, as a shader asks for it (`POSITION`, `NORMAL`, `UV`, or a name of
 * the script's own), and [count] numbers a vertex, one vertex after another — each a word of four bytes (a float's
 * bits, for floats).
 */
class MeshAttribute(val semantic: String, val type: AttributeType, val count: Int, val words: IntArray) {
    companion object {
        fun floats(semantic: String, count: Int, values: FloatArray) = MeshAttribute(semantic, AttributeType.Float, count, IntArray(values.size) { values[it].toRawBits() })
        fun ints(semantic: String, count: Int, values: IntArray) = MeshAttribute(semantic, AttributeType.Int, count, values.copyOf())
        fun uints(semantic: String, count: Int, values: IntArray) = MeshAttribute(semantic, AttributeType.UInt, count, values.copyOf())
    }
    /** Its numbers as floats (what they are, for a float attribute). */
    fun asFloats(): FloatArray = FloatArray(words.size) { Float.fromBits(words[it]) }
}

/** A mesh's numbers, as read back: its vertices' attributes and its indices. */
data class MeshData(val vertexCount: Int, val attributes: List<MeshAttribute>, val indices: IntArray) {
    fun find(semantic: String): MeshAttribute? = attributes.firstOrNull { it.semantic == semantic }
}

/**
 * The scene's meshes, as a script makes and reads them. A mesh made here is called `$script/name`, and is what an
 * entity's Mesh holds to have it ([give]). Not saved with the scene: a script makes its meshes when it runs.
 */
class Meshes internal constructor(private val world: World) {
    private val document get() = world.document

    /**
     * Makes a mesh of [vertexCount] vertices, of these attributes (one of them POSITION, of floats). [buffers] says
     * which vertex buffer each is in — `"POSITION UV; NORMAL TANGENT"`, a buffer to each ';', what none names in one
     * more — or, null, all in one. Returns its name. One so named already is replaced: every entity that held it holds
     * this one.
     */
    fun create(name: String, vertexCount: Int, attributes: List<MeshAttribute>, indices: IntArray? = null, buffers: String? = null): String =
        Arena.ofConfined().use { a ->
            val native = attributes(a, attributes)
            val at = if (indices == null) MemorySegment.NULL else a.allocateFrom(JAVA_INT, *indices)
            result(Native.kengine_mesh_create.invokeExact(document, a.allocateFrom(name), vertexCount, native, attributes.size, at, indices?.size ?: 0,
                                                          text(a, buffers)) as MemorySegment)
        }

    /** A copy of [mesh] — any: `$cube`, a model's, a script's — with these attributes added, or in place of its own of the same semantic. */
    fun withAttributes(mesh: String, name: String, attributes: List<MeshAttribute>, buffers: String? = null): String = Arena.ofConfined().use { a ->
        result(Native.kengine_mesh_with_attributes.invokeExact(document, a.allocateFrom(mesh), a.allocateFrom(name), attributes(a, attributes), attributes.size,
                                                               text(a, buffers)) as MemorySegment)
    }

    /** A copy of [mesh] without the attributes named. */
    fun withoutAttributes(mesh: String, name: String, vararg semantics: String): String = Arena.ofConfined().use { a ->
        result(Native.kengine_mesh_without_attributes.invokeExact(document, a.allocateFrom(mesh), a.allocateFrom(name), a.allocateFrom(semantics.joinToString(" "))) as MemorySegment)
    }

    /** A mesh's numbers, read back from the device: waits for it. */
    fun read(mesh: String): MeshData = Arena.ofConfined().use { a ->
        val id = a.allocateFrom(mesh)
        val vertices = Native.kengine_mesh_vertex_count.invokeExact(document, id) as Int
        if (vertices == 0) throw EngineException(Native.lastError)
        val count = Native.kengine_mesh_attribute_count.invokeExact(document, id) as Int
        val attributes = (0 until count).map { i ->
            val out = a.allocate(Native.ATTRIBUTE)
            if (!(Native.kengine_mesh_attribute.invokeExact(document, id, i, out) as Boolean)) throw EngineException(Native.lastError)
            val numbers = out.get(JAVA_INT, 12)
            val words = out.get(ADDRESS, 16).reinterpret(4L * numbers * vertices).toArray(JAVA_INT)
            MeshAttribute(Native.text(out.get(ADDRESS, 0)), AttributeType.entries.first { it.value == out.get(JAVA_INT, 8) }, numbers, words)
        }
        val size = Native.kengine_mesh_indices.invokeExact(document, id, MemorySegment.NULL, 0L) as Long
        val indices = a.allocate(JAVA_INT, maxOf(size, 1L))
        Native.kengine_mesh_indices.invokeExact(document, id, indices, size) as Long
        MeshData(vertices, attributes, if (size == 0L) IntArray(0) else indices.toArray(JAVA_INT).copyOf(size.toInt()))
    }

    /** Gives an entity a mesh, by its name: its Mesh holds it. */
    fun give(entity: Entity, mesh: String) {
        if (!world.has(entity, "Mesh")) world.add(entity, "Mesh")
        world.setResource(entity, "Mesh", "mesh", mesh)
    }

    private fun attributes(a: Arena, attributes: List<MeshAttribute>): MemorySegment {
        if (attributes.isEmpty()) return MemorySegment.NULL
        val native = a.allocate(Native.ATTRIBUTE, attributes.size.toLong())
        attributes.forEachIndexed { i, attribute ->
            val at = native.asSlice(i * Native.ATTRIBUTE.byteSize(), Native.ATTRIBUTE)
            at.set(ADDRESS, 0, a.allocateFrom(attribute.semantic))
            at.set(JAVA_INT, 8, attribute.type.value)
            at.set(JAVA_INT, 12, attribute.count)
            at.set(ADDRESS, 16, if (attribute.words.isEmpty()) MemorySegment.NULL else a.allocateFrom(JAVA_INT, *attribute.words))
        }
        return native
    }
    private fun text(a: Arena, text: String?): MemorySegment = if (text == null) MemorySegment.NULL else a.allocateFrom(text)
    private fun result(made: MemorySegment): String = if (made == MemorySegment.NULL) throw EngineException(Native.lastError) else Native.text(made)
}
