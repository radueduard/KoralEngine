// Meshes a script makes: of any attributes — the usual ones, or its own (WIND) — in the buffers it says.

using System.Runtime.InteropServices;
using System.Text;

namespace Koral.Engine;

/// <summary>What an attribute's numbers are. Each is four bytes.</summary>
public enum AttributeType : uint { Float = 0, Int = 1, UInt = 2 }

/// <summary>
/// One attribute of every vertex of a mesh: what it is, as a shader asks for it (<c>POSITION</c>, <c>NORMAL</c>,
/// <c>UV</c>, or a name of the script's own), and <see cref="Count"/> numbers a vertex, one vertex after another — each
/// a word of four bytes (a float's bits, for floats).
/// </summary>
public sealed class MeshAttribute(string semantic, AttributeType type, int count, uint[] words)
{
    public string Semantic { get; } = semantic;
    public AttributeType Type { get; } = type;
    public int Count { get; } = count;
    public uint[] Words { get; } = words;

    public static MeshAttribute Floats(string semantic, int count, float[] values) =>
        new(semantic, AttributeType.Float, count, MemoryMarshal.Cast<float, uint>(values).ToArray());
    public static MeshAttribute Ints(string semantic, int count, int[] values) =>
        new(semantic, AttributeType.Int, count, MemoryMarshal.Cast<int, uint>(values).ToArray());
    public static MeshAttribute UInts(string semantic, int count, uint[] values) => new(semantic, AttributeType.UInt, count, (uint[])values.Clone());

    /// <summary>Its numbers as floats (what they are, for a float attribute).</summary>
    public float[] AsFloats() => MemoryMarshal.Cast<uint, float>(Words).ToArray();
    public int[] AsInts() => MemoryMarshal.Cast<uint, int>(Words).ToArray();
}

/// <summary>A mesh's numbers, as read back: its vertices' attributes and its indices.</summary>
public sealed record MeshData(int VertexCount, IReadOnlyList<MeshAttribute> Attributes, uint[] Indices)
{
    public MeshAttribute? Find(string semantic) => Attributes.FirstOrDefault(a => a.Semantic == semantic);
}

/// <summary>
/// The scene's meshes, as a script makes and reads them. A mesh made here is called <c>$script/name</c>, and is what an
/// entity's Mesh holds to have it (<see cref="Give"/>). Not saved with the scene: a script makes its meshes when it runs.
/// </summary>
public sealed unsafe class Meshes
{
    private readonly World _world;
    internal Meshes(World world) => _world = world;

    /// <summary>
    /// Makes a mesh of <paramref name="vertexCount"/> vertices, of these attributes (one of them POSITION, of floats).
    /// <paramref name="buffers"/> says which vertex buffer each is in — <c>"POSITION UV; NORMAL TANGENT"</c>, a buffer to
    /// each ';', what none names in one more — or, null, all in one. Returns its name. One so named already is replaced:
    /// every entity that held it holds this one.
    /// </summary>
    public string Create(string name, int vertexCount, IReadOnlyList<MeshAttribute> attributes, uint[]? indices = null, string? buffers = null) =>
        With(attributes, native =>
        {
            fixed (uint* at = indices)
                return Native.kengine_mesh_create(_world.Document, name, (uint)vertexCount, native, (uint)attributes.Count, at, (uint)(indices?.Length ?? 0), buffers);
        });

    /// <summary>A copy of <paramref name="mesh"/> — any: <c>$cube</c>, a model's, a script's — with these attributes added, or in place of its own of the same semantic.</summary>
    public string WithAttributes(string mesh, string name, IReadOnlyList<MeshAttribute> attributes, string? buffers = null) =>
        With(attributes, native => Native.kengine_mesh_with_attributes(_world.Document, mesh, name, native, (uint)attributes.Count, buffers));

    /// <summary>A copy of <paramref name="mesh"/> without the attributes named.</summary>
    public string WithoutAttributes(string mesh, string name, params string[] semantics) =>
        Result(Native.kengine_mesh_without_attributes(_world.Document, mesh, name, string.Join(' ', semantics)));

    /// <summary>A mesh's numbers, read back from the device: waits for it.</summary>
    public MeshData Read(string mesh)
    {
        var vertices = Native.kengine_mesh_vertex_count(_world.Document, mesh);
        if (vertices == 0) throw new EngineException(Native.LastError());
        var count = Native.kengine_mesh_attribute_count(_world.Document, mesh);
        var attributes = new List<MeshAttribute>();
        for (uint i = 0; i < count; ++i)
        {
            KengineAttribute read;
            if (!Native.kengine_mesh_attribute(_world.Document, mesh, i, &read)) throw new EngineException(Native.LastError());
            var words = new uint[read.Count * vertices];
            new ReadOnlySpan<uint>(read.Data, words.Length).CopyTo(words);
            attributes.Add(new MeshAttribute(Native.Text(read.Semantic), (AttributeType)read.Type, (int)read.Count, words));
        }
        var indices = new uint[(int)Native.kengine_mesh_indices(_world.Document, mesh, null, 0)];
        fixed (uint* at = indices) Native.kengine_mesh_indices(_world.Document, mesh, at, (nuint)indices.Length);
        return new MeshData((int)vertices, attributes, indices);
    }

    /// <summary>Gives an entity a mesh, by its name: its Mesh holds it.</summary>
    public void Give(Entity entity, string mesh)
    {
        if (!_world.Has(entity, "Mesh")) _world.Add(entity, "Mesh");
        _world.SetResource(entity, "Mesh", "mesh", mesh);
    }

    private delegate byte* Call(KengineAttribute* attributes);

    private static string With(IReadOnlyList<MeshAttribute> attributes, Call call)
    {
        var handles = new List<GCHandle>();
        var semantics = new List<IntPtr>();
        try
        {
            var native = new KengineAttribute[attributes.Count];
            for (var i = 0; i < attributes.Count; ++i)
            {
                var handle = GCHandle.Alloc(attributes[i].Words, GCHandleType.Pinned);
                handles.Add(handle);
                var semantic = Marshal.StringToCoTaskMemUTF8(attributes[i].Semantic);
                semantics.Add(semantic);
                native[i] = new KengineAttribute { Semantic = (byte*)semantic, Type = (uint)attributes[i].Type, Count = (uint)attributes[i].Count, Data = (void*)handle.AddrOfPinnedObject() };
            }
            fixed (KengineAttribute* at = native) return Result(call(at));
        }
        finally
        {
            foreach (var handle in handles) handle.Free();
            foreach (var semantic in semantics) Marshal.FreeCoTaskMem(semantic);
        }
    }

    private static string Result(byte* made) => made is null ? throw new EngineException(Native.LastError()) : Native.Text(made);
}
