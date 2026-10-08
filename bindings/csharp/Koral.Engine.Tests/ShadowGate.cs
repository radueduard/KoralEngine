// E6's gate, in C#: shadow mapping in a project the engine's own test made and saved. A Script moves the light, a
// GraphicsScript turns it into the camera the shadow pass draws from, and both are edited while the scene runs.
// What is checked is the picture: where the shadow falls.

using Koral;
using Koral.Engine;
using Buffer = Koral.Buffer;

internal static class ShadowGate
{
    private const string LightAlongX = """
        public sealed class Light : Script
        {
            public Vec2 Slant;          // how the light leans
            public float Speed;         // how far it leans further, each frame
            protected override void Update(float delta) => Slant = new Vec2(Slant.X + Speed, Slant.Y);
        }
        """;
    private const string LightAlongY = """
        public sealed class Light : Script
        {
            public Vec2 Slant;
            public float Speed;
            protected override void Update(float delta) => Slant = new Vec2(Slant.X, Slant.Y + Speed);     // the other way now
        }
        """;
    private const string LightRenamed = """
        public sealed class Light : Script
        {
            public Vec2 Slant;          // as it was: every light keeps where it leans
            public float Rate;          // was Speed: new, and zero
            public bool Paused;         // new
            protected override void Update(float delta) { if (!Paused) Slant = new Vec2(Slant.X, Slant.Y + Rate); }
        }
        """;
    private const string Camera = """
        // The light, as a camera: the matrix that takes the world to where the light sees it, a column after another.
        public sealed class LightCamera : GraphicsScript
        {
            public float Reach;
            protected override void Prepare()
            {
                var light = World.Read<Light>(Entity);
                if (light is null) return;
                float x = light.Slant.X * Reach, y = light.Slant.Y * Reach;
                World.Buffer("light camera")?.Write(new float[] { 1, 0, 0, 0,   0, 1, 0, 0,   x, y, -1, 0,   0, 0, 1, 1 });
            }
        }
        """;
    private const string CameraMirrored = """
        public sealed class LightCamera : GraphicsScript
        {
            public float Reach;
            protected override void Prepare()
            {
                var light = World.Read<Light>(Entity);
                if (light is null) return;
                float x = -light.Slant.X * Reach, y = light.Slant.Y * Reach;       // seen from the other side
                World.Buffer("light camera")?.Write(new float[] { 1, 0, 0, 0,   0, 1, 0, 0,   x, y, -1, 0,   0, 0, 1, 1 });
            }
        }
        """;

    public static int Run(string project)
    {
        var failures = 0;
        void Check(bool ok, string what)
        {
            if (ok) return;
            ++failures;
            Console.Error.WriteLine($"FAILED: {what}");
        }

        var scripts = Path.Combine(project, "scripts");
        if (Directory.Exists(scripts)) Directory.Delete(scripts, true);
        Directory.CreateDirectory(scripts);
        void Write(string file, string source) => File.WriteAllText(Path.Combine(scripts, file), source);
        Write("Light.cs", LightAlongX);
        Write("LightCamera.cs", Camera);

        using var app = new App(new AppSettings { Platform = WindowPlatform.eNone });
        app.LoadLibrary(EngineLibrary.Path);
        var scene = app.OpenOffscreen("Play", new OffscreenSettings { Title = "shadow", Extent = new UVec2(32, 32) }, new SceneArgs().Set("project", project));
        var world = World.Of(scene) ?? throw new Exception("the Play scene has no world");
        void Frames(int count) { for (var i = 0; i < count; ++i) app.Frame(); }

        foreach (var node in new[] { "shadow", "lit", "cast", "draw", "show" })
            Check(world.NodeOk(node, out var why), $"the project's graph is built: {node}: {why}");
        var light = world.Query([], []).Single(e => world.NameOf(e) == "Light");

        // The picture, read back: the lit pass's image, a byte a channel.
        using var readback = new Buffer.Builder<byte>().SetInstanceCount(32 * 32 * 4).SetUsage(Buffer.Usage.eTransferDst).SetType(Buffer.Type.eReadback).Build();
        byte Red(int x, int y)
        {
            using var canvas = world.Image("canvas") ?? throw new Exception("there is no canvas");
            CommandBuffer.SingleTimeCommand(cb => cb.CopyImageToBuffer(canvas, readback)).Wait();
            return readback.Read<byte>()[(y * 32 + x) * 4];
        }
        bool Dark(int x, int y) { var r = Red(x, y); return r is > 30 and < 80; }
        bool Lit(int x, int y) => Red(x, y) > 200;

        using var host = new ScriptHost(world, scripts);
        Check(host.Load(), $"the scripts compile: {string.Join("; ", host.Errors)}");
        world.Add(light, "Light");
        world.Add(light, "LightCamera");
        world.Set(light, "LightCamera", "Reach", 1);

        // The light straight overhead: the shadow is under the occluder, in the middle of the ground.
        Frames(2);
        Check(Dark(16, 16), $"the middle is in shadow ({Red(16, 16)})");
        Check(Lit(4, 4) && Lit(24, 16), "and the rest of the ground is lit");

        // Leaning: the shadow of something half a unit up falls half the lean away. A lean of 1 is 8 pixels.
        world.SetVector(light, "Light", "Slant", 1, 0);
        Frames(2);
        Check(Dark(24, 16) && Lit(16, 16), $"the shadow follows the light ({Red(24, 16)}, {Red(16, 16)})");

        // The script moving it, a step a frame.
        world.Set(light, "Light", "Speed", -0.25);
        Frames(8);
        world.Set(light, "Light", "Speed", 0);
        Check(world.FieldJson(light, "Light", "Slant").Replace(" ", "") == "[-1,0]", $"the script moved the light: {world.FieldJson(light, "Light", "Slant")}");
        Frames(2);
        Check(Dark(8, 16) && Lit(16, 16) && Lit(24, 16), $"and the shadow went with it ({Red(8, 16)})");

        // ---- edited while it runs -----------------------------------------------------------------------------

        // What a method does: the light moves the other way now. It carries on from where it was.
        Write("Light.cs", LightAlongY);
        Check(host.Reload(), $"the edited script compiles: {string.Join("; ", host.Errors)}");
        Check(world.FieldJson(light, "Light", "Slant").Replace(" ", "") == "[-1,0]", "the light is where it was");
        world.Set(light, "Light", "Speed", 0.25);
        Frames(4);
        world.Set(light, "Light", "Speed", 0);
        Frames(2);
        Check(Dark(8, 24) && Lit(8, 16), $"the new code moves the shadow down, from where it was ({Red(8, 24)}, {Red(8, 16)})");

        // The graphics script, edited: the camera sees from the other side.
        Write("LightCamera.cs", CameraMirrored);
        Check(host.Reload(), $"the edited graphics script compiles: {string.Join("; ", host.Errors)}");
        Check(world.FieldJson(light, "LightCamera", "Reach") == "1", "it kept its reach");
        Frames(2);
        Check(Dark(24, 24) && Lit(8, 24), $"and the shadow is on the other side ({Red(24, 24)}, {Red(8, 24)})");

        // A script's shape: a field renamed and one added. The light keeps what kept its name, and the rest is new.
        Write("Light.cs", LightRenamed);
        Check(host.Reload(), $"the reshaped script compiles: {string.Join("; ", host.Errors)}");
        Check(world.FieldJson(light, "Light", "Slant").Replace(" ", "") == "[-1,1]", $"where it leans is kept: {world.FieldJson(light, "Light", "Slant")}");
        Check(world.FieldJson(light, "Light", "Rate") == "0" && world.FieldJson(light, "Light", "Paused") == "false", "the new fields start at nothing");
        Check(world.FieldJson(light, "Light", "Speed") == "null", "and the old one is gone");
        world.Set(light, "Light", "Rate", -0.25);
        Frames(4);
        world.Set(light, "Light", "Paused", true);
        Frames(3);
        Check(world.FieldJson(light, "Light", "Slant").Replace(" ", "") == "[-1,0]", $"the new fields are what the new code reads: {world.FieldJson(light, "Light", "Slant")}");
        Check(Dark(24, 16) && Lit(24, 24), $"and the shadow shows it ({Red(24, 16)})");

        // And saved, as an editor saves it: noticed and loaded without being asked.
        host.PollInterval = TimeSpan.FromMilliseconds(10);
        host.SettleTime = TimeSpan.FromMilliseconds(60);
        Write("LightCamera.cs", Camera);
        var generation = host.Generation;
        for (var i = 0; i < 300 && host.Generation == generation; ++i) { Thread.Sleep(10); host.Poll(); }
        Frames(2);
        Check(host.Generation == generation + 1 && Dark(8, 16) && Lit(24, 16), $"a saved script is loaded, and the shadow is back on its side ({Red(8, 16)})");

        Console.WriteLine(failures == 0 ? "Koral.Engine shadow gate: all checks passed" : $"Koral.Engine shadow gate: {failures} checks FAILED");
        return failures == 0 ? 0 : 1;
    }
}
