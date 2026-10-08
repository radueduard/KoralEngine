package koral.engine

import java.lang.foreign.Arena
import java.lang.foreign.MemorySegment
import java.lang.foreign.SegmentAllocator
import java.lang.foreign.ValueLayout.JAVA_INT
import koral.Buffer
import koral.Image
import koral.Resource
import koral.Scene
import koral.resourceFromHandle

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
            return if (document == MemorySegment.NULL) null else World(document)
        }
    }

    /** Whether this is still the scene's: an editor that opens another project starts a new one. */
    fun isCurrentFor(scene: Scene): Boolean = (Native.kengine_document.invokeExact(scene.graph.nativeHandle) as MemorySegment) == document

    private inline fun <R> native(body: (Arena) -> R): R = Arena.ofConfined().use(body)
    private fun id(arena: Arena, component: String): Int = Native.kecs_component_find.invokeExact(handle, arena.allocateFrom(component)) as Int
    private fun known(arena: Arena, component: String): Int =
        id(arena, component).also { if (it == Native.NO_COMPONENT) throw EngineException("there is no component '$component'") }

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
    fun setParent(child: Entity, parent: Entity): Boolean =
        native { a -> Native.kecs_set_parent.invokeExact(handle, Native.entity(a, child), Native.entity(a, parent)) as Boolean }
    fun parentOf(entity: Entity): Entity =
        native { a -> Native.entity(Native.kecs_parent.invokeExact(a as SegmentAllocator, handle, Native.entity(a, entity)) as MemorySegment) }

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
    /** Sets a number field. */
    fun set(entity: Entity, component: String, field: String, value: Number) = setJson(entity, component, field, value.toString())
    fun set(entity: Entity, component: String, field: String, value: Boolean) = setJson(entity, component, field, value.toString())
    /** Sets a vector field, component by component. */
    fun setVector(entity: Entity, component: String, field: String, vararg values: Float) =
        setJson(entity, component, field, values.joinToString(",", "[", "]"))
    /** Gives a resource field the output of a node, by the node's id; or nothing, with null. */
    fun setResource(entity: Entity, component: String, field: String, node: String?) =
        setJson(entity, component, field, if (node == null) "null" else "\"" + node.replace("\\", "\\\\").replace("\"", "\\\"") + "\"")

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

    private fun dataOf(a: Arena, entity: Entity, layout: ScriptLayout): MemorySegment? {
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
    fun buffer(node: String): Buffer? = resource(node)
    fun image(node: String): Image? = resource(node)

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
