package koral.engine

import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.lang.foreign.ValueLayout.JAVA_DOUBLE_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_FLOAT_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED
import java.lang.reflect.Field
import java.lang.reflect.Modifier
import koral.Mat4
import koral.Quat
import koral.Vec2
import koral.Vec3
import koral.Vec4

/**
 * A script: a class added to entities. Its public `var`s are a component — data the engine keeps, shows in the
 * inspector, saves with the scene and carries across a reload — and its methods are called on each entity that has it.
 *
 * ```kotlin
 * class Spin : Script() {
 *     var speed = 0f          // shown in the inspector, saved, kept across a reload
 *     var angle = 0f
 *     private var frames = 0  // this run's only: not saved, and gone on a reload
 *
 *     override fun update(delta: Float) { angle += speed * delta }
 * }
 * ```
 *
 * A `var` can be a Float, a Double, an Int, a Long, a Boolean, a [Vec2], [Vec3], [Vec4], [Mat4], [Quat] or an
 * [Entity]. Before each call the properties hold what the engine has; after it, the engine has what they hold. Edit
 * the class while it runs and each entity keeps the properties whose name and type are the same.
 */
abstract class Script {
    /** The entity this is on. */
    var entity: Entity = Entity.Null
        internal set
    /** The scene it is in: its entities, and the nodes of its graph. */
    lateinit var world: World
        internal set
    /** The keys, the mouse and the gamepads of the window the scene is in: `input.isKeyHeld(Key.eW)`. */
    val input: koral.Input get() = world.input

    /** It was added to the entity: once, switched on or off. */
    open fun onCreated() {}
    /** It was switched on, or added switched on. */
    open fun onEnabled() {}
    /** Before its first update: once. */
    open fun start() {}
    /** A steady step of the game: 0 to 8 times a frame. */
    open fun fixedUpdate(step: Float) {}
    /** Once a frame, while it is switched on. */
    open fun update(delta: Float) {}
    /** After every script's update. */
    open fun lateUpdate(delta: Float) {}
    /** It was switched off, or is about to go while on. */
    open fun onDisabled() {}
    /** It is about to go: removed, or its entity destroyed. */
    open fun onDestroyed() {}
}

/**
 * A script that makes what the graph's passes take and nothing else makes: a light turned into a camera, a table of
 * matrices. [prepare] runs after every script's updates and before anything is drawn, so what it writes is what this
 * frame's passes read.
 */
abstract class GraphicsScript : Script() {
    /** After the updates, before the passes: write what they read. */
    open fun prepare() {}
}

/** Gives a script's component another name than its class's. */
@Target(AnnotationTarget.CLASS)
@Retention(AnnotationRetention.RUNTIME)
annotation class ScriptName(val name: String)

internal enum class Hook { Created, Enabled, Started, FixedUpdate, Update, LateUpdate, Prepare, Disabled, Destroyed }

/** How a script class's properties are laid out as a component's bytes. */
internal class ScriptLayout private constructor(type: Class<out Script>) {
    class Property(val field: Field, val type: String, val offset: Int, val size: Int)

    val name: String = nameOf(type)
    val properties: List<Property>
    val size: Int
    val alignment: Int

    init {
        val found = mutableListOf<Property>()
        var offset = 0
        var widest = 1
        // A base class's first, each in the order it is written.
        val chain = generateSequence<Class<*>>(type) { it.superclass }.takeWhile { it != Script::class.java && it != GraphicsScript::class.java }.toList().reversed()
        for (declaring in chain) {
            for (field in declaring.declaredFields) {
                if (Modifier.isStatic(field.modifiers) || field.isSynthetic) continue
                // A property the engine keeps is a public var: one with a getter and a setter anyone can call.
                val suffix = field.name.replaceFirstChar { it.uppercaseChar() }
                val getter = declaring.methods.any { (it.name == "get$suffix" || it.name == field.name) && it.parameterCount == 0 }
                val setter = declaring.methods.any { (it.name == "set$suffix" || (field.name.startsWith("is") && it.name == "set" + field.name.drop(2))) && it.parameterCount == 1 }
                if (!getter || !setter) continue
                val (typeName, size, alignment) = describe(field.type)
                    ?: throw EngineException("${type.simpleName}.${field.name} is a ${field.type.simpleName}, which a script cannot keep: its properties are " +
                                             "Float, Double, Int, Long, Boolean, Vec2, Vec3, Vec4, Mat4, Quat and Entity. Make it private if it is only this run's.")
                offset = (offset + alignment - 1) / alignment * alignment
                field.isAccessible = true
                found += Property(field, typeName, offset, size)
                offset += size
                widest = maxOf(widest, alignment)
            }
        }
        properties = found
        alignment = widest
        size = (offset + widest - 1) / widest * widest
    }

    /** The engine's bytes into the object's properties. */
    fun load(script: Script, data: MemorySegment) {
        for (p in properties) {
            val at = p.offset.toLong()
            fun f(i: Int) = data.get(JAVA_FLOAT_UNALIGNED, at + i * 4L)
            // Numbers as they are, not boxed: a property a script, a script an entity, every frame.
            when (p.type) {
                "f32" -> { p.field.setFloat(script, f(0)); continue }
                "f64" -> { p.field.setDouble(script, data.get(JAVA_DOUBLE_UNALIGNED, at)); continue }
                "i32" -> { p.field.setInt(script, data.get(JAVA_INT_UNALIGNED, at)); continue }
                "i64" -> { p.field.setLong(script, data.get(JAVA_LONG_UNALIGNED, at)); continue }
                "bool" -> { p.field.setBoolean(script, data.get(JAVA_BYTE, at) != 0.toByte()); continue }
            }
            val value: Any = when (p.type) {
                "vec2" -> Vec2(f(0), f(1))
                "vec3" -> Vec3(f(0), f(1), f(2))
                "vec4" -> Vec4(f(0), f(1), f(2), f(3))
                "quat" -> Quat(f(0), f(1), f(2), f(3))
                "mat4" -> Mat4(FloatArray(16) { f(it) })
                else -> Entity(data.get(JAVA_INT_UNALIGNED, at), data.get(JAVA_INT_UNALIGNED, at + 4))
            }
            p.field.set(script, value)
        }
    }

    /** The object's properties into the engine's bytes. */
    fun store(script: Script, data: MemorySegment) {
        for (p in properties) {
            val at = p.offset.toLong()
            fun f(i: Int, v: Float) = data.set(JAVA_FLOAT_UNALIGNED, at + i * 4L, v)
            when (p.type) {
                "f32" -> { f(0, p.field.getFloat(script)); continue }
                "f64" -> { data.set(JAVA_DOUBLE_UNALIGNED, at, p.field.getDouble(script)); continue }
                "i32" -> { data.set(JAVA_INT_UNALIGNED, at, p.field.getInt(script)); continue }
                "i64" -> { data.set(JAVA_LONG_UNALIGNED, at, p.field.getLong(script)); continue }
                "bool" -> { data.set(JAVA_BYTE, at, if (p.field.getBoolean(script)) 1 else 0); continue }
            }
            when (val value = p.field.get(script)) {
                is Float -> f(0, value)
                is Double -> data.set(JAVA_DOUBLE_UNALIGNED, at, value)
                is Int -> data.set(JAVA_INT_UNALIGNED, at, value)
                is Long -> data.set(JAVA_LONG_UNALIGNED, at, value)
                is Boolean -> data.set(JAVA_BYTE, at, if (value) 1 else 0)
                is Vec2 -> { f(0, value.x); f(1, value.y) }
                is Vec3 -> { f(0, value.x); f(1, value.y); f(2, value.z) }
                is Vec4 -> { f(0, value.x); f(1, value.y); f(2, value.z); f(3, value.w) }
                is Quat -> { f(0, value.x); f(1, value.y); f(2, value.z); f(3, value.w) }
                is Mat4 -> for (i in 0 until 16) f(i, value[i / 4, i % 4])
                is Entity -> { data.set(JAVA_INT_UNALIGNED, at, value.index); data.set(JAVA_INT_UNALIGNED, at + 4, value.generation) }
            }
        }
    }

    companion object {
        private val layouts = HashMap<Class<*>, ScriptLayout>()

        fun nameOf(type: Class<*>): String = type.getAnnotation(ScriptName::class.java)?.name ?: type.simpleName

        @Synchronized fun of(type: Class<out Script>): ScriptLayout = layouts.getOrPut(type) { ScriptLayout(type) }
        /** Forgets every class [loader] loaded: it is being let go of. */
        @Synchronized fun forget(loader: ClassLoader) { layouts.keys.removeIf { it.classLoader === loader } }

        /** What a property type is called as a component's, how big it is, and how it is aligned; null for one a script cannot keep. */
        private fun describe(type: Class<*>): Triple<String, Int, Int>? = when (type) {
            Float::class.javaPrimitiveType -> Triple("f32", 4, 4)
            Double::class.javaPrimitiveType -> Triple("f64", 8, 8)
            Int::class.javaPrimitiveType -> Triple("i32", 4, 4)
            Long::class.javaPrimitiveType -> Triple("i64", 8, 8)
            Boolean::class.javaPrimitiveType -> Triple("bool", 1, 1)
            Vec2::class.java -> Triple("vec2", 8, 4)
            Vec3::class.java -> Triple("vec3", 12, 4)
            Vec4::class.java -> Triple("vec4", 16, 4)
            Quat::class.java -> Triple("quat", 16, 4)
            Mat4::class.java -> Triple("mat4", 64, 4)
            Entity::class.java -> Triple("entity", 8, 4)
            else -> null
        }
    }
}
