using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Koral.Engine;

/// <summary>
/// The engine's programs (<c>koral-editor</c>, <c>koral-play</c>) running a project's C# scripts in their own process:
/// .NET is started inside them (hostfxr), and these are what they call, between frames. The engine is the program's
/// own copy, not a library loaded beside it: its C interface is exported by the program itself.
/// </summary>
public static unsafe class Embedded
{
    private static ScriptHost? s_scripts;

    /// <summary>
    /// Once, before anything else: the module the engine's C interface is in — the program's own — and where Koral's
    /// library is (the program loaded it already). 1 when done, 0 having logged why not.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Start(IntPtr engine)
    {
        try
        {
            EngineLibrary.UseLoaded(engine == IntPtr.Zero ? NativeLibrary.GetMainProgramHandle() : engine);
            return 1;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[scripts] C# could not start: {e}");
            return 0;
        }
    }

    /// <summary>
    /// The scripts of <paramref name="project"/> (UTF-8, its folder) run on <paramref name="document"/>'s entities from
    /// now: compiled, and registered as components. 1 when they are, 0 when they did not compile (they are compiled
    /// again when one is saved) or could not be run.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Attach(IntPtr document, byte* project)
    {
        try
        {
            Stop();
            var folder = Marshal.PtrToStringUTF8((IntPtr)project) ?? "";
            s_scripts = new ScriptHost(new World(document), Path.Combine(folder, "scripts"));
            return s_scripts.Load() ? 1 : 0;
        }
        catch (Exception e)
        {
            Log.Error($"[scripts] the project's C# scripts could not be run: {e}");
            return 0;
        }
    }

    /// <summary>Between frames: a saved script compiled and loaded again.</summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static void Poll()
    {
        try { s_scripts?.Poll(); }
        catch (Exception e) { Log.Error($"[scripts] {e}"); }
    }

    /// <summary>The scripts stop: their components are the engine's, and stay; nothing is called on them.</summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static void Detach()
    {
        try { Stop(); }
        catch (Exception e) { Log.Error($"[scripts] {e}"); }
    }

    private static void Stop()
    {
        s_scripts?.Dispose();
        s_scripts = null;
    }
}
