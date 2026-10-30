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

/**
 * What a [MeshGenerator] writes a mesh into: its attributes by semantic — the usual ones, or its own — its indices, and,
 * if it says, the buffers they are in. What it does not say is the engine's to choose.
 */
class MeshWriter internal constructor() {
    internal val attributes = mutableListOf<MeshAttribute>()
    internal var indexList: IntArray? = null
    internal var bufferLayout: String? = null
    /** How many vertices it has: as many as its positions. */
    var vertexCount = 0
        private set

    /** Where each vertex is, three floats each: what makes it a mesh, and how many vertices it has. */
    fun positions(xyz: FloatArray) { vertexCount = xyz.size / 3; attribute(MeshAttribute.floats("POSITION", 3, xyz)) }
    /** Which way each vertex's surface faces, three floats each. */
    fun normals(xyz: FloatArray) = attribute(MeshAttribute.floats("NORMAL", 3, xyz))
    /** Where on a picture each vertex is, two floats each: `UV`, or `UV1` for [set] 1. */
    fun uvs(uv: FloatArray, set: Int = 0) = attribute(MeshAttribute.floats(if (set == 0) "UV" else "UV$set", 2, uv))
    /** The way each vertex's u grows along its surface, and in w which side of it v does: four floats each. */
    fun tangents(xyzw: FloatArray) = attribute(MeshAttribute.floats("TANGENT", 4, xyzw))
    /** Any attribute, by its semantic: in place of one of the same semantic written before. */
    fun attribute(attribute: MeshAttribute) { attributes.removeAll { it.semantic == attribute.semantic }; attributes += attribute }
    /** Its triangles, three indices each, counter-clockwise seen from outside. None: drawn in the order of its vertices. */
    fun indices(indices: IntArray) { indexList = indices.copyOf() }
    /** Which vertex buffer each attribute is in — `"POSITION; NORMAL UV"`, a buffer to each ';' — rather than the engine's choice. */
    fun buffers(layout: String) { bufferLayout = layout }
}

/**
 * What a script takes to make an entity's mesh: a sphere of so many subdivisions, a terrain of a seed. Its public `var`s
 * are what it is made of; whenever they change — in the inspector, while the scene plays or not — it is made again, and
 * the entity's Mesh holds it. A generator is given only what it writes: what it makes is of its fields alone, so every
 * entity with the same fields shares one mesh, and what was made is kept in the project's cache, there when the scene is
 * opened without scripts.
 *
 * ```kotlin
 * class Pyramid : Setup(), MeshGenerator {
 *     var height = 1f
 *     override fun initialize(mesh: MeshWriter) {
 *         mesh.positions(floatArrayOf(-0.5f, 0f, -0.5f, 0.5f, 0f, -0.5f, 0.5f, 0f, 0.5f, -0.5f, 0f, 0.5f, 0f, height, 0f))
 *         mesh.indices(intArrayOf(0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0, 0, 1, 2, 0, 2, 3))
 *     }
 * }
 * ```
 */
interface MeshGenerator {
    /** Writes the mesh, of the fields as they are now. */
    fun initialize(mesh: MeshWriter)
}

/** What an [ImageGenerator] writes a picture into: so many texels, four bytes each — red, green, blue, alpha. */
class ImageWriter internal constructor() {
    internal var texels = ByteArray(256 * 256 * 4)
    /** How many texels across and down: [size] says, and it starts 256 by 256. */
    var width = 256
        private set
    var height = 256
        private set

    /** How big it is: every texel black and see-through again. */
    fun size(width: Int, height: Int) {
        require(width > 0 && height > 0) { "a picture has at least one texel" }
        this.width = width
        this.height = height
        texels = ByteArray(width * height * 4)
    }
    /** The texel at [x], [y] (from the top left): each of red, green, blue and alpha 0 to 1. */
    fun set(x: Int, y: Int, red: Float, green: Float, blue: Float, alpha: Float = 1f) {
        val at = (y * width + x) * 4
        fun byte(value: Float) = (value * 255f + 0.5f).coerceIn(0f, 255f).toInt().toByte()
        texels[at] = byte(red); texels[at + 1] = byte(green); texels[at + 2] = byte(blue); texels[at + 3] = byte(alpha)
    }
    /** Its texels as bytes, four a texel, a row after another from the top: there to be written whole. */
    val bytes: ByteArray get() = texels
}

/**
 * What a script takes to make a picture of its entity's material: a noise, a height map, a checker. Its public `var`s
 * are what it is made of; whenever they change it is made again, and the entity's material field [into] holds it. As a
 * [MeshGenerator], it is given only what it writes; every entity of the same fields shares one, and what was made is
 * kept in the project's cache.
 */
interface ImageGenerator {
    /** The picture field of its entity's material that holds what it makes: `baseColorMap` unless it says. */
    val into: String get() = "baseColorMap"
    /** Writes the picture, of the fields as they are now. */
    fun initialize(image: ImageWriter)
}
