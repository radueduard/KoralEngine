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
EngineLibrary.Load();     // as Koral's modules are loaded: see Load
app.LoadLibrary(EngineLibrary.Path);
var scene = app.OpenOffscreen("Play", new OffscreenSettings { Title = "play", Extent = new UVec2(32, 32) });
var world = World.Of(scene) ?? throw new Exception("the Play scene has no world");
void Frames(int count) { for (var i = 0; i < count; ++i) app.Frame(); }
float Number(Entity entity, string component, string field) => float.Parse(world.FieldJson(entity, component, field), System.Globalization.CultureInfo.InvariantCulture);

// ---- what a script that is steered reads: the keys of the scene's window -----------------------------------------
scene.SceneInput.FeedKey(Key.eW, true);
Frames(1);
Check(world.Input.IsKeyPressed(Key.eW) || world.Input.IsKeyHeld(Key.eW), "a script reads the keys its scene is given");
scene.SceneInput.FeedKey(Key.eW, false);
Frames(1);

// ---- a script is a component, and its methods are called on each entity that has it ----------------------------

// What the editor's Assets, New script makes: as it is, it has to compile and to turn its entity.
Write("Roller.cs", """
    public sealed class Roller : Setup, PerFrame
    {
        public float Speed;         // a field starts at zero: set it in the inspector, or in Initialize

        protected override void Initialize()
        {
            if (Speed == 0) Speed = 45;
        }

        public void Update(float delta)
        {
            // Turns its entity about the up axis, so many degrees a second.
            World.SetRotation(Entity, World.RotationOf(Entity) + new Vec3(0, Speed * delta, 0));
        }
    }
    """);
Write("Restarter.cs", """
    // Starts over when its size is changed, and not for what else changes.
    public sealed class Restarter : Setup, PerFrame
    {
        [Reinitialize] public int Size;
        public int Other, Made, Gone, MadeOf, Kept;
        private int _frames;
        protected override void Initialize() { ++Made; MadeOf = Size; }
        protected override void OnDestroyed() { World.Set(Entity, "Restarter", "Gone", Gone + 1); }
        public void Update(float delta) { Kept = ++_frames; if (Other == 7) Size = 99; }
    }
    """);
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
Write("Sign.cs", """
    // Text: the engine's own, read into the field each frame and written back after.
    public sealed class Sign : Script
    {
        public string Words = "";
        public int Frames;
        protected override void Update(float delta) { ++Frames; Words += "!"; }
    }
    """);
Write("Feeder.cs", """
    // The graphics half: what a pass reads, written from an entity's data before anything is drawn.
    public sealed class Feeder : Setup, BeforeDraw
    {
        public float Value;
        public int Prepared;
        public void Prepare()
        {
            ++Prepared;
            World.Buffer("numbers")?.Write([Value, Value * 2]);
        }
    }
    """);
Write("Pyramid.cs", """
    // A mesh made of its fields: made again when they change, and shared by every entity of the same ones.
    public sealed class Pyramid : Setup, MeshGenerator
    {
        public float Height = 1;
        public void Initialize(MeshWriter mesh)
        {
            mesh.Positions([new(-0.5f, 0, -0.5f), new(0.5f, 0, -0.5f), new(0.5f, 0, 0.5f), new(-0.5f, 0, 0.5f), new(0, Height, 0)]);
            mesh.Indices([0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0, 0, 1, 2, 0, 2, 3]);
        }
    }
    """);
Write("Checker.cs", """
    // A picture made of its fields, in its entity's material: made again when they change.
    public sealed class Checker : Setup, ImageGenerator
    {
        public int Squares = 2;
        public void Initialize(ImageWriter image)
        {
            image.Size(8, 8);
            for (var y = 0; y < image.Height; ++y)
                for (var x = 0; x < image.Width; ++x)
                    image.Set(x, y, (x * Squares / image.Width + y * Squares / image.Height) % 2 == 0 ? new Vec4(1, 1, 1, 1) : new Vec4(0, 0, 0, 1));
        }
    }
    """);
Write("Ramp.cs", """
    // Makes a buffer of a number each, a step apart, an image a step brighter a row, a view of it and a sampler. A
    // Script node of the graph runs it, with its fields, and gives what it makes.
    public sealed class Ramp : ResourceScript
    {
        public float Step = 1;
        public Buffer Table = new(instanceCount: 32, usage: "eStorage");
        public Image Rows = new(format: "eR32_SFLOAT", width: 4, height: 8, usage: "eSampled | eTransferSrc | eTransferDst");
        public ImageView RowsView = new(image: nameof(Rows));
        public Sampler Nearest = new(minFilter: "eNearest", magFilter: "eNearest");
        protected override void Init()
        {
            var numbers = Table.As<float>();
            for (var i = 0; i < numbers.Length; ++i) numbers[i] = i * Step;
            var texels = Rows.As<float>();
            for (var y = 0; y < Rows.Height; ++y)
                for (var x = 0; x < Rows.Width; ++x) texels[y * Rows.Width + x] = y * Step;
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
// The project's: the engine's package is compiled with them, so that a script reads a light as it reads its own, but
// its cameras, lights and sky are Data, which no host runs.
Check(host.Scripts.Order().SequenceEqual(["Checker", "Feeder", "Pyramid", "Restarter", "Roller", "Sign", "Spin", "Watcher"]),
      $"each is a script, and the package's data none: {string.Join(", ", host.Scripts)}");
Check(world.HasComponentType("Spin"), "a script is a component type, by its class name");
{
    // A package's Data starts as its source says, the engine having read it: its own fields and its kinds'.
    var lamp = world.Create("Lamp");
    Check(world.Add(lamp, "PointLight"), "a point light is a component");
    Check(world.FieldJson(lamp, "PointLight", "range") == "5", $"it starts as its source says: {world.FieldJson(lamp, "PointLight", "range")}");
    Check(world.FieldJson(lamp, "PointLight", "intensity") == "1", $"with what its kind, Light, says: {world.FieldJson(lamp, "PointLight", "intensity")}");
    Check(!world.Add(lamp, "Light"), "a kind is had by no entity itself");
    Check(world.With("Light").Contains(lamp) && world.With("LocalLight").Contains(lamp), "and is found by each of its kinds");
    world.Destroy(lamp);
}

var wheel = world.Create("Wheel");

{
    // A generator is its entity's mesh: of its fields, shared by those of the same fields, made again when they change.
    var a = world.Create("Pyramid A");
    var b = world.Create("Pyramid B");
    Check(world.Add(a, "Pyramid") && world.Add(b, "Pyramid"), "a generator is a component");
    world.Set(a, "Pyramid", "Height", 2f);
    world.Set(b, "Pyramid", "Height", 2f);
    Frames(2);
    var made = world.FieldJson(a, "Mesh", "mesh").Trim('"');
    Check(made.StartsWith("$script/gen/Pyramid/"), $"its entity's Mesh holds what it made: {made}");
    Equal(world.FieldJson(b, "Mesh", "mesh").Trim('"'), made, "the same fields, the same mesh");
    Equal(world.Meshes.Read(made).VertexCount, 5, "as it wrote it");
    Near(world.Meshes.Read(made).Find("POSITION")!.AsFloats()[13], 2, "of its fields");
    world.Set(b, "Pyramid", "Height", 3f);
    Frames(2);
    var again = world.FieldJson(b, "Mesh", "mesh").Trim('"');
    Check(again != made && again.StartsWith("$script/gen/Pyramid/"), "changed, it is made again");
    Equal(world.FieldJson(a, "Mesh", "mesh").Trim('"'), made, "and the other keeps its own");
    world.Destroy(a);
    world.Destroy(b);
}

{
    // A picture generator is its entity's material's picture: of its fields, made again when they change.
    var tiled = world.Create("Tiled");
    Check(world.Add(tiled, "PbrMaterial") && world.Add(tiled, "Checker"), "a picture generator is a component");
    world.Set(tiled, "Checker", "Squares", 2);
    Frames(2);
    var made = world.FieldJson(tiled, "PbrMaterial", "baseColorMap").Trim('"');
    Check(made.StartsWith("$script/gen/Checker/"), $"its material's picture is what it made: {made}");
    world.Set(tiled, "Checker", "Squares", 4);
    Frames(2);
    Check(world.FieldJson(tiled, "PbrMaterial", "baseColorMap").Trim('"') != made, "changed, it is made again");
    world.Destroy(tiled);
}

{
    var barrel = world.Create("Barrel");
    Check(world.Add(barrel, "Roller"), "the script the editor makes is a component");
    Frames(3);
    Near(Number(barrel, "Roller", "Speed"), 45, "it started");
    Check(world.RotationOf(barrel).Y > 0, "and turns its entity");
    Check(world.Has(barrel, "PerFrame"), "an entity running a script that takes PerFrame has its tag");
    Check(!world.Has(barrel, "FixedStep"), "and not one of a piece it does not take");
    world.Destroy(barrel);
}

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
// What Initialize reads, changed from outside: the script starts over, a new object, once. Nothing else does that.
{
    var restarted = world.Create("Restarted");
    Check(world.Add(restarted, "Restarter"), "an entity is given a script that starts over");
    int Field(string name) => int.Parse(world.FieldJson(restarted, "Restarter", name));
    Frames(3);
    Check(Field("Made") == 1, "initialized once");
    world.Set(restarted, "Restarter", "Size", 5);
    Frames(1);
    Check(Field("Made") == 2, "changed: initialized again");
    Check(Field("Gone") == 1, "what the old one held let go of first");
    Check(Field("MadeOf") == 5, "and of what it is now");
    Check(Field("Kept") == 1, "a new object: nothing private is left of the old");
    Frames(3);
    Check(Field("Made") == 2, "once for a change, not once a frame");
    world.Set(restarted, "Restarter", "Other", 3);
    Frames(2);
    Check(Field("Made") == 2, "a field not marked starts nothing over");
    world.Set(restarted, "Restarter", "Other", 7);
    Frames(3);
    Check(Field("Size") == 99 && Field("Made") == 2, "and what the script changes itself is its own to do");
    world.Destroy(restarted);
}

Check(world.Add(wheel, "Spin"), "an entity is given a script");
world.Set(wheel, "Spin", "Speed", 2);
world.Set(wheel, "Spin", "Angle", 10);
Frames(3);
Near(Number(wheel, "Spin", "Updates"), 3, "updated once a frame");
Near(Number(wheel, "Spin", "Angle"), 16, "its data is the engine's: 10 + 3 * 2");
Equal(world.FieldJson(wheel, "Spin", "Started"), "true", "started, before its first update");
Equal(world.FieldJson(wheel, "Spin", "Axis").Replace(" ", ""), "[0,1,0]", "a vector field");
// Set as whatever the field is: a vector, an entity, none.
world.Set(wheel, "Spin", "Axis", new Vec3(1, 0.5f, 0));
Equal(world.FieldJson(wheel, "Spin", "Axis").Replace(" ", ""), "[1,0.5,0]", "a vector, set whole");
{
    var hub = world.Create("Hub");
    world.Set(wheel, "Spin", "Target", hub);
    Check(world.FieldJson(wheel, "Spin", "Target").Replace(" ", "").Contains($"\"entity\":{hub.Index}"), $"an entity: {world.FieldJson(wheel, "Spin", "Target")}");
    world.Set(wheel, "Spin", "Target", null);
    Equal(world.FieldJson(wheel, "Spin", "Target").Replace(" ", ""), "null", "and none");
    world.Destroy(hub);
}
{   // Many at once: a value an entity, in two calls whatever the number.
    Entity[] many = [world.Create("Many 0"), world.Create("Many 1"), world.Create("Many 2")];
    Equal(world.SetRotations(many, [new Vec3(0, 10, 0), new Vec3(0, 20, 0), new Vec3(0, 30, 0)]).ToString(), "3", "every one of them set");
    Near(world.RotationOf(many[1]).Y, 20, "each its own");
    var turned = new Vec3[3];
    world.RotationsOf(many, turned);
    Near(turned[2].Y, 30, "and read back in order");
    world.Destroy(many[2]);
    Equal(world.SetPositions(many, [new Vec3(1, 1, 1), new Vec3(1, 1, 1), new Vec3(1, 1, 1)]).ToString(), "2", "one that is gone is left out");
    world.ScalesOf(many, turned);
    Near(turned[2].X, 1, "and reads as nothing: a scale of one");
    // Turned by quaternions: kept as given, made length 1, the angles worked out of them.
    var h = MathF.Sqrt(0.5f);
    Equal(world.SetOrientations(many, [new Quat(0, 1, 0, 1), new Quat(0, 0, 0, 1), new Quat(0, 0, 0, 1)]).ToString(), "2", "the two alive");
    var quats = new Quat[3];
    world.OrientationsOf(many, quats);
    Near(quats[0].Y, h, "made length 1");
    Near(quats[2].W, 1, "the identity, for one that is gone");
    Check(MathF.Abs(world.RotationOf(many[0]).Y - 90) < 0.05f, "and the angles follow (a quarter turn about Y is where they are least exact)");
    var drawn = world.Debug.LineCount;
    world.Debug.Line(new Vec3(0, 0, 0), new Vec3(1, 0, 0));
    Check(world.Debug.LineCount > drawn, "a script draws with the scene's debug lines");
    Check(world.SetOrientation(many[1], new Quat(h, 0, 0, h)), "one at a time");
    Near(world.OrientationOf(many[1]).X, h, "kept");
    world.Destroy(many[0]);
    world.Destroy(many[1]);
}
{   // A field of many at once, as numbers: a Vec3 is three an entity, a bool one.
    Entity[] two = [wheel, world.Create("No Spin")];
    Equal(world.FieldWidth("Spin", "Axis").ToString(), "3", "a Vec3 is three numbers");
    Equal(world.FieldWidth("Spin", "Nothing").ToString(), "0", "a field there is not");
    var before = new float[6];
    world.FieldFloats(two, "Spin", "Axis", before);
    Equal(world.SetFieldFloats(two, "Spin", "Axis", [1, 2, 3, 4, 5, 6]).ToString(), "1", "only the one with a Spin");
    Equal(world.FieldJson(wheel, "Spin", "Axis").Replace(" ", ""), "[1,2,3]", "written where it is");
    var axes = new float[6];
    world.FieldFloats(two, "Spin", "Axis", axes);
    Near(axes[4], 0, "nothing for one without it");
    var started = new int[2];
    world.FieldInts(two, "Spin", "Started", started);
    Equal($"{started[0]},{started[1]}", "1,0", "a bool as 1 and 0");
    world.SetFieldFloats(two, "Spin", "Axis", before);
    world.Destroy(two[1]);
}
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

// ---- a script fills a resource as it is made ----------------------------------------------------------------------

Check(host.ResourceScripts.SequenceEqual(["Ramp"]), $"a script that makes resources is one: {string.Join(", ", host.ResourceScripts)}");
Check(!world.HasComponentType("Ramp"), "and no component");
world.AddNode("Script", "ramp");
world.SetInput("ramp", "script", "\"Ramp\"");
world.SetInput("ramp", "field.Step", "0.25");
Check(world.NodeOk("ramp", out why), $"what it makes is made: {why}");
Frames(1);
var ramp = world.Buffer("ramp", "Table")!.Read<float>(8);
Near(ramp[3], 0.75f, "with what the script filled it with, given the node's field");
Check(world.Image("ramp", "Rows") is not null, "its image, an output by its name");
Check(world.Resource<Resource>("ramp", "RowsView") is not null && world.Resource<Resource>("ramp", "Nearest") is not null, "and its view and its sampler");
world.SetInput("ramp", "field.Step", "2");
Frames(2);
Near(world.Buffer("ramp", "Table")!.Read<float>(8)[3], 6, "a field changed: made and filled again");

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
Write("Spin.cs", "public sealed class Spin : Script { public float Angle; public System.Collections.Generic.List<int> Name = new(); }");
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

// ---- a text field: the engine's text, both ways ------------------------------------------------------------------
{
    var sign = world.Create("Sign");
    Check(world.Add(sign, "Sign"), "a script with a text field is a component");
    world.Set(sign, "Sign", "Words", "héllo, \"you\"");
    Frames(2);
    Equal(world.FieldJson(sign, "Sign", "Words"), "\"héllo, \\\"you\\\"!!\"", "text set from outside, read by the script, and written back, twice over");
    world.Set(sign, "Sign", "Words", new string('x', 300));
    Frames(1);
    Equal(world.FieldJson(sign, "Sign", "Words").Length, 300 + 1 + 2, "a long one, too: 300 and one, quoted");
}

var laps = world.FieldJson(wheel, "Spin", "Laps");
host.Dispose();
Frames(2);
Equal(world.FieldJson(wheel, "Spin", "Laps"), laps, "with the host gone nothing runs, and the data is still the scene's");

// ---- a script's meshes, of attributes of its own ------------------------------------------------------------------

{
    var blade = world.Meshes.Create("blade", 3, [
        MeshAttribute.Floats("POSITION", 3, [0, 0, 0, 1, 0, 0, 0, 1, 0]),
        MeshAttribute.Floats("WIND", 1, [0f, 0.5f, 1f]),
    ], [0, 1, 2], "POSITION; WIND");
    Equal(blade, "$script/blade", "a script's mesh is named for it");
    var grass = world.Create("Grass");
    world.Meshes.Give(grass, blade);
    Equal(world.FieldJson(grass, "Mesh", "mesh"), "\"$script/blade\"", "and given to an entity");
    var read = world.Meshes.Read(blade);
    Equal(read.VertexCount, 3, "read back: its vertices");
    Near(read.Find("WIND")?.AsFloats()[2] ?? -1, 1f, "its own attribute");
    var coloured = world.Meshes.WithAttributes("$cube", "coloured cube", [MeshAttribute.Floats("COLOR", 3, new float[world.Meshes.Read("$cube").VertexCount * 3])]);
    Equal(world.Meshes.Read(coloured).Attributes.Count, 5, "a default mesh (position, normal, UV, tangent), with an attribute more");
    Equal(world.Meshes.Read(world.Meshes.WithoutAttributes(coloured, "bare cube", "COLOR", "UV")).Attributes.Count, 3, "and fewer");
    try { world.Meshes.Create("wrong", 3, [MeshAttribute.Floats("WIND", 1, [0f])]); Check(false, "a mesh without POSITION is refused"); }
    catch (EngineException e) { Check(e.Message.Contains("WIND") || e.Message.Contains("POSITION"), $"and says why: {e.Message}"); }
    Frames(2);
}

Console.WriteLine(failures == 0 ? "Koral.Engine: all checks passed" : $"Koral.Engine: {failures} checks FAILED");
return failures == 0 ? 0 : 1;
