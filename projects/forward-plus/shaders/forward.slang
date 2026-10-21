// Forward shading: each pixel lit by the point lights of its cluster and by the sun through its cascaded shadows,
// as a rough or polished, metal or not surface. Into the picture as light, and beside it where the surface is, which
// way it faces and how it mirrors — what the ambient occlusion and the reflections after it read.
//
// The depth pre-pass has already laid the depth down, so this shades only what is seen.

import forwardCommon;

ConstantBuffer<Sun> sun;
StructuredBuffer<PointLight> lights;
StructuredBuffer<uint2> lightGrid;          // for each cluster: where its lights start in the list, and how many
StructuredBuffer<uint> lightIndices;
Sampler2D<float> shadowMap;                 // the four cascades, two by two
ConstantBuffer<LampShadows> lampShadows;
Sampler2DArray<float> lampShadowMaps;       // each point light's cube: six layers a light, a face each

// Each entity's own pictures: left unwired on the pipeline's node, so they are fields of its component, naming files
// (`@textures/wall.png`) — or `$white` for one it has not, which leaves what it multiplies as it is. Their sampler
// is the node's, the same for all.
Sampler2D<float4> albedoMap;                // its colour; a: how opaque
Sampler2D<float4> normalMap;                // which way its surface faces, texel by texel, relative to the mesh's own
Sampler2D<float4> roughnessMap;             // g: how rough; b: how much a metal
Sampler2D<float4> emissiveMap;

struct Vertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : UV;
};

struct RasterInfo {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
    float2 uv;
};

// `model` is the engine's to give. The rest is the entity's own: the fields of the component this pipeline's node
// makes, which is what makes an entity drawn.
struct DrawData {
    float4x4 model;
    float3 albedo;      // its colour, as picked
    float metallic;
    float3 emission;    // the light it gives off itself, as light: above 1 it blooms
    float roughness;
    float alphaCutoff;  // above 0: where its albedoMap is less opaque than this, nothing is drawn
    float transparency; // how much of what is behind it shows through, over what its albedoMap lets: 0 none, 1 all of it — glass
};
[[vk::push_constant]] ConstantBuffer<DrawData> draw;

struct Output {
    float4 color : SV_Target0;
    float4 position : SV_Target1;       // where the surface is in the world; w is 1 where there is one
    float4 normal : SV_Target2;         // which way it faces
    float4 specular : SV_Target3;       // rgb: what it mirrors straight on; a: how rough it is
};

[shader("vertex")]
RasterInfo vertex(Vertex vertex)
{
    float4x4 model = draw.model;
    RasterInfo raster;
    raster.worldPosition = mul(float4(vertex.position, 1.0), model).xyz;
    raster.position = toClip(raster.worldPosition);
    // The normal through the cofactors of the model's 3x3: square to the surface however unevenly it is scaled.
    float3 a0 = model[0].xyz, a1 = model[1].xyz, a2 = model[2].xyz;
    float3 normal = vertex.normal.x * cross(a1, a2) + vertex.normal.y * cross(a2, a0) + vertex.normal.z * cross(a0, a1);
    raster.normal = normal * sign(dot(a0, cross(a1, a2)));
    raster.uv = vertex.uv;
    return raster;
}

static const float Pi = 3.14159265359;

struct Surface {
    float3 normal;
    float3 toEye;
    float facingEye;
    float alpha;            // roughness, squared
    float3 straightOn;      // what it mirrors straight on
    float3 scattered;       // what it scatters: none, for a metal
    float metallic;
};

float3 fresnel(float facing, float3 straightOn)
{
    return straightOn + (1.0 - straightOn) * pow(saturate(1.0 - facing), 5.0);
}

/** How much of a surface's tiny facets face the halfway direction: Trowbridge-Reitz (GGX). */
float facets(float facingHalfway, float alpha)
{
    float a2 = alpha * alpha;
    float f = (facingHalfway * a2 - facingHalfway) * facingHalfway + 1.0;
    return a2 / (Pi * f * f);
}

/** How much of them is hidden neither from the light nor from the eye: Smith's, height-correlated (Heitz 2014). */
float visible(float facingEye, float facingLight, float alpha)
{
    float a2 = alpha * alpha;
    float v = facingLight * sqrt(facingEye * facingEye * (1.0 - a2) + a2);
    float l = facingEye * sqrt(facingLight * facingLight * (1.0 - a2) + a2);
    return v + l > 0.0 ? 0.5 / (v + l) : 0.0;
}

/** What a surface sends to the eye of light arriving along [toLight]. */
float3 directLighting(Surface surface, float3 toLight, float3 radiance)
{
    float facingLight = max(dot(surface.normal, toLight), 0.0);
    if (facingLight == 0.0) return 0.0;
    float3 halfway = normalize(toLight + surface.toEye);
    float3 mirrored = fresnel(max(dot(surface.toEye, halfway), 0.0), surface.straightOn);
    float3 highlight = facets(max(dot(surface.normal, halfway), 0.0), surface.alpha) * visible(surface.facingEye, facingLight, surface.alpha) * mirrored;
    float3 matte = (1.0 - mirrored) * (1.0 - surface.metallic) * surface.scattered / Pi;
    return (matte + highlight) * facingLight * radiance;
}

/** How much of the sun reaches a point through one cascade: 1 lit, 0 in shadow, and 1 outside the cascade. A 5x5 filter. */
float sampleCascade(int cascade, float3 world, float3 normal, float facingLight)
{
    // Moved off the surface by a pixel or so of this cascade's map, more at a glancing angle: nothing shadows itself.
    float worldTexel = sun.worldTexel[cascade];
    float3 point = world + normal * worldTexel * (2.0 + 4.0 * saturate(1.0 - facingLight));
    float4 lit = mul(float4(point, 1.0), sun.lightViewProj[cascade]);
    float3 seen = lit.xyz / lit.w;
    float2 uv = seen.xy * 0.5 + 0.5;
    if (seen.z > 1.0 || any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    // The filter is as big in the world in every cascade, so that a shadow's edge is not wider past each split.
    float radius = max(sun.pcfRadius * sun.worldTexel[0] / worldTexel, 1.0);
    float2 tile = float2(float(cascade & 1), float(cascade >> 1)) * 0.5;
    float reached = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            float2 at = saturate(uv + float2(x, y) * sun.tileTexel * radius);
            reached += seen.z - sun.bias > shadowMap.SampleLevel(tile + at * 0.5, 0.0) ? 0.0 : 1.0;
        }
    return reached / 25.0;
}

/**
 * How much of point light @p index reaches a point: looked up in the face of the light's cube that looks the point's
 * way. A light past those that have a cube casts no shadow.
 */
float lampShadow(uint index, float3 lightPosition, float3 world, float3 normal, float facingLight)
{
    if (float(index) >= lampShadows.count) return 1.0;
    // Moved off the surface by a pixel or so of the map there, more at a glancing angle: nothing shadows itself.
    float away = length(world - lightPosition);
    float3 point = world + normal * away * 2.0 * lampShadows.texel * (1.5 + 3.0 * saturate(1.0 - facingLight));
    float3 from = point - lightPosition;
    float3 size = abs(from);
    uint face = size.x >= size.y && size.x >= size.z ? (from.x > 0.0 ? 0 : 1) : size.y >= size.z ? (from.y > 0.0 ? 2 : 3) : (from.z > 0.0 ? 4 : 5);
    uint layer = index * 6 + face;
    float4 lit = mul(float4(point, 1.0), lampShadows.faces[layer]);
    if (lit.w <= 0.0) return 1.0;
    float3 seen = lit.xyz / lit.w;
    float2 uv = seen.xy * 0.5 + 0.5;
    float reached = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float2 at = saturate(uv + float2(x, y) * lampShadows.texel * lampShadows.softness);
            reached += seen.z - lampShadows.bias > lampShadowMaps.SampleLevel(float3(at, float(layer)), 0.0) ? 0.0 : 1.0;
        }
    return reached / 9.0;
}

/** How much of the sun reaches a point: from the sharpest cascade that covers it, faded into the next before its end. */
float sunShadow(float3 world, float3 normal, float facingLight, float depth)
{
    int cascade = int(CascadeCount) - 1;
    for (int i = 0; i < int(CascadeCount); ++i)
        if (depth < sun.splits[i]) { cascade = i; break; }
    float shadow = sampleCascade(cascade, world, normal, facingLight);
    if (cascade < int(CascadeCount) - 1) {
        float start = cascade == 0 ? 0.0 : sun.splits[cascade - 1];
        float end = sun.splits[cascade];
        float band = (end - start) * sun.blend;
        float t = band > 0.0 ? (end - depth) / band : 1.0;
        if (t < 1.0) shadow = lerp(sampleCascade(cascade + 1, world, normal, facingLight), shadow, smoothstep(0.0, 1.0, t));
    }
    return shadow;
}

/**
 * The mesh's normal turned as a normal map's texel says. The mesh has no tangents: which way the picture's u and v
 * run over the surface is worked out from how the position and the texture coordinate change across the pixel.
 */
float3 mapped(float3 normal, float3 world, float2 uv, float3 texel)
{
    float3 dp1 = ddx(world), dp2 = ddy(world);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, normal), dp1perp = cross(normal, dp1);
    float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
    float longest = max(dot(tangent, tangent), dot(bitangent, bitangent));
    if (longest < 1e-20) return normal;
    float scale = rsqrt(longest);
    return normalize(tangent * scale * texel.x + bitangent * scale * texel.y + normal * texel.z);
}

[shader("fragment")]
Output fragment(RasterInfo raster)
{
    float4 base = albedoMap.Sample(raster.uv);
    if (draw.alphaCutoff > 0.0 && base.a < draw.alphaCutoff) discard;
    float4 material = roughnessMap.Sample(raster.uv);
    float3 albedo = pow(max(draw.albedo, 0.0), 2.2) * pow(base.rgb, 2.2);
    float roughness = saturate(draw.roughness * material.g);
    float3 world = raster.worldPosition;

    Surface surface;
    surface.normal = normalize(raster.normal);
    surface.toEye = normalize(camera.position - world);
    if (dot(surface.normal, surface.toEye) < 0.0) surface.normal = -surface.normal;    // seen from behind, it faces the eye
    float3 texel = normalMap.Sample(raster.uv).xyz;
    if (any(texel < 0.99)) surface.normal = mapped(surface.normal, world, raster.uv, normalize(texel * 2.0 - 1.0));   // (white: it has none)
    surface.metallic = saturate(draw.metallic * material.b);
    surface.alpha = max(roughness * roughness, 0.0001);
    // Wider where the normal turns fast across a pixel, or a small highlight sparkles (Tokuyoshi and Kaplanyan).
    float3 du = ddx(surface.normal), dv = ddy(surface.normal);
    surface.alpha = clamp(surface.alpha + min(0.5 * (dot(du, du) + dot(dv, dv)), 0.18), 0.0001, 1.0);
    surface.facingEye = max(dot(surface.normal, surface.toEye), 1e-4);
    surface.straightOn = lerp(float3(0.04, 0.04, 0.04), albedo, surface.metallic);
    surface.scattered = albedo * (1.0 - surface.metallic);

    // The pixel's cluster: where it is in the picture, and how deep — each slice as many times deeper as the last.
    float depth = viewDepth(world);
    float slice = log(max(depth, camera.zNear) / camera.zNear) / log(camera.zFar / camera.zNear) * float(camera.gridSize.z);
    uint3 cluster = min(uint3(uint2(raster.position.xy / camera.screenSize * float2(camera.gridSize.xy)), uint(max(slice, 0.0))), camera.gridSize - 1);
    uint2 run = lightGrid[cluster.x + cluster.y * camera.gridSize.x + cluster.z * camera.gridSize.x * camera.gridSize.y];

    float3 lit = 0.0;
    for (uint i = run.x; i < run.x + run.y; ++i) {
        uint index = lightIndices[i];
        PointLight light = lights[index];
        float3 offset = light.position - world;
        float distance = length(offset);
        if (distance >= light.radius) continue;
        float x = distance / light.radius;
        float falloff = (1.0 - x * x) * (1.0 - x * x);
        float3 toLight = offset / max(distance, 1e-4);
        float facing = dot(surface.normal, toLight);
        if (facing <= 0.0) continue;
        lit += directLighting(surface, toLight, pow(max(light.color, 0.0), 2.2) * light.intensity * falloff) * lampShadow(index, light.position, world, surface.normal, facing);
    }

    if (sun.present > 0.0) {
        float3 toSun = -sun.direction;
        float facing = dot(surface.normal, toSun);
        if (facing > 0.0) lit += directLighting(surface, toSun, sun.color * sunShadow(world, surface.normal, facing, depth));
    }

    // The sky's light from all round, scattered; and what of the sky it mirrors, the less the rougher it is.
    lit += skyLight(sun, surface.normal) * surface.scattered;
    lit += skyLight(sun, reflect(-surface.toEye, surface.normal)) * fresnel(surface.facingEye, surface.straightOn) * (1.0 - roughness);
    lit += draw.emission * pow(emissiveMap.Sample(raster.uv).rgb, 2.2);

    Output output;
    // How opaque it is: what the blended pipeline draws it over the picture by. What it mirrors is on it however
    // clear it is: a pane seen along itself shows more of the sky than of what is behind it.
    float clear = saturate(draw.transparency);
    float opaque = base.a * (1.0 - clear);
    if (clear > 0.0) opaque = saturate(opaque + (1.0 - opaque) * dot(fresnel(surface.facingEye, surface.straightOn), float3(0.2126, 0.7152, 0.0722)) * (1.0 - roughness));
    output.color = float4(lit, opaque);
    output.position = float4(world, 1.0);
    output.normal = float4(surface.normal, 1.0);
    output.specular = float4(surface.straightOn, sqrt(surface.alpha));
    return output;
}
