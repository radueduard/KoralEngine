package koral.engine

import java.lang.foreign.Arena
import java.lang.foreign.MemorySegment
import java.lang.foreign.SegmentAllocator
import java.lang.foreign.ValueLayout.JAVA_FLOAT
import java.lang.foreign.ValueLayout.JAVA_INT
import koral.Buffer
import koral.DebugDraw
import koral.Image
import koral.Input
import koral.Mat4
import koral.Quat
import koral.Resource
import koral.Scene
import koral.Vec2
import koral.Vec3
import koral.Vec4
import koral.resourceFromHandle

/**
 * Entities kept where the engine reads them, for the calls that do the same to many at once ([World.rotationsOf],
 * [World.floatsOf]): made once, from a list, and given again each frame — not a list copied for the engine every
 * time. With room beside them for the numbers those calls are given and give back through, as many as the most asked.
 */
class Entities private constructor(entities: Collection<Entity>) : AbstractList<Entity>() {
    override val size: Int = entities.size
    internal val segment: MemorySegment = Arena.ofAuto().allocate(Native.ENTITY, maxOf(size, 1).toLong())
    private var room: MemorySegment = Arena.ofAuto().allocate(JAVA_FLOAT, maxOf(size * 3L, 1L))

    /** Room for [numbers] of four bytes: the same each time, made bigger when asked for more. */
    internal fun room(numbers: Long): MemorySegment {
        if (room.byteSize() < numbers * 4) room = Arena.ofAuto().allocate(JAVA_FLOAT, numbers)
        return room
    }

    init {
        entities.forEachIndexed { i, entity ->
            segment.set(JAVA_INT, i * 8L, entity.index)
            segment.set(JAVA_INT, i * 8L + 4, entity.generation)
        }
    }

    override fun get(index: Int): Entity = Native.entity(segment.asSlice(index * 8L, 8))

    companion object {
        fun of(entities: Collection<Entity>) = Entities(entities)
        fun of(vararg entities: Entity) = Entities(entities.toList())
    }
}

/** An entity: a number, and which use of that number it is. Nothing until a [World] is asked about it. */
data class Entity(val index: Int, val generation: Int) {
    val isNull: Boolean get() = index == -1
    override fun toString() = if (isNull) "no entity" else "entity $index"

    companion object {
        /** No entity. */
        val Null = Entity(-1, 0)
    }
}

/**
 * A scene the engine runs: its entities, and the nodes of its graph. What a script is handed, and what a program
 * that opened one of the engine's scenes asks for it by ([World.of]).
 */
class World internal constructor(internal val document: MemorySegment) {
    internal val handle: MemorySegment = Native.kengine_document_world.invokeExact(document) as MemorySegment

    companion object {
        /** The engine's side of [scene]: one of its scenes, "Play" or "Editor". Null for any other. */
        fun of(scene: Scene): World? {
            val document = Native.kengine_document.invokeExact(scene.graph.nativeHandle) as MemorySegment
            return if (document == MemorySegment.NULL) null else World(document).also { it.scene = scene }
        }
    }

    /** The scene this is the engine's side of: its window, its time. Null for a world not found through [of]. */
    var scene: Scene? = null
        private set
    /** Whether the scene plays: its scripts are updated. On for a scene run on its own; in an editor, when Play is pressed. */
    var playing: Boolean
        get() = Native.kengine_playing.invokeExact(document) as Boolean
        set(value) { val ignored = Native.kengine_set_playing.invokeExact(document, value) as Boolean }
    /**
     * The keys, the mouse and the gamepads a script that is steered reads: the scene's window's — or, in the editor, the
     * Game panel's, which has them while the pointer is over it or it was the last clicked.
     */
    val input: Input get() {
        val native = Native.kengine_script_input.invokeExact(document) as MemorySegment
        if (native != MemorySegment.NULL) return Input.of(native)
        return scene?.input ?: throw EngineException("this world was not found through a scene: it has no input")
    }

    /**
     * The lines and shapes a script draws to see what it does — contacts, normals, bounds: the scene's, shown over the
     * editor's view of it. Drawn in [Script.update] they last the frame; [Script.fixedUpdate] runs 0 to 8 times a frame,
     * so what is to be seen every frame is drawn in update, or given a duration.
     */
    val debug: DebugDraw get() {
        val native = Native.kengine_debug.invokeExact(document) as MemorySegment
        if (native != MemorySegment.NULL) return DebugDraw.of(native)
        return scene?.debug ?: throw EngineException("this world was not found through a scene: it has no debug lines")
    }

    /** The folder of the project the scene is of; null for one that is in none. */
    val projectDirectory: java.nio.file.Path?
        get() = Native.text(Native.kengine_project_directory.invokeExact(document) as MemorySegment).takeIf { it.isNotEmpty() }?.let { java.nio.file.Path.of(it) }

    /** Whether this is still the scene's: an editor that opens another project starts a new one. */
    fun isCurrentFor(scene: Scene): Boolean = (Native.kengine_document.invokeExact(scene.graph.nativeHandle) as MemorySegment) == document

    private inline fun <R> native(body: (SegmentAllocator) -> R): R = Scratch.use(body)
    private fun id(arena: SegmentAllocator, component: String): Int = Native.kecs_component_find.invokeExact(handle, arena.allocateFrom(component)) as Int
    private fun known(arena: SegmentAllocator, component: String): Int =
        id(arena, component).also { if (it == Native.NO_COMPONENT) throw EngineException("there is no component '$component'") }

    /** The scene's meshes: made of any attributes a script likes, read back, and given to entities. */
    val meshes: Meshes by lazy { Meshes(this) }

    // ---- entities -------------------------------------------------------------------------------------------

    /** A new entity, named as the editor names one. */
    fun create(name: String = "Entity"): Entity = native { a ->
        Native.entity(Native.kengine_entity_create.invokeExact(a as SegmentAllocator, document, a.allocateFrom(name)) as MemorySegment)
    }
    /** Destroys an entity, and everything under it. False when it was not alive. */
    fun destroy(entity: Entity): Boolean = native { a -> Native.kecs_destroy.invokeExact(handle, Native.entity(a, entity)) as Boolean }
    fun alive(entity: Entity): Boolean = native { a -> Native.kecs_alive.invokeExact(handle, Native.entity(a, entity)) as Boolean }
    val entityCount: Int get() = (Native.kecs_entity_count.invokeExact(handle) as Long).toInt()
    fun nameOf(entity: Entity): String = native { a -> Native.text(Native.kengine_entity_name.invokeExact(document, Native.entity(a, entity)) as MemorySegment) }
    fun rename(entity: Entity, name: String): Boolean =
        native { a -> Native.kengine_entity_set_name.invokeExact(document, Native.entity(a, entity), a.allocateFrom(name)) as Boolean }
    /**
     * Puts [child] under [parent] ([Entity.Null]: at the top), at [index] among what is there (last, when below 0).
     * It stays where it is in the world unless [keepWorldTransform] is false.
     */
    fun setParent(child: Entity, parent: Entity, index: Int = -1, keepWorldTransform: Boolean = true): Boolean = native { a ->
        Native.kecs_set_parent_at.invokeExact(handle, Native.entity(a, child), Native.entity(a, parent), index, keepWorldTransform) as Boolean
    }
    fun parentOf(entity: Entity): Entity =
        native { a -> Native.entity(Native.kecs_parent.invokeExact(a as SegmentAllocator, handle, Native.entity(a, entity)) as MemorySegment) }
    /** The entities directly under one, in order. */
    fun childrenOf(entity: Entity): List<Entity> = native { a ->
        val count = (Native.kecs_child_count.invokeExact(handle, Native.entity(a, entity)) as Long).toInt()
        List(count) { Native.entity(Native.kecs_child.invokeExact(a as SegmentAllocator, handle, Native.entity(a, entity), it.toLong()) as MemorySegment) }
    }
    /** The entities at the top of the hierarchy, in order. */
    val roots: List<Entity> get() = childrenOf(Entity.Null)
    /** The first entity so named, parents before their children; [Entity.Null] when there is none. */
    fun find(name: String): Entity =
        native { a -> Native.entity(Native.kecs_find.invokeExact(a as SegmentAllocator, handle, a.allocateFrom(name)) as MemorySegment) }

    // ---- transforms -----------------------------------------------------------------------------------------
    //
    // Every entity has one, relative to what it is under. It is the world's own, and no component.

    private fun three(read: java.lang.invoke.MethodHandle, entity: Entity, otherwise: Vec3): Vec3 = native { a ->
        val out = a.allocate(12, 4)
        if (read.invokeExact(handle, Native.entity(a, entity), out) as Boolean) Vec3(out.get(JAVA_FLOAT, 0), out.get(JAVA_FLOAT, 4), out.get(JAVA_FLOAT, 8)) else otherwise
    }
    private fun setThree(write: java.lang.invoke.MethodHandle, entity: Entity, value: Vec3): Boolean = native { a ->
        val given = a.allocate(12, 4)
        given.set(JAVA_FLOAT, 0, value.x); given.set(JAVA_FLOAT, 4, value.y); given.set(JAVA_FLOAT, 8, value.z)
        write.invokeExact(handle, Native.entity(a, entity), given) as Boolean
    }
    private fun sixteen(read: java.lang.invoke.MethodHandle, entity: Entity): Mat4 = native { a ->
        val out = a.allocate(64, 4)
        if (read.invokeExact(handle, Native.entity(a, entity), out) as Boolean) Mat4(FloatArray(16) { out.get(JAVA_FLOAT, it * 4L) }) else Mat4.Identity
    }

    /** Where an entity is, relative to what it is under. */
    fun positionOf(entity: Entity): Vec3 = three(Native.kecs_position, entity, Vec3.Zero)
    /** How it is turned: Euler angles, in degrees — about X, about Y, about Z. */
    fun rotationOf(entity: Entity): Vec3 = three(Native.kecs_rotation, entity, Vec3.Zero)
    fun scaleOf(entity: Entity): Vec3 = three(Native.kecs_scale, entity, Vec3(1f, 1f, 1f))
    fun setPosition(entity: Entity, position: Vec3): Boolean = setThree(Native.kecs_set_position, entity, position)
    fun setRotation(entity: Entity, degrees: Vec3): Boolean = setThree(Native.kecs_set_rotation, entity, degrees)
    fun setScale(entity: Entity, scale: Vec3): Boolean = setThree(Native.kecs_set_scale, entity, scale)
    /**
     * How it is turned, as a quaternion: what its matrix is made with. Set as one, it is kept as given (made length 1)
     * and [rotationOf] is worked out of it, so a rotation integrated a step at a time is not rounded off by the angles.
     */
    fun orientationOf(entity: Entity): Quat = native { a ->
        val out = a.allocate(16, 4)
        if (Native.kecs_orientation.invokeExact(handle, Native.entity(a, entity), out) as Boolean)
            Quat(out.get(JAVA_FLOAT, 0), out.get(JAVA_FLOAT, 4), out.get(JAVA_FLOAT, 8), out.get(JAVA_FLOAT, 12)) else Quat()
    }
    fun setOrientation(entity: Entity, rotation: Quat): Boolean = native { a ->
        val given = a.allocate(16, 4)
        given.set(JAVA_FLOAT, 0, rotation.x); given.set(JAVA_FLOAT, 4, rotation.y); given.set(JAVA_FLOAT, 8, rotation.z); given.set(JAVA_FLOAT, 12, rotation.w)
        Native.kecs_set_orientation.invokeExact(handle, Native.entity(a, entity), given) as Boolean
    }
    /** Its own transform, as a matrix. */
    fun localMatrix(entity: Entity): Mat4 = sixteen(Native.kecs_local_matrix, entity)
    /** Where it is in the world: the transforms of everything above it, and its own. */
    fun worldMatrix(entity: Entity): Mat4 = sixteen(Native.kecs_world_matrix, entity)
    // Of many at once: three floats an entity, one after another in the order of [entities] — what a script that
    // moves thousands does in two calls a frame, not two an entity. Pass the array back in each frame: it is made only
    // when it is not there or too small. One that is not alive reads as zeros (a scale as ones) and is not set.

    private fun threes(read: java.lang.invoke.MethodHandle, entities: Entities, into: FloatArray?, width: Int = 3): FloatArray {
        val out = if (into != null && into.size >= entities.size * width) into else FloatArray(entities.size * width)
        val room = entities.room(entities.size.toLong() * width)
        read.invokeExact(handle, entities.segment, entities.size.toLong(), room) as Long
        MemorySegment.copy(room, JAVA_FLOAT, 0L, out, 0, entities.size * width)
        return out
    }
    private fun setThrees(write: java.lang.invoke.MethodHandle, entities: Entities, values: FloatArray, width: Int = 3): Int {
        require(values.size >= entities.size * width) { "$width floats an entity: ${entities.size * width} of them, not ${values.size}" }
        val room = entities.room(entities.size.toLong() * width)
        MemorySegment.copy(values, 0, room, JAVA_FLOAT, 0L, entities.size * width)
        return (write.invokeExact(handle, entities.segment, entities.size.toLong(), room) as Long).toInt()
    }
    fun positionsOf(entities: Entities, into: FloatArray? = null): FloatArray = threes(Native.kecs_positions, entities, into)
    fun rotationsOf(entities: Entities, into: FloatArray? = null): FloatArray = threes(Native.kecs_rotations, entities, into)
    fun scalesOf(entities: Entities, into: FloatArray? = null): FloatArray = threes(Native.kecs_scales, entities, into)
    /** How many were alive, and so set. */
    fun setPositions(entities: Entities, values: FloatArray): Int = setThrees(Native.kecs_set_positions, entities, values)
    fun setRotations(entities: Entities, values: FloatArray): Int = setThrees(Native.kecs_set_rotations, entities, values)
    fun setScales(entities: Entities, values: FloatArray): Int = setThrees(Native.kecs_set_scales, entities, values)
    /** Their quaternions, four floats an entity (x, y, z, w); one not alive reads as the identity. */
    fun orientationsOf(entities: Entities, into: FloatArray? = null): FloatArray = threes(Native.kecs_orientations, entities, into, 4)
    fun setOrientations(entities: Entities, values: FloatArray): Int = setThrees(Native.kecs_set_orientations, entities, values, 4)

    // A field of a component, of many at once: its numbers, [fieldWidth] of them an entity — 1 for a Float, 3 for a
    // Vec3, 16 for a Mat4 — one entity's after another. Floats for a field of them, Ints for one of whole numbers or
    // true or false (1 and 0). One without the component reads as zeros and is not set; each setter says how many had it.

    /** How many numbers [field] of [component] is an entity; 0 for one there is not, or not of numbers. */
    fun fieldWidth(component: String, field: String): Int = native { a ->
        val id = id(a, component)
        if (id == Native.NO_COMPONENT) 0 else Native.kecs_field_width.invokeExact(handle, id, a.allocateFrom(field)) as Int
    }
    private fun fieldCall(entities: Entities, component: String, field: String, call: java.lang.invoke.MethodHandle, wide: (Int) -> Unit, move: (MemorySegment, Int) -> Unit): Long =
        native { a ->
            val id = known(a, component)
            val width = Native.kecs_field_width.invokeExact(handle, id, a.allocateFrom(field)) as Int
            if (width == 0) throw EngineException("'$component' has no field '$field' of numbers")
            wide(width)
            val room = entities.room(entities.size.toLong() * width)
            move(room, width)
            call.invokeExact(handle, entities.segment, entities.size.toLong(), id, a.allocateFrom(field), room) as Long
        }
    /** Pass the array back in each frame: it is made only when it is not there or too small. */
    fun floatsOf(entities: Entities, component: String, field: String, into: FloatArray? = null): FloatArray {
        var out = into ?: FloatArray(0)
        var wide = 1
        fieldCall(entities, component, field, Native.kecs_field_floats, { width -> wide = width; if (out.size < entities.size * width) out = FloatArray(entities.size * width) }) { _, _ -> }
        MemorySegment.copy(entities.room(0), JAVA_FLOAT, 0L, out, 0, entities.size * wide)     // what the call left in the room
        return out
    }
    fun setFloats(entities: Entities, component: String, field: String, values: FloatArray): Int =
        fieldCall(entities, component, field, Native.kecs_set_field_floats, { width ->
            require(values.size >= entities.size * width) { "$width numbers an entity: ${entities.size * width} of them, not ${values.size}" }
        }) { room, width -> MemorySegment.copy(values, 0, room, JAVA_FLOAT, 0L, entities.size * width) }.toInt()
    fun intsOf(entities: Entities, component: String, field: String, into: IntArray? = null): IntArray {
        var out = into ?: IntArray(0)
        var wide = 1
        fieldCall(entities, component, field, Native.kecs_field_ints, { width -> wide = width; if (out.size < entities.size * width) out = IntArray(entities.size * width) }) { _, _ -> }
        MemorySegment.copy(entities.room(0), JAVA_INT, 0L, out, 0, entities.size * wide)
        return out
    }
    fun setInts(entities: Entities, component: String, field: String, values: IntArray): Int =
        fieldCall(entities, component, field, Native.kecs_set_field_ints, { width ->
            require(values.size >= entities.size * width) { "$width numbers an entity: ${entities.size * width} of them, not ${values.size}" }
        }) { room, width -> MemorySegment.copy(values, 0, room, JAVA_INT, 0L, entities.size * width) }.toInt()

    /** Places it in the world, by changing its own transform. */
    fun setWorldMatrix(entity: Entity, matrix: Mat4): Boolean = native { a ->
        val given = a.allocate(64, 4)
        for (i in 0 until 16) given.set(JAVA_FLOAT, i * 4L, matrix[i / 4, i % 4])
        Native.kecs_set_world_matrix.invokeExact(handle, Native.entity(a, entity), given) as Boolean
    }

    // ---- components, by name --------------------------------------------------------------------------------

    /** Whether there is a component type so named: a pipeline's, a script's, a tag. */
    fun hasComponentType(component: String): Boolean = native { a -> id(a, component) != Native.NO_COMPONENT }
    /** A tag: a component with nothing in it, for a node's `with` and `without`. */
    fun addTag(name: String) { native { a -> Native.kecs_register_tag.invokeExact(handle, a.allocateFrom(name)) as Int } }

    /** Gives an entity a component, with its fields at their defaults. A script's is its class name. */
    fun add(entity: Entity, component: String): Boolean =
        native { a -> Native.kecs_add.invokeExact(handle, Native.entity(a, entity), known(a, component), MemorySegment.NULL) as Boolean }
    fun remove(entity: Entity, component: String): Boolean =
        native { a -> Native.kecs_remove.invokeExact(handle, Native.entity(a, entity), known(a, component)) as Boolean }
    fun has(entity: Entity, component: String): Boolean = native { a ->
        val id = id(a, component)
        id != Native.NO_COMPONENT && Native.kecs_has.invokeExact(handle, Native.entity(a, entity), id) as Boolean
    }
    /** Switches a component off, or back on: off, the entity has it still and nothing runs for it. */
    fun setEnabled(entity: Entity, component: String, enabled: Boolean): Boolean =
        native { a -> Native.kecs_set_enabled.invokeExact(handle, Native.entity(a, entity), known(a, component), enabled) as Boolean }
    fun isEnabled(entity: Entity, component: String): Boolean =
        native { a -> Native.kecs_is_enabled.invokeExact(handle, Native.entity(a, entity), known(a, component)) as Boolean }

    /** The entities that have every one of [components], switched on. */
    fun with(vararg components: String): List<Entity> = query(components.toList())

    /** The entities that have every component of [all] and none of [none]. */
    fun query(all: List<String>, none: List<String> = emptyList(), includeDisabled: Boolean = false): List<Entity> = native { a ->
        val allIds = a.allocate(JAVA_INT, maxOf(all.size, 1).toLong())
        val noneIds = a.allocate(JAVA_INT, maxOf(none.size, 1).toLong())
        all.forEachIndexed { i, name -> allIds.setAtIndex(JAVA_INT, i.toLong(), id(a, name)) }    // one nothing is: nothing has it
        none.forEachIndexed { i, name -> noneIds.setAtIndex(JAVA_INT, i.toLong(), id(a, name)) }
        val count = Native.kecs_query.invokeExact(handle, allIds, all.size.toLong(), noneIds, none.size.toLong(), includeDisabled, MemorySegment.NULL, 0L) as Long
        if (count == 0L) return@native emptyList()
        val found = a.allocate(Native.ENTITY, count)
        Native.kecs_query.invokeExact(handle, allIds, all.size.toLong(), noneIds, none.size.toLong(), includeDisabled, found, count) as Long
        List(count.toInt()) { Native.entity(found.asSlice(it * 8L, 8)) }
    }

    // ---- a component's fields -------------------------------------------------------------------------------

    /** A field as JSON: a number, true or false, a list of numbers for a vector, a node's id for a resource. */
    fun fieldJson(entity: Entity, component: String, field: String): String = native { a ->
        Native.text(Native.kengine_field_json.invokeExact(document, Native.entity(a, entity), a.allocateFrom(component), a.allocateFrom(field)) as MemorySegment)
    }
    fun setJson(entity: Entity, component: String, field: String, json: String) = native { a ->
        val ok = Native.kengine_field_set_json.invokeExact(document, Native.entity(a, entity), a.allocateFrom(component), a.allocateFrom(field), a.allocateFrom(json)) as Boolean
        if (!ok) throw EngineException(Native.lastError)
    }
    /**
     * Sets a field to [value], whatever a field can be: a number, true or false, a [Vec2], [Vec3], [Vec4], [Quat] or
     * [Mat4], an [Entity], a node's id for a resource (as [setResource]), or null for none.
     */
    fun set(entity: Entity, component: String, field: String, value: Any?) = setJson(entity, component, field, jsonOf(value))
    /** Sets a vector field, component by component. */
    fun setVector(entity: Entity, component: String, field: String, vararg values: Float) =
        setJson(entity, component, field, values.joinToString(",", "[", "]"))
    /** Gives a resource field the output of a node, by the node's id; or nothing, with null. */
    fun setResource(entity: Entity, component: String, field: String, node: String?) =
        setJson(entity, component, field, if (node == null) "null" else "\"" + node.replace("\\", "\\\\").replace("\"", "\\\"") + "\"")

    /** A field's value as the engine reads it: what [fieldJson] gives back. */
    private fun jsonOf(value: Any?): String = when (value) {
        null -> "null"
        is Number, is Boolean -> value.toString()
        is Vec2 -> numbers(value.x, value.y)
        is Vec3 -> numbers(value.x, value.y, value.z)
        is Vec4 -> numbers(value.x, value.y, value.z, value.w)
        is Quat -> numbers(value.x, value.y, value.z, value.w)
        is Mat4 -> numbers(*FloatArray(16) { value[it / 4, it % 4] })     // as a script's Mat4 property is stored
        is Entity -> if (value.isNull) "null" else "{\"entity\":${value.index},\"generation\":${value.generation}}"
        is String -> "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
        else -> throw IllegalArgumentException("a field is a number, true or false, a vector, a Quat, a Mat4, an Entity or a node's id: not a ${value.javaClass.simpleName}")
    }
    private fun numbers(vararg values: Float) = values.joinToString(",", "[", "]")

    /**
     * What a script on [entity] holds now, as an object of its class: read, not kept. Null when the entity has none.
     * [write] puts one back.
     */
    fun <T : Script> read(entity: Entity, type: Class<T>): T? = native { a ->
        val layout = ScriptLayout.of(type)
        val data = dataOf(a, entity, layout) ?: return@native null
        val script = type.getDeclaredConstructor().newInstance()
        script.entity = entity
        script.world = this
        layout.load(script, data)
        script
    }
    inline fun <reified T : Script> read(entity: Entity): T? = read(entity, T::class.java)

    /** Makes a script on [entity] hold what [script] does. False when the entity has none. */
    fun write(entity: Entity, script: Script): Boolean = native { a ->
        val layout = ScriptLayout.of(script.javaClass)
        val data = dataOf(a, entity, layout) ?: return@native false
        layout.store(script, data)
        Native.kecs_mark_changed.invokeExact(handle, Native.entity(a, entity), id(a, layout.name)) as Boolean
        true
    }

    private fun dataOf(a: SegmentAllocator, entity: Entity, layout: ScriptLayout): MemorySegment? {
        val id = id(a, layout.name)
        if (id == Native.NO_COMPONENT) return null
        val data = Native.kecs_get.invokeExact(handle, Native.entity(a, entity), id) as MemorySegment
        return if (data == MemorySegment.NULL) null else data.reinterpret(maxOf(layout.size, 1).toLong())
    }

    // ---- the graph ------------------------------------------------------------------------------------------

    /**
     * What a node made: a buffer to write, an image to read. Null when it has made nothing. A node built again makes
     * another, so ask when one is needed rather than keeping it.
     */
    fun <T : Resource> resource(node: String, output: String = "out"): T? = native { a ->
        resourceFromHandle(Native.kengine_node_resource.invokeExact(document, a.allocateFrom(node), a.allocateFrom(output)) as MemorySegment)
    }
    fun buffer(node: String, output: String = "out"): Buffer? = resource(node, output)
    fun image(node: String, output: String = "out"): Image? = resource(node, output)

    /** Adds a node, as the editor does. The id it got. */
    fun addNode(type: String, id: String? = null, x: Double = 0.0, y: Double = 0.0): String = native { a ->
        val made = Native.kengine_node_add.invokeExact(document, a.allocateFrom(type), if (id == null) MemorySegment.NULL else a.allocateFrom(id), x, y) as MemorySegment
        if (made == MemorySegment.NULL) throw EngineException(Native.lastError)
        Native.text(made)
    }
    fun removeNode(node: String): Boolean = native { a -> Native.kengine_node_remove.invokeExact(document, a.allocateFrom(node)) as Boolean }
    /** Sets a node's input from JSON: `"16"`, `"[\"eStorage\"]"`. */
    fun setInput(node: String, input: String, json: String) = native { a ->
        if (!(Native.kengine_node_set_input_json.invokeExact(document, a.allocateFrom(node), a.allocateFrom(input), a.allocateFrom(json)) as Boolean))
            throw EngineException(Native.lastError)
    }
    fun connect(from: String, node: String, input: String, output: String = "out") = native { a ->
        if (!(Native.kengine_node_connect.invokeExact(document, a.allocateFrom(from), a.allocateFrom(output), a.allocateFrom(node), a.allocateFrom(input)) as Boolean))
            throw EngineException(Native.lastError)
    }
    /** Why a node is not built; null when it is. */
    fun nodeProblem(node: String): String? = native { a ->
        if (Native.kengine_node_ok.invokeExact(document, a.allocateFrom(node)) as Boolean) null else Native.lastError
    }

    /** The scene as it is saved. */
    fun toJson(): String = Native.text(Native.kengine_document_json.invokeExact(document) as MemorySegment)
    fun save(path: String) = native { a ->
        if (!(Native.kengine_document_save.invokeExact(document, a.allocateFrom(path)) as Boolean)) throw EngineException(Native.lastError)
    }
}
