module lit;

import engineCommon;
import koral_debug;     // what the editor's scene view shows of the material: its albedo, its normal...

struct Vertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct RasterInfo {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
};

// One block of push constants both stages read. `model` is the engine's to give: where the entity is in the world;
// `lightCounts` the Lights function's `counts`, the same for every draw. The rest is what the entity's PbrMaterial is,
// wired to the Draw from a Filter of what has one (baseColor to albedo):
//   albedo     its colour, as picked: what it is under white light
//   metallic   0 for a thing that is not metal, 1 for one that is: its colour is then its reflection's
//   roughness  0 polished, 1 matte
//   emission   the light it gives off itself, as a colour: added whatever lights it
struct DrawData {
    float4x4 model;
    float4 albedo;          // a: how opaque, which nothing here uses
    float metallic;
    float3 emission;
    float roughness;
    uint4 lightCounts;      // how many lights of each kind: x directional, y point, z spot
};
[[vk::push_constant]] ConstantBuffer<DrawData> draw;

[shader("vertex")]
RasterInfo vertex(Vertex vertex)
{
    RasterInfo raster;
    const float4 world = mul(float4(vertex.position, 1.0), draw.model);
    raster.worldPosition = world.xyz;
    raster.position = camera.ToClip(world.xyz);
    raster.normal = KoralTransformNormal(vertex.normal, draw.model);     // square to the surface however it is scaled
    return raster;
}

[shader("fragment")]
float4 fragment(RasterInfo raster) : SV_Target
{
    // The engine's physically based shading of the material's fields: GGX over Lambert.
    KoralPbr pbr;
    pbr.normal = KoralSafeNormalize(raster.normal);
    pbr.baseColor = KoralLinearFromGamma22(draw.albedo.rgb);
    pbr.metallic = saturate(draw.metallic);
    pbr.roughness = clamp(draw.roughness, 0.045, 1.0);     // none at all is a highlight too small to draw
    pbr.occlusion = 1.0;
    pbr.emission = KoralLinearFromGamma22(draw.emission);
    pbr.opacity = 1.0;
    if (koralDebugShowing()) return float4(koralDebugColor(KoralDebugSurface(pbr.baseColor, pbr.normal, pbr.roughness, pbr.metallic, pbr.emission)), 1.0);

    // Lit by every light of the scene, each kind by itself, and by the sky all round.
    const float3 lit = KoralLit(pbr, raster.worldPosition, pbr.normal, FrameCamera(), FrameLights(draw.lightCounts.xyz), FrameAmbient());
    return float4(KoralDisplay(lit), 1.0);
}
