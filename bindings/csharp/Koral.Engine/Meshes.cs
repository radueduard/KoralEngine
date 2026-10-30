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

/// <summary>
/// What a <see cref="MeshGenerator"/> writes a mesh into: its attributes by semantic — the usual ones, or its own — its
/// indices, and, if it says, the buffers they are in. What it does not say is the engine's to choose.
/// </summary>
public sealed class MeshWriter
{
    internal readonly List<MeshAttribute> Attributes = [];
    internal uint[]? IndexList;
    internal string? BufferLayout;
    /// <summary>How many vertices it has: as many as its positions.</summary>
    public int VertexCount { get; private set; }

    /// <summary>Where each vertex is: what makes it a mesh, and how many vertices it has.</summary>
    public void Positions(ReadOnlySpan<Vec3> positions)
    {
        VertexCount = positions.Length;
        Attribute(MeshAttribute.Floats("POSITION", 3, MemoryMarshal.Cast<Vec3, float>(positions).ToArray()));
    }
    /// <summary>Which way each vertex's surface faces.</summary>
    public void Normals(ReadOnlySpan<Vec3> normals) => Attribute(MeshAttribute.Floats("NORMAL", 3, MemoryMarshal.Cast<Vec3, float>(normals).ToArray()));
    /// <summary>Where on a picture each vertex is: <c>UV</c>, or <c>UV1</c> for <paramref name="set"/> 1.</summary>
    public void Uvs(ReadOnlySpan<Vec2> uvs, int set = 0) => Attribute(MeshAttribute.Floats(set == 0 ? "UV" : $"UV{set}", 2, MemoryMarshal.Cast<Vec2, float>(uvs).ToArray()));
    /// <summary>The way each vertex's u grows along its surface, and in w which side of it v does (1 or -1).</summary>
    public void Tangents(ReadOnlySpan<Vec4> tangents) => Attribute(MeshAttribute.Floats("TANGENT", 4, MemoryMarshal.Cast<Vec4, float>(tangents).ToArray()));
    /// <summary>Any attribute, by its semantic: in place of one of the same semantic written before.</summary>
    public void Attribute(MeshAttribute attribute)
    {
        Attributes.RemoveAll(a => a.Semantic == attribute.Semantic);
        Attributes.Add(attribute);
    }
    /// <summary>Its triangles, three indices each, counter-clockwise seen from outside. None: drawn in the order of its vertices.</summary>
    public void Indices(ReadOnlySpan<uint> indices) => IndexList = indices.ToArray();
    /// <summary>Which vertex buffer each attribute is in — <c>"POSITION; NORMAL UV"</c>, a buffer to each ';' — rather than the engine's choice.</summary>
    public void Buffers(string layout) => BufferLayout = layout;
}

/// <summary>
/// What a script takes to make an entity's mesh: a sphere of so many subdivisions, a terrain of a seed. Its public fields
/// are what it is made of; whenever they change — in the inspector, while the scene plays or not — it is made again,
/// and the entity's Mesh holds it. A generator is given only what it writes: what it makes is of its fields alone, so
/// every entity with the same fields shares one mesh, and what was made is kept in the project's cache, there when the
/// scene is opened without scripts.
/// </summary>
/// <remarks>
/// <code>
/// public sealed class Pyramid : Setup, MeshGenerator
/// {
///     [Range(0.1f, 10)] public float Height = 1;
///     public void Initialize(MeshWriter mesh)
///     {
///         mesh.Positions([new(-0.5f, 0, -0.5f), new(0.5f, 0, -0.5f), new(0.5f, 0, 0.5f), new(-0.5f, 0, 0.5f), new(0, Height, 0)]);
///         mesh.Indices([0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0, 0, 1, 2, 0, 2, 3]);
///     }
/// }
/// </code>
/// </remarks>
public interface MeshGenerator
{
    /// <summary>Writes the mesh, of the fields as they are now.</summary>
    void Initialize(MeshWriter mesh);
}

/// <summary>What an <see cref="ImageGenerator"/> writes a picture into: so many texels, four bytes each — red, green, blue, alpha.</summary>
public sealed class ImageWriter
{
    internal byte[] Texels = [];
    /// <summary>How many texels across and down: <see cref="Size"/> says, and it starts 256 by 256.</summary>
    public int Width { get; private set; }
    public int Height { get; private set; }

    internal ImageWriter() => Size(256, 256);

    /// <summary>How big it is: every texel black and see-through again.</summary>
    public void Size(int width, int height)
    {
        if (width <= 0 || height <= 0) throw new ArgumentException("a picture has at least one texel");
        (Width, Height) = (width, height);
        Texels = new byte[width * height * 4];
    }
    /// <summary>The texel at <paramref name="x"/>, <paramref name="y"/> (from the top left): each of red, green, blue and alpha 0 to 1.</summary>
    public void Set(int x, int y, Vec4 colour)
    {
        var at = (y * Width + x) * 4;
        Texels[at] = (byte)Math.Clamp(colour.X * 255f + 0.5f, 0f, 255f);
        Texels[at + 1] = (byte)Math.Clamp(colour.Y * 255f + 0.5f, 0f, 255f);
        Texels[at + 2] = (byte)Math.Clamp(colour.Z * 255f + 0.5f, 0f, 255f);
        Texels[at + 3] = (byte)Math.Clamp(colour.W * 255f + 0.5f, 0f, 255f);
    }
    /// <summary>Its texels as bytes, four a texel, a row after another from the top: there to be written whole.</summary>
    public Span<byte> Bytes => Texels;
}

/// <summary>
/// What a script takes to make a picture of its entity's material: a noise, a height map, a checker. Its public fields
/// are what it is made of; whenever they change it is made again, and the entity's material field <see cref="Into"/>
/// holds it. As a <see cref="MeshGenerator"/>, it is given only what it writes; every entity of the same fields shares one,
/// and what was made is kept in the project's cache.
/// </summary>
/// <remarks>
/// <code>
/// public sealed class Checker : Setup, ImageGenerator
/// {
///     public int Squares = 8;
///     public void Initialize(ImageWriter image)
///     {
///         for (var y = 0; y &lt; image.Height; ++y)
///             for (var x = 0; x &lt; image.Width; ++x)
///                 image.Set(x, y, (x * Squares / image.Width + y * Squares / image.Height) % 2 == 0 ? new Vec4(1, 1, 1, 1) : new Vec4(0, 0, 0, 1));
///     }
/// }
/// </code>
/// </remarks>
public interface ImageGenerator
{
    /// <summary>The picture field of its entity's material that holds what it makes: <c>baseColorMap</c> unless it says.</summary>
    string Into => "baseColorMap";
    /// <summary>Writes the picture, of the fields as they are now.</summary>
    void Initialize(ImageWriter image);
}
