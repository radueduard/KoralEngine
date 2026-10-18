using System.Reflection;
using System.Runtime.InteropServices;

namespace Koral.Engine;

/// <summary>
/// A script: a class added to entities. Its public fields are a component — data the engine keeps, shows in the
/// inspector, saves with the scene and carries across a reload — and its methods are called on each entity that has it.
/// </summary>
/// <remarks>
/// <code>
/// public sealed class Spin : Script
/// {
///     public float Speed = 1;         // shown in the inspector, saved, kept across a reload
///     public float Angle;
///     private int _frames;            // this run's only: not saved, and gone on a reload
///
///     protected override void Update(float delta) => Angle += Speed * delta;
/// }
/// </code>
/// A field can be a number, a <c>bool</c>, one of Koral's vectors, a <c>Mat4</c>, a <c>Quat</c> or an
/// <see cref="Engine.Entity"/>. Before each call the fields hold what the engine has; after it, the engine has what they
/// hold. Edit the class while it runs and each entity keeps the fields whose name and type are the same.
/// </remarks>
public abstract class Script
{
    /// <summary>The entity this is on.</summary>
    public Entity Entity { get; internal set; }
    /// <summary>The scene it is in: its entities, and the nodes of its graph.</summary>
    public World World { get; internal set; } = null!;
    /// <summary>The keys, the mouse and the gamepads it is steered by: <c>Input.IsKeyHeld(Key.eW)</c>.</summary>
    public Koral.Input Input => World.Input;

    /// <summary>It was added to the entity: once, switched on or off.</summary>
    protected virtual void OnCreated() { }
    /// <summary>It was switched on, or added switched on.</summary>
    protected virtual void OnEnabled() { }
    /// <summary>Before its first update: once.</summary>
    protected virtual void Start() { }
    /// <summary>A steady step of the game: 0 to 8 times a frame.</summary>
    protected virtual void FixedUpdate(float step) { }
    /// <summary>Once a frame, while it is switched on.</summary>
    protected virtual void Update(float delta) { }
    /// <summary>After every script's Update.</summary>
    protected virtual void LateUpdate(float delta) { }
    /// <summary>It was switched off, or is about to go while on.</summary>
    protected virtual void OnDisabled() { }
    /// <summary>It is about to go: removed, or its entity destroyed.</summary>
    protected virtual void OnDestroyed() { }

    private protected virtual void PrepareCore() { }

    internal enum Hook { Created, Enabled, Started, FixedUpdate, Update, LateUpdate, Prepare, Disabled, Destroyed }

    internal void Call(Hook hook, float time)
    {
        switch (hook)
        {
            case Hook.Created: OnCreated(); break;
            case Hook.Enabled: OnEnabled(); break;
            case Hook.Started: Start(); break;
            case Hook.FixedUpdate: FixedUpdate(time); break;
            case Hook.Update: Update(time); break;
            case Hook.LateUpdate: LateUpdate(time); break;
            case Hook.Prepare: PrepareCore(); break;
            case Hook.Disabled: OnDisabled(); break;
            case Hook.Destroyed: OnDestroyed(); break;
        }
    }
}

/// <summary>
/// A script that makes what the graph's passes take and nothing else makes: a light turned into a camera, a table of
/// matrices. <see cref="Prepare"/> runs after every script's updates and before anything is drawn, so what it writes
/// is what this frame's passes read.
/// </summary>
/// <remarks>
/// <code>
/// public sealed class LightCamera : GraphicsScript
/// {
///     public Vec3 Position;
///     protected override void Prepare() =>
///         World.Buffer("light camera")?.Write([KMath.LookAt(Position, Vec3.Zero, Vec3.UnitY)]);
/// }
/// </code>
/// </remarks>
public abstract class GraphicsScript : Script
{
    /// <summary>After the updates, before the passes: write what they read.</summary>
    protected virtual void Prepare() { }
    private protected sealed override void PrepareCore() => Prepare();
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
        for (var t = type; t is not null && t != typeof(Script) && t != typeof(GraphicsScript); t = t.BaseType) chain.Insert(0, t);
        foreach (var declaring in chain)
        {
            foreach (var info in declaring.GetFields(BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly).OrderBy(f => f.MetadataToken))
            {
                if (info.IsInitOnly || info.IsLiteral) continue;
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
    public void Load(Script script, IntPtr data)
    {
        foreach (var field in Fields)
        {
            var at = data + field.Offset;
            field.Info.SetValue(script, field.Info.FieldType == typeof(bool) ? *(byte*)at != 0 : Marshal.PtrToStructure(at, field.Info.FieldType));
        }
    }

    /// <summary>The object's fields into the engine's bytes.</summary>
    public void Store(Script script, IntPtr data)
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
