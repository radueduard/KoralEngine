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
 * inspector, saves with the scene and carries across a reload. Every script is a Setup: it is initialised, switched on
 * and off, and goes. What else it does it says by the pieces it takes — [PerFrame] to be called every frame, [FixedStep]
 * on the game's steady step, [BeforeDraw] before the frame is drawn — and only what it takes is ever called on it.
 *
 * ```kotlin
 * class Spin : Setup(), PerFrame {
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
 * the class while it runs and each entity keeps the properties whose name and type are the same. An entity running a
 * script is given a tag for each piece it takes — `PerFrame`, `FixedStep`, `BeforeDraw` — for a graph's Filter to find
 * it by.
 */
abstract class Setup {
    /** The entity this is on. */
    var entity: Entity = Entity.Null
        internal set
    /** The scene it is in: its entities, and the nodes of its graph. */
    lateinit var world: World
        internal set
    /** The keys, the mouse and the gamepads of the window the scene is in: `input.isKeyHeld(Key.eW)`. */
    val input: koral.Input get() = world.input

    /** Once, before anything else is called on it while the scene plays. */
    open fun initialize() {}
    /** It was switched on, or added switched on. */
    open fun onEnabled() {}
    /** It was switched off, or is about to go while on. */
    open fun onDisabled() {}
    /** It is about to go: removed, or its entity destroyed. */
    open fun onDestroyed() {}

    internal open fun createdCore() {}
    internal open fun initializeCore() = initialize()
}

/** What a script takes to be called every frame while the scene plays. */
interface PerFrame {
    /** Once a frame, while it is switched on. */
    fun update(delta: Float)
    /** After every script's update: what follows what the others did — a camera following its target. */
    fun lateUpdate(delta: Float) {}
}

/** What a script takes to be called on the game's steady step: physics, what must not depend on the frame rate. */
interface FixedStep {
    /** A steady step of the game: 0 to 8 times a frame. */
    fun fixedUpdate(step: Float)
}

/**
 * What a script takes to make what the graph's passes read and nothing else makes: a light turned into a camera, a table
 * of matrices. [prepare] runs after every script's updates and before anything is drawn — in the editor too, whether or
 * not the scene plays.
 */
interface BeforeDraw {
    /** After the updates, before the passes: write what they read. */
    fun prepare()
}

/**
 * A script as one was written before [Setup] and its pieces: every hook there is, each overridden or not. It still runs,
 * with only what it overrides called; a new script is a [Setup] with the pieces it uses.
 */
abstract class Script : Setup(), PerFrame, FixedStep {
    /** It was added to the entity: once, switched on or off. */
    open fun onCreated() {}
    /** Before its first update: once. */
    open fun start() {}
    /** A steady step of the game: 0 to 8 times a frame. */
    override fun fixedUpdate(step: Float) {}
    /** Once a frame, while it is switched on. */
    override fun update(delta: Float) {}
    /** After every script's update. */
    override fun lateUpdate(delta: Float) {}

    internal final override fun createdCore() = onCreated()
    internal final override fun initializeCore() = start()
}

/** A [Script] that prepares what is drawn, as one was written before [BeforeDraw]. A new one is a [Setup] that takes [BeforeDraw]. */
abstract class GraphicsScript : Script(), BeforeDraw {
    /** After the updates, before the passes: write what they read. */
    override fun prepare() {}
}

/**
 * A script that makes resources — Images, Buffers, views of them, Samplers — and fills its images and buffers on the CPU
 * as they are made: a Script node of the graph names it, and gives each as an output, by its property's name. Its public
 * `var`s are its settings — the Script node's inputs, given there or wired — and it is no component: no entity has it. It
 * runs when what it makes is made, and again when that is made again: a size changed, a setting changed, the script was
 * edited.
 *
 * A resource's arguments are what it is made with, by the name of its node type's input: its source is read for them, so
 * they are written as they are, not worked out — `width`, `height` and `depth` are its extent's, flags are
 * `"eSampled | eStorage"`, and a view names what it is of, `::height.name`. The Script node can say otherwise:
 * `height.format`, `height.extent.x`.
 *
 * ```kotlin
 * class Terrain : ResourceScript() {
 *     var seed = 7
 *     val height = Image(format = "eR32_SFLOAT", width = 256, height = 256, usage = "eSampled | eStorage | eTransferDst")
 *     val heightView = ImageView(image = ::height.name)
 *     override fun init() {
 *         val random = java.util.Random(seed.toLong())
 *         for (i in 0 until height.bytes.capacity() / 4) height.bytes.putFloat(i * 4, random.nextFloat())
 *     }
 * }
 * ```
 */
abstract class ResourceScript {
    /** The scene whose graph it makes them in. */
    lateinit var world: World
        internal set

    /** Fills what it makes: each [Image]'s and [Buffer]'s bytes, zeros to begin with, as they were made. */
    abstract fun init()

    /** One of what a script makes: an output of its Script node, by the property's name. */
    abstract class Resource internal constructor(internal val kind: String, vararg given: Pair<String, Any?>) {
        internal val settings: List<Pair<String, Any>> = given.mapNotNull { (input, value) -> value?.let { input to it } }
        /** The property it is: the Script node's output. */
        var name: String = ""
            internal set
    }

    /** A resource that holds bytes the script writes: an image's texels, a buffer's every byte. */
    abstract class Filled internal constructor(kind: String, vararg given: Pair<String, Any?>) : Resource(kind, *given) {
        internal var data: java.nio.ByteBuffer? = null
        /** Its bytes, zeros to begin with, little-endian: there to be written while `init` runs. `bytes.putFloat(i * 4, ...)`. */
        val bytes: java.nio.ByteBuffer get() = data ?: throw IllegalStateException("$name is filled while init runs, and not otherwise")
    }

    /** An image, its first level filled: each layer one after another, each a row after another from the top, each a texel after another. */
    class Image(format: String? = null, width: Int? = null, height: Int? = null, depth: Int? = null, mipLevels: Int? = null, arrayLayers: Int? = null,
                usage: String? = null, type: String? = null, sampleCount: String? = null)
        : Filled("Image", "format" to format, "extent.x" to width, "extent.y" to height, "extent.z" to depth, "mipLevels" to mipLevels,
                 "arrayLayers" to arrayLayers, "usage" to usage, "type" to type, "sampleCount" to sampleCount) {
        /** Its format, by name, as it was made: `eRGBA8_UNORM`. */
        var format: String = ""
            internal set
        var width: Int = 0
            internal set
        var height: Int = 0
            internal set
        var depth: Int = 0
            internal set
        var layers: Int = 0
            internal set
        /** How many bytes one texel is. */
        var texelBytes: Int = 0
            internal set
    }

    /** A buffer, every byte of it filled. */
    class Buffer(instanceCount: Int? = null, usage: String? = null, type: String? = null, sharedAcrossQueues: Boolean? = null)
        : Filled("Buffer", "instanceCount" to instanceCount, "usage" to usage, "type" to type, "sharedAcrossQueues" to sharedAcrossQueues)

    /** A view of one of the script's images: `ImageView(image = ::height.name)`. */
    class ImageView(image: String? = null, viewType: String? = null, baseMipLevel: Int? = null, mipLevelCount: Int? = null,
                    baseArrayLayer: Int? = null, arrayLayerCount: Int? = null)
        : Resource("ImageView", "image" to image, "viewType" to viewType, "baseMipLevel" to baseMipLevel, "mipLevelCount" to mipLevelCount,
                   "baseArrayLayer" to baseArrayLayer, "arrayLayerCount" to arrayLayerCount)

    /** A view of one of the script's buffers as texels: `BufferView(buffer = ::table.name, format = "eR32_SFLOAT")`. */
    class BufferView(buffer: String? = null, format: String? = null, offset: Long? = null, range: Long? = null)
        : Resource("BufferView", "buffer" to buffer, "format" to format, "offset" to offset, "range" to range)

    /** How an image is read in a shader: filtered, wrapped, compared. */
    class Sampler(minFilter: String? = null, magFilter: String? = null, mipmapMode: String? = null, addressModeU: String? = null,
                  addressModeV: String? = null, addressModeW: String? = null, mipLodBias: Float? = null, anisotropyEnable: Boolean? = null,
                  maxAnisotropy: Float? = null, compareEnable: Boolean? = null, compareOp: String? = null, minLod: Float? = null,
                  maxLod: Float? = null, unnormalizedCoordinates: Boolean? = null)
        : Resource("Sampler", "minFilter" to minFilter, "magFilter" to magFilter, "mipmapMode" to mipmapMode, "addressModeU" to addressModeU,
                   "addressModeV" to addressModeV, "addressModeW" to addressModeW, "mipLodBias" to mipLodBias, "anisotropyEnable" to anisotropyEnable,
                   "maxAnisotropy" to maxAnisotropy, "compareEnable" to compareEnable, "compareOp" to compareOp, "minLod" to minLod,
                   "maxLod" to maxLod, "unnormalizedCoordinates" to unnormalizedCoordinates)
}

/** The least and the most a property's numbers are — each of a vector's — as the editor lets them be set: `@Range(0f, 10f)`. */
@Target(AnnotationTarget.PROPERTY, AnnotationTarget.FIELD)
@Retention(AnnotationRetention.SOURCE)
annotation class Range(val min: Float, val max: Float)

/** The least a property's numbers are — each of a vector's — as the editor lets them be set: `@Min(0f)`. */
@Target(AnnotationTarget.PROPERTY, AnnotationTarget.FIELD)
@Retention(AnnotationRetention.SOURCE)
annotation class Min(val min: Float)

/** A property the editor drags by so much of what it is, not by so much: a distance, an intensity, a size. `@Logarithmic @Min(0f)`. */
@Target(AnnotationTarget.PROPERTY, AnnotationTarget.FIELD)
@Retention(AnnotationRetention.SOURCE)
annotation class Logarithmic

/** What a whole-number property is one of, by name: the first is 0, the second 1. `@Choices("Opaque", "Masked", "Blended")`. */
@Target(AnnotationTarget.PROPERTY, AnnotationTarget.FIELD)
@Retention(AnnotationRetention.SOURCE)
annotation class Choices(vararg val names: String)

/**
 * A kind of what a drawn thing is made of: data and nothing else. Its public vars are a component, shown on an entity as
 * a script's are, which a shader takes by annotating its own members (`[PbrMaterial("roughness")]`). Nothing is called
 * on it. The engine's package has `PbrMaterial` and `BlinnPhongMaterial`; a project may declare more.
 *
 * ```kotlin
 * class Toon : Material() {
 *     var tint = Vec4(1f, 1f, 1f, 1f)
 *     var ramp = Texture("@textures/ramp.png")
 *     @Range(1f, 8f) var bands = 3
 * }
 * ```
 */
abstract class Material

/** A Material's picture, by the name it is found by: `"@textures/wall.png"`, or one of a texel — [White], [Black], [Normal]. */
data class Texture(val name: String) {
    companion object {
        @JvmField val White = Texture("\$white")
        @JvmField val Black = Texture("\$black")
        /** Straight out of the surface. */
        @JvmField val Normal = Texture("\$normal")
    }
}

/** Gives a script's component another name than its class's. */
@Target(AnnotationTarget.CLASS)
@Retention(AnnotationRetention.RUNTIME)
annotation class ScriptName(val name: String)

internal enum class Hook { Created, Enabled, Started, FixedUpdate, Update, LateUpdate, Prepare, Disabled, Destroyed }

/** How a script class's properties are laid out as a component's bytes. */
internal class ScriptLayout private constructor(type: Class<*>) {
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
        val chain = generateSequence<Class<*>>(type) { it.superclass }.takeWhile { it !in bases }.toList().reversed()
        for (declaring in chain) {
            for (field in declaring.declaredFields) {
                if (Modifier.isStatic(field.modifiers) || field.isSynthetic) continue
                if (ResourceScript.Resource::class.java.isAssignableFrom(field.type)) continue     // what a ResourceScript makes: no setting
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
    fun load(script: Any, data: MemorySegment) {
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
    fun store(script: Setup, data: MemorySegment) {
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

        @Synchronized fun of(type: Class<*>): ScriptLayout = layouts.getOrPut(type) { ScriptLayout(type) }
        /** What a script derives from: none of its properties are the script's own. */
        private val bases = setOf(Setup::class.java, Script::class.java, GraphicsScript::class.java, ResourceScript::class.java, Any::class.java)
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
