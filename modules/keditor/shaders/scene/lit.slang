module lit;

import engineCommon;
import koral_math;
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

// One block of push constants both stages read. `model` is the engine's to give: where the entity
// is in the world, and which entity it is. The rest is what the entity's PbrMaterial is, wired to the Draw from
// a Filter of what has one (baseColor to albedo):
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
};
[[vk::push_constant]] ConstantBuffer<DrawData> draw;

[shader("vertex")]
RasterInfo vertex(Vertex vertex)
{
    float4x4 model = draw.model;
    RasterInfo raster;
    float4 world = mul(float4(vertex.position, 1.0), model);
    raster.worldPosition = world.xyz;
    raster.position = mul(world, frame.viewProjection);
    raster.normal = KoralTransformNormal(vertex.normal, model);     // square to the surface however it is scaled
    return raster;
}

/** How much of a light at [distance] is left, out to [range]: falling with the square of the distance, to 0 at the end. */
float falloff(float distance, float range)
{
    float edge = saturate(1.0 - pow(distance / range, 4.0));
    return edge * edge / (distance * distance + 1.0);
}

/** How much of a surface's tiny facets face [halfway]: Trowbridge-Reitz (GGX). */
float facets(float facingHalfway, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = facingHalfway * facingHalfway * (a2 - 1.0) + 1.0;
    return a2 / (KoralPi * d * d);
}

/** How much of them is neither hidden from the light nor from the eye: Smith's, with Schlick's fit. */
float unshadowed(float facingEye, float facingLight, float roughness)
{
    float r = roughness + 1.0;
    float k = r * r / 8.0;
    return (facingEye / (facingEye * (1.0 - k) + k)) * (facingLight / (facingLight * (1.0 - k) + k));
}

/** How much is reflected rather than taken in, seen at a glancing angle or straight on: Schlick's fit of Fresnel. */
float3 reflected(float facing, float3 straightOn)
{
    return straightOn + (1.0 - straightOn) * pow(saturate(1.0 - facing), 5.0);
}

[shader("fragment")]
float4 fragment(RasterInfo raster) : SV_Target
{
    float3 albedo = KoralLinearFromGamma22(draw.albedo.rgb);
    float metallic = saturate(draw.metallic);
    float roughness = clamp(draw.roughness, 0.045, 1.0);   // none at all is a highlight too small to draw
    float3 normal = normalize(raster.normal);
    float3 toEye = normalize(frame.cameraPosition.xyz - raster.worldPosition);
    float facingEye = max(dot(normal, toEye), 1e-4);
    if (koralDebugShowing()) return float4(koralDebugColor(KoralSurface(albedo, normal, roughness, metallic, KoralLinearFromGamma22(draw.emission))), 1.0);

    // What it reflects straight on: a little of everything for what is not metal, its own colour for what is.
    // And what is left to scatter: a metal scatters none.
    float3 straightOn = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 scattered = albedo * (1.0 - metallic);

    // The sky's light from all round, scattered; and what of the sky it mirrors, the less the rougher it is.
    float3 lit = skyLight(normal) * scattered;
    lit += skyLight(reflect(-toEye, normal)) * reflected(facingEye, straightOn) * (1.0 - roughness);

    for (uint i = 0; i < frame.counts.x; i++) {
        Light light = lights[i];
        uint type = uint(light.direction.w);
        float3 toLight;
        float amount = 1.0;
        if (type == LightDirectional) {
            toLight = -light.direction.xyz;
        } else {
            float3 offset = light.position.xyz - raster.worldPosition;
            float distance = length(offset);
            toLight = offset / max(distance, 1e-4);
            amount = falloff(distance, light.position.w);
            if (type == LightSpot)
                amount *= smoothstep(light.cone.y, light.cone.x, dot(-toLight, light.direction.xyz));
        }
        float facing = dot(normal, toLight);
        if (facing <= 0.0 || amount <= 0.0) continue;
        float3 halfway = normalize(toLight + toEye);
        float3 mirrored = reflected(max(dot(halfway, toEye), 0.0), straightOn);
        float3 highlight = facets(max(dot(normal, halfway), 0.0), roughness) * unshadowed(facingEye, facing, roughness) * mirrored
                         / (4.0 * facingEye * facing);
        // A light's colour is what a matte white surface facing it shows: so the scattered part is not divided
        // by pi, and the highlight is multiplied by it.
        lit += light.color.rgb * amount * facing * (scattered * (1.0 - mirrored) + highlight * KoralPi);
    }
    lit += KoralLinearFromGamma22(draw.emission);

    return float4(KoralDisplay(lit), 1.0);
}
