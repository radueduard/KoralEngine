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

    /// <summary>Whether this is still the scene's: an editor that opens another project starts a new one.</summary>
    public bool IsCurrentFor(Scene scene) => Native.kengine_document(scene.Graph.Native) == Document;

    // ---- entities -------------------------------------------------------------------------------------------

    /// <summary>A new entity, named as the editor names one.</summary>
    public Entity Create(string name = "Entity") => Native.kengine_entity_create(Document, name);
    /// <summary>Destroys an entity, and everything under it. False when it was not alive.</summary>
    public bool Destroy(Entity entity) => Native.kecs_destroy(Handle, entity);
    public bool Alive(Entity entity) => Native.kecs_alive(Handle, entity);
    public int EntityCount => (int)Native.kecs_entity_count(Handle);
    public string NameOf(Entity entity) => Native.Text(Native.kengine_entity_name(Document, entity));
    public bool Rename(Entity entity, string name) => Native.kengine_entity_set_name(Document, entity, name);
    public bool SetParent(Entity child, Entity parent) => Native.kecs_set_parent(Handle, child, parent);
    public Entity ParentOf(Entity entity) => Native.kecs_parent(Handle, entity);

    // ---- components, by name --------------------------------------------------------------------------------

    /// <summary>Whether there is a component type so named: a pipeline's, a script's, a tag.</summary>
    public bool HasComponentType(string component) => Native.kecs_component_find(Handle, component) != Native.NoComponent;
    /// <summary>A tag: a component with nothing in it, for a node's <c>with</c> and <c>without</c>.</summary>
    public void AddTag(string name) => Find(Native.kecs_register_tag(Handle, name), name);

    /// <summary>Gives an entity a component, with its fields at their defaults. A script's is its class name.</summary>
    public bool Add(Entity entity, string component) => Native.kecs_add(Handle, entity, Id(component), null);
    public bool Add<T>(Entity entity) where T : Script => Add(entity, ScriptLayout.NameOf(typeof(T)));
    public bool Remove(Entity entity, string component) => Native.kecs_remove(Handle, entity, Id(component));
    public bool Remove<T>(Entity entity) where T : Script => Remove(entity, ScriptLayout.NameOf(typeof(T)));
    public bool Has(Entity entity, string component)
    {
        var id = Native.kecs_component_find(Handle, component);
        return id != Native.NoComponent && Native.kecs_has(Handle, entity, id);
    }
    public bool Has<T>(Entity entity) where T : Script => Has(entity, ScriptLayout.NameOf(typeof(T)));
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

    /// <summary>Sets a number field.</summary>
    public void Set(Entity entity, string component, string field, double value) =>
        SetJson(entity, component, field, value.ToString("R", CultureInfo.InvariantCulture));
    public void Set(Entity entity, string component, string field, bool value) => SetJson(entity, component, field, value ? "true" : "false");
    /// <summary>Sets a vector field, component by component.</summary>
    public void SetVector(Entity entity, string component, string field, params float[] values) =>
        SetJson(entity, component, field, "[" + string.Join(",", values.Select(v => v.ToString("R", CultureInfo.InvariantCulture))) + "]");
    /// <summary>Gives a resource field the output of a node, by the node's id; or nothing, with null.</summary>
    public void SetResource(Entity entity, string component, string field, string? node) =>
        SetJson(entity, component, field, node is null ? "null" : System.Text.Json.JsonSerializer.Serialize(node));
    public void SetJson(Entity entity, string component, string field, string json)
    {
        if (!Native.kengine_field_set_json(Document, entity, component, field, json)) throw new EngineException(Native.LastError());
    }

    /// <summary>
    /// What a script on <paramref name="entity"/> holds now, as an object of its class: read, not kept. Null when the
    /// entity has none. <see cref="Write{T}"/> puts one back.
    /// </summary>
    public T? Read<T>(Entity entity) where T : Script, new()
    {
        var layout = ScriptLayout.Of(typeof(T));
        var id = Native.kecs_component_find(Handle, layout.Name);
        var data = id == Native.NoComponent ? null : Native.kecs_get(Handle, entity, id);
        if (data is null) return null;
        var script = new T { Entity = entity, World = this };
        layout.Load(script, (IntPtr)data);
        return script;
    }

    /// <summary>Makes a script on <paramref name="entity"/> hold what <paramref name="script"/> does. False when the entity has none.</summary>
    public bool Write<T>(Entity entity, T script) where T : Script
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
    public Buffer? Buffer(string node) => Resource<Buffer>(node);
    public Image? Image(string node) => Resource<Image>(node);

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
