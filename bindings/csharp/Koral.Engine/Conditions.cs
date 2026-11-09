// Conditions: what a ForEach or a Filter of the graph keeps an entity by, written in a script.

using System.Globalization;
using System.Reflection;
using System.Text.Json;

namespace Koral.Engine;

/// <summary>
/// A class of conditions: tests a ForEach or a Filter keeps an entity by, each named in its <c>conditions</c> as
/// <c>&lt;Class&gt;.&lt;function&gt;</c>. A condition is a public static function marked <see cref="ConditionAttribute"/>
/// that returns a bool: its first N arguments are the entity's components — <see cref="EntityMesh"/> for its Mesh, a
/// <see cref="Data"/> class for one of a script's — and the rest are its inputs, each an input on the node,
/// <c>&lt;Class&gt;.&lt;function&gt;.&lt;argument&gt;</c>: a number, a bool, a string, a vector, a <see cref="MeshLayout"/>,
/// a list of strings, or a component to compare with (a Bundle of it, wired). It is to read nothing but its arguments:
/// what it said of an entity is asked again only when one of those components changes, or an input does.
/// <code>
/// public sealed class MeshTests : Conditions
/// {
///     [Condition(1)]
///     public static bool HasNormals(EntityMesh mesh, float least = 1f) => mesh.Layout.Has("NORMAL");
/// }
/// </code>
/// </summary>
public abstract class Conditions;

/// <summary>
/// Marks a <see cref="Conditions"/> class's public static function a condition, whose first <paramref name="components"/>
/// arguments are the entity's components, and the rest its inputs.
/// </summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class ConditionAttribute(int components) : Attribute
{
    public int Components { get; } = components;
}

/// <summary>One of a mesh's vertex attributes: what it is (<c>POSITION</c>), the type of its numbers (<c>eFloat</c>), how many.</summary>
public sealed record MeshLayoutAttribute(string Semantic, string Type = "eFloat", int Count = 0);

/// <summary>What a mesh's vertices are made of: its attributes, by semantic. A condition's input of a vertex layout is one too.</summary>
public sealed class MeshLayout(IReadOnlyList<MeshLayoutAttribute> attributes)
{
    public IReadOnlyList<MeshLayoutAttribute> Attributes { get; } = attributes;

    /// <summary>Whether it has an attribute of <paramref name="semantic"/>.</summary>
    public bool Has(string semantic) => Attributes.Any(a => a.Semantic == semantic);
    /// <summary>Whether every attribute of it, by semantic, <paramref name="other"/> has too.</summary>
    public bool IsSubsetOf(MeshLayout other) => Attributes.All(a => other.Has(a.Semantic));

    public override string ToString() => string.Join(", ", Attributes.Select(a => a.Semantic));

    /// <summary>
    /// One as the graph gives it: a vertex layout (<c>{"attributes": [{"semantic": ...}]}</c>), a list of attributes, or a
    /// list of semantics.
    /// </summary>
    internal static MeshLayout FromJson(JsonElement json)
    {
        if (json.ValueKind == JsonValueKind.Object)
            foreach (var name in new[] { "attributes", "layout" })
                if (json.TryGetProperty(name, out var inner)) return FromJson(inner);
        var attributes = new List<MeshLayoutAttribute>();
        if (json.ValueKind == JsonValueKind.Array)
            foreach (var item in json.EnumerateArray())
            {
                if (item.ValueKind == JsonValueKind.String) attributes.Add(new MeshLayoutAttribute(item.GetString()!));
                else if (item.ValueKind == JsonValueKind.Object && item.TryGetProperty("semantic", out var semantic))
                    attributes.Add(new MeshLayoutAttribute(semantic.GetString() ?? "",
                        item.TryGetProperty("type", out var type) && type.ValueKind == JsonValueKind.String ? type.GetString()! : "eFloat",
                        item.TryGetProperty("count", out var count) && count.ValueKind == JsonValueKind.Number ? count.GetInt32() : 0));
            }
        return new MeshLayout(attributes);
    }
}

/// <summary>An entity's Mesh, as a condition is given it: what its mesh lays its vertices out as (nothing, while it is read).</summary>
public sealed class EntityMesh(MeshLayout layout)
{
    public MeshLayout Layout { get; } = layout;
}

/// <summary>A condition's arguments from what the engine gives: its components', and its inputs', values as JSON.</summary>
internal static class ConditionValues
{
    /// <summary>What a component argument of <paramref name="type"/> is called in the world.</summary>
    public static string ComponentName(Type type) => type == typeof(EntityMesh) ? "Mesh" : ScriptLayout.NameOf(type);

    /// <summary>Whether an argument of <paramref name="type"/> is one of the entity's components: its Mesh, a script's Data, a script.</summary>
    public static bool IsComponent(Type type) => type == typeof(EntityMesh)
        || ((type.IsSubclassOf(typeof(Data)) || type.IsSubclassOf(typeof(Setup))) && !type.IsAbstract && !type.IsSubclassOf(typeof(ResourceScript)));

    /// <summary>What an input of <paramref name="type"/> is to the graph, as a port's type; null for one it cannot be.</summary>
    public static string? PortType(Type type) =>
        type == typeof(float) || type == typeof(double) || type == typeof(int) || type == typeof(uint) ? "number"
        : type == typeof(bool) ? "bool"
        : type == typeof(string) ? "string"
        : type == typeof(Vec2) || type == typeof(Vec3) || type == typeof(Vec4) ? "vector"
        : type == typeof(MeshLayout) ? "struct KoralVertexLayout"
        : type == typeof(string[]) || type == typeof(List<string>) ? "list"
        : IsComponent(type) && type != typeof(EntityMesh) ? "bundle"
        : null;

    /// <summary>An input's default, as JSON: what the node starts with. Null for none.</summary>
    public static string? Initial(ParameterInfo parameter) => !parameter.HasDefaultValue || parameter.DefaultValue is null ? null : parameter.DefaultValue switch
    {
        bool on => on ? "true" : "false",
        string text => JsonSerializer.Serialize(text),
        IFormattable number => number.ToString(null, CultureInfo.InvariantCulture),
        _ => null,
    };

    public static object? From(JsonElement json, Type type)
    {
        float Number(JsonElement e) => e.ValueKind == JsonValueKind.Number ? e.GetSingle() : e.ValueKind == JsonValueKind.True ? 1f : 0f;
        float[] Numbers(JsonElement e, int count)
        {
            var numbers = new float[count];
            if (e.ValueKind == JsonValueKind.Array)
            {
                var i = 0;
                foreach (var item in e.EnumerateArray()) { if (i >= count) break; numbers[i++] = Number(item); }
            }
            else numbers[0] = Number(e);
            return numbers;
        }
        if (type == typeof(float)) return Number(json);
        if (type == typeof(double)) return (double)Number(json);
        if (type == typeof(int)) return (int)Math.Round(Number(json));
        if (type == typeof(uint)) return (uint)Math.Max(0, Math.Round(Number(json)));
        if (type == typeof(bool)) return json.ValueKind == JsonValueKind.True || (json.ValueKind == JsonValueKind.Number && json.GetDouble() != 0);
        if (type == typeof(string)) return json.ValueKind == JsonValueKind.String ? json.GetString() : json.ToString();
        if (type == typeof(Vec2)) { var n = Numbers(json, 2); return new Vec2(n[0], n[1]); }
        if (type == typeof(Vec3)) { var n = Numbers(json, 3); return new Vec3(n[0], n[1], n[2]); }
        if (type == typeof(Vec4)) { var n = Numbers(json, 4); return new Vec4(n[0], n[1], n[2], n[3]); }
        if (type == typeof(MeshLayout)) return MeshLayout.FromJson(json);
        if (type == typeof(string[]) || type == typeof(List<string>))
        {
            var texts = json.ValueKind == JsonValueKind.Array ? json.EnumerateArray().Select(e => e.ValueKind == JsonValueKind.String ? e.GetString()! : e.ToString()).ToList() : [];
            return type == typeof(string[]) ? texts.ToArray() : texts;
        }
        if (type == typeof(EntityMesh))
        {
            // The component's mesh field, as its layout.
            if (json.ValueKind == JsonValueKind.Object)
                foreach (var field in json.EnumerateObject())
                    if (field.Value.ValueKind == JsonValueKind.Object && field.Value.TryGetProperty("layout", out var layout)) return new EntityMesh(MeshLayout.FromJson(layout));
            return new EntityMesh(new MeshLayout([]));
        }
        if (type.IsEnum) return Enum.ToObject(type, (int)Math.Round(Number(json)));
        if (IsComponent(type))
        {
            // One of a script's: a new one, each field the JSON has set from it.
            var made = Activator.CreateInstance(type)!;
            if (json.ValueKind == JsonValueKind.Object)
                foreach (var field in type.GetFields(BindingFlags.Public | BindingFlags.Instance))
                    if (json.TryGetProperty(field.Name, out var value) && value.ValueKind != JsonValueKind.Null && (PortType(field.FieldType) is not null || field.FieldType.IsEnum))
                        field.SetValue(made, From(value, field.FieldType));
            return made;
        }
        return null;
    }
}
