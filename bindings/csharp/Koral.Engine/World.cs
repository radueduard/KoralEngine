using System.Globalization;
using System.Runtime.InteropServices;

namespace Koral.Engine;

/// <summary>An entity: a number, and which use of that number it is. Nothing until a <see cref="World"/> is asked about it.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Entity(uint Index, uint Generation)
{
    /// <summary>No entity.</summary>
    public static readonly Entity Null = new(0xFFFFFFFFu, 0);
    public bool IsNull => Index == 0xFFFFFFFFu;
    public override string ToString() => IsNull ? "no entity" : $"entity {Index}";
}

/// <summary>
/// A scene the engine runs: its entities, and the nodes of its graph. What a script is handed, and what a program
/// that opened one of the engine's scenes asks for it by (<see cref="Of"/>).
/// </summary>
public sealed unsafe class World
{
    internal IntPtr Document;
    internal IntPtr Handle;

    internal World(IntPtr document)
    {
        Document = document;
        Handle = Native.kengine_document_world(document);
    }

    /// <summary>The engine's side of <paramref name="scene"/>: one of its scenes, "Play" or "Editor". Null for any other.</summary>
    public static World? Of(Scene scene)
    {
        var document = Native.kengine_document(scene.Graph.Native);
        return document == IntPtr.Zero ? null : new World(document);
    }

    /// <summary>The folder of the project the scene is of; null for one that is in none.</summary>
    public string? ProjectDirectory => Native.Text(Native.kengine_project_directory(Document)) is { Length: > 0 } folder ? folder : null;

    /// <summary>Whether this is still the scene's: an editor that opens another project starts a new one.</summary>
    public bool IsCurrentFor(Scene scene) => Native.kengine_document(scene.Graph.Native) == Document;

    /// <summary>
    /// The keys, the mouse and the gamepads a script that is steered reads: the scene's window's — or, in the editor,
    /// the Game panel's, which has them while the pointer is over it or it was the last clicked.
    /// </summary>
    public Koral.Input Input
    {
        get
        {
            var native = Native.kengine_script_input(Document);
            return native != IntPtr.Zero ? Koral.Input.Of(native) : throw new EngineException("no scene runs this world: it has no input");
        }
    }

    /// <summary>
    /// How big, in pixels, the window is whose mouse <see cref="Input"/> reads — the scene's, or the editor's Game panel:
    /// what <c>Input.MousePosition</c> is from the top-left of, to make a ray of the cursor through the camera. Zero
    /// when no scene runs.
    /// </summary>
    public unsafe Koral.Vec2 ViewportSize
    {
        get
        {
            float width = 0, height = 0;
            return Native.kengine_script_viewport(Document, &width, &height) ? new Koral.Vec2(width, height) : new Koral.Vec2(0, 0);
        }
    }

    /// <summary>
    /// The lines and shapes a script draws to see what it does — contacts, normals, bounds: the scene's, shown over the
    /// editor's view of it. Drawn in Update they last the frame; FixedUpdate runs 0 to 8 times a frame, so what is to be
    /// seen every frame is drawn in Update, or given a duration.
    /// </summary>
    public Koral.DebugDraw Debug
    {
        get
        {
            var native = Native.kengine_debug(Document);
            return native != IntPtr.Zero ? new Koral.DebugDraw(native) : throw new EngineException("no scene runs this world: it has no debug lines");
        }
    }

    // ---- entities -------------------------------------------------------------------------------------------

    /// <summary>A new entity, named as the editor names one.</summary>
    /// <summary>The scene's meshes: made of any attributes a script likes, read back, and given to entities.</summary>
    public Meshes Meshes => _meshes ??= new Meshes(this);
    /// <summary>The meshes generators made in this scene, by what they were made of: each made once, however many entities have it.</summary>
    internal Dictionary<string, string> Generated { get; } = [];
    private Meshes? _meshes;

    public Entity Create(string name = "Entity") => Native.kengine_entity_create(Document, name);
    /// <summary>Destroys an entity, and everything under it. False when it was not alive.</summary>
    public bool Destroy(Entity entity) => Native.kecs_destroy(Handle, entity);
    public bool Alive(Entity entity) => Native.kecs_alive(Handle, entity);
    public int EntityCount => (int)Native.kecs_entity_count(Handle);
    public string NameOf(Entity entity) => Native.Text(Native.kengine_entity_name(Document, entity));
    public bool Rename(Entity entity, string name) => Native.kengine_entity_set_name(Document, entity, name);
    /// <summary>
    /// Puts <paramref name="child"/> under <paramref name="parent"/> (<see cref="Entity.Null"/>: at the top), at
    /// <paramref name="index"/> among what is there (last, when below 0). It stays where it is in the world unless
    /// <paramref name="keepWorldTransform"/> is false.
    /// </summary>
    public bool SetParent(Entity child, Entity parent, int index = -1, bool keepWorldTransform = true) =>
        Native.kecs_set_parent_at(Handle, child, parent, index, keepWorldTransform);
    public Entity ParentOf(Entity entity) => Native.kecs_parent(Handle, entity);
    /// <summary>The entities directly under one, in order.</summary>
    public Entity[] ChildrenOf(Entity entity)
    {
        var children = new Entity[(int)Native.kecs_child_count(Handle, entity)];
        for (var i = 0; i < children.Length; i++) children[i] = Native.kecs_child(Handle, entity, (nuint)i);
        return children;
    }
    /// <summary>The entities at the top of the hierarchy, in order.</summary>
    public Entity[] Roots => ChildrenOf(Entity.Null);
    /// <summary>The first entity so named, parents before their children; <see cref="Entity.Null"/> when there is none.</summary>
    public Entity Find(string name) => Native.kecs_find(Handle, name);

    // ---- transforms -------------------------------------------------------------------------------------------
    //
    // Every entity has one, relative to what it is under. It is the world's own, and no component.

    /// <summary>Where an entity is, relative to what it is under.</summary>
    public unsafe Vec3 PositionOf(Entity entity) { Vec3 value = default; Native.kecs_position(Handle, entity, &value); return value; }
    /// <summary>How it is turned: Euler angles, in degrees — about X, about Y, about Z.</summary>
    public unsafe Vec3 RotationOf(Entity entity) { Vec3 value = default; Native.kecs_rotation(Handle, entity, &value); return value; }
    public unsafe Vec3 ScaleOf(Entity entity) { var value = new Vec3(1f, 1f, 1f); Native.kecs_scale(Handle, entity, &value); return value; }
    public unsafe bool SetPosition(Entity entity, Vec3 position) => Native.kecs_set_position(Handle, entity, &position);
    public unsafe bool SetRotation(Entity entity, Vec3 degrees) => Native.kecs_set_rotation(Handle, entity, &degrees);
    /// <summary>
    /// How it is turned, as a quaternion: what its matrix is made with. Set as one, it is kept as given (made length 1)
    /// and RotationOf is worked out of it, so a rotation integrated a step at a time is not rounded off by the angles.
    /// </summary>
    public unsafe Quat OrientationOf(Entity entity) { var value = new Quat(); Native.kecs_orientation(Handle, entity, &value); return value; }
    public unsafe bool SetOrientation(Entity entity, Quat rotation) => Native.kecs_set_orientation(Handle, entity, &rotation);

    // Of many at once, in the order of the entities: what a script that moves thousands does in two calls a frame, not
    // two an entity. Read into a span at least as long, kept from frame to frame; one not alive reads as zeros (a scale
    // as ones) and is not set. Each says how many were alive.
    private unsafe int Threes(ReadOnlySpan<Entity> entities, Span<Vec3> values, delegate*<IntPtr, Entity*, nuint, Vec3*, nuint> call)
    {
        if (values.Length < entities.Length) throw new ArgumentException($"a value an entity: {entities.Length} of them, not {values.Length}");
        fixed (Entity* e = entities) fixed (Vec3* v = values) return (int)call(Handle, e, (nuint)entities.Length, v);
    }
    public unsafe int PositionsOf(ReadOnlySpan<Entity> entities, Span<Vec3> into) => Threes(entities, into, &Native.kecs_positions);
    public unsafe int RotationsOf(ReadOnlySpan<Entity> entities, Span<Vec3> into) => Threes(entities, into, &Native.kecs_rotations);
    public unsafe int ScalesOf(ReadOnlySpan<Entity> entities, Span<Vec3> into) => Threes(entities, into, &Native.kecs_scales);
    public unsafe int SetPositions(ReadOnlySpan<Entity> entities, ReadOnlySpan<Vec3> values) => Threes(entities, MemoryMarshal.CreateSpan(ref MemoryMarshal.GetReference(values), values.Length), &Native.kecs_set_positions);
    public unsafe int SetRotations(ReadOnlySpan<Entity> entities, ReadOnlySpan<Vec3> values) => Threes(entities, MemoryMarshal.CreateSpan(ref MemoryMarshal.GetReference(values), values.Length), &Native.kecs_set_rotations);
    private unsafe int Fours(ReadOnlySpan<Entity> entities, Span<Quat> values, delegate*<IntPtr, Entity*, nuint, Quat*, nuint> call)
    {
        if (values.Length < entities.Length) throw new ArgumentException($"a value an entity: {entities.Length} of them, not {values.Length}");
        fixed (Entity* e = entities) fixed (Quat* v = values) return (int)call(Handle, e, (nuint)entities.Length, v);
    }
    /// <summary>Their quaternions; one not alive reads as the identity.</summary>
    public unsafe int OrientationsOf(ReadOnlySpan<Entity> entities, Span<Quat> into) => Fours(entities, into, &Native.kecs_orientations);
    public unsafe int SetOrientations(ReadOnlySpan<Entity> entities, ReadOnlySpan<Quat> values) => Fours(entities, MemoryMarshal.CreateSpan(ref MemoryMarshal.GetReference(values), values.Length), &Native.kecs_set_orientations);
    // A field of a component, of many at once: its numbers, FieldWidth of them an entity — 1 for a float, 3 for a Vec3,
    // 16 for a Mat4 — one entity's after another. Floats for a field of them, ints for one of whole numbers or a bool
    // (1 and 0). One without the component reads as zeros and is not set; each says how many had it.

    /// <summary>How many numbers <paramref name="field"/> of <paramref name="component"/> is an entity; 0 for one there is not, or not of numbers.</summary>
    public int FieldWidth(string component, string field)
    {
        var id = Native.kecs_component_find(Handle, component);
        return id == Native.NoComponent ? 0 : (int)Native.kecs_field_width(Handle, id, field);
    }
    private int Width(string component, string field, int entities, int values)
    {
        var width = FieldWidth(component, field);
        if (width == 0) throw new EngineException($"'{component}' has no field '{field}' of numbers");
        if (values < entities * width) throw new ArgumentException($"{width} numbers an entity: {entities * width} of them, not {values}");
        return width;
    }
    public unsafe int FieldFloats(ReadOnlySpan<Entity> entities, string component, string field, Span<float> into)
    {
        Width(component, field, entities.Length, into.Length);
        fixed (Entity* e = entities) fixed (float* v = into) return (int)Native.kecs_field_floats(Handle, e, (nuint)entities.Length, Id(component), field, v);
    }
    public unsafe int SetFieldFloats(ReadOnlySpan<Entity> entities, string component, string field, ReadOnlySpan<float> values)
    {
        Width(component, field, entities.Length, values.Length);
        fixed (Entity* e = entities) fixed (float* v = values) return (int)Native.kecs_set_field_floats(Handle, e, (nuint)entities.Length, Id(component), field, v);
    }
    public unsafe int FieldInts(ReadOnlySpan<Entity> entities, string component, string field, Span<int> into)
    {
        Width(component, field, entities.Length, into.Length);
        fixed (Entity* e = entities) fixed (int* v = into) return (int)Native.kecs_field_ints(Handle, e, (nuint)entities.Length, Id(component), field, v);
    }
    public unsafe int SetFieldInts(ReadOnlySpan<Entity> entities, string component, string field, ReadOnlySpan<int> values)
    {
        Width(component, field, entities.Length, values.Length);
        fixed (Entity* e = entities) fixed (int* v = values) return (int)Native.kecs_set_field_ints(Handle, e, (nuint)entities.Length, Id(component), field, v);
    }

    public unsafe int SetScales(ReadOnlySpan<Entity> entities, ReadOnlySpan<Vec3> values) => Threes(entities, MemoryMarshal.CreateSpan(ref MemoryMarshal.GetReference(values), values.Length), &Native.kecs_set_scales);
    public unsafe bool SetScale(Entity entity, Vec3 scale) => Native.kecs_set_scale(Handle, entity, &scale);
    /// <summary>Its own transform, as a matrix.</summary>
    public unsafe Mat4 LocalMatrix(Entity entity) { var value = Mat4.Identity; Native.kecs_local_matrix(Handle, entity, &value); return value; }
    /// <summary>Where it is in the world: the transforms of everything above it, and its own.</summary>
    public unsafe Mat4 WorldMatrix(Entity entity) { var value = Mat4.Identity; Native.kecs_world_matrix(Handle, entity, &value); return value; }
    /// <summary>Places it in the world, by changing its own transform.</summary>
    public unsafe bool SetWorldMatrix(Entity entity, Mat4 matrix) => Native.kecs_set_world_matrix(Handle, entity, &matrix);

    // ---- components, by name --------------------------------------------------------------------------------

    /// <summary>Whether there is a component type so named: a pipeline's, a script's, a tag.</summary>
    public bool HasComponentType(string component) => Native.kecs_component_find(Handle, component) != Native.NoComponent;
    /// <summary>A tag: a component with nothing in it, for a node's <c>with</c> and <c>without</c>.</summary>
    public void AddTag(string name) => Find(Native.kecs_register_tag(Handle, name), name);

    /// <summary>Gives an entity a component, with its fields at their defaults. A script's is its class name.</summary>
    public bool Add(Entity entity, string component) => Native.kecs_add(Handle, entity, Id(component), null);
    public bool Add<T>(Entity entity) where T : Setup => Add(entity, ScriptLayout.NameOf(typeof(T)));
    public bool Remove(Entity entity, string component) => Native.kecs_remove(Handle, entity, Id(component));
    public bool Remove<T>(Entity entity) where T : Setup => Remove(entity, ScriptLayout.NameOf(typeof(T)));
    public bool Has(Entity entity, string component)
    {
        var id = Native.kecs_component_find(Handle, component);
        return id != Native.NoComponent && Native.kecs_has(Handle, entity, id);
    }
    public bool Has<T>(Entity entity) where T : Setup => Has(entity, ScriptLayout.NameOf(typeof(T)));
    /// <summary>Switches a component off, or back on: off, the entity has it still and nothing runs for it.</summary>
    public bool SetEnabled(Entity entity, string component, bool enabled) => Native.kecs_set_enabled(Handle, entity, Id(component), enabled);
    public bool IsEnabled(Entity entity, string component) => Native.kecs_is_enabled(Handle, entity, Id(component));

    /// <summary>The entities that have every one of <paramref name="components"/>, switched on.</summary>
    public Entity[] With(params string[] components) => Query(components, []);

    /// <summary>The entities that have every component of <paramref name="all"/> and none of <paramref name="none"/>.</summary>
    public Entity[] Query(IReadOnlyList<string> all, IReadOnlyList<string> none, bool includeDisabled = false)
    {
        var allIds = stackalloc uint[all.Count];
        var noneIds = stackalloc uint[none.Count];
        for (var i = 0; i < all.Count; ++i) allIds[i] = Native.kecs_component_find(Handle, all[i]);   // one nothing is: nothing has it
        for (var i = 0; i < none.Count; ++i) noneIds[i] = Native.kecs_component_find(Handle, none[i]);
        var count = Native.kecs_query(Handle, allIds, (nuint)all.Count, noneIds, (nuint)none.Count, includeDisabled, null, 0);
        var found = new Entity[count];
        if (count == 0) return found;
        fixed (Entity* into = found)
            Native.kecs_query(Handle, allIds, (nuint)all.Count, noneIds, (nuint)none.Count, includeDisabled, into, count);
        return found;
    }

    // ---- a component's fields -------------------------------------------------------------------------------

    /// <summary>A field as JSON: a number, true or false, a list of numbers for a vector, a node's id for a resource.</summary>
    public string FieldJson(Entity entity, string component, string field) => Native.Text(Native.kengine_field_json(Document, entity, component, field));

    /// <summary>
    /// Sets a field to <paramref name="value"/>, whatever a field can be: a number, a <c>bool</c>, one of Koral's vectors, a
    /// <c>Quat</c> or a <c>Mat4</c>, an <see cref="Engine.Entity"/>, a node's id for a resource (as <see cref="SetResource"/>), or null for none.
    /// </summary>
    public void Set(Entity entity, string component, string field, object? value) => SetJson(entity, component, field, JsonOf(value));
    /// <summary>Sets a vector field, component by component.</summary>
    public void SetVector(Entity entity, string component, string field, params float[] values) =>
        SetJson(entity, component, field, "[" + string.Join(",", values.Select(v => v.ToString("R", CultureInfo.InvariantCulture))) + "]");
    /// <summary>Gives a resource field the output of a node, by the node's id; or nothing, with null.</summary>
    public void SetResource(Entity entity, string component, string field, string? node) =>
        SetJson(entity, component, field, node is null ? "null" : System.Text.Json.JsonSerializer.Serialize(node));
    /// <summary>A field's value as the engine reads it: what <see cref="FieldJson"/> gives back.</summary>
    private static string JsonOf(object? value) => value switch
    {
        null => "null",
        bool flag => flag ? "true" : "false",
        float number => number.ToString("R", CultureInfo.InvariantCulture),
        double number => number.ToString("R", CultureInfo.InvariantCulture),
        sbyte or short or int or long or byte or ushort or uint or ulong => Convert.ToString(value, CultureInfo.InvariantCulture)!,
        Vec2 v => Numbers<Vec2, float>(v),
        Vec3 v => Numbers<Vec3, float>(v),
        Vec4 v => Numbers<Vec4, float>(v),
        Quat q => Numbers<Quat, float>(q),
        Mat4 m => Numbers<Mat4, float>(m),     // in the order a script's Mat4 field is kept
        IVec2 v => Numbers<IVec2, int>(v),
        IVec3 v => Numbers<IVec3, int>(v),
        IVec4 v => Numbers<IVec4, int>(v),
        UVec2 v => Numbers<UVec2, uint>(v),
        UVec3 v => Numbers<UVec3, uint>(v),
        UVec4 v => Numbers<UVec4, uint>(v),
        Entity e => e.IsNull ? "null" : $"{{\"entity\":{e.Index},\"generation\":{e.Generation}}}",
        string node => System.Text.Json.JsonSerializer.Serialize(node),
        _ => throw new ArgumentException($"a field is a number, a bool, a vector, a Quat, a Mat4, an Entity or a node's id: not a {value.GetType().Name}"),
    };

    /// <summary>A value's numbers, as they are in memory: as a script's field of it is kept.</summary>
    private static string Numbers<T, N>(T value) where T : unmanaged where N : unmanaged, IFormattable =>
        "[" + string.Join(",", MemoryMarshal.Cast<T, N>(new ReadOnlySpan<T>(ref value)).ToArray().Select(n => n.ToString(typeof(N) == typeof(float) ? "R" : null, CultureInfo.InvariantCulture))) + "]";

    public void SetJson(Entity entity, string component, string field, string json)
    {
        if (!Native.kengine_field_set_json(Document, entity, component, field, json)) throw new EngineException(Native.LastError());
    }

    /// <summary>
    /// What a script — or a <see cref="Data"/> — on <paramref name="entity"/> holds now, as an object of its class: read,
    /// not kept. Null when the entity has none. <see cref="Write{T}"/> puts one back.
    /// </summary>
    public T? Read<T>(Entity entity) where T : class, new()
    {
        var layout = ScriptLayout.Of(typeof(T));
        var id = Native.kecs_component_find(Handle, layout.Name);
        var data = id == Native.NoComponent ? null : Native.kecs_get(Handle, entity, id);
        if (data is null) return null;
        var script = new T();
        if (script is Setup setup) { setup.Entity = entity; setup.World = this; }
        layout.Load(script, (IntPtr)data);
        return script;
    }

    /// <summary>Makes a script, or a Data, on <paramref name="entity"/> hold what <paramref name="script"/> does. False when the entity has none.</summary>
    public bool Write<T>(Entity entity, T script) where T : class
    {
        var layout = ScriptLayout.Of(typeof(T));
        var id = Native.kecs_component_find(Handle, layout.Name);
        var data = id == Native.NoComponent ? null : Native.kecs_get(Handle, entity, id);
        if (data is null) return false;
        layout.Store(script, (IntPtr)data);
        Native.kecs_mark_changed(Handle, entity, id);
        return true;
    }

    // ---- the graph ------------------------------------------------------------------------------------------

    /// <summary>
    /// What a node made: a buffer to write, an image to read. Null when it has made nothing. A node built again makes
    /// another, so ask when one is needed rather than keeping it.
    /// </summary>
    public T? Resource<T>(string node, string output = "out") where T : Resource =>
        Koral.Resource.Wrap<T>(Native.kengine_node_resource(Document, node, output));
    public Buffer? Buffer(string node, string output = "out") => Resource<Buffer>(node, output);
    public Image? Image(string node, string output = "out") => Resource<Image>(node, output);

    /// <summary>Adds a node, as the editor does. The id it got.</summary>
    public string AddNode(string type, string? id = null, double x = 0, double y = 0)
    {
        var made = Native.kengine_node_add(Document, type, id, x, y);
        return made is null ? throw new EngineException(Native.LastError()) : Native.Text(made);
    }
    public bool RemoveNode(string node) => Native.kengine_node_remove(Document, node);
    /// <summary>Sets a node's input from JSON: <c>"16"</c>, <c>"[\"eStorage\"]"</c>.</summary>
    public void SetInput(string node, string input, string json)
    {
        if (!Native.kengine_node_set_input_json(Document, node, input, json)) throw new EngineException(Native.LastError());
    }
    public void Connect(string from, string node, string input, string output = "out")
    {
        if (!Native.kengine_node_connect(Document, from, output, node, input)) throw new EngineException(Native.LastError());
    }
    /// <summary>What a node's output holds now, as JSON (<c>"3"</c>, <c>"[1, 0, 0]"</c>); null when it gives no such value.</summary>
    public string? ValueJson(string node, string output = "out") => Native.kengine_node_value_json(Document, node, output) is var at && at is null ? null : Native.Text(at);
    /// <summary>Whether a node is built; <paramref name="why"/> says why not.</summary>
    public bool NodeOk(string node, out string why)
    {
        var ok = Native.kengine_node_ok(Document, node);
        why = ok ? "" : Native.LastError();
        return ok;
    }

    /// <summary>The scene as it is saved.</summary>
    public string ToJson() => Native.Text(Native.kengine_document_json(Document));
    public void Save(string path)
    {
        if (!Native.kengine_document_save(Document, path)) throw new EngineException(Native.LastError());
    }

    private uint Id(string component) => Find(Native.kecs_component_find(Handle, component), component);
    private static uint Find(uint id, string component) =>
        id != Native.NoComponent ? id : throw new EngineException($"there is no component '{component}'");
}
