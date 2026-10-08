// koral-engine-dotnet <project folder> [--edit] [--frames N]
//
// Runs a Koral Engine project with the C# scripts in its `scripts` folder: on its own, or (--edit) in the editor.
// Either way a script saved while it runs is compiled and loaded again, its data kept.

using Koral;
using Koral.Engine;

string? project = null;
var edit = false;
long frames = 0;
for (var i = 0; i < args.Length; ++i)
{
    if (args[i] == "--edit") edit = true;
    else if (args[i] == "--frames" && i + 1 < args.Length) frames = long.Parse(args[++i]);
    else if (args[i] is "--help" or "-h") project = null;
    else project = args[i];
}
if (project is null)
{
    Console.WriteLine("usage: koral-engine-dotnet <project folder> [--edit] [--frames N]");
    return 2;
}
project = Path.GetFullPath(project);
if (!File.Exists(Path.Combine(project, "project.json")))
{
    Console.Error.WriteLine($"koral-engine-dotnet: there is no project in '{project}': no project.json");
    return 1;
}

try
{
    using var app = new App();
    app.LoadLibrary(EngineLibrary.Path);
    var title = Path.GetFileName(project);
    var scene = edit
        ? app.Open("Editor", new WindowSettings { Title = $"Koral Engine: {title}", Extent = new UVec2(1600, 950) }, new SceneArgs().Set("project", project))
        : app.Open("Play", new WindowSettings { Title = title }, new SceneArgs().Set("project", project));

    World? world = null;
    ScriptHost? scripts = null;
    long frame = 0;
    while (app.Frame())
    {
        // The scene the scripts run in: found again when an editor has opened another.
        if (world is null || !world.IsCurrentFor(scene))
        {
            scripts?.Dispose();
            world = World.Of(scene);
            scripts = world is null ? null : new ScriptHost(world, Path.Combine(project, "scripts"));
            scripts?.Load();
        }
        scripts?.Poll();
        if (frames > 0 && ++frame >= frames) break;
    }
    scripts?.Dispose();
    return 0;
}
catch (Exception e) when (e is KoralException or EngineException or DllNotFoundException)
{
    Console.Error.WriteLine($"koral-engine-dotnet: {e.Message}");
    return 1;
}
