// A sun's shadows: the meshes as the sun sees them, a cascade at a time, each into its quarter of one atlas — two by
// two. The engine's SunShadows function runs it, a turn of a loop a cascade.
//
// `cascades` is any buffer whose first four matrices are the cascades' — a ShadowCascades node's `matrices`, as a
// shader's sun block that starts with them holds them. `model` is where the entity is in the world, and `cascade`
// which of the four is being drawn: the turn's number (push.cascade).

struct Cascades {
    float4x4 lightViewProj[4];
};
ConstantBuffer<Cascades> cascades;

struct Cast {
    float4 position : SV_Position;
    float inside[4] : SV_ClipDistance;  // how far inside its cascade's quarter of the atlas, each way
};

// A cascade is drawn into its quarter of the atlas, and nothing of it into another's.
[shader("vertex")]
Cast shadowVertex(float3 position : POSITION, uniform float4x4 model, uniform uint cascade)
{
    float4 world = mul(float4(position, 1.0), model);
    float4 lit = mul(float4(world.xyz, 1.0), cascades.lightViewProj[cascade]);
    Cast cast;
    cast.position = float4(lit.xy * 0.5 + float2(float(cascade & 1), float(cascade >> 1)) - 0.5, lit.z, 1.0);
    cast.inside[0] = lit.x + 1.0;
    cast.inside[1] = 1.0 - lit.x;
    cast.inside[2] = lit.y + 1.0;
    cast.inside[3] = 1.0 - lit.y;
    return cast;
}

[shader("fragment")]
void shadowFragment() {}

// ---- a map a layer: a point light's face, a spot ----------------------------------------------------------------
//
// The engine's PointLightShadows and SpotLightShadows functions draw each map into a layer of one depth image, a turn of a loop
// a layer: `maps` is a buffer of the maps' matrices (a PointShadows or a SpotShadows node's `matrices`), and `layer`
// which is being drawn (push.layer, the turn's number).

StructuredBuffer<float4x4> maps;

[shader("vertex")]
float4 layerVertex(float3 position : POSITION, uniform float4x4 model, uniform uint layer) : SV_Position
{
    return mul(mul(float4(position, 1.0), model), maps[layer]);
}
