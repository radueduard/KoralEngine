using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace Koral.Engine;

/// <summary>
/// A project's scripts, running in a scene: compiled from the <c>.cs</c> files of a folder, each <see cref="Setup"/>
/// class registered with the engine as a component, and all of it compiled and registered again when a file changes.
/// </summary>
/// <remarks>
/// <para>
/// The scripts are compiled together, with <c>System</c>, <c>System.Linq</c>, <c>System.Collections.Generic</c>,
/// <c>Koral</c> and <c>Koral.Engine</c> already in scope. Every public, concrete class deriving from
/// <see cref="Setup"/> is a script, named by its class (or its <see cref="ScriptNameAttribute"/>).
/// </para>
/// <para>
/// <see cref="Poll"/>, between frames, notices when the folder changed and has stopped changing, and loads it again.
/// The data is the engine's, so it stays: each entity keeps the fields whose name and type are the same, a new field
/// starts at zero, and a script that was started is not started again. Scripts that do not compile change nothing:
/// the ones running carry on, and the errors are logged.
/// </para>
/// </remarks>
public sealed unsafe class ScriptHost : IDisposable
{
    private readonly World _world;
    private ScriptContext? _context;
    private readonly Dictionary<string, Registered> _registered = [];
    private readonly Dictionary<string, RegisteredResource> _resources = [];
    private string _snapshot = "", _pending = "";
    private DateTime _pendingSince, _lastPoll;
    private int _generation;

    /// <summary>Runs the scripts of <paramref name="directory"/> on the entities of <paramref name="world"/>.</summary>
    public ScriptHost(World world, string directory)
    {
        _world = world;
        Directory = Path.GetFullPath(directory);
    }

    /// <summary>Where the scripts are.</summary>
    public string Directory { get; }
    /// <summary>The scripts there are now, by name.</summary>
    public IReadOnlyCollection<string> Scripts => _registered.Keys;
    /// <summary>The scripts that make resources there are now, by name: what the graph's Script nodes can name.</summary>
    public IReadOnlyCollection<string> ResourceScripts => _resources.Keys;
    /// <summary>How many times the scripts have been loaded.</summary>
    public int Generation => _generation;
    /// <summary>The errors of the last build that failed; empty once one succeeds.</summary>
    public IReadOnlyList<string> Errors { get; private set; } = [];
    /// <summary>How long the files must have stopped changing before they are loaded again.</summary>
    public TimeSpan SettleTime { get; set; } = TimeSpan.FromMilliseconds(300);
    /// <summary>How often <see cref="Poll"/> looks at the files.</summary>
    public TimeSpan PollInterval { get; set; } = TimeSpan.FromMilliseconds(250);
    /// <summary>The last unloaded build's context, for as long as something still holds it. For tests.</summary>
    public WeakReference? PreviousContext { get; private set; }

    /// <summary>Compiles and registers the scripts. False, with <see cref="Errors"/>, when they do not compile. A folder with no scripts is fine.</summary>
    public bool Load()
    {
        _snapshot = Snapshot();
        return Reload();
    }

    /// <summary>Between frames: loads the scripts again when the files changed and have settled. True when it did.</summary>
    public bool Poll()
    {
        var now = DateTime.UtcNow;
        if (now - _lastPoll < PollInterval) return false;
        _lastPoll = now;
        var snapshot = Snapshot();
        if (snapshot == _snapshot) { _pending = ""; return false; }
        if (snapshot != _pending) { _pending = snapshot; _pendingSince = now; return false; }
        if (now - _pendingSince < SettleTime) return false;
        _snapshot = snapshot;
        _pending = "";
        return Reload();
    }

    /// <summary>Loads the scripts as they are now. False, changing nothing, when they do not compile.</summary>
    public bool Reload()
    {
        ScriptContext context;
        List<Type> types;
        try
        {
            (context, types) = Build();
        }
        catch (ScriptBuildException e)
        {
            Errors = e.Errors;
            Log.Error($"[scripts] they did not compile; what was running carries on.\n{string.Join('\n', e.Errors)}");
            return false;
        }

        // The layouts first: a field a script cannot keep is an error like one of the compiler's, and changes nothing.
        var layouts = new List<(Type Type, ScriptLayout Layout)>();
        try
        {
            foreach (var type in types) layouts.Add((type, ScriptLayout.Of(type)));
        }
        catch (EngineException e)
        {
            Errors = [e.Message];
            Log.Error($"[scripts] {e.Message}");
            Unload(context);
            return false;
        }

        // The code that is going stops being called before its functions go. Its data stays where it is.
        var gone = _registered.Keys.Concat(_resources.Keys).Except(layouts.Select(l => l.Layout.Name)).ToList();
        foreach (var registered in _registered.Values) Native.kengine_script_unregister(_world.Document, registered.Layout.Name);
        foreach (var registered in _registered.Values) registered.Free();
        _registered.Clear();
        foreach (var resource in _resources.Values) Native.kengine_resource_script_unregister(_world.Document, resource.Layout.Name);
        foreach (var resource in _resources.Values) resource.Free();
        _resources.Clear();

        var failed = new List<string>();
        foreach (var (type, layout) in layouts)
        {
            // One that makes resources is a Script node's to run, and no component.
            if (type.IsSubclassOf(typeof(ResourceScript)))
            {
                var resource = new RegisteredResource(_world, type, layout);
                string? why;
                try { why = resource.Register() ? null : Native.LastError(); }
                catch (TargetInvocationException e) { why = $"making one threw {e.InnerException?.GetType().Name}: {e.InnerException?.Message}"; }
                if (why is null) _resources[layout.Name] = resource;
                else { failed.Add($"{layout.Name}: {why}"); resource.Free(); }
                continue;
            }
            var registered = new Registered(_world, type, layout);
            if (registered.Register()) _registered[layout.Name] = registered;
            else { failed.Add($"{layout.Name}: {Native.LastError()}"); registered.Free(); }
        }
        var previous = _context;
        _context = context;
        ++_generation;
        Errors = failed;
        if (previous is not null) Unload(previous);
        foreach (var error in failed) Log.Error($"[scripts] {error}");
        Log.Info($"[scripts] loaded: {(_registered.Count + _resources.Count == 0 ? "none" : string.Join(", ", _registered.Keys.Concat(_resources.Keys).Order()))}"
                 + (gone.Count > 0 ? $" (gone: {string.Join(", ", gone)})" : ""));
        return true;
    }

    public void Dispose()
    {
        foreach (var registered in _registered.Values)
        {
            Native.kengine_script_unregister(_world.Document, registered.Layout.Name);
            registered.Free();
        }
        _registered.Clear();
        foreach (var resource in _resources.Values)
        {
            Native.kengine_resource_script_unregister(_world.Document, resource.Layout.Name);
            resource.Free();
        }
        _resources.Clear();
        if (_context is not null) Unload(_context);
        _context = null;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private void Unload(ScriptContext context)
    {
        foreach (var assembly in context.Assemblies) ScriptLayout.Forget(assembly);
        PreviousContext = new WeakReference(context);
        context.Unload();
    }

    // ---- one script type, registered ---------------------------------------------------------------------------

    /// <summary>A script class as the engine has it: the functions it calls, and the object each entity's calls are made on.</summary>
    private sealed class Registered(World world, Type type, ScriptLayout layout)
    {
        public ScriptLayout Layout { get; } = layout;
        private readonly Dictionary<Entity, Setup> _instances = [];
        private GCHandle _self;
        private uint _component = Native.NoComponent;
        private readonly HashSet<Setup.Hook> _reported = [];
        private readonly bool _generator = typeof(MeshGenerator).IsAssignableFrom(type) || typeof(ImageGenerator).IsAssignableFrom(type);
        private readonly Dictionary<Entity, ulong> _generated = [];      // each entity's fields when its mesh was made of them

        /// <summary>Whether <paramref name="type"/> does something of its own in the hook called <paramref name="name"/>.</summary>
        private static bool Overrides(Type type, string name, params Type[] parameters)
        {
            var method = type.GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, parameters, null);
            return method is not null && method.DeclaringType != typeof(Setup) && method.DeclaringType != typeof(Script) && method.DeclaringType != typeof(GraphicsScript);
        }

        /// <summary>Whether <paramref name="type"/> takes <paramref name="piece"/> and has its own <paramref name="name"/> of it, not the piece's own that does nothing.</summary>
        private static bool Does(Type type, Type piece, string name)
        {
            if (!piece.IsAssignableFrom(type)) return false;
            var map = type.GetInterfaceMap(piece);
            for (var i = 0; i < map.InterfaceMethods.Length; ++i)
                if (map.InterfaceMethods[i].Name == name) return map.TargetMethods[i].DeclaringType != piece;
            return false;
        }

        public bool Register()
        {
            _self = GCHandle.Alloc(this);
            // Only what the class does: a hook it leaves as Setup has it — or a piece it does not take — is not called at all — for thousands of entities a
            // frame, a call each into a function that does nothing. Destroyed as well when anything is: it is when an entity's
            // object goes. A script that is only data — fields, and no hook — is called for nothing, and costs nothing a frame.
            var legacy = typeof(Script).IsAssignableFrom(type);
            var callbacks = new KengineScriptCallbacks
            {
                User = GCHandle.ToIntPtr(_self),
                Created = legacy && Overrides(type, "OnCreated") ? &OnCreated : null,
                Enabled = Overrides(type, "OnEnabled") ? &OnEnabled : null,
                Started = Overrides(type, legacy ? "Start" : "Initialize") ? &OnStarted : null,
                // A script as written before its pieces overrides what it does; one now takes the piece.
                FixedUpdate = (legacy ? Overrides(type, "FixedUpdate", typeof(float)) : Does(type, typeof(FixedStep), "FixedUpdate")) ? &OnFixedUpdate : null,
                Update = (legacy ? Overrides(type, "Update", typeof(float)) : Does(type, typeof(PerFrame), "Update")) ? &OnUpdate : null,
                LateUpdate = (legacy ? Overrides(type, "LateUpdate", typeof(float)) : Does(type, typeof(PerFrame), "LateUpdate")) ? &OnLateUpdate : null,
                // A generator is looked at before each frame is drawn too: made again when its fields have changed.
                Prepare = (legacy ? typeof(GraphicsScript).IsAssignableFrom(type) && Overrides(type, "Prepare") : Does(type, typeof(BeforeDraw), "Prepare") || _generator) ? &OnPrepare : null,
                Disabled = Overrides(type, "OnDisabled") ? &OnDisabled : null,
            };
            if (Overrides(type, "OnDestroyed") || callbacks.Created != null || callbacks.Enabled != null || callbacks.Started != null || callbacks.FixedUpdate != null
                || callbacks.Update != null || callbacks.LateUpdate != null || callbacks.Prepare != null || callbacks.Disabled != null)
                callbacks.Destroyed = &OnDestroyed;
            var names = new List<IntPtr>();
            try
            {
                var fields = stackalloc KecsField[Layout.Fields.Count];
                for (var i = 0; i < Layout.Fields.Count; ++i)
                {
                    var field = Layout.Fields[i];
                    var name = Marshal.StringToCoTaskMemUTF8(field.Info.Name);
                    var typeName = Marshal.StringToCoTaskMemUTF8(field.Type);
                    names.Add(name);
                    names.Add(typeName);
                    fields[i] = new KecsField { Name = (byte*)name, Type = (byte*)typeName, Offset = (uint)field.Offset, Size = (uint)field.Size };
                }
                _component = Native.kengine_script_register(world.Document, Layout.Name, (nuint)Layout.Size, (nuint)Layout.Alignment,
                                                            fields, (nuint)Layout.Fields.Count, &callbacks);
            }
            finally
            {
                foreach (var name in names) Marshal.FreeCoTaskMem(name);
            }
            return _component != Native.NoComponent;
        }

        public void Free()
        {
            _instances.Clear();
            if (_self.IsAllocated) _self.Free();
        }

        /// <summary>What it starts over for, and each entity's bytes of it as the script last left them.</summary>
        private ScriptLayout.Field[] Restarting => _restarting ??= Layout.Fields.Where(f => f.Reinitializes).ToArray();
        private ScriptLayout.Field[]? _restarting;
        private readonly Dictionary<Entity, byte[]> _seen = [];

        private byte[] Watched(IntPtr data)
        {
            var bytes = new byte[Restarting.Sum(f => f.Size)];
            var at = 0;
            foreach (var field in Restarting)
            {
                Marshal.Copy(data + field.Offset, bytes, at, field.Size);
                at += field.Size;
            }
            return bytes;
        }

        /// <summary>Something else changed what it starts over for: it goes, and is added again.</summary>
        private void StartOverIfChanged(Entity entity, IntPtr data)
        {
            if (!_seen.TryGetValue(entity, out var last) || last.AsSpan().SequenceEqual(Watched(data))) return;
            foreach (var hook in new[] { Setup.Hook.Disabled, Setup.Hook.Destroyed, Setup.Hook.Created, Setup.Hook.Enabled, Setup.Hook.Started })
            {
                var now = Native.kecs_get(world.Handle, entity, _component);
                if (now is null) return;
                Call(hook, entity, (IntPtr)now, 0f);
            }
        }

        private void Call(Setup.Hook hook, Entity entity, IntPtr data, float time)
        {
            if (Restarting.Length > 0 && hook is Setup.Hook.Update or Setup.Hook.FixedUpdate)
            {
                StartOverIfChanged(entity, data);
                var moved = Native.kecs_get(world.Handle, entity, _component);
                if (moved is null) return;
                data = (IntPtr)moved;
            }
            if (!_instances.TryGetValue(entity, out var script))
            {
                script = (Setup)Activator.CreateInstance(type)!;
                script.Entity = entity;
                script.World = world;
                _instances[entity] = script;
            }
            Layout.Load(script, data);
            try
            {
                script.Call(hook, time);
                if (_generator && hook == Setup.Hook.Prepare) Generate(script, entity, data);
            }
            catch (Exception e)
            {
                // Said once for each kind of call: a script that throws every frame would bury everything else.
                if (_reported.Add(hook)) Log.Error($"[scripts] {Layout.Name}.{hook} threw on {entity}: {e}");
            }
            // What it did may have moved its data, or taken it away: found again before anything is put back.
            if (hook == Setup.Hook.Destroyed) { _instances.Remove(entity); _generated.Remove(entity); _seen.Remove(entity); return; }
            var now = Native.kecs_get(world.Handle, entity, _component);
            if (now is not null)
            {
                Layout.Store(script, (IntPtr)now);
                if (Restarting.Length > 0) _seen[entity] = Watched((IntPtr)now);
            }
            else
            {
                _instances.Remove(entity);
                _seen.Remove(entity);
            }
        }

        /// <summary>
        /// Makes <paramref name="entity"/>'s mesh of the generator's fields as they are, when they are not what it was made of:
        /// named for the script and the fields, so that every entity of the same fields has the same one, made once.
        /// </summary>
        private void Generate(Setup script, Entity entity, IntPtr data)
        {
            var bytes = new ReadOnlySpan<byte>((void*)data, Layout.Size);
            var hash = 14695981039346656037ul;
            foreach (var b in bytes) hash = (hash ^ b) * 1099511628211ul;
            if (_generated.TryGetValue(entity, out var was) && was == hash) return;
            _generated[entity] = hash;
            var name = $"gen/{Layout.Name}/{hash:x16}";
            if (script is MeshGenerator generator)
            {
                if (!world.Generated.TryGetValue(name, out var made))
                {
                    var mesh = new MeshWriter();
                    generator.Initialize(mesh);
                    if (mesh.VertexCount == 0) throw new EngineException($"{Layout.Name} wrote no positions: a mesh is made of where its vertices are");
                    world.Generated[name] = made = world.Meshes.Create(name, mesh.VertexCount, mesh.Attributes, mesh.IndexList, mesh.BufferLayout);
                }
                world.Meshes.Give(entity, made);
            }
            if (script is ImageGenerator painter)
            {
                var picture = name + "/picture";
                if (!world.Generated.TryGetValue(picture, out var made))
                {
                    var image = new ImageWriter();
                    painter.Initialize(image);
                    fixed (byte* texels = image.Texels)
                    {
                        var created = Native.kengine_image_create(world.Document, picture, (uint)image.Width, (uint)image.Height, texels);
                        if (created is null) throw new EngineException(Native.LastError());
                        world.Generated[picture] = made = Native.Text(created);
                    }
                }
                if (!Native.kengine_entity_give_picture(world.Document, entity, painter.Into, made))
                    throw new EngineException($"{Layout.Name} makes its entity's {painter.Into}, and no material of it has a picture so named");
            }
        }

        private static Registered? From(IntPtr user) => GCHandle.FromIntPtr(user).Target as Registered;

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnCreated(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Setup.Hook.Created, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnEnabled(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Setup.Hook.Enabled, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnStarted(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Setup.Hook.Started, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnFixedUpdate(IntPtr user, Entity e, IntPtr data, float step) => From(user)?.Call(Setup.Hook.FixedUpdate, e, data, step);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnUpdate(IntPtr user, Entity e, IntPtr data, float delta) => From(user)?.Call(Setup.Hook.Update, e, data, delta);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnLateUpdate(IntPtr user, Entity e, IntPtr data, float delta) => From(user)?.Call(Setup.Hook.LateUpdate, e, data, delta);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnPrepare(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Setup.Hook.Prepare, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnDisabled(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Setup.Hook.Disabled, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnDestroyed(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Setup.Hook.Destroyed, e, data, 0);
    }

    // ---- one script that makes resources, registered -----------------------------------------------------------

    /// <summary>
    /// A script class that makes resources, as the engine has it: an object made each time it fills what it makes, given
    /// the Script node's settings, so that nothing one node's were filled with is left over for the next.
    /// </summary>
    private sealed class RegisteredResource(World world, Type type, ScriptLayout layout)
    {
        public ScriptLayout Layout { get; } = layout;
        private GCHandle _self;
        private IntPtr _error;      // why the last fill could not be: the engine reads it before the next

        /// <summary>The fields of what it makes, in the order they are written.</summary>
        private static IEnumerable<FieldInfo> Made(Type type) =>
            type.GetFields(BindingFlags.Public | BindingFlags.Instance).Where(f => typeof(ResourceScript.Resource).IsAssignableFrom(f.FieldType)).OrderBy(f => f.MetadataToken);

        /// <summary>One of a resource type, given nothing: what a field left null makes, as its node type starts one.</summary>
        private static ResourceScript.Resource Blank(Type type) =>
            (ResourceScript.Resource)Activator.CreateInstance(type, type.GetConstructors()[0].GetParameters().Select(_ => (object?)null).ToArray())!;

        /// <summary>What a resource is made with, as JSON: flags given as <c>"eSampled | eStorage"</c> a list of them.</summary>
        private static string Json(ResourceScript.Resource resource)
        {
            static string Text(string text) => System.Text.Json.JsonSerializer.Serialize(text);
            var parts = resource.Settings.Select(s => $"{Text(s.Input)}: {s.Value switch
            {
                string text when text.Contains('|') => $"[{string.Join(", ", text.Split('|', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(Text))}]",
                string text => Text(text),
                bool on => on ? "true" : "false",
                IFormattable number => number.ToString(null, System.Globalization.CultureInfo.InvariantCulture),
                var other => Text(other.ToString() ?? ""),
            }}");
            return "{" + string.Join(", ", parts) + "}";
        }

        public bool Register()
        {
            _self = GCHandle.Alloc(this);
            var callbacks = new KengineResourceScriptCallbacks { User = GCHandle.ToIntPtr(_self), Fill = &OnFill };
            var names = new List<IntPtr>();
            IntPtr Utf8(string text) { var at = Marshal.StringToCoTaskMemUTF8(text); names.Add(at); return at; }
            try
            {
                var fields = stackalloc KecsField[Math.Max(Layout.Fields.Count, 1)];
                for (var i = 0; i < Layout.Fields.Count; ++i)
                {
                    var field = Layout.Fields[i];
                    fields[i] = new KecsField { Name = (byte*)Utf8(field.Info.Name), Type = (byte*)Utf8(field.Type), Offset = (uint)field.Offset, Size = (uint)field.Size };
                }
                // What it makes, as an object of it makes them: each as its field's initialiser says.
                var sample = (ResourceScript)Activator.CreateInstance(type)!;
                var made = Made(type).Select(f => (f.Name, Resource: (ResourceScript.Resource?)f.GetValue(sample) ?? Blank(f.FieldType))).ToList();
                var resources = stackalloc KengineScriptResource[Math.Max(made.Count, 1)];
                for (var i = 0; i < made.Count; ++i)
                    resources[i] = new KengineScriptResource { Name = (byte*)Utf8(made[i].Name), Kind = (byte*)Utf8(made[i].Resource.Kind), Settings = (byte*)Utf8(Json(made[i].Resource)) };
                return Native.kengine_resource_script_register(world.Document, Layout.Name, (nuint)Layout.Size, fields, (nuint)Layout.Fields.Count,
                                                               resources, (nuint)made.Count, &callbacks);
            }
            finally
            {
                foreach (var name in names) Marshal.FreeCoTaskMem(name);
            }
        }

        public void Free()
        {
            if (_self.IsAllocated) _self.Free();
            if (_error != IntPtr.Zero) { Marshal.FreeCoTaskMem(_error); _error = IntPtr.Zero; }
        }

        private byte* Fill(KengineContents* contents, nuint count, byte* settings, nuint size)
        {
            if (_error != IntPtr.Zero) { Marshal.FreeCoTaskMem(_error); _error = IntPtr.Zero; }
            var node = count > 0 ? Native.Text(contents[0].Node) : "";
            var filled = new List<ResourceScript.Filled>();
            try
            {
                var script = (ResourceScript)Activator.CreateInstance(type)!;
                script.World = world;
                if ((int)size >= Layout.Size) Layout.Load(script, (IntPtr)settings);
                foreach (var field in Made(type))
                {
                    if (field.GetValue(script) is not ResourceScript.Resource resource)
                    {
                        resource = Blank(field.FieldType);
                        field.SetValue(script, resource);
                    }
                    resource.Name = field.Name;
                }
                // Each of its images and buffers, as it was made, to be written.
                for (nuint i = 0; i < count; ++i)
                {
                    var given = &contents[i];
                    var name = Native.Text(given->Name);
                    if (Made(type).FirstOrDefault(f => f.Name == name)?.GetValue(script) is not ResourceScript.Filled resource) continue;
                    resource.Data = (byte*)given->Bytes;
                    resource.Size = checked((int)given->Size);
                    if (resource is ResourceScript.Image image)
                    {
                        image.Format = Native.Text(given->Format);
                        image.Width = (int)given->ExtentX; image.Height = (int)given->ExtentY; image.Depth = (int)given->ExtentZ;
                        image.Layers = (int)given->Layers; image.TexelBytes = (int)given->TexelBytes;
                    }
                    filled.Add(resource);
                }
                script.InitCore();
                return null;
            }
            catch (Exception e)
            {
                var thrown = e is TargetInvocationException { InnerException: not null } ? e.InnerException! : e;
                Log.Error($"[scripts] {Layout.Name}.Init threw on {node}: {thrown}");
                _error = Marshal.StringToCoTaskMemUTF8($"it threw {thrown.GetType().Name}: {thrown.Message}");
                return (byte*)_error;
            }
            finally
            {
                // What it was given is the engine's again: written to after this, it says so.
                foreach (var resource in filled) { resource.Data = null; resource.Size = 0; }
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static byte* OnFill(IntPtr user, KengineContents* contents, nuint count, byte* settings, nuint size) =>
            GCHandle.FromIntPtr(user).Target is RegisteredResource registered ? registered.Fill(contents, count, settings, size) : null;
    }

    // ---- building ----------------------------------------------------------------------------------------------

    private sealed class ScriptBuildException(IReadOnlyList<string> errors) : Exception("the scripts did not compile")
    {
        public IReadOnlyList<string> Errors { get; } = errors;
    }

    /// <summary>A build's own load context: collectible, so the code of a build that was replaced can go.</summary>
    private sealed class ScriptContext(string name) : AssemblyLoadContext(name, isCollectible: true)
    {
        // Koral and the engine are the application's own, never copies: a Script here is the Script there.
        protected override Assembly? Load(AssemblyName assemblyName) => null;
    }

    private static readonly string[] GlobalUsings =
        ["System", "System.Collections.Generic", "System.Linq", "System.Numerics", "Koral", "Koral.Engine"];

    /// <summary>The project's scripts, and the engine's package's: the Camera, the lights and the Sky every project has.</summary>
    private IEnumerable<string> SourceFiles()
    {
        var package = Path.Combine(Native.PackageDirectory(), "scripts");
        static List<string> SourcesIn(string folder) => !System.IO.Directory.Exists(folder) ? [] :
            System.IO.Directory.EnumerateFiles(folder, "*.cs", SearchOption.AllDirectories)
                .Where(f => !f.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}") && !f.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
                .Order(StringComparer.Ordinal).ToList();
        // The project's, and its modules': what a module has is the project's too.
        var modules = (Native.Text(Native.kengine_module_scripts(_world.Document)) ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries);
        var own = SourcesIn(Directory).Concat(modules.SelectMany(SourcesIn)).ToList();
        if (own.Count == 0 || !System.IO.Directory.Exists(package)) return own;     // no scripts: nothing to run, the package's data being the engine's already
        return System.IO.Directory.EnumerateFiles(package, "*.cs").Order(StringComparer.Ordinal).Concat(own);
    }

    private string Snapshot()
    {
        var text = new StringBuilder();
        foreach (var file in SourceFiles())
        {
            var info = new FileInfo(file);
            text.Append(file).Append('|').Append(info.LastWriteTimeUtc.Ticks).Append('|').Append(info.Length).Append('\n');
        }
        return text.ToString();
    }

    private (ScriptContext, List<Type>) Build()
    {
        var generation = _generation + 1;
        var parse = new CSharpParseOptions(LanguageVersion.Latest);
        var trees = new List<SyntaxTree>
        {
            CSharpSyntaxTree.ParseText(string.Concat(GlobalUsings.Select(u => $"global using {u};\n"))
                // Both namespaces have one of each: Koral's are the ones a script means.
                + "global using Buffer = Koral.Buffer;\nglobal using Quat = Koral.Quat;\n", parse, "GlobalUsings.g.cs", Encoding.UTF8),
        };
        var embedded = new List<EmbeddedText>();
        foreach (var file in SourceFiles())
        {
            var bytes = File.ReadAllBytes(file);
            var text = SourceText.From(bytes, bytes.Length, Encoding.UTF8, canBeEmbedded: true);
            trees.Add(CSharpSyntaxTree.ParseText(text, parse, file));
            embedded.Add(EmbeddedText.FromSource(file, text));
        }
        var context = new ScriptContext($"Koral.Engine.Scripts.{generation}");
        if (trees.Count == 1) return (context, []);     // no scripts: nothing to run, and nothing wrong

        var compilation = CSharpCompilation.Create($"Koral.Engine.Scripts.{generation}", trees, References(),
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, nullableContextOptions: NullableContextOptions.Enable,
                                         optimizationLevel: OptimizationLevel.Debug, allowUnsafe: true));
        using var pe = new MemoryStream();
        using var pdb = new MemoryStream();
        var result = compilation.Emit(pe, pdb, embeddedTexts: embedded, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));
        if (!result.Success)
        {
            context.Unload();
            throw new ScriptBuildException(result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error).Select(d => d.ToString()).ToList());
        }
        pe.Position = 0;
        pdb.Position = 0;
        var assembly = context.LoadFromStream(pe, pdb);
        var types = assembly.GetExportedTypes()
            .Where(t => t.IsClass && !t.IsAbstract && (t.IsSubclassOf(typeof(Setup)) || t.IsSubclassOf(typeof(ResourceScript))) && t.GetConstructor(Type.EmptyTypes) is not null)
            .OrderBy(t => t.FullName, StringComparer.Ordinal)
            .ToList();
        return (context, types);
    }

    private static List<MetadataReference>? s_references;

    /// <summary>Everything the application itself runs on: the platform's assemblies, Koral's, and this one.</summary>
    private static List<MetadataReference> References()
    {
        if (s_references is not null) return s_references;
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") is string trusted)
            foreach (var path in trusted.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)) paths.Add(path);
        paths.Add(typeof(Scene).Assembly.Location);
        paths.Add(typeof(Script).Assembly.Location);
        return s_references = paths.Where(File.Exists).Select(p => (MetadataReference)MetadataReference.CreateFromFile(p)).ToList();
    }
}
