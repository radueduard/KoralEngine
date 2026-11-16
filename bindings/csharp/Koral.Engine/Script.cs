using System.Reflection;
using System.Runtime.InteropServices;

namespace Koral.Engine;

/// <summary>
/// A script: a class added to entities. Its public fields are a component — data the engine keeps, shows in the
/// inspector, saves with the scene and carries across a reload. Every script is a Setup: it is initialised, switched on
/// and off, and goes. What else it does it says by the pieces it takes — <see cref="PerFrame"/> to be called every frame,
/// <see cref="FixedStep"/> on the game's steady step, <see cref="BeforeDraw"/> before the frame is drawn — and only what
/// it takes is ever called on it.
/// </summary>
/// <remarks>
/// <code>
/// public sealed class Spin : Setup, PerFrame
/// {
///     public float Speed = 1;         // shown in the inspector, saved, kept across a reload
///     public float Angle;
///     private int _frames;            // this run's only: not saved, and gone on a reload
///
///     public void Update(float delta) => Angle += Speed * delta;
/// }
/// </code>
/// A field can be a number, a <c>bool</c>, one of Koral's vectors, a <c>Mat4</c>, a <c>Quat</c> or an
/// <see cref="Engine.Entity"/>. Before each call the fields hold what the engine has; after it, the engine has what they
/// hold. Edit the class while it runs and each entity keeps the fields whose name and type are the same. An entity
/// running a script is given a tag for each piece it takes — <c>PerFrame</c>, <c>FixedStep</c>, <c>BeforeDraw</c> — for a
/// graph's Filter to find it by.
/// </remarks>
public abstract class Setup
{
    /// <summary>The entity this is on.</summary>
    public Entity Entity { get; internal set; }
    /// <summary>The scene it is in: its entities, and the nodes of its graph.</summary>
    public World World { get; internal set; } = null!;
    /// <summary>The keys, the mouse and the gamepads it is steered by: <c>Input.IsKeyHeld(Key.eW)</c>.</summary>
    public Koral.Input Input => World.Input;

    /// <summary>Once, before anything else is called on it while the scene plays.</summary>
    protected virtual void Initialize() { }
    /// <summary>It was switched on, or added switched on.</summary>
    protected virtual void OnEnabled() { }
    /// <summary>It was switched off, or is about to go while on.</summary>
    protected virtual void OnDisabled() { }
    /// <summary>It is about to go: removed, or its entity destroyed.</summary>
    protected virtual void OnDestroyed() { }

    private protected virtual void CreatedCore() { }
    private protected virtual void InitializeCore() => Initialize();

    internal enum Hook { Created, Enabled, Started, FixedUpdate, Update, LateUpdate, Prepare, Disabled, Destroyed }

    internal void Call(Hook hook, float time)
    {
        switch (hook)
        {
            case Hook.Created: CreatedCore(); break;
            case Hook.Enabled: OnEnabled(); break;
            case Hook.Started: InitializeCore(); break;
            case Hook.FixedUpdate: if (this is FixedStep fixedStep) fixedStep.FixedUpdate(time); break;
            case Hook.Update: if (this is PerFrame perFrame) perFrame.Update(time); break;
            case Hook.LateUpdate: if (this is PerFrame late) late.LateUpdate(time); break;
            case Hook.Prepare: if (this is BeforeDraw draw) draw.Prepare(); break;
            case Hook.Disabled: OnDisabled(); break;
            case Hook.Destroyed: OnDestroyed(); break;
        }
    }
}

/// <summary>What a script takes to be called every frame while the scene plays.</summary>
public interface PerFrame
{
    /// <summary>Once a frame, while it is switched on.</summary>
    void Update(float delta);
    /// <summary>After every script's Update: what follows what the others did — a camera following its target.</summary>
    void LateUpdate(float delta) { }
}

/// <summary>What a script takes to be called on the game's steady step: physics, what must not depend on the frame rate.</summary>
public interface FixedStep
{
    /// <summary>A steady step of the game: 0 to 8 times a frame.</summary>
    void FixedUpdate(float step);
}

/// <summary>
/// What a script takes to make what the graph's passes read and nothing else makes: a light turned into a camera, a
/// table of matrices. <see cref="Prepare"/> runs after every script's updates and before anything is drawn — in the
/// editor too, whether or not the scene plays.
/// </summary>
/// <remarks>
/// <code>
/// public sealed class LightCamera : Setup, BeforeDraw
/// {
///     public Vec3 Position;
///     public void Prepare() =>
///         World.Buffer("light camera")?.Write([KMath.LookAt(Position, Vec3.Zero, Vec3.UnitY)]);
/// }
/// </code>
/// </remarks>
public interface BeforeDraw
{
    /// <summary>After the updates, before the passes: write what they read.</summary>
    void Prepare();
}

/// <summary>
/// A script as one was written before <see cref="Setup"/> and its pieces: every hook there is, each overridden or not.
/// It still runs, with only what it overrides called; a new script is a <see cref="Setup"/> with the pieces it uses.
/// </summary>
public abstract class Script : Setup, PerFrame, FixedStep
{
    /// <summary>It was added to the entity: once, switched on or off.</summary>
    protected virtual void OnCreated() { }
    /// <summary>Before its first update: once.</summary>
    protected virtual void Start() { }
    /// <summary>A steady step of the game: 0 to 8 times a frame.</summary>
    protected virtual void FixedUpdate(float step) { }
    /// <summary>Once a frame, while it is switched on.</summary>
    protected virtual void Update(float delta) { }
    /// <summary>After every script's Update.</summary>
    protected virtual void LateUpdate(float delta) { }

    private protected sealed override void CreatedCore() => OnCreated();
    private protected sealed override void InitializeCore() => Start();
    void PerFrame.Update(float delta) => Update(delta);
    void PerFrame.LateUpdate(float delta) => LateUpdate(delta);
    void FixedStep.FixedUpdate(float step) => FixedUpdate(step);
}

/// <summary>
/// A <see cref="Script"/> that prepares what is drawn, as one was written before <see cref="BeforeDraw"/>. A new one is a
/// <see cref="Setup"/> that takes <see cref="BeforeDraw"/>.
/// </summary>
public abstract class GraphicsScript : Script, BeforeDraw
{
    /// <summary>After the updates, before the passes: write what they read.</summary>
    protected virtual void Prepare() { }
    void BeforeDraw.Prepare() => Prepare();
}

/// <summary>
/// A script that says what a pass does, a frame at a time: a ScriptPass node of the graph names it, and runs
/// <see cref="Record"/> each frame, before the frame is drawn. What it uses — the pipelines it dispatches, the buffers and
/// images it reads or writes — are its fields of <see cref="Pipeline"/>, <see cref="Buffer"/> and <see cref="Image"/>: each
/// an input of the ScriptPass node, by the field's name, wired a resource. A pipeline's parameters are wired on its node; a
/// Buffer or an Image it writes says so. Its public fields are its settings — the node's inputs — and it is no component.
/// </summary>
/// <remarks>
/// In Record it says what the pass does, in order: dispatches, as many as it likes, each with its push constants by their
/// names in the shader; buffers written and copied; images cleared. What is needed between them is put there. What is
/// wrong is said on the node, and the pass does nothing that frame. Its settings are what the node gives, each frame;
/// anything else it keeps from frame to frame is the one object's, whichever ScriptPass node runs it.
/// <code>
/// public sealed class Spread : PassScript
/// {
///     public int Rounds = 4;
///     public Pipeline Step = new();
///     public Buffer Counts = new(writes: true);
///     protected override void Record(Pass pass)
///     {
///         pass.Write(Counts, new int[4]);
///         for (var i = 0; i &lt; Rounds; ++i) pass.Dispatch(Step, 64, 1, 1, ("round", i));
///     }
/// }
/// </code>
/// </remarks>
public abstract class PassScript
{
    /// <summary>The scene whose graph runs it.</summary>
    public World World { get; internal set; } = null!;
    /// <summary>Says what the pass does this frame, into <paramref name="pass"/>.</summary>
    protected abstract void Record(Pass pass);
    internal void RecordCore(Pass pass) => Record(pass);

    /// <summary>One of what a pass script uses: an input of its ScriptPass node, by the field's name.</summary>
    public abstract class Use
    {
        internal abstract string Kind { get; }
        /// <summary>The field it is: the ScriptPass node's input.</summary>
        public string Name { get; internal set; } = "";
        internal int Index = -1;
        /// <summary>Made by the ScriptPass node, as this node type: one of <see cref="Make"/>'s. Null: wired in.</summary>
        internal virtual string? Makes => null;
        /// <summary>What one made is made with, by its node type's inputs.</summary>
        internal readonly List<(string Input, object Value)> Settings = [];
        private protected void Set(string input, object? value) { if (value is not null) Settings.Add((input, value)); }
    }

    /// <summary>A ComputePipeline it dispatches: its parameters wired on its node.</summary>
    public class Pipeline : Use { internal override string Kind => "Pipeline"; }
    /// <summary>A GraphicsPipeline it draws with: its parameters wired on its node. It draws into its node's framebuffer, or one like it.</summary>
    public class GraphicsPipeline : Use { internal override string Kind => "GraphicsPipeline"; }
    /// <summary>A Framebuffer it draws into, other than its pipelines' own: of the same formats.</summary>
    public class Framebuffer : Use { internal override string Kind => "Framebuffer"; }
    /// <summary>A mesh it draws: a Primitive's, a default one, a model's.</summary>
    public sealed class Mesh : Use { internal override string Kind => "Mesh"; }
    /// <summary>A buffer it reads — copies from, dispatches from — or, <paramref name="writes"/>, writes and copies into too.</summary>
    public class Buffer(bool writes = false) : Use { internal override string Kind => "Buffer"; internal bool WritesIt { get; } = writes; }
    /// <summary>An image it reads, or, <paramref name="writes"/>, clears too.</summary>
    public class Image(bool writes = false) : Use { internal override string Kind => "Image"; internal bool WritesIt { get; } = writes; }

    /// <summary>
    /// What a pass script makes, rather than is wired: each a node of its ScriptPass's own, made as its arguments say —
    /// each one of the node type's inputs, by name; the node can say otherwise, <c>&lt;field&gt;.&lt;input&gt;</c> — and an
    /// output of the node, by the field's name, for what comes after to use. One that names another of the script's
    /// resources or uses, <c>nameof(Canvas)</c>, is made of it; a pipeline's shader parameters named as one of them are
    /// wired to it. What it makes it writes, and uses as it uses what is wired.
    /// </summary>
    public static class Make
    {
        public sealed class Image : PassScript.Image
        {
            public Image(string? format = null, int? width = null, int? height = null, int? depth = null, int? mipLevels = null, int? arrayLayers = null,
                         string? usage = null, string? type = null, string? sampleCount = null) : base(true)
            {
                Set("format", format); Set("extent.x", width); Set("extent.y", height); Set("extent.z", depth); Set("mipLevels", mipLevels);
                Set("arrayLayers", arrayLayers); Set("usage", usage); Set("type", type); Set("sampleCount", sampleCount);
            }
            internal override string? Makes => "Image";
        }
        public sealed class Buffer : PassScript.Buffer
        {
            public Buffer(int? instanceCount = null, string? usage = null, string? type = null) : base(true) { Set("instanceCount", instanceCount); Set("usage", usage); Set("type", type); }
            internal override string? Makes => "Buffer";
        }
        public sealed class ImageView : Use
        {
            public ImageView(string? image = null, string? viewType = null, int? baseMipLevel = null, int? mipLevelCount = null, int? baseArrayLayer = null, int? arrayLayerCount = null)
            {
                Set("image", image); Set("viewType", viewType); Set("baseMipLevel", baseMipLevel); Set("mipLevelCount", mipLevelCount);
                Set("baseArrayLayer", baseArrayLayer); Set("arrayLayerCount", arrayLayerCount);
            }
            internal override string Kind => "ImageView";
            internal override string? Makes => "ImageView";
        }
        public sealed class BufferView : Use
        {
            public BufferView(string? buffer = null, string? format = null, long? offset = null, long? range = null) { Set("buffer", buffer); Set("format", format); Set("offset", offset); Set("range", range); }
            internal override string Kind => "BufferView";
            internal override string? Makes => "BufferView";
        }
        public sealed class Sampler : Use
        {
            public Sampler(string? minFilter = null, string? magFilter = null, string? mipmapMode = null, string? addressModeU = null, string? addressModeV = null,
                           string? addressModeW = null, float? maxAnisotropy = null, bool? anisotropyEnable = null)
            {
                Set("minFilter", minFilter); Set("magFilter", magFilter); Set("mipmapMode", mipmapMode); Set("addressModeU", addressModeU);
                Set("addressModeV", addressModeV); Set("addressModeW", addressModeW); Set("maxAnisotropy", maxAnisotropy); Set("anisotropyEnable", anisotropyEnable);
            }
            internal override string Kind => "Sampler";
            internal override string? Makes => "Sampler";
        }
        /// <summary>Drawn into: <c>color</c>, a view of one of the script's images, by name; <c>depth</c>, another, of a depth image.</summary>
        public sealed class Framebuffer : PassScript.Framebuffer
        {
            public Framebuffer(string? color = null, string? depth = null) { Set("color[0].view", color); Set("depthStencil.view", depth); }
            internal override string? Makes => "Framebuffer";
        }
        /// <summary>Dispatched: <c>module</c> the shader module, <c>entry</c> its entry point (<c>computeMain</c> when not given).</summary>
        public sealed class ComputePipeline : PassScript.Pipeline
        {
            public ComputePipeline(string? module = null, string? entry = null) { Set("compute.module", module); Set("compute.entry", entry); }
            internal override string? Makes => "ComputePipeline";
        }
        /// <summary>Drawn with, into <c>framebuffer</c>: each stage's module, its entry point <c>vertexMain</c>, <c>fragmentMain</c>... when not given.</summary>
        public sealed class GraphicsPipeline : PassScript.GraphicsPipeline
        {
            public GraphicsPipeline(string? vertexModule = null, string? fragmentModule = null, string? framebuffer = null, string? vertexEntry = null,
                                    string? fragmentEntry = null, string? stages = null, string? meshModule = null, string? meshEntry = null,
                                    string? taskModule = null, string? taskEntry = null)
            {
                Set("vertex.module", vertexModule); Set("vertex.entry", vertexEntry); Set("fragment.module", fragmentModule); Set("fragment.entry", fragmentEntry);
                Set("framebuffer", framebuffer); Set("stages", stages); Set("mesh.module", meshModule); Set("mesh.entry", meshEntry);
                Set("task.module", taskModule); Set("task.entry", taskEntry);
            }
            internal override string? Makes => "GraphicsPipeline";
        }
    }

    internal static bool WritesOf(Use use) => use switch { Buffer b => b.WritesIt, Image i => i.WritesIt, _ => false };

    /// <summary>What the pass does this frame, said in order.</summary>
    public sealed unsafe class Pass
    {
        private readonly IntPtr _commands;
        internal Pass(IntPtr commands, bool made) { _commands = commands; Made = made; }
        /// <summary>The first frame since the pass was built: what the script makes may be new, and empty — to fill now.</summary>
        public bool Made { get; }

        private static uint At(Use use) => use.Index >= 0 ? (uint)use.Index : throw new InvalidOperationException($"{(use.Name.Length > 0 ? use.Name : "a use")} is not one of the script's own fields");

        /// <summary>Runs <paramref name="pipeline"/> so many groups each way, with these push constants: <c>("radius", 2f)</c>, a vector, a Mat4, a bool.</summary>
        public void Dispatch(Pipeline pipeline, uint x = 1, uint y = 1, uint z = 1, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_dispatch(_commands, At(pipeline), x, y, z);
            Push(push);
        }

        /// <summary>Runs <paramref name="pipeline"/> as many groups as the three numbers of 32 bits at <paramref name="offset"/> of <paramref name="counts"/> say.</summary>
        public void DispatchIndirect(Pipeline pipeline, Buffer counts, ulong offset = 0, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_dispatch_indirect(_commands, At(pipeline), At(counts), offset);
            Push(push);
        }

        private void Push((string Name, object Value)[] push)
        {
            foreach (var (name, value) in push)
            {
                var numbers = NumbersOf(name, value);
                fixed (double* at = numbers) Native.kengine_pass_push(_commands, name, at, (nuint)numbers.Length);
            }
        }

        private static double[] NumbersOf(string name, object value) => value switch
        {
            bool on => [on ? 1 : 0],
            Vec2 v => [v.X, v.Y],
            Vec3 v => [v.X, v.Y, v.Z],
            Vec4 v => [v.X, v.Y, v.Z, v.W],
            Mat4 m => Enumerable.Range(0, 16).Select(i => (double)m[i / 4, i % 4]).ToArray(),     // a column after another
            float[] a => a.Select(f => (double)f).ToArray(),
            int[] a => a.Select(n => (double)n).ToArray(),
            uint[] a => a.Select(n => (double)n).ToArray(),
            double[] a => a,
            IConvertible number => [number.ToDouble(System.Globalization.CultureInfo.InvariantCulture)],
            _ => throw new ArgumentException($"push constant '{name}' is a {value.GetType().Name}: a number, a bool, a vector, a Mat4 or an array of numbers"),
        };

        /// <summary>Writes <paramref name="bytes"/> into <paramref name="buffer"/> at <paramref name="offset"/>: both multiples of 4, 64 KiB at most.</summary>
        public void Write(Buffer buffer, ReadOnlySpan<byte> bytes, ulong offset = 0)
        {
            fixed (byte* at = bytes) Native.kengine_pass_write(_commands, At(buffer), offset, at, (nuint)bytes.Length);
        }
        /// <summary>Writes <paramref name="values"/> into <paramref name="buffer"/> at <paramref name="offset"/>: <c>int</c>s, <c>float</c>s, <c>Vec4</c>s.</summary>
        public void Write<T>(Buffer buffer, ReadOnlySpan<T> values, ulong offset = 0) where T : unmanaged => Write(buffer, MemoryMarshal.AsBytes(values), offset);
        public void Write<T>(Buffer buffer, T[] values, ulong offset = 0) where T : unmanaged => Write(buffer, MemoryMarshal.AsBytes(values.AsSpan()), offset);

        /// <summary>Copies <paramref name="size"/> bytes of <paramref name="from"/>, at <paramref name="fromOffset"/>, into <paramref name="to"/>, at <paramref name="toOffset"/>: as many as fit, when 0.</summary>
        public void Copy(Buffer from, Buffer to, ulong size = 0, ulong fromOffset = 0, ulong toOffset = 0) =>
            Native.kengine_pass_copy(_commands, At(from), At(to), size, fromOffset, toOffset);

        /// <summary>Makes every texel of <paramref name="image"/> this colour, in its format's own range.</summary>
        public void Clear(Image image, float red = 0, float green = 0, float blue = 0, float alpha = 0) =>
            Native.kengine_pass_clear(_commands, At(image), red, green, blue, alpha);

        /// <summary>Copies <paramref name="buffer"/>, from <paramref name="offset"/>, into <paramref name="image"/>: its first level, every layer.</summary>
        public void Copy(Buffer buffer, Image image, ulong offset = 0) => Native.kengine_pass_copy_buffer_to_image(_commands, At(buffer), At(image), offset);
        /// <summary>Copies <paramref name="image"/>'s first level, every layer, into <paramref name="buffer"/> at <paramref name="offset"/>.</summary>
        public void Copy(Image image, Buffer buffer, ulong offset = 0) => Native.kengine_pass_copy_image_to_buffer(_commands, At(image), At(buffer), offset);
        /// <summary>Copies <paramref name="from"/> into <paramref name="to"/>, an image of its size and format.</summary>
        public void Copy(Image from, Image to) => Native.kengine_pass_copy_image(_commands, At(from), At(to));
        /// <summary>Copies <paramref name="from"/> into <paramref name="to"/>, scaled and filtered to fit it.</summary>
        public void Blit(Image from, Image to) => Native.kengine_pass_blit(_commands, At(from), At(to));
        /// <summary>Makes <paramref name="image"/>'s smaller levels from its largest.</summary>
        public void GenerateMipmaps(Image image) => Native.kengine_pass_generate_mipmaps(_commands, At(image));

        // ---- drawing ----

        /// <summary>
        /// Draws into <paramref name="into"/> — a GraphicsPipeline's framebuffer, or a Framebuffer — until the returned
        /// scope is disposed: cleared to <paramref name="clear"/> and <paramref name="depth"/> first, or, <paramref name="load"/>,
        /// as it was. Only draws, and what they are drawn with, are said in it: <c>using (pass.Render(Shade)) pass.Draw(Shade);</c>
        /// </summary>
        public Rendering Render(Use into, Vec4? clear = null, float depth = 1, bool load = false)
        {
            var color = clear ?? new Vec4(0, 0, 0, 1);
            Native.kengine_pass_begin_rendering(_commands, At(into), load, color.X, color.Y, color.Z, color.W, depth);
            return new Rendering(_commands);
        }

        /// <summary>A rendering begun: drawn into once it is disposed.</summary>
        public readonly struct Rendering : IDisposable
        {
            private readonly IntPtr _commands;
            internal Rendering(IntPtr commands) { _commands = commands; }
            public void Dispose() => Native.kengine_pass_end_rendering(_commands);
        }

        /// <summary>Where what follows is drawn, in pixels: all of the framebuffer, as a rendering begins.</summary>
        public void Viewport(uint x, uint y, uint width, uint height) => Native.kengine_pass_viewport(_commands, x, y, width, height);
        /// <summary>What of the framebuffer what follows may draw over, in pixels.</summary>
        public void Scissor(uint x, uint y, uint width, uint height) => Native.kengine_pass_scissor(_commands, x, y, width, height);
        /// <summary>Binds <paramref name="buffer"/> at vertex <paramref name="binding"/>, for the draws after it: what a mesh drawn does not bind itself.</summary>
        public void Vertices(uint binding, Buffer buffer, ulong offset = 0) => Native.kengine_pass_vertex_buffer(_commands, binding, At(buffer), offset);

        /// <summary>Draws so many vertices with <paramref name="pipeline"/>, read by its shader as it likes.</summary>
        public void Draw(GraphicsPipeline pipeline, uint vertexCount = 3, uint instances = 1, uint firstVertex = 0, uint firstInstance = 0, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_draw(_commands, At(pipeline), vertexCount, instances, firstVertex, firstInstance);
            Push(push);
        }
        /// <summary>Draws all of <paramref name="mesh"/> with <paramref name="pipeline"/> — made for its layout — through its indices when it has them.</summary>
        public void DrawMesh(GraphicsPipeline pipeline, Mesh mesh, uint instances = 1, uint firstInstance = 0, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_draw_mesh(_commands, At(pipeline), At(mesh), instances, firstInstance);
            Push(push);
        }
        /// <summary>Draws so many of <paramref name="mesh"/>'s indices — all of them, or its part's, when 0.</summary>
        public void DrawIndexed(GraphicsPipeline pipeline, Mesh mesh, uint indexCount = 0, uint instances = 1, uint firstIndex = 0, int vertexOffset = 0, uint firstInstance = 0,
                                params (string Name, object Value)[] push)
        {
            Native.kengine_pass_draw_indexed(_commands, At(pipeline), At(mesh), indexCount, instances, firstIndex, vertexOffset, firstInstance);
            Push(push);
        }
        /// <summary>So many draws as <paramref name="args"/> says — of <paramref name="mesh"/>'s vertices, or none.</summary>
        public void DrawIndirect(GraphicsPipeline pipeline, Buffer args, ulong offset = 0, uint count = 1, uint stride = 0, Mesh? mesh = null, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_draw_indirect(_commands, At(pipeline), mesh is null ? uint.MaxValue : At(mesh), false, At(args), offset, count, stride);
            Push(push);
        }
        /// <summary>So many draws through <paramref name="mesh"/>'s indices, as <paramref name="args"/> says.</summary>
        public void DrawIndexedIndirect(GraphicsPipeline pipeline, Mesh mesh, Buffer args, ulong offset = 0, uint count = 1, uint stride = 0, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_draw_indirect(_commands, At(pipeline), At(mesh), true, At(args), offset, count, stride);
            Push(push);
        }
        /// <summary>Draws <paramref name="pipeline"/>'s mesh shader, so many task groups each way.</summary>
        public void DrawMeshTasks(GraphicsPipeline pipeline, uint x = 1, uint y = 1, uint z = 1, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_draw_mesh_tasks(_commands, At(pipeline), x, y, z);
            Push(push);
        }
        /// <summary>So many of them as <paramref name="args"/> says.</summary>
        public void DrawMeshTasksIndirect(GraphicsPipeline pipeline, Buffer args, ulong offset = 0, uint count = 1, uint stride = 0, params (string Name, object Value)[] push)
        {
            Native.kengine_pass_draw_mesh_tasks_indirect(_commands, At(pipeline), At(args), offset, count, stride);
            Push(push);
        }
    }
}

/// <summary>
/// A script that makes resources — Images, Buffers, views of them, Samplers — and fills its images and buffers on the CPU
/// as they are made: a Script node of the graph names it, and gives each as an output, by its field's name. Its other
/// public fields are its settings — the Script node's inputs, given there or wired — and it is no component: no entity has
/// it. It runs when what it makes is made, and again when that is made again: a size changed, a field changed, the
/// script was edited.
/// </summary>
/// <remarks>
/// A resource field's arguments are what it is made with, by the name of its node type's input: its source is read for
/// them, so they are written as they are, not worked out — <c>width</c>, <c>height</c> and <c>depth</c> are its extent's,
/// flags are <c>"eSampled | eStorage"</c>, and a view names what it is of, <c>nameof(Height)</c>. The Script node can say
/// otherwise: <c>Height.format</c>, <c>Height.extent.x</c>.
/// <code>
/// public sealed class Terrain : ResourceScript
/// {
///     public int Seed = 7;
///     public Image Height = new(format: "eR32_SFLOAT", width: 256, height: 256, usage: "eSampled | eStorage | eTransferDst");
///     public ImageView HeightView = new(image: nameof(Height));
///     public Sampler Smooth = new(minFilter: "eLinear", magFilter: "eLinear");
///     protected override void Init()
///     {
///         var random = new Random(Seed);
///         foreach (ref var h in Height.As&lt;float&gt;()) h = random.NextSingle();
///     }
/// }
/// </code>
/// </remarks>
public abstract class ResourceScript
{
    /// <summary>The scene whose graph it makes them in.</summary>
    public World World { get; internal set; } = null!;
    /// <summary>Fills what it makes: each <see cref="Image"/>'s and <see cref="Buffer"/>'s bytes, zeros to begin with, as they were made.</summary>
    protected abstract void Init();
    internal void InitCore() => Init();

    /// <summary>One of what a script makes: an output of its Script node, by the field's name.</summary>
    public abstract class Resource
    {
        internal readonly List<(string Input, object Value)> Settings = [];
        internal abstract string Kind { get; }
        /// <summary>The field it is: the Script node's output.</summary>
        public string Name { get; internal set; } = "";
        private protected void Set(string input, object? value) { if (value is not null) Settings.Add((input, value)); }
    }

    /// <summary>A resource that holds bytes the script writes: an image's texels, a buffer's every byte.</summary>
    public abstract unsafe class Filled : Resource
    {
        internal byte* Data;
        internal int Size;
        /// <summary>Its bytes, zeros to begin with: there to be written while <c>Init</c> runs.</summary>
        public Span<byte> Bytes => Data is null ? throw new InvalidOperationException($"{Name} is filled while Init runs, and not otherwise") : new Span<byte>(Data, Size);
        /// <summary>Its bytes as <typeparamref name="T"/>s: <c>Span&lt;uint&gt;</c> for an RGBA8 image, <c>Span&lt;Vec4&gt;</c> for an RGBA32F one, <c>Span&lt;float&gt;</c>.</summary>
        public Span<T> As<T>() where T : unmanaged => MemoryMarshal.Cast<byte, T>(Bytes);
    }

    /// <summary>An image, its first level filled: each layer one after another, each a row after another from the top, each a texel after another.</summary>
    public sealed class Image : Filled
    {
        public Image(string? format = null, uint? width = null, uint? height = null, uint? depth = null, uint? mipLevels = null, uint? arrayLayers = null,
                     string? usage = null, string? type = null, string? sampleCount = null)
        {
            Set("format", format); Set("extent.x", width); Set("extent.y", height); Set("extent.z", depth); Set("mipLevels", mipLevels);
            Set("arrayLayers", arrayLayers); Set("usage", usage); Set("type", type); Set("sampleCount", sampleCount);
        }
        internal override string Kind => "Image";
        /// <summary>Its format, by name, as it was made: <c>eRGBA8_UNORM</c>.</summary>
        public string Format { get; internal set; } = "";
        public int Width { get; internal set; }
        public int Height { get; internal set; }
        public int Depth { get; internal set; }
        public int Layers { get; internal set; }
        /// <summary>How many bytes one texel is.</summary>
        public int TexelBytes { get; internal set; }
    }

    /// <summary>A buffer, every byte of it filled.</summary>
    public sealed class Buffer : Filled
    {
        public Buffer(uint? instanceCount = null, string? usage = null, string? type = null, bool? sharedAcrossQueues = null)
        {
            Set("instanceCount", instanceCount); Set("usage", usage); Set("type", type); Set("sharedAcrossQueues", sharedAcrossQueues);
        }
        internal override string Kind => "Buffer";
    }

    /// <summary>A view of one of the script's images: <c>new(image: nameof(Height))</c>.</summary>
    public sealed class ImageView : Resource
    {
        public ImageView(string? image = null, string? viewType = null, uint? baseMipLevel = null, uint? mipLevelCount = null,
                         uint? baseArrayLayer = null, uint? arrayLayerCount = null)
        {
            Set("image", image); Set("viewType", viewType); Set("baseMipLevel", baseMipLevel); Set("mipLevelCount", mipLevelCount);
            Set("baseArrayLayer", baseArrayLayer); Set("arrayLayerCount", arrayLayerCount);
        }
        internal override string Kind => "ImageView";
    }

    /// <summary>A view of one of the script's buffers as texels: <c>new(buffer: nameof(Table), format: "eR32_SFLOAT")</c>.</summary>
    public sealed class BufferView : Resource
    {
        public BufferView(string? buffer = null, string? format = null, ulong? offset = null, ulong? range = null)
        {
            Set("buffer", buffer); Set("format", format); Set("offset", offset); Set("range", range);
        }
        internal override string Kind => "BufferView";
    }

    /// <summary>How an image is read in a shader: filtered, wrapped, compared.</summary>
    public sealed class Sampler : Resource
    {
        public Sampler(string? minFilter = null, string? magFilter = null, string? mipmapMode = null, string? addressModeU = null, string? addressModeV = null,
                       string? addressModeW = null, float? mipLodBias = null, bool? anisotropyEnable = null, float? maxAnisotropy = null,
                       bool? compareEnable = null, string? compareOp = null, float? minLod = null, float? maxLod = null, bool? unnormalizedCoordinates = null)
        {
            Set("minFilter", minFilter); Set("magFilter", magFilter); Set("mipmapMode", mipmapMode); Set("addressModeU", addressModeU);
            Set("addressModeV", addressModeV); Set("addressModeW", addressModeW); Set("mipLodBias", mipLodBias); Set("anisotropyEnable", anisotropyEnable);
            Set("maxAnisotropy", maxAnisotropy); Set("compareEnable", compareEnable); Set("compareOp", compareOp); Set("minLod", minLod);
            Set("maxLod", maxLod); Set("unnormalizedCoordinates", unnormalizedCoordinates);
        }
        internal override string Kind => "Sampler";
    }
}

/// <summary>
/// The least and the most a field's numbers are — each of a vector's — as the editor lets them be set: <c>[Range(0, 10)]</c>.
/// Read from the script's source, as Unity's is.
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class RangeAttribute(float min, float max) : Attribute
{
    public float Min { get; } = min;
    public float Max { get; } = max;
}

/// <summary>The least a field's numbers are — each of a vector's — as the editor lets them be set: <c>[Min(0)]</c>.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class MinAttribute(float min) : Attribute
{
    public float Min { get; } = min;
}

/// <summary>
/// A field the editor drags by so much of what it is, not by so much: a distance, an intensity, a size that may be 0.1 or
/// 10 000. <c>[Logarithmic, Min(0)]</c>. Read from the script's source.
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class LogarithmicAttribute : Attribute;

/// <summary>
/// A field among the few of its component the editor shows at first: taken apart in the graph — a ForEach's Bundle of the
/// component, unbundled — these are on the node, and the rest there to be asked for. A component none of whose fields
/// says it shows them all. <c>[Shown] public float roughness;</c> Read from the script's source.
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class ShownAttribute : Attribute;

/// <summary>
/// A field its script starts over for: while the scene plays, when something other than the script changes it — the
/// inspector, another script — the script is switched off, destroyed and made anew, a new object with the entity's fields
/// as they are, as if it had just been added: what Initialize set up from it is set up again, and what OnDestroyed lets
/// go of is let go of first. For what Initialize reads: <c>[Reinitialize] public int ParticleCount = 100;</c>
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class ReinitializeAttribute : Attribute;

/// <summary>
/// What a whole-number field is one of, by name: the first is 0, the second 1. The editor shows and is given the name.
/// <c>[Choices("Opaque", "Masked", "Blended")] public uint alphaMode;</c>. Read from the script's source.
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class ChoicesAttribute(params string[] names) : Attribute
{
    public string[] Names { get; } = names;
}

/// <summary>
/// A kind of what a drawn thing is made of: data and nothing else. Its public fields are a component, shown on an
/// entity as a script's are, which a shader takes by annotating its own members (<c>[PbrMaterial("roughness")]</c>).
/// Nothing is called on it. The engine's package has <c>PbrMaterial</c> and <c>BlinnPhongMaterial</c>; a project may
/// declare more.
/// <code>
/// public sealed class Toon : Material
/// {
///     public Vec4 tint = new(1f, 1f, 1f, 1f);
///     public Texture ramp = "@textures/ramp.png";
///     [Range(1, 8)] public int bands = 3;
/// }
/// </code>
/// </summary>
public abstract class Material : Data;

/// <summary>
/// Structured data and nothing else: a component that no script runs. Its public fields are a component, shown on an
/// entity, saved with the scene and set from a script by name, as a script's are; nothing is called on it, it takes no
/// pieces, and it needs no runtime to be what it is — the engine reads it from its source. What it is for is the
/// graph's, which reads it: a camera, a light, a sky.
/// <code>
/// public abstract class Camera : Data           // a kind: no entity has it, and a query for it finds every camera
/// {
///     [Min(0.001f)] public float near = 0.1f;
///     public float far = 100f;
/// }
/// public sealed class PerspectiveCamera : Camera
/// {
///     [Range(1f, 179f)] public float fieldOfView = 60f;
/// }
/// </code>
/// An abstract one is a kind: what derives from it has its fields first, and is found by its name —
/// <c>World.With("Camera")</c>, a graph's <c>Entity</c> of <c>Camera</c> — with those fields read from whichever it is.
/// </summary>
public abstract class Data;

/// <summary>
/// A Material's picture, by the name it is found by: <c>"@textures/wall.png"</c>, or one of a texel —
/// <see cref="White"/>, <see cref="Black"/>, <see cref="Normal"/> (straight out of the surface).
/// </summary>
public readonly record struct Texture(string Name)
{
    public static readonly Texture White = new("$white");
    public static readonly Texture Black = new("$black");
    public static readonly Texture Normal = new("$normal");

    public static implicit operator Texture(string name) => new(name);
}

/// <summary>Gives a script's component another name than its class's.</summary>
[AttributeUsage(AttributeTargets.Class)]
public sealed class ScriptNameAttribute(string name) : Attribute
{
    public string Name { get; } = name;
}

/// <summary>How a script class's fields are laid out as a component's bytes.</summary>
internal sealed unsafe class ScriptLayout
{
    internal sealed record Field(FieldInfo Info, string Type, int Offset, int Size)
    {
        public bool Reinitializes { get; } = Info.IsDefined(typeof(ReinitializeAttribute));
    }

    public string Name { get; }
    public int Size { get; }
    public int Alignment { get; }
    public IReadOnlyList<Field> Fields { get; }

    private static readonly Dictionary<Type, ScriptLayout> s_layouts = [];
    /// <summary>What a script derives from: none of its fields are the script's own.</summary>
    private static readonly HashSet<Type> s_bases = [typeof(Setup), typeof(Script), typeof(GraphicsScript), typeof(ResourceScript), typeof(PassScript), typeof(Data), typeof(Material), typeof(object)];

    /// <summary>What a field type is called as a component's, and how big it is; null for one a script cannot keep.</summary>
    private static (string Type, int Size, int Alignment)? Describe(Type type)
    {
        if (type == typeof(float)) return ("f32", 4, 4);
        if (type == typeof(double)) return ("f64", 8, 8);
        if (type == typeof(bool)) return ("bool", 1, 1);
        if (type == typeof(sbyte)) return ("i8", 1, 1);
        if (type == typeof(short)) return ("i16", 2, 2);
        if (type == typeof(int)) return ("i32", 4, 4);
        if (type == typeof(long)) return ("i64", 8, 8);
        if (type == typeof(byte)) return ("u8", 1, 1);
        if (type == typeof(ushort)) return ("u16", 2, 2);
        if (type == typeof(uint)) return ("u32", 4, 4);
        if (type == typeof(ulong)) return ("u64", 8, 8);
        if (type == typeof(Vec2)) return ("vec2", 8, 4);
        if (type == typeof(Vec3)) return ("vec3", 12, 4);
        if (type == typeof(Vec4)) return ("vec4", 16, 4);
        if (type == typeof(IVec2)) return ("ivec2", 8, 4);
        if (type == typeof(IVec3)) return ("ivec3", 12, 4);
        if (type == typeof(IVec4)) return ("ivec4", 16, 4);
        if (type == typeof(UVec2)) return ("uvec2", 8, 4);
        if (type == typeof(UVec3)) return ("uvec3", 12, 4);
        if (type == typeof(UVec4)) return ("uvec4", 16, 4);
        if (type == typeof(Mat4)) return ("mat4", 64, 4);
        if (type == typeof(Quat)) return ("quat", 16, 4);
        if (type == typeof(Entity)) return ("entity", 8, 4);
        // Text: the engine's own, laid out as it is there, and read and written through it.
        if (type == typeof(string)) { var (size, alignment) = TextLayout; return ("string", size, alignment); }
        return null;
    }

    public static string NameOf(Type type) => type.GetCustomAttribute<ScriptNameAttribute>()?.Name ?? type.Name;

    public static ScriptLayout Of(Type type)
    {
        lock (s_layouts)
        {
            if (!s_layouts.TryGetValue(type, out var layout)) s_layouts[type] = layout = new ScriptLayout(type);
            return layout;
        }
    }

    /// <summary>Forgets every class of <paramref name="assembly"/>: it is being unloaded.</summary>
    public static void Forget(Assembly assembly)
    {
        lock (s_layouts)
            foreach (var type in s_layouts.Keys.Where(t => t.Assembly == assembly).ToList()) s_layouts.Remove(type);
    }

    private ScriptLayout(Type type)
    {
        Name = NameOf(type);
        var fields = new List<Field>();
        int offset = 0, alignment = 1;
        // Public fields, a base class's first, in the order they are written.
        var chain = new List<Type>();
        for (var t = type; t is not null && !s_bases.Contains(t); t = t.BaseType) chain.Insert(0, t);
        foreach (var declaring in chain)
        {
            foreach (var info in declaring.GetFields(BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly).OrderBy(f => f.MetadataToken))
            {
                if (info.IsInitOnly || info.IsLiteral) continue;
                if (typeof(ResourceScript.Resource).IsAssignableFrom(info.FieldType)) continue;    // what a ResourceScript makes: no setting
                if (typeof(PassScript.Use).IsAssignableFrom(info.FieldType)) continue;            // what a PassScript uses: no setting
                var described = Describe(info.FieldType)
                    ?? throw new EngineException($"{type.Name}.{info.Name} is a {info.FieldType.Name}, which a script cannot keep: its fields are numbers, bool, " +
                                                 "Koral's vectors, Mat4, Quat, Entity and string. Make it private if it is only this run's.");
                offset = (offset + described.Alignment - 1) / described.Alignment * described.Alignment;
                fields.Add(new Field(info, described.Type, offset, described.Size));
                offset += described.Size;
                alignment = Math.Max(alignment, described.Alignment);
            }
        }
        Fields = fields;
        Alignment = alignment;
        Size = (offset + alignment - 1) / alignment * alignment;
    }

    /// <summary>How big the engine's text is, and on what: where a text field goes.</summary>
    private static (int Size, int Alignment) TextLayout
    {
        get
        {
            if (s_textLayout is { } known) return known;
            nuint size, alignment;
            Native.kengine_text_layout(&size, &alignment);
            return (s_textLayout = ((int)size, (int)alignment)).Value;
        }
    }
    private static (int Size, int Alignment)? s_textLayout;

    /// <summary>The engine's bytes into the object's fields.</summary>
    public void Load(object script, IntPtr data)
    {
        foreach (var field in Fields)
        {
            var at = data + field.Offset;
            if (field.Info.FieldType == typeof(string))
            {
                nuint length;
                var text = Native.kengine_text_get((void*)at, &length);
                field.Info.SetValue(script, System.Text.Encoding.UTF8.GetString(text, (int)length));
                continue;
            }
            field.Info.SetValue(script, field.Info.FieldType == typeof(bool) ? *(byte*)at != 0 : Marshal.PtrToStructure(at, field.Info.FieldType));
        }
    }

    /// <summary>The object's fields into the engine's bytes.</summary>
    public void Store(object script, IntPtr data)
    {
        foreach (var field in Fields)
        {
            var at = data + field.Offset;
            if (field.Info.FieldType == typeof(string))
            {
                var bytes = System.Text.Encoding.UTF8.GetBytes((string?)field.Info.GetValue(script) ?? "");
                fixed (byte* utf8 = bytes) Native.kengine_text_set((void*)at, utf8, (nuint)bytes.Length);
                continue;
            }
            var value = field.Info.GetValue(script)!;
            if (value is bool flag) *(byte*)at = flag ? (byte)1 : (byte)0;
            else Marshal.StructureToPtr(value, at, false);
        }
    }
}
