// The editor's own ways of looking at a scene, whatever its graph and its materials: every entity with a Mesh, drawn
// by what the mesh is — its edges, its shape under a light from the eye, its normals, its UVs, how often a pixel is
// drawn over, which entity it is. The editor's scene-view graph (packages/koral/editor/scene-view.kgraph) runs them.
//
// `model` and `entityId` are wired to each Draw from a Filter of Mesh, so no entity needs anything to be drawn here. A
// mesh that has not what a vertex entry reads (a NORMAL, a UV) is not drawn by it.

struct View {
    float4x4 viewProjection;
    float4x4 view;
};
ConstantBuffer<View> view;

struct Object {
    float4x4 model;
    uint entityId;
};
[[vk::push_constant]] ConstantBuffer<Object> object;

struct Shaped {
    float4 position : SV_Position;
    float3 normal;
};

struct Mapped {
    float4 position : SV_Position;
    float2 uv;
};

float4 placed(float3 position) { return mul(mul(float4(position, 1.0), object.model), view.viewProjection); }

/** A normal into the world through the cofactors of the model's 3x3: square to the surface however it is scaled. */
float3 turned(float3 normal)
{
    float4x4 model = object.model;
    float3 a0 = model[0].xyz, a1 = model[1].xyz, a2 = model[2].xyz;
    float3 n = normal.x * cross(a1, a2) + normal.y * cross(a2, a0) + normal.z * cross(a0, a1);
    return normalize(n * sign(dot(a0, cross(a1, a2))));
}

[shader("vertex")]
float4 positionOnly(float3 position : POSITION) : SV_Position { return placed(position); }

[shader("vertex")]
Shaped withNormal(float3 position : POSITION, float3 normal : NORMAL)
{
    Shaped shaped;
    shaped.position = placed(position);
    shaped.normal = turned(normal);
    return shaped;
}

[shader("vertex")]
Mapped withUV(float3 position : POSITION, float2 uv : UV)
{
    Mapped mapped;
    mapped.position = placed(position);
    mapped.uv = uv;
    return mapped;
}

/** Its edges, in one light colour. */
[shader("fragment")]
float4 edges() : SV_Target { return float4(0.82, 0.85, 0.9, 1.0); }

/** Its shape alone: grey clay lit from the eye, so that what faces it is brightest. */
[shader("fragment")]
float4 clay(Shaped shaped) : SV_Target
{
    float3 seen = mul(float4(normalize(shaped.normal), 0.0), view.view).xyz;
    float light = 0.18 + 0.82 * pow(abs(normalize(seen).z), 0.8);
    return float4(float3(0.78, 0.76, 0.72) * light, 1.0);
}

/** Its normals in the world, -1 to 1 as 0 to 1: +X red, +Y green, +Z blue. */
[shader("fragment")]
float4 normals(Shaped shaped) : SV_Target { return float4(normalize(shaped.normal) * 0.5 + 0.5, 1.0); }

/** Its UVs: a checker of eight squares a unit, tinted by where in the unit it is — a seam or a stretch is seen. */
[shader("fragment")]
float4 uvChecker(Mapped mapped) : SV_Target
{
    float2 cells = floor(mapped.uv * 8.0);
    float check = fmod(abs(cells.x + cells.y), 2.0) < 1.0 ? 0.35 : 0.85;
    float2 inUnit = frac(mapped.uv);
    return float4(lerp(float3(check), float3(inUnit, 0.5), 0.35), 1.0);
}

/** A little each time a pixel is drawn, added up: what is drawn over many times glows. */
[shader("fragment")]
float4 overdraw() : SV_Target { return float4(0.08, 0.035, 0.012, 1.0); }

/** Which entity it is, as a colour of its own. */
[shader("fragment")]
float4 entities() : SV_Target
{
    uint h = object.entityId * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    float3 color = float3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0;
    return float4(0.25 + 0.75 * color, 1.0);
}
