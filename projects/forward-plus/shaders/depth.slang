// Depth only, twice over: the pre-pass, which lays down how far everything is from the camera before anything is
// shaded, so that the forward pass shades each pixel once; and the shadow pass, which draws the same meshes as the
// sun sees them, a cascade at a time, into its quarter of the shadow atlas.

import forwardCommon;

ConstantBuffer<Sun> sun;

// `model` is the engine's to give: where the entity is in the world. `cascade` is which of the sun's four maps is
// being drawn: the turn's number of the loop the shadow pipeline is in (push.cascade).

[shader("vertex")]
float4 prepassVertex(float3 position : POSITION, uniform float4x4 model) : SV_Position
{
    return toClip(mul(float4(position, 1.0), model).xyz);
}

[shader("fragment")]
void prepassFragment() {}

struct Cast {
    float4 position : SV_Position;
    float inside[4] : SV_ClipDistance;  // how far inside its cascade's quarter of the atlas, each way
};

// A cascade is drawn into its quarter of the atlas — two by two — and nothing of it into another's.
[shader("vertex")]
Cast shadowVertex(float3 position : POSITION, uniform float4x4 model, uniform uint cascade)
{
    float4 world = mul(float4(position, 1.0), model);
    float4 lit = mul(float4(world.xyz, 1.0), sun.lightViewProj[cascade]);
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

ConstantBuffer<LampShadows> lampShadows;

// A point light's shadows: the same meshes as one face of the cube round the light sees them. `face` is which face of
// which light — six a light, one after another: the turn's number of the loop the pipeline is in (push.face).
[shader("vertex")]
float4 lampVertex(float3 position : POSITION, uniform float4x4 model, uniform uint face) : SV_Position
{
    float4 world = mul(float4(position, 1.0), model);
    return mul(float4(world.xyz, 1.0), lampShadows.faces[face]);
}

[shader("fragment")]
void lampFragment() {}
