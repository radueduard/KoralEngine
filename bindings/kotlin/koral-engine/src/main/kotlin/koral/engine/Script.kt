package koral.engine

import java.lang.foreign.Arena
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.lang.foreign.ValueLayout.JAVA_DOUBLE_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_FLOAT_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_LONG
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

/**
 * A script that says what a pass does, a frame at a time: a ScriptPass node of the graph names it, and runs [record] each
 * frame, before the frame is drawn. What it uses — the pipelines it dispatches and draws with, what it draws into and
 * draws, the buffers and images it reads or writes — are its properties of [Pipeline], [GraphicsPipeline], [Framebuffer],
 * [Mesh], [Buffer] and [Image]: each an input of the ScriptPass node, by the property's name, wired a resource. A pipeline's parameters are wired on its node; a Buffer or an Image it writes says so. Its public
 * `var`s are its settings — the node's inputs, given there or wired — and it is no component: no entity has it.
 *
 * In [record] it says what the pass does, in order — dispatches, as many as it likes, each with its push constants by
 * their names in the shader; buffers written and copied; images cleared. What is needed between them is put there. What
 * is wrong is said on the node, and the pass does nothing that frame. Its settings are what the node gives, each frame;
 * anything else it keeps from frame to frame is the one object's, whichever ScriptPass node runs it.
 *
 * ```kotlin
 * class Spread : PassScript() {
 *     var rounds = 4
 *     val step = Pipeline()
 *     val counts = Buffer(writes = true)
 *     override fun record(pass: Pass) {
 *         pass.write(counts, IntArray(4))
 *         repeat(rounds) { pass.dispatch(step, 64, 1, 1, "round" to it) }
 *     }
 * }
 * ```
 */
abstract class PassScript {
    /** The scene whose graph runs it. */
    lateinit var world: World
        internal set

    /** Says what the pass does this frame, into [pass]. */
    abstract fun record(pass: Pass)

    /** One of what a pass script uses: an input of its ScriptPass node, by the property's name. */
    abstract class Use internal constructor(internal val kind: String, internal val writes: Boolean) {
        /** Made by the ScriptPass node, as this node type: one of [Make]'s. Null: wired in. */
        internal open val makes: String? get() = null
        /** What one made is made with, by its node type's inputs. */
        internal open val settings: List<Pair<String, Any>> get() = emptyList()
        /** The property it is: the ScriptPass node's input. */
        var name: String = ""
            internal set
        internal var index = -1
    }

    /** A ComputePipeline it dispatches: its parameters wired on its node. */
    open class Pipeline : Use("Pipeline", false)
    /** A GraphicsPipeline it draws with: its parameters wired on its node. It draws into its node's framebuffer, or one like it. */
    open class GraphicsPipeline : Use("GraphicsPipeline", false)
    /** A Framebuffer it draws into, other than its pipelines' own: of the same formats. */
    open class Framebuffer : Use("Framebuffer", false)
    /** A mesh it draws: a Primitive's, a default one, a model's. */
    class Mesh : Use("Mesh", false)
    /** A buffer it reads — copies from, dispatches from — or, [writes], writes and copies into too. */
    open class Buffer(writes: Boolean = false) : Use("Buffer", writes)
    /** An image it reads, or, [writes], clears too. */
    open class Image(writes: Boolean = false) : Use("Image", writes)

    /**
     * What a pass script makes, rather than is wired: each a node of its ScriptPass's own, made as its arguments say —
     * each one of the node type's inputs, by name; the node can say otherwise, `<property>.<input>` — and an output of
     * the node, by the property's name, for what comes after to use. One that names another of the script's resources or
     * uses, `::canvas.name`, is made of it; a pipeline's shader parameters named as one of them are wired to it. What it
     * makes it writes, and uses as it uses what is wired: `dispatch(blur, …)`, `render(target) { … }`, `copy(…)`.
     *
     * ```kotlin
     * val canvas = Make.Image(format = "eRGBA8_UNORM", width = 256, height = 256, usage = "eColorAttachment | eSampled")
     * val canvasView = Make.ImageView(image = ::canvas.name)
     * val target = Make.Framebuffer(color = ::canvasView.name)
     * val shade = Make.GraphicsPipeline(vertexModule = "shade", fragmentModule = "shade", framebuffer = ::target.name)
     * ```
     */
    object Make {
        private fun given(vararg settings: Pair<String, Any?>) = settings.mapNotNull { (input, value) -> value?.let { input to it } }

        class Image(format: String? = null, width: Int? = null, height: Int? = null, depth: Int? = null, mipLevels: Int? = null, arrayLayers: Int? = null,
                    usage: String? = null, type: String? = null, sampleCount: String? = null) : PassScript.Image(true) {
            override val makes get() = "Image"
            override val settings = given("format" to format, "extent.x" to width, "extent.y" to height, "extent.z" to depth, "mipLevels" to mipLevels,
                                          "arrayLayers" to arrayLayers, "usage" to usage, "type" to type, "sampleCount" to sampleCount)
        }
        class Buffer(instanceCount: Int? = null, usage: String? = null, type: String? = null) : PassScript.Buffer(true) {
            override val makes get() = "Buffer"
            override val settings = given("instanceCount" to instanceCount, "usage" to usage, "type" to type)
        }
        class ImageView(image: String? = null, viewType: String? = null, baseMipLevel: Int? = null, mipLevelCount: Int? = null,
                        baseArrayLayer: Int? = null, arrayLayerCount: Int? = null) : Use("ImageView", false) {
            override val makes get() = "ImageView"
            override val settings = given("image" to image, "viewType" to viewType, "baseMipLevel" to baseMipLevel, "mipLevelCount" to mipLevelCount,
                                          "baseArrayLayer" to baseArrayLayer, "arrayLayerCount" to arrayLayerCount)
        }
        class BufferView(buffer: String? = null, format: String? = null, offset: Long? = null, range: Long? = null) : Use("BufferView", false) {
            override val makes get() = "BufferView"
            override val settings = given("buffer" to buffer, "format" to format, "offset" to offset, "range" to range)
        }
        class Sampler(minFilter: String? = null, magFilter: String? = null, mipmapMode: String? = null, addressModeU: String? = null,
                      addressModeV: String? = null, addressModeW: String? = null, maxAnisotropy: Float? = null, anisotropyEnable: Boolean? = null) : Use("Sampler", false) {
            override val makes get() = "Sampler"
            override val settings = given("minFilter" to minFilter, "magFilter" to magFilter, "mipmapMode" to mipmapMode, "addressModeU" to addressModeU,
                                          "addressModeV" to addressModeV, "addressModeW" to addressModeW, "maxAnisotropy" to maxAnisotropy, "anisotropyEnable" to anisotropyEnable)
        }
        /** Drawn into: [color], a view of one of the script's images, by name; [depth], another, of a depth image. */
        class Framebuffer(color: String? = null, depth: String? = null) : PassScript.Framebuffer() {
            override val makes get() = "Framebuffer"
            override val settings = given("color[0].view" to color, "depthStencil.view" to depth)
        }
        /** Dispatched: [module] the shader module, [entry] its entry point (`computeMain` when not given). */
        class ComputePipeline(module: String? = null, entry: String? = null) : PassScript.Pipeline() {
            override val makes get() = "ComputePipeline"
            override val settings = given("compute.module" to module, "compute.entry" to entry)
        }
        /**
         * Drawn with, into [framebuffer] — one of the script's, or of what it is wired, by name. Each stage's module, its
         * entry point `vertexMain`, `fragmentMain`, `meshMain`, `taskMain` when not given; [stages] `eMesh` for mesh shaders.
         */
        class GraphicsPipeline(vertexModule: String? = null, fragmentModule: String? = null, framebuffer: String? = null, vertexEntry: String? = null,
                               fragmentEntry: String? = null, stages: String? = null, meshModule: String? = null, meshEntry: String? = null,
                               taskModule: String? = null, taskEntry: String? = null) : PassScript.GraphicsPipeline() {
            override val makes get() = "GraphicsPipeline"
            override val settings = given("vertex.module" to vertexModule, "vertex.entry" to vertexEntry, "fragment.module" to fragmentModule,
                                          "fragment.entry" to fragmentEntry, "framebuffer" to framebuffer, "stages" to stages, "mesh.module" to meshModule,
                                          "mesh.entry" to meshEntry, "task.module" to taskModule, "task.entry" to taskEntry)
        }
    }

    /** What the pass does this frame, said in order. */
    class Pass internal constructor(private val commands: MemorySegment, private val arena: Arena,
                                    /** The first frame since the pass was built: what the script makes may be new, and empty — to fill now. */
                                    val made: Boolean) {
        private fun at(use: Use): Int = if (use.index >= 0) use.index else throw IllegalStateException("${use.name.ifEmpty { "a use" }} is not one of the script's own properties")

        /** Runs [pipeline] [x] by [y] by [z] groups, with these push constants: `"radius" to 2f`, a vector, a matrix, a Boolean. */
        fun dispatch(pipeline: Pipeline, x: Int = 1, y: Int = 1, z: Int = 1, vararg push: Pair<String, Any>) {
            Native.kengine_pass_dispatch.invokeExact(commands, at(pipeline), x, y, z)
            push(push)
        }

        /** Runs [pipeline] as many groups as the three numbers of 32 bits at [offset] of [counts] say — numbers a pass before wrote. */
        fun dispatchIndirect(pipeline: Pipeline, counts: Buffer, offset: Long = 0, vararg push: Pair<String, Any>) {
            Native.kengine_pass_dispatch_indirect.invokeExact(commands, at(pipeline), at(counts), offset)
            push(push)
        }

        private fun push(push: Array<out Pair<String, Any>>) {
            for ((name, value) in push) {
                val numbers = numbersOf(name, value)
                val values = arena.allocate(java.lang.foreign.ValueLayout.JAVA_DOUBLE, maxOf(numbers.size, 1).toLong())
                numbers.forEachIndexed { i, n -> values.setAtIndex(java.lang.foreign.ValueLayout.JAVA_DOUBLE, i.toLong(), n) }
                Native.kengine_pass_push.invokeExact(commands, arena.allocateFrom(name), values, numbers.size.toLong())
            }
        }

        private fun numbersOf(name: String, value: Any): DoubleArray = when (value) {
            is Boolean -> doubleArrayOf(if (value) 1.0 else 0.0)
            is Number -> doubleArrayOf(value.toDouble())
            is Vec2 -> doubleArrayOf(value.x.toDouble(), value.y.toDouble())
            is Vec3 -> doubleArrayOf(value.x.toDouble(), value.y.toDouble(), value.z.toDouble())
            is Vec4 -> doubleArrayOf(value.x.toDouble(), value.y.toDouble(), value.z.toDouble(), value.w.toDouble())
            is Mat4 -> DoubleArray(16) { value[it / 4, it % 4].toDouble() }     // a column after another
            is FloatArray -> DoubleArray(value.size) { value[it].toDouble() }
            is IntArray -> DoubleArray(value.size) { value[it].toDouble() }
            is DoubleArray -> value
            else -> throw IllegalArgumentException("push constant '$name' is a ${value.javaClass.simpleName}: a number, a Boolean, a vector, a Mat4 or an array of numbers")
        }

        /** Writes [bytes] into [buffer] at [offset]: both multiples of 4, 64 KiB at most. */
        fun write(buffer: Buffer, bytes: ByteArray, offset: Long = 0) {
            val data = arena.allocate(maxOf(bytes.size, 1).toLong())
            MemorySegment.copy(bytes, 0, data, JAVA_BYTE, 0, bytes.size)
            Native.kengine_pass_write.invokeExact(commands, at(buffer), offset, data, bytes.size.toLong())
        }
        /** Writes [values] into [buffer] at [offset], 32 bits each. */
        fun write(buffer: Buffer, values: IntArray, offset: Long = 0) =
            write(buffer, java.nio.ByteBuffer.allocate(values.size * 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).also { b -> values.forEach { b.putInt(it) } }.array(), offset)
        fun write(buffer: Buffer, values: FloatArray, offset: Long = 0) =
            write(buffer, java.nio.ByteBuffer.allocate(values.size * 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).also { b -> values.forEach { b.putFloat(it) } }.array(), offset)

        /** Copies [size] bytes of [from], at [fromOffset], into [to], at [toOffset]: as many as fit, when 0. */
        fun copy(from: Buffer, to: Buffer, size: Long = 0, fromOffset: Long = 0, toOffset: Long = 0) {
            Native.kengine_pass_copy.invokeExact(commands, at(from), at(to), size, fromOffset, toOffset)
        }

        /** Makes every texel of [image] this colour, in its format's own range. */
        fun clear(image: Image, red: Float = 0f, green: Float = 0f, blue: Float = 0f, alpha: Float = 0f) {
            Native.kengine_pass_clear.invokeExact(commands, at(image), red, green, blue, alpha)
        }

        /** Copies [buffer], from [offset], into [image]: its first level, every layer, a texel after another. */
        fun copy(buffer: Buffer, image: Image, offset: Long = 0) { Native.kengine_pass_copy_buffer_to_image.invokeExact(commands, at(buffer), at(image), offset) }
        /** Copies [image]'s first level, every layer, into [buffer] at [offset]. */
        fun copy(image: Image, buffer: Buffer, offset: Long = 0) { Native.kengine_pass_copy_image_to_buffer.invokeExact(commands, at(image), at(buffer), offset) }
        /** Copies [from] into [to], an image of its size and format. */
        fun copy(from: Image, to: Image) { Native.kengine_pass_copy_image.invokeExact(commands, at(from), at(to)) }
        /** Copies [from] into [to], scaled and filtered to fit it. */
        fun blit(from: Image, to: Image) { Native.kengine_pass_blit.invokeExact(commands, at(from), at(to)) }
        /** Makes [image]'s smaller levels from its largest. */
        fun generateMipmaps(image: Image) { Native.kengine_pass_generate_mipmaps.invokeExact(commands, at(image)) }

        // ---- drawing ----

        /**
         * Draws into [into] — a [GraphicsPipeline]'s framebuffer, or a [Framebuffer] — what [draws] says: cleared to
         * [clear] and [depth] first, or, [load], as it was. Only draws, and what they are drawn with, are said in it.
         */
        fun render(into: Use, clear: Vec4 = Vec4(0f, 0f, 0f, 1f), depth: Float = 1f, load: Boolean = false, draws: Pass.() -> Unit) {
            Native.kengine_pass_begin_rendering.invokeExact(commands, at(into), load, clear.x, clear.y, clear.z, clear.w, depth)
            try { draws() } finally { Native.kengine_pass_end_rendering.invokeExact(commands) }
        }

        /** Where what follows is drawn, in pixels: all of the framebuffer, as a rendering begins. */
        fun viewport(x: Int, y: Int, width: Int, height: Int) { Native.kengine_pass_viewport.invokeExact(commands, x, y, width, height) }
        /** What of the framebuffer what follows may draw over, in pixels. */
        fun scissor(x: Int, y: Int, width: Int, height: Int) { Native.kengine_pass_scissor.invokeExact(commands, x, y, width, height) }
        /** Binds [buffer] at vertex [binding], from [offset], for the draws after it: what a mesh drawn does not bind itself. */
        fun vertices(binding: Int, buffer: Buffer, offset: Long = 0) { Native.kengine_pass_vertex_buffer.invokeExact(commands, binding, at(buffer), offset) }

        /** Draws [vertexCount] vertices with [pipeline], from [firstVertex] — [instances] of them from [firstInstance] — read by its shader as it likes. */
        fun draw(pipeline: GraphicsPipeline, vertexCount: Int = 3, instances: Int = 1, firstVertex: Int = 0, firstInstance: Int = 0, vararg push: Pair<String, Any>) {
            Native.kengine_pass_draw.invokeExact(commands, at(pipeline), vertexCount, instances, firstVertex, firstInstance)
            push(push)
        }
        /** Draws all of [mesh] with [pipeline] — made for its layout — through its indices when it has them. */
        fun drawMesh(pipeline: GraphicsPipeline, mesh: Mesh, instances: Int = 1, firstInstance: Int = 0, vararg push: Pair<String, Any>) {
            Native.kengine_pass_draw_mesh.invokeExact(commands, at(pipeline), at(mesh), instances, firstInstance)
            push(push)
        }
        /** Draws [indexCount] of [mesh]'s indices from [firstIndex] — all of them, or its part's, when 0 — [vertexOffset] added to each. */
        fun drawIndexed(pipeline: GraphicsPipeline, mesh: Mesh, indexCount: Int = 0, instances: Int = 1, firstIndex: Int = 0, vertexOffset: Int = 0,
                        firstInstance: Int = 0, vararg push: Pair<String, Any>) {
            Native.kengine_pass_draw_indexed.invokeExact(commands, at(pipeline), at(mesh), indexCount, instances, firstIndex, vertexOffset, firstInstance)
            push(push)
        }
        /** [count] draws as [args] says from [offset], [stride] bytes apart (0: one after another) — of [mesh]'s vertices, or none. */
        fun drawIndirect(pipeline: GraphicsPipeline, args: Buffer, offset: Long = 0, count: Int = 1, stride: Int = 0, mesh: Mesh? = null, vararg push: Pair<String, Any>) {
            Native.kengine_pass_draw_indirect.invokeExact(commands, at(pipeline), if (mesh == null) -1 else at(mesh), false, at(args), offset, count, stride)
            push(push)
        }
        /** [count] draws through [mesh]'s indices, as [args] says from [offset], [stride] bytes apart. */
        fun drawIndexedIndirect(pipeline: GraphicsPipeline, mesh: Mesh, args: Buffer, offset: Long = 0, count: Int = 1, stride: Int = 0, vararg push: Pair<String, Any>) {
            Native.kengine_pass_draw_indirect.invokeExact(commands, at(pipeline), at(mesh), true, at(args), offset, count, stride)
            push(push)
        }
        /** Draws [pipeline]'s mesh shader, [x] by [y] by [z] task groups. */
        fun drawMeshTasks(pipeline: GraphicsPipeline, x: Int = 1, y: Int = 1, z: Int = 1, vararg push: Pair<String, Any>) {
            Native.kengine_pass_draw_mesh_tasks.invokeExact(commands, at(pipeline), x, y, z)
            push(push)
        }
        /** [count] of them, as [args] says from [offset], [stride] bytes apart. */
        fun drawMeshTasksIndirect(pipeline: GraphicsPipeline, args: Buffer, offset: Long = 0, count: Int = 1, stride: Int = 0, vararg push: Pair<String, Any>) {
            Native.kengine_pass_draw_mesh_tasks_indirect.invokeExact(commands, at(pipeline), at(args), offset, count, stride)
            push(push)
        }
    }
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

/**
 * A property among the few of its component the editor shows at first: taken apart in the graph — a ForEach's Bundle of
 * the component, unbundled — these are on the node, and the rest there to be asked for. A component none of whose
 * properties says it shows them all. `@Shown var roughness = 0.5f`.
 */
@Target(AnnotationTarget.PROPERTY, AnnotationTarget.FIELD)
@Retention(AnnotationRetention.SOURCE)
annotation class Shown

/**
 * A property its script starts over for: while the scene plays, when something other than the script changes it — the
 * inspector, another script — the script is switched off, destroyed and made anew, a new object with the entity's
 * properties as they are, as if it had just been added: what `initialize` set up from it is set up again, and what
 * `onDestroyed` lets go of is let go of first. For what `initialize` reads: `@Reinitialize var particleCount = 100`.
 */
@Target(AnnotationTarget.FIELD)
@Retention(AnnotationRetention.RUNTIME)
annotation class Reinitialize

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
abstract class Material : Data()

/**
 * Structured data and nothing else: a component that no script runs. Its public vars are a component, shown on an
 * entity, saved with the scene and set from a script by name, as a script's are; nothing is called on it, it takes no
 * pieces, and it needs no runtime to be what it is — the engine reads it from its source. What it is for is the
 * graph's, which reads it: a camera, a light, a sky.
 *
 * ```kotlin
 * abstract class Camera : Data() {          // a kind: no entity has it, and a query for it finds every camera
 *     @Min(0.001f) var near = 0.1f
 *     var far = 100f
 * }
 * class PerspectiveCamera : Camera() {
 *     @Range(1f, 179f) var fieldOfView = 60f
 * }
 * ```
 *
 * An abstract one is a kind: what derives from it has its properties first, and is found by its name —
 * `world.with("Camera")`, a graph's `Entity` of `Camera` — with those read from whichever it is.
 */
abstract class Data

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
    class Property(val field: Field, val type: String, val offset: Int, val size: Int) {
        val reinitializes = field.isAnnotationPresent(Reinitialize::class.java)
    }

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
                if (PassScript.Use::class.java.isAssignableFrom(field.type)) continue              // what a PassScript uses: no setting
                // A property the engine keeps is a public var: one with a getter and a setter anyone can call.
                val suffix = field.name.replaceFirstChar { it.uppercaseChar() }
                val getter = declaring.methods.any { (it.name == "get$suffix" || it.name == field.name) && it.parameterCount == 0 }
                val setter = declaring.methods.any { (it.name == "set$suffix" || (field.name.startsWith("is") && it.name == "set" + field.name.drop(2))) && it.parameterCount == 1 }
                if (!getter || !setter) continue
                val (typeName, size, alignment) = describe(field.type)
                    ?: throw EngineException("${type.simpleName}.${field.name} is a ${field.type.simpleName}, which a script cannot keep: its properties are " +
                                             "Float, Double, Int, Long, Boolean, Vec2, Vec3, Vec4, Mat4, Quat, Entity and String. Make it private if it is only this run's.")
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
                "string" -> { p.field.set(script, textAt(data.asSlice(at))); continue }
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
    fun store(script: Any, data: MemorySegment) {
        for (p in properties) {
            val at = p.offset.toLong()
            fun f(i: Int, v: Float) = data.set(JAVA_FLOAT_UNALIGNED, at + i * 4L, v)
            when (p.type) {
                "f32" -> { f(0, p.field.getFloat(script)); continue }
                "f64" -> { data.set(JAVA_DOUBLE_UNALIGNED, at, p.field.getDouble(script)); continue }
                "i32" -> { data.set(JAVA_INT_UNALIGNED, at, p.field.getInt(script)); continue }
                "i64" -> { data.set(JAVA_LONG_UNALIGNED, at, p.field.getLong(script)); continue }
                "bool" -> { data.set(JAVA_BYTE, at, if (p.field.getBoolean(script)) 1 else 0); continue }
                "string" -> { setTextAt(data.asSlice(at), p.field.get(script) as String? ?: ""); continue }
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
        private val bases = setOf(Setup::class.java, Script::class.java, GraphicsScript::class.java, ResourceScript::class.java, PassScript::class.java, Data::class.java,
                                  Material::class.java, Any::class.java)
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
            // Text: the engine's own, laid out as it is there, and read and written through it.
            String::class.java -> Triple("string", textLayout.first, textLayout.second)
            else -> null
        }

        /** How big the engine's text is, and on what: where a text property goes. */
        private val textLayout: Pair<Int, Int> by lazy {
            Arena.ofConfined().use { a ->
                val size = a.allocate(JAVA_LONG)
                val alignment = a.allocate(JAVA_LONG)
                Native.kengine_text_layout.invokeExact(size, alignment)
                Pair(size.get(JAVA_LONG, 0).toInt(), alignment.get(JAVA_LONG, 0).toInt())
            }
        }

        /** The text at [at], a text property in a script's data. */
        fun textAt(at: MemorySegment): String = Arena.ofConfined().use { a ->
            val length = a.allocate(JAVA_LONG)
            val bytes = Native.kengine_text_get.invokeExact(at, length) as MemorySegment
            val count = length.get(JAVA_LONG, 0)
            if (count == 0L) "" else String(bytes.reinterpret(count).toArray(JAVA_BYTE), Charsets.UTF_8)
        }

        /** Sets the text at [at], a text property in a script's data. */
        fun setTextAt(at: MemorySegment, text: String): Unit = Arena.ofConfined().use { a ->
            val bytes = text.toByteArray(Charsets.UTF_8)
            val native = a.allocate(maxOf(bytes.size, 1).toLong())
            MemorySegment.copy(bytes, 0, native, JAVA_BYTE, 0, bytes.size)
            Native.kengine_text_set.invokeExact(at, native, bytes.size.toLong())
            Unit
        }
    }
}
