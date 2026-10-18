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
/// A project's scripts, running in a scene: compiled from the <c>.cs</c> files of a folder, each <see cref="Script"/>
/// class registered with the engine as a component, and all of it compiled and registered again when a file changes.
/// </summary>
/// <remarks>
/// <para>
/// The scripts are compiled together, with <c>System</c>, <c>System.Linq</c>, <c>System.Collections.Generic</c>,
/// <c>Koral</c> and <c>Koral.Engine</c> already in scope. Every public, concrete class deriving from
/// <see cref="Script"/> is a script, named by its class (or its <see cref="ScriptNameAttribute"/>).
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
        var gone = _registered.Keys.Except(layouts.Select(l => l.Layout.Name)).ToList();
        foreach (var registered in _registered.Values) Native.kengine_script_unregister(_world.Document, registered.Layout.Name);
        foreach (var registered in _registered.Values) registered.Free();
        _registered.Clear();

        var failed = new List<string>();
        foreach (var (type, layout) in layouts)
        {
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
        Log.Info($"[scripts] loaded: {(_registered.Count == 0 ? "none" : string.Join(", ", _registered.Keys.Order()))}"
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
        private readonly Dictionary<Entity, Script> _instances = [];
        private GCHandle _self;
        private uint _component = Native.NoComponent;
        private readonly HashSet<Script.Hook> _reported = [];

        /// <summary>Whether <paramref name="type"/> does something of its own in the hook called <paramref name="name"/>.</summary>
        private static bool Overrides(Type type, string name, params Type[] parameters)
        {
            var method = type.GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, parameters, null);
            return method is null || (method.DeclaringType != typeof(Script) && method.DeclaringType != typeof(GraphicsScript));
        }

        public bool Register()
        {
            _self = GCHandle.Alloc(this);
            // Only what the class does: a hook it leaves as Script has it is not called at all — for thousands of entities a
            // frame, a call each into a function that does nothing. Destroyed as well when anything is: it is when an entity's
            // object goes. A script that is only data — fields, and no hook — is called for nothing, and costs nothing a frame.
            var callbacks = new KengineScriptCallbacks
            {
                User = GCHandle.ToIntPtr(_self),
                Created = Overrides(type, "OnCreated") ? &OnCreated : null,
                Enabled = Overrides(type, "OnEnabled") ? &OnEnabled : null,
                Started = Overrides(type, "Start") ? &OnStarted : null,
                FixedUpdate = Overrides(type, "FixedUpdate", typeof(float)) ? &OnFixedUpdate : null,
                Update = Overrides(type, "Update", typeof(float)) ? &OnUpdate : null,
                LateUpdate = Overrides(type, "LateUpdate", typeof(float)) ? &OnLateUpdate : null,
                Prepare = typeof(GraphicsScript).IsAssignableFrom(type) && Overrides(type, "Prepare") ? &OnPrepare : null,
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

        private void Call(Script.Hook hook, Entity entity, IntPtr data, float time)
        {
            if (!_instances.TryGetValue(entity, out var script))
            {
                script = (Script)Activator.CreateInstance(type)!;
                script.Entity = entity;
                script.World = world;
                _instances[entity] = script;
            }
            Layout.Load(script, data);
            try
            {
                script.Call(hook, time);
            }
            catch (Exception e)
            {
                // Said once for each kind of call: a script that throws every frame would bury everything else.
                if (_reported.Add(hook)) Log.Error($"[scripts] {Layout.Name}.{hook} threw on {entity}: {e}");
            }
            // What it did may have moved its data, or taken it away: found again before anything is put back.
            if (hook == Script.Hook.Destroyed) { _instances.Remove(entity); return; }
            var now = Native.kecs_get(world.Handle, entity, _component);
            if (now is not null) Layout.Store(script, (IntPtr)now);
            else _instances.Remove(entity);
        }

        private static Registered? From(IntPtr user) => GCHandle.FromIntPtr(user).Target as Registered;

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnCreated(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Script.Hook.Created, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnEnabled(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Script.Hook.Enabled, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnStarted(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Script.Hook.Started, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnFixedUpdate(IntPtr user, Entity e, IntPtr data, float step) => From(user)?.Call(Script.Hook.FixedUpdate, e, data, step);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnUpdate(IntPtr user, Entity e, IntPtr data, float delta) => From(user)?.Call(Script.Hook.Update, e, data, delta);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnLateUpdate(IntPtr user, Entity e, IntPtr data, float delta) => From(user)?.Call(Script.Hook.LateUpdate, e, data, delta);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnPrepare(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Script.Hook.Prepare, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnDisabled(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Script.Hook.Disabled, e, data, 0);
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnDestroyed(IntPtr user, Entity e, IntPtr data) => From(user)?.Call(Script.Hook.Destroyed, e, data, 0);
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
        var own = !System.IO.Directory.Exists(Directory) ? [] :
            System.IO.Directory.EnumerateFiles(Directory, "*.cs", SearchOption.AllDirectories)
                .Where(f => !f.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}") && !f.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
                .Order(StringComparer.Ordinal).ToList();
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
            .Where(t => t.IsClass && !t.IsAbstract && t.IsSubclassOf(typeof(Script)) && t.GetConstructor(Type.EmptyTypes) is not null)
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
