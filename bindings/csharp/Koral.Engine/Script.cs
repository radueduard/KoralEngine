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
public abstract class Material;

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
    internal sealed record Field(FieldInfo Info, string Type, int Offset, int Size);

    public string Name { get; }
    public int Size { get; }
    public int Alignment { get; }
    public IReadOnlyList<Field> Fields { get; }

    private static readonly Dictionary<Type, ScriptLayout> s_layouts = [];
    /// <summary>What a script derives from: none of its fields are the script's own.</summary>
    private static readonly HashSet<Type> s_bases = [typeof(Setup), typeof(Script), typeof(GraphicsScript), typeof(ResourceScript), typeof(object)];

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
                var described = Describe(info.FieldType)
                    ?? throw new EngineException($"{type.Name}.{info.Name} is a {info.FieldType.Name}, which a script cannot keep: its fields are numbers, bool, " +
                                                 "Koral's vectors, Mat4, Quat and Entity. Make it private if it is only this run's.");
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

    /// <summary>The engine's bytes into the object's fields.</summary>
    public void Load(object script, IntPtr data)
    {
        foreach (var field in Fields)
        {
            var at = data + field.Offset;
            field.Info.SetValue(script, field.Info.FieldType == typeof(bool) ? *(byte*)at != 0 : Marshal.PtrToStructure(at, field.Info.FieldType));
        }
    }

    /// <summary>The object's fields into the engine's bytes.</summary>
    public void Store(Setup script, IntPtr data)
    {
        foreach (var field in Fields)
        {
            var at = data + field.Offset;
            var value = field.Info.GetValue(script)!;
            if (value is bool flag) *(byte*)at = flag ? (byte)1 : (byte)0;
            else Marshal.StructureToPtr(value, at, false);
        }
    }
}
