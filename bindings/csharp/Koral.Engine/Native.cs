// koral_engine.h, as C# calls it. One library, found beside this assembly or where KORAL_ENGINE_LIBRARY says.

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Koral.Engine;

/// <summary>Where the engine's native library is: what an application loads for the engine's scenes.</summary>
public static class EngineLibrary
{
    internal const string Name = "koral-engine";

    /// <summary>The file the library is, on this OS.</summary>
    public static string FileName =>
        OperatingSystem.IsWindows() ? "koral-engine.dll" :
        OperatingSystem.IsMacOS() ? "libkoral-engine.dylib" :
        "libkoral-engine.so";

    /// <summary>
    /// The library's path: <c>KORAL_ENGINE_LIBRARY</c> when that names it, and otherwise beside this assembly or one
    /// folder up from it. Throws when it is in none of them.
    /// </summary>
    public static string Path
    {
        get
        {
            if (s_path is not null) return s_path;
            var candidates = new List<string>();
            if (Environment.GetEnvironmentVariable("KORAL_ENGINE_LIBRARY") is { Length: > 0 } named) candidates.Add(named);
            var here = System.IO.Path.GetDirectoryName(typeof(EngineLibrary).Assembly.Location) ?? ".";
            candidates.Add(System.IO.Path.Combine(here, FileName));
            candidates.Add(System.IO.Path.Combine(here, "..", FileName));
            foreach (var candidate in candidates)
                if (File.Exists(candidate)) return s_path = System.IO.Path.GetFullPath(candidate);
            throw new DllNotFoundException(
                $"Koral Engine's native library ({FileName}) was not found beside {here}. Set KORAL_ENGINE_LIBRARY to the library itself.");
        }
    }
    private static string? s_path;

    /// <summary>
    /// The library, loaded once, as Koral's own modules are: its import of Koral.dll is then the engine C# has loaded,
    /// not the managed assembly of that name. On Windows its other imports — Koral's modules, spdlog — are looked for
    /// beside the program and on PATH, not beside Koral: Koral's folder and its modules/ go on PATH first.
    /// </summary>
    public static IntPtr Load()
    {
        if (s_handle != IntPtr.Zero) return s_handle;
        if (OperatingSystem.IsWindows() && Koral.Native.NativeLibraryResolver.LoadedFrom is { } koral && System.IO.Path.IsPathRooted(koral))
        {
            var folder = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(koral))!;
            Environment.SetEnvironmentVariable("PATH", string.Join(';', folder, System.IO.Path.Combine(folder, "modules"), Environment.GetEnvironmentVariable("PATH")));
        }
        return s_handle = Koral.Native.NativeLibraryResolver.LoadBesideKoral(Path);
    }
    private static IntPtr s_handle;

#pragma warning disable CA2255
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void Register() =>
        NativeLibrary.SetDllImportResolver(typeof(EngineLibrary).Assembly, static (name, _, _) => name == Name ? Load() : IntPtr.Zero);
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct KecsField
{
    public byte* Name;
    public byte* Type;
    public uint Offset;
    public uint Size;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct KengineScriptCallbacks
{
    public IntPtr User;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, void> Created;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, void> Enabled;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, void> Started;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, float, void> FixedUpdate;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, float, void> Update;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, float, void> LateUpdate;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, void> Prepare;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, void> Disabled;
    public delegate* unmanaged[Cdecl]<IntPtr, Entity, IntPtr, void> Destroyed;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct KengineScriptResource
{
    public byte* Name;
    public byte* Kind;
    public byte* Settings;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct KengineContents
{
    public byte* Name;
    public byte* Node;
    public byte* Kind;
    public byte* Format;
    public uint ExtentX, ExtentY, ExtentZ;
    public uint Layers;
    public uint TexelBytes;
    public byte* Bytes;
    public nuint Size;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct KengineResourceScriptCallbacks
{
    public IntPtr User;
    public delegate* unmanaged[Cdecl]<IntPtr, KengineContents*, nuint, byte*, nuint, byte*> Fill;
}

internal static unsafe partial class Native
{
    private const string Lib = EngineLibrary.Name;
    public const uint NoComponent = 0xFFFFFFFFu;

    public static string Text(byte* text) => text is null ? "" : Marshal.PtrToStringUTF8((IntPtr)text) ?? "";
    public static string LastError() => Text(kengine_last_error());

    [LibraryImport(Lib)] public static partial byte* kengine_last_error();
    [LibraryImport(Lib)] public static partial byte* kengine_package_directory();
    /// <summary>Where the engine's own package is: the scripts every project has, the scene's Camera and lights among them.</summary>
    public static string PackageDirectory() => Text(kengine_package_directory());

    [LibraryImport(Lib)] public static partial IntPtr kengine_document(IntPtr graph);
    [LibraryImport(Lib)] public static partial IntPtr kengine_document_world(IntPtr document);
    [LibraryImport(Lib)] public static partial IntPtr kengine_script_input(IntPtr document);
    [LibraryImport(Lib)] public static partial IntPtr kengine_debug(IntPtr document);
    [LibraryImport(Lib)] public static partial byte* kengine_document_json(IntPtr document);
    [LibraryImport(Lib)] public static partial byte* kengine_project_directory(IntPtr document);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_document_save(IntPtr document, string path);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial byte* kengine_node_add(IntPtr document, string type, string? id, double x, double y);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_node_remove(IntPtr document, string node);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_node_connect(IntPtr document, string from, string? output, string node, string input);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_node_clear_input(IntPtr document, string node, string input);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial IntPtr kengine_node_resource(IntPtr document, string node, string? output);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial byte* kengine_node_value_json(IntPtr document, string node, string? output);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_node_set_input_json(IntPtr document, string node, string input, string json);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_node_ok(IntPtr document, string node);

    [LibraryImport(Lib)] public static partial Entity kecs_create(IntPtr world);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_destroy(IntPtr world, Entity entity);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_alive(IntPtr world, Entity entity);
    [LibraryImport(Lib)] public static partial nuint kecs_entity_count(IntPtr world);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_set_parent(IntPtr world, Entity child, Entity parent);
    [LibraryImport(Lib)] public static partial Entity kecs_parent(IntPtr world, Entity entity);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kecs_set_parent_at(IntPtr world, Entity child, Entity parent, int index, [MarshalAs(UnmanagedType.U1)] bool keepWorldTransform);
    [LibraryImport(Lib)] public static partial nuint kecs_child_count(IntPtr world, Entity entity);
    [LibraryImport(Lib)] public static partial Entity kecs_child(IntPtr world, Entity entity, nuint index);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial Entity kecs_find(IntPtr world, string name);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_position(IntPtr world, Entity entity, Vec3* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_rotation(IntPtr world, Entity entity, Vec3* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_scale(IntPtr world, Entity entity, Vec3* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_set_position(IntPtr world, Entity entity, Vec3* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_set_rotation(IntPtr world, Entity entity, Vec3* value);
    [LibraryImport(Lib)] public static partial nuint kecs_positions(IntPtr world, Entity* entities, nuint count, Vec3* values);
    [LibraryImport(Lib)] public static partial nuint kecs_rotations(IntPtr world, Entity* entities, nuint count, Vec3* values);
    [LibraryImport(Lib)] public static partial nuint kecs_scales(IntPtr world, Entity* entities, nuint count, Vec3* values);
    [LibraryImport(Lib)] public static partial nuint kecs_set_positions(IntPtr world, Entity* entities, nuint count, Vec3* values);
    [LibraryImport(Lib)] public static partial nuint kecs_set_rotations(IntPtr world, Entity* entities, nuint count, Vec3* values);
    [LibraryImport(Lib)] public static partial nuint kecs_set_scales(IntPtr world, Entity* entities, nuint count, Vec3* values);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_orientation(IntPtr world, Entity entity, Quat* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_set_orientation(IntPtr world, Entity entity, Quat* value);
    [LibraryImport(Lib)] public static partial nuint kecs_orientations(IntPtr world, Entity* entities, nuint count, Quat* values);
    [LibraryImport(Lib)] public static partial nuint kecs_set_orientations(IntPtr world, Entity* entities, nuint count, Quat* values);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial uint kecs_field_width(IntPtr world, uint component, string field);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial nuint kecs_field_floats(IntPtr world, Entity* entities, nuint count, uint component, string field, float* values);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial nuint kecs_set_field_floats(IntPtr world, Entity* entities, nuint count, uint component, string field, float* values);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial nuint kecs_field_ints(IntPtr world, Entity* entities, nuint count, uint component, string field, int* values);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial nuint kecs_set_field_ints(IntPtr world, Entity* entities, nuint count, uint component, string field, int* values);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_set_scale(IntPtr world, Entity entity, Vec3* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_local_matrix(IntPtr world, Entity entity, Mat4* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_world_matrix(IntPtr world, Entity entity, Mat4* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_set_world_matrix(IntPtr world, Entity entity, Mat4* value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial Entity kengine_entity_create(IntPtr document, string? name);
    [LibraryImport(Lib)] public static partial byte* kengine_entity_name(IntPtr document, Entity entity);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_entity_set_name(IntPtr document, Entity entity, string name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial uint kecs_component_find(IntPtr world, string name);
    [LibraryImport(Lib)] public static partial uint kecs_component_count(IntPtr world);
    [LibraryImport(Lib)] public static partial byte* kecs_component_name(IntPtr world, uint component);
    [LibraryImport(Lib)] public static partial nuint kecs_component_size(IntPtr world, uint component);
    [LibraryImport(Lib)] public static partial uint kecs_component_field_count(IntPtr world, uint component);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_component_field(IntPtr world, uint component, uint index, KecsField* field);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial uint kecs_register_tag(IntPtr world, string name);

    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_add(IntPtr world, Entity entity, uint component, void* value);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_remove(IntPtr world, Entity entity, uint component);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_has(IntPtr world, Entity entity, uint component);
    [LibraryImport(Lib)] public static partial void* kecs_get(IntPtr world, Entity entity, uint component);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kecs_set_enabled(IntPtr world, Entity entity, uint component, [MarshalAs(UnmanagedType.U1)] bool enabled);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_is_enabled(IntPtr world, Entity entity, uint component);
    [LibraryImport(Lib)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool kecs_mark_changed(IntPtr world, Entity entity, uint component);
    [LibraryImport(Lib)]
    public static partial nuint kecs_query(IntPtr world, uint* all, nuint allCount, uint* none, nuint noneCount, [MarshalAs(UnmanagedType.U1)] bool includeDisabled,
                                           Entity* found, nuint capacity);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial byte* kengine_field_json(IntPtr document, Entity entity, string component, string field);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_field_set_json(IntPtr document, Entity entity, string component, string field, string json);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial uint kengine_script_register(IntPtr document, string name, nuint size, nuint alignment, KecsField* fields, nuint fieldCount,
                                                       KengineScriptCallbacks* callbacks);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial void kengine_script_unregister(IntPtr document, string name);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_resource_script_register(IntPtr document, string name, nuint size, KecsField* fields, nuint fieldCount,
                                                                KengineScriptResource* resources, nuint resourceCount, KengineResourceScriptCallbacks* callbacks);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial void kengine_resource_script_unregister(IntPtr document, string name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial byte* kengine_image_create(IntPtr document, string name, uint width, uint height, byte* rgba);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_entity_give_picture(IntPtr document, Entity entity, string field, string picture);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial byte* kengine_mesh_create(IntPtr document, string name, uint vertexCount, KengineAttribute* attributes, uint attributeCount,
                                                    uint* indices, uint indexCount, string? buffers);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial byte* kengine_mesh_with_attributes(IntPtr document, string mesh, string name, KengineAttribute* attributes, uint attributeCount, string? buffers);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    public static partial byte* kengine_mesh_without_attributes(IntPtr document, string mesh, string name, string semantics);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial uint kengine_mesh_vertex_count(IntPtr document, string mesh);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial uint kengine_mesh_attribute_count(IntPtr document, string mesh);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool kengine_mesh_attribute(IntPtr document, string mesh, uint index, KengineAttribute* out_);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial nuint kengine_mesh_indices(IntPtr document, string mesh, uint* out_, nuint capacity);
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct KengineAttribute
{
    public byte* Semantic;
    public uint Type;
    public uint Count;
    public void* Data;
}

/// <summary>Something the engine refused, with why.</summary>
public sealed class EngineException(string message) : Exception(message);
