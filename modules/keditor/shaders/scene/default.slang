// The standard scene's shader: each entity's mesh, a matte colour of its own, lit by the sun. A Draw gives it each
// entity's `model` (where it is) and `color` (the field of the pipeline's component, named after its node); the graph
// fills `camera` and `light` from the scene's camera and its directional light.

module default;

import entity;
import koral_shadings;
import koral_display;
import koral_debug;     // what the editor's scene view shows of the material: its colour, its normal

struct Vertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct RasterData {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 worldNormal;
};

struct Camera {
    float4x4 view;
    float4x4 projection;
    float3 eye;
};
ConstantBuffer<Camera> camera;

struct DirectionalLight {
    float3 direction;       // the way it shines
    float intensity;
    float3 color;           // as picked
};
ConstantBuffer<DirectionalLight> light;

[shader("vertex")]
RasterData vertex(Vertex vertex, uniform float4x4 model)
{
    RasterData raster;
    const float4 world = mul(float4(vertex.position, 1.0), model);
    raster.position = mul(mul(world, camera.view), camera.projection);
    raster.worldPosition = world.xyz;
    raster.worldNormal = KoralTransformNormal(vertex.normal, model);
    return raster;
}

[shader("fragment")]
float4 fragment(RasterData raster, uniform float4x4 model, uniform float3 color) : SV_Target
{
    const float3 normal = KoralSafeNormalize(raster.worldNormal);
    const float3 albedo = KoralLinearFromGamma22(color);
    if (koralDebugShowing()) return float4(koralDebugColor(KoralDebugSurface(albedo, normal, 1.0, 0.0)), 1.0);

    // The sun, and a little light from all round so that what faces away from it is not black.
    const KoralDirectionalLight sun = KoralDirectionalLight(light.direction, KoralLinearFromGamma22(light.color) * light.intensity);
    const float3 lit = KoralLit(KoralMatte(normal, albedo), raster.worldPosition, normal, KoralCamera(camera.eye),
                                KoralSingleLight(sun), KoralUniformEnvironment(float3(0.03)));
    return float4(KoralDisplay(lit), 1.0);
}
