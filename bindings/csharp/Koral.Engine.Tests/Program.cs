// Scripts in C#, on a running scene, with no display. Each check says what it is; the first that fails ends it.

using Koral;
using Koral.Engine;

// `shadow <project>`: E6's gate instead, in a process of its own (an application is one per process).
if (args.Length == 2 && args[0] == "shadow") return ShadowGate.Run(args[1]);

var failures = 0;
void Check(bool ok, string what)
{
    if (ok) return;
    ++failures;
    Console.Error.WriteLine($"FAILED: {what}");
}
void Equal<T>(T actual, T expected, string what) => Check(EqualityComparer<T>.Default.Equals(actual, expected), $"{what}: expected {expected}, got {actual}");
void Near(float actual, float expected, string what) => Check(MathF.Abs(actual - expected) < 1e-4f, $"{what}: expected {expected}, got {actual}");

var scripts = Path.Combine(Path.GetTempPath(), "koral-engine-csharp-tests", "scripts");
if (Directory.Exists(scripts)) Directory.Delete(scripts, true);
Directory.CreateDirectory(scripts);
void Write(string file, string source) => File.WriteAllText(Path.Combine(scripts, file), source);

using var app = new App(new AppSettings { Platform = WindowPlatform.eNone });
app.LoadLibrary(EngineLibrary.Path);
var scene = app.OpenOffscreen("Play", new OffscreenSettings { Title = "play", Extent = new UVec2(32, 32) });
var world = World.Of(scene) ?? throw new Exception("the Play scene has no world");
void Frames(int count) { for (var i = 0; i < count; ++i) app.Frame(); }
float Number(Entity entity, string component, string field) => float.Parse(world.FieldJson(entity, component, field), System.Globalization.CultureInfo.InvariantCulture);

// ---- a script is a component, and its methods are called on each entity that has it ----------------------------

Write("Spin.cs", """
    public sealed class Spin : Script
    {
        public float Angle;
        public float Speed = 99;        // an initialiser is the class's, not the component's: a new component starts at zero
        public int Updates;
        public bool Started;
        public Vec3 Axis;
        public Entity Target;
        private int _mine;              // this run's only

        protected override void Start() { Started = true; Axis = new Vec3(0, 1, 0); }
        protected override void Update(float delta) { Angle += Speed; ++Updates; ++_mine; }
    }
    """);
Write("Feeder.cs", """
    // The graphics half: what a pass reads, written from an entity's data before anything is drawn.
    public sealed class Feeder : GraphicsScript
    {
        public float Value;
        public int Prepared;
        protected override void Prepare()
        {
            ++Prepared;
            World.Buffer("numbers")?.Write([Value, Value * 2]);
        }
    }
    """);
Write("Watcher.cs", """
    // Reads the Spin of the first entity that has one, and sets it turning from 100: once.
    public sealed class Watcher : Script
    {
        public float Seen;
        public bool Done;
        protected override void LateUpdate(float delta)
        {
            if (Done) return;
            foreach (var entity in World.With("Spin"))
            {
                var spin = World.Read<Spin>(entity)!;
                Seen = spin.Angle;
                spin.Angle = 118;
                World.Write(entity, spin);
                Done = true;
            }
        }
    }
    """);
using var host = new ScriptHost(world, scripts);
Check(host.Load(), $"the scripts compile: {string.Join("; ", host.Errors)}");
Check(host.Scripts.Order().SequenceEqual(["Feeder", "Spin", "Watcher"]), $"each is a script: {string.Join(", ", host.Scripts)}");
Check(world.HasComponentType("Spin"), "a script is a component type, by its class name");

var wheel = world.Create("Wheel");

// Every entity has a name, a place in a hierarchy and a transform: the world's own, and no components.
{
    var car = world.Create("Car");
    Check(world.SetParent(wheel, car), "an entity goes under another");
    Equal(world.ParentOf(wheel), car, "and knows what it is under");
    Check(world.ChildrenOf(car).SequenceEqual([wheel]), "which knows what is under it");
    Check(world.Roots.Contains(car) && !world.Roots.Contains(wheel), "the top of the hierarchy is what is under nothing");
    Equal(world.Find("Wheel"), wheel, "found by name");
    Check(world.SetPosition(car, new Vec3(10, 0, 0)) && world.SetPosition(wheel, new Vec3(1, 2, 0)), "placed");
    world.SetRotation(wheel, new Vec3(0, 30, 0));
    Equal(world.PositionOf(wheel), new Vec3(1, 2, 0), "its own place");
    Equal(world.RotationOf(wheel), new Vec3(0, 30, 0), "its own turn, in degrees");
    Equal(world.ScaleOf(wheel), new Vec3(1, 1, 1), "a scale of one, to begin with");
    Near(world.WorldMatrix(wheel)[3].X, 11, "in the world, it is where its parent is and then where it is");
    Near(world.LocalMatrix(wheel)[3].X, 1, "its own matrix is its own");
    Check(world.SetParent(wheel, Entity.Null), "taken out from under it");
    Near(world.PositionOf(wheel).X, 11, "it stays where it was in the world");
    Check(world.SetWorldMatrix(wheel, Mat4.Identity), "placed in the world");
    Near(world.PositionOf(wheel).X, 0, "which is its own place, under nothing");
    world.SetRotation(wheel, default);
    Check(world.ToJson().Contains("\"name\": \"Car\""), "and the scene's file has it");
    world.Destroy(car);
}
Check(world.Add(wheel, "Spin"), "an entity is given a script");
world.Set(wheel, "Spin", "Speed", 2);
world.Set(wheel, "Spin", "Angle", 10);
Frames(3);
Near(Number(wheel, "Spin", "Updates"), 3, "updated once a frame");
Near(Number(wheel, "Spin", "Angle"), 16, "its data is the engine's: 10 + 3 * 2");
Equal(world.FieldJson(wheel, "Spin", "Started"), "true", "started, before its first update");
Equal(world.FieldJson(wheel, "Spin", "Axis").Replace(" ", ""), "[0,1,0]", "a vector field");
Check(world.ToJson().Contains("\"Spin\""), "and it is in the scene's file");

// One script reading and writing another's, on another entity.
var watcher = world.Create("Watcher");
world.Add(watcher, "Watcher");
Frames(1);
Near(Number(watcher, "Watcher", "Seen"), 18, "a script reads another entity's script");
Near(Number(wheel, "Spin", "Angle"), 118, "and writes it");

// Switched off: it has the script, and nothing runs.
world.SetEnabled(wheel, "Spin", false);
Frames(1);
var off = Number(wheel, "Spin", "Updates");
Frames(2);
Near(Number(wheel, "Spin", "Updates"), off, "switched off, it is left alone");
world.SetEnabled(wheel, "Spin", true);
Frames(1);
Near(Number(wheel, "Spin", "Updates"), off + 1, "and switched on, it carries on");
Equal(world.With("Spin").Length, 1, "found by a query");

// ---- a graphics script writes what the graph takes ---------------------------------------------------------------

world.AddNode("Buffer", "numbers");
world.SetInput("numbers", "instanceCount", "8");
world.SetInput("numbers", "usage", "[\"eStorage\"]");
Check(world.NodeOk("numbers", out var why), $"the buffer node builds: {why}");
var source = world.Create("Source");
world.Add(source, "Feeder");
world.Set(source, "Feeder", "Value", 21);
Frames(2);
var written = world.Buffer("numbers")!.Read<float>(2);
Near(written[0], 21, "what Prepare wrote is in the node's buffer");
Near(written[1], 42, "all of it");
Check(Number(source, "Feeder", "Prepared") >= 2, "once a frame");

// ---- edited while it runs: the code is new, the data is the entity's -------------------------------------------

Write("Spin.cs", """
    public sealed class Spin : Script
    {
        public float Angle;             // as it was: kept
        public double Speed;            // a double now: reset
        public int Updates;             // as it was: kept
        public int Laps;                // new: zero
        // Started, Axis and Target are gone

        protected override void Update(float delta) { Angle -= 1; ++Updates; ++Laps; }
    }
    """);
var (angleBefore, updatesBefore) = (Number(wheel, "Spin", "Angle"), (int)Number(wheel, "Spin", "Updates"));
Check(host.Reload(), $"the edited script compiles: {string.Join("; ", host.Errors)}");
Equal(host.Generation, 2, "loaded a second time");
Near(Number(wheel, "Spin", "Angle"), angleBefore, "a field that kept its name and type kept its value");
Near(Number(wheel, "Spin", "Updates"), updatesBefore, "and so did this one");
Equal(world.FieldJson(wheel, "Spin", "Speed"), "0", "one that changed type starts again");
Equal(world.FieldJson(wheel, "Spin", "Laps"), "0", "and so does a new one");
Equal(world.FieldJson(wheel, "Spin", "Started"), "null", "one that is gone is gone");
Frames(2);
Equal(world.FieldJson(wheel, "Spin", "Laps"), "2", "the new code runs, on the same entity");
Near(Number(wheel, "Spin", "Updates"), updatesBefore + 2, "counting on from where it was");
Near(Number(wheel, "Spin", "Angle"), angleBefore - 2, "the new Update, not the old");
Near(world.Buffer("numbers")!.Read<float>(1)[0], 21, "a script that was not edited carries on as it was");

// ---- an edit that does not compile changes nothing -------------------------------------------------------------

Write("Spin.cs", "public sealed class Spin : Script { public float Angle; protected override void Update(float delta) { this is not C# } }");
Check(!host.Reload(), "what does not compile is refused");
Check(host.Errors.Count > 0 && host.Errors[0].Contains("Spin.cs"), $"with the file and the error: {host.Errors.FirstOrDefault()}");
Equal(host.Generation, 2, "nothing was loaded");
Frames(1);
Equal(world.FieldJson(wheel, "Spin", "Laps"), "3", "the scripts that were running carry on");

// A field of a kind a script cannot keep is refused the same way, and said plainly.
Write("Spin.cs", "public sealed class Spin : Script { public float Angle; public string Name = \"\"; }");
Check(!host.Reload(), "a field a script cannot keep is refused");
Check(host.Errors.Count == 1 && host.Errors[0].Contains("Spin.Name") && host.Errors[0].Contains("private"), $"saying which, and what to do: {host.Errors.FirstOrDefault()}");
Frames(1);
Equal(world.FieldJson(wheel, "Spin", "Laps"), "4", "and what was running still runs");

// ---- a script that throws is reported, and the rest carry on -----------------------------------------------------

Write("Spin.cs", """
    public sealed class Spin : Script
    {
        public float Angle; public double Speed; public int Updates; public int Laps;
        protected override void Update(float delta) { ++Laps; throw new InvalidOperationException("on purpose"); }
    }
    """);
Check(host.Reload(), "it compiles");
Frames(3);
Equal(world.FieldJson(wheel, "Spin", "Laps"), "7", "what it did before throwing is kept, each frame");
Check(Number(source, "Feeder", "Prepared") > 5, "and the other scripts were not stopped by it");

// ---- a script removed from the source stops, and its data stays for when it is back ---------------------------

File.Delete(Path.Combine(scripts, "Spin.cs"));
File.Delete(Path.Combine(scripts, "Watcher.cs"));     // it reads a Spin, and would not compile without one
Check(host.Reload(), "with a script's file gone, the rest load");
Check(!host.Scripts.Contains("Spin") && host.Scripts.Contains("Feeder"), "and it is no longer one");
Frames(2);
Equal(world.FieldJson(wheel, "Spin", "Laps"), "7", "nothing runs for it, and the entity keeps what it had");

// ---- files watched: saved, settled, loaded --------------------------------------------------------------------

host.PollInterval = TimeSpan.FromMilliseconds(10);
host.SettleTime = TimeSpan.FromMilliseconds(60);
Write("Spin.cs", """
    public sealed class Spin : Script
    {
        public float Angle; public double Speed; public int Updates; public int Laps;
        protected override void Update(float delta) => Laps += 10;
    }
    """);
var generation = host.Generation;
for (var i = 0; i < 200 && host.Generation == generation; ++i) { Thread.Sleep(10); host.Poll(); }
Equal(host.Generation, generation + 1, "a saved script is noticed and loaded");
Frames(1);
Equal(world.FieldJson(wheel, "Spin", "Laps"), "17", "and runs, with what the entity had");

// ---- the code that was replaced can go -------------------------------------------------------------------------

var previous = host.PreviousContext;
for (var i = 0; i < 20 && previous is { IsAlive: true }; ++i) { GC.Collect(); GC.WaitForPendingFinalizers(); Frames(1); }
Check(previous is { IsAlive: false }, "a build that was replaced is unloaded");

var laps = world.FieldJson(wheel, "Spin", "Laps");
host.Dispose();
Frames(2);
Equal(world.FieldJson(wheel, "Spin", "Laps"), laps, "with the host gone nothing runs, and the data is still the scene's");

Console.WriteLine(failures == 0 ? "Koral.Engine: all checks passed" : $"Koral.Engine: {failures} checks FAILED");
return failures == 0 ? 0 : 1;
