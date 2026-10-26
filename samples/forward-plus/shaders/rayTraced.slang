// The scene ray traced: a ray a pixel through the RayScene the forward pass's instances make, shaded where it hits by
// the same materials as the forward pass — lit by the sun, and by whatever gives off light (an emissive material: no
// point light is), each through a shadow ray;
// shaded all round by the sky, as much as rays a short way out do not find something (ambient occlusion, ray traced);
// and mirroring, if polished, what one more ray finds. The sky where nothing is hit. Into a picture as light, and where
// each pixel's surface is, for the antialiasing after it.
//
// Glass and decals (the tag Blended) are a scene of their own: each layer of it between the eye and the solid surface
// is shaded too, and laid over what is behind by how opaque it is, as the blended raster pipeline lays it.
//
// The hit shader only says what was hit; the ray generation shader shades it. A cut-out (a picture's alpha below its
// cutoff) is left out by the any-hit shader, for the instances the RayScene says are masked.

import forwardCommon;

ConstantBuffer<Sun> sun;
// What gives off light: emitters.slang's list, of the frame before.
StructuredBuffer<Emitter> emitters;
StructuredBuffer<uint> emitterCount;

[entity("PER_INSTANCE")] StructuredBuffer<RayMaterial> instances;
StructuredBuffer<uint> meshOf;              // the RayScene's: which mesh of the pack each instance is
StructuredBuffer<uint4> scene;              // the RayScene's: x, its structure's bindless index
struct MeshRange {
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint unused;
    float4 boundsMin;
    float4 boundsMax;
};
StructuredBuffer<MeshRange> ranges;         // the pack's
StructuredBuffer<uint> meshIndices;         // the pack's indices
ByteAddressBuffer meshPositions;            // a position 12 bytes
ByteAddressBuffer meshNormals;              // a normal 12 bytes
ByteAddressBuffer meshUVs;                  // a UV 8 bytes
// The same of the glass: its own Draw's instances, RayScene and pack.
StructuredBuffer<Instance> glassInstances;
StructuredBuffer<uint> glassMeshOf;
StructuredBuffer<uint4> glassScene;
StructuredBuffer<MeshRange> glassRanges;
StructuredBuffer<uint> glassIndices;
ByteAddressBuffer glassPositions;
ByteAddressBuffer glassNormals;
ByteAddressBuffer glassUVs;
SamplerState textureSampler;
StructuredBuffer<FrameState> frameState;

[[vk::image_format("rgba16f")]] RWTexture2D<float4> picture;
[[vk::image_format("rgba32f")]] RWTexture2D<float4> positions;

[[vk::push_constant]] cbuffer Tracing {
    float occlusionRadius;      // how far an occlusion ray looks
    uint occlusionRays;         // how many, each pixel each frame
    float reflectionRoughness;  // rougher than this mirrors nothing
    float shadowsFrom;          // how far off a surface its shadow rays start
    uint glassLayers;           // how many layers of glass a ray is seen through, at most
    uint emitterCandidates;     // how many emitters, picked at random, each shadow ray to one is chosen among
    uint emitterSamples;        // how many shadow rays to what gives off light, a point
};

static const float Pi = 3.14159265359;

static const uint Solid = 0, Glass = 1;     // which scene, and pack, a hit is of

struct Hit {
    float distance;     // < 0: nothing
    uint pack;          // Solid or Glass
    uint instance;
    uint triangle;
    float2 barycentrics;
};

struct Shadow {
    uint reached;       // 1 when nothing was in the way
};

// ---- what was hit ---------------------------------------------------------------------------------------------------

[shader("closesthit")]
void hit(inout Hit payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.distance = RayTCurrent();
    payload.instance = InstanceID();
    payload.triangle = PrimitiveIndex();
    payload.barycentrics = attributes.barycentrics;
}

[shader("miss")]
void missed(inout Hit payload)
{
    payload.distance = -1.0;
}

[shader("miss")]
void shadowMissed(inout Shadow payload)
{
    payload.reached = 1;
}

// What a pack has, of the solid scene's or the glass's: the glass is shaded by the blended pipeline's material.
Instance instanceOf(uint pack, uint instance)
{
    if (pack == Glass) return glassInstances[instance];
    const RayMaterial own = instances[instance];
    Instance made;
    made.model = own.model;
    made.albedo = own.albedo;
    made.metallic = own.metallic;
    made.emission = own.emission;
    made.roughness = own.roughness;
    made.alphaCutoff = own.alphaCutoff;
    made.transparency = 0.0;
    made.albedoMap = own.albedoMap;
    made.normalMap = own.normalMap;
    made.roughnessMap = own.roughnessMap;
    made.emissiveMap = own.emissiveMap;
    made.translucency = own.translucency;
    return made;
}
float3 positionOf(uint pack, uint vertex) { return asfloat(pack == Glass ? glassPositions.Load3(vertex * 12) : meshPositions.Load3(vertex * 12)); }
float3 normalOf(uint pack, uint vertex) { return asfloat(pack == Glass ? glassNormals.Load3(vertex * 12) : meshNormals.Load3(vertex * 12)); }
float2 uvOf(uint pack, uint vertex) { return asfloat(pack == Glass ? glassUVs.Load2(vertex * 8) : meshUVs.Load2(vertex * 8)); }

/** A triangle's corners, as the pack's vertices: which vertex each is. */
uint3 cornersOf(uint pack, uint instance, uint triangle)
{
    const MeshRange range = pack == Glass ? glassRanges[glassMeshOf[instance]] : ranges[meshOf[instance]];
    const uint at = range.firstIndex + triangle * 3;
    const uint3 corners = pack == Glass ? uint3(glassIndices[at], glassIndices[at + 1], glassIndices[at + 2]) : uint3(meshIndices[at], meshIndices[at + 1], meshIndices[at + 2]);
    return corners + uint(range.vertexOffset);
}

float2 uvAt(uint pack, uint3 corners, float2 barycentrics)
{
    const float2 a = uvOf(pack, corners.x), b = uvOf(pack, corners.y), c = uvOf(pack, corners.z);
    return a * (1.0 - barycentrics.x - barycentrics.y) + b * barycentrics.x + c * barycentrics.y;
}

/** A cut-out's clear part is not hit: what its picture is less opaque than its cutoff. Only the solid scene has any. */
[shader("anyhit")]
void cutOut(inout Hit payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const RayMaterial draw = instances[InstanceID()];
    if (draw.alphaCutoff <= 0.0 || draw.albedoMap == ~0u) return;
    const float2 uv = uvAt(Solid, cornersOf(Solid, InstanceID(), PrimitiveIndex()), attributes.barycentrics);
    if (kor::texture2D(draw.albedoMap).SampleLevel(textureSampler, uv, 0.0).a < draw.alphaCutoff) IgnoreHit();
}

// ---- shading, as the forward pass shades -----------------------------------------------------------------------------

struct Surface {
    float3 normal;
    float3 toEye;
    float facingEye;
    float alpha;
    float3 straightOn;
    float3 scattered;
    float metallic;
    float roughness;
    float3 emitted;
    float opaque;       // how much of what is behind it does not show through: 1 for all but glass and decals
    float blur;         // how much more its reflection rays spread than its roughness says
    float through;      // how much of the light on its back shows through: its translucency
};

float3 fresnel(float facing, float3 straightOn) { return straightOn + (1.0 - straightOn) * pow(saturate(1.0 - facing), 5.0); }

float facets(float facingHalfway, float alpha)
{
    float a2 = alpha * alpha;
    float f = (facingHalfway * a2 - facingHalfway) * facingHalfway + 1.0;
    return a2 / (Pi * f * f);
}

float visible(float facingEye, float facingLight, float alpha)
{
    float a2 = alpha * alpha;
    float v = facingLight * sqrt(facingEye * facingEye * (1.0 - a2) + a2);
    float l = facingEye * sqrt(facingLight * facingLight * (1.0 - a2) + a2);
    return v + l > 0.0 ? 0.5 / (v + l) : 0.0;
}

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

RaytracingAccelerationStructure world() { return kor::accelerationStructure(scene[0].x); }

/** The nearest solid surface along a way, up to @p far. */
Hit trace(float3 from, float3 along, float far)
{
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = along;
    ray.TMin = 0.0;
    ray.TMax = far;
    Hit payload;
    payload.distance = -1.0;
    TraceRay(world(), RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    payload.pack = Solid;
    return payload;
}

/** The nearest glass along a way, from @p near to @p far: none when there is no glass. */
Hit traceGlass(float3 from, float3 along, float near, float far)
{
    Hit payload;
    payload.distance = -1.0;
    payload.pack = Glass;
    if (glassScene[0].x == ~0u || near >= far) return payload;
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = along;
    ray.TMin = near;
    ray.TMax = far;
    TraceRay(kor::accelerationStructure(glassScene[0].x), RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    payload.pack = Glass;
    return payload;
}

/** Whether nothing is between a point and @p far along a way: no closest hit is looked for, the first one ends it. */
bool clear(float3 from, float3 along, float far)
{
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = along;
    ray.TMin = 0.0;
    ray.TMax = far;
    Shadow payload;
    payload.reached = 0;
    // Mask 1: what casts a shadow — not a lamp's own ball (the RayScene's noShadow).
    TraceRay(world(), RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0x01, 0, 0, 1, ray, payload);
    return payload.reached != 0;
}

/** A number from 0 to 1, another for each pixel, frame and use: what a ray is sent by. */
float random(uint2 pixel, uint frame, uint use)
{
    uint h = pixel.x * 1973u + pixel.y * 9277u + frame * 26699u + use * 7919u;
    h = (h ^ 61u) ^ (h >> 16);
    h *= 9u;
    h ^= h >> 4;
    h *= 0x27d4eb2du;
    h ^= h >> 15;
    return float(h) / 4294967296.0;
}

/** A way round @p normal, as often as light from it falls on a surface (cosine weighted). */
float3 around(float3 normal, float u, float v)
{
    const float r = sqrt(u), phi = 2.0 * Pi * v;
    const float3 side = abs(normal.y) < 0.99 ? normalize(cross(normal, float3(0.0, 1.0, 0.0))) : float3(1.0, 0.0, 0.0);
    const float3 up = cross(normal, side);
    return normalize(side * (r * cos(phi)) + up * (r * sin(phi)) + normal * sqrt(max(0.0, 1.0 - u)));
}

/** A normal of a mesh into the world, through the cofactors of the model's 3x3: square to the surface however it is scaled. */
float3 turned(float4x4 model, float3 n)
{
    const float3 a0 = model[0].xyz, a1 = model[1].xyz, a2 = model[2].xyz;
    return normalize((n.x * cross(a1, a2) + n.y * cross(a2, a0) + n.z * cross(a0, a1)) * sign(dot(a0, cross(a1, a2))));
}

/** Where a ray from @p eye along @p way meets the plane of the triangle @p p0 @p p1 @p p2, as its UVs there; @p otherwise when it runs along the plane. */
float2 uvWhere(float3 eye, float3 way, float3 p0, float3 p1, float3 p2, float2 t0, float2 t1, float2 t2, float2 otherwise)
{
    const float3 face = cross(p1 - p0, p2 - p0);
    const float facing = dot(way, face);
    if (abs(facing) < 1e-12) return otherwise;
    const float3 at = eye + way * (dot(p0 - eye, face) / facing);
    const float area = dot(face, face);
    const float b1 = dot(cross(at - p0, p2 - p0), face) / area, b2 = dot(cross(p1 - p0, at - p0), face) / area;
    return t0 * (1.0 - b1 - b2) + t1 * b1 + t2 * b2;
}

/** The way through a point of the picture, -1 to 1 each way. */
float3 through(float2 at)
{
    const float4 seen = mul(float4(at, 1.0, 1.0), camera.inverseProjection);
    return normalize(mul(float4(normalize(seen.xyz / seen.w), 0.0), camera.inverseView).xyz);
}

/** One of an instance's pictures at @p uv, filtered over the footprint the gradients say: as a raster pass's is. */
float4 pictureAt(uint index, float2 uv, float2 dx, float2 dy)
{
    return index == ~0u ? float4(1.0, 1.0, 1.0, 1.0) : kor::texture2D(index).SampleGrad(textureSampler, uv, dx, dy);
}

/**
 * What the surface @p found is made of, where the ray hit it from @p eye along @p along: its pictures read over the
 * footprint of the pixel there, as a raster pass reads them — so far off, and at a slant, as sharp. With
 * @p differentials, @p acrossX and @p acrossY are the rays through the next pixel across and down, met with the
 * triangle's plane: their UVs, less this one's, are the gradients, and the sampler's anisotropy does the rest. Without,
 * the ray is a cone, @p spread wider for each unit it goes, its footprint in the picture by how many texels the triangle
 * has to its area and how much at a slant it is seen.
 */
Surface surfaceOf(Hit found, float3 eye, float3 along, float spread, out float3 point, out float3 flat, bool differentials = false, float3 acrossX = 0.0, float3 acrossY = 0.0)
{
    const Instance draw = instanceOf(found.pack, found.instance);
    const uint3 corners = cornersOf(found.pack, found.instance, found.triangle);
    const float3 weights = float3(1.0 - found.barycentrics.x - found.barycentrics.y, found.barycentrics);
    const float2 uv = uvAt(found.pack, corners, found.barycentrics);
    point = eye + along * found.distance;
    const float4x4 model = draw.model;
    const float3 p0 = mul(float4(positionOf(found.pack, corners.x), 1.0), model).xyz;
    const float3 p1 = mul(float4(positionOf(found.pack, corners.y), 1.0), model).xyz;
    const float3 p2 = mul(float4(positionOf(found.pack, corners.z), 1.0), model).xyz;
    const float2 t0 = uvOf(found.pack, corners.x), t1 = uvOf(found.pack, corners.y), t2 = uvOf(found.pack, corners.z);
    const float3 face = cross(p1 - p0, p2 - p0);
    float2 dx, dy;
    if (differentials) {
        dx = uvWhere(eye, acrossX, p0, p1, p2, t0, t1, t2, uv) - uv;
        dy = uvWhere(eye, acrossY, p0, p1, p2, t0, t1, t2, uv) - uv;
    } else {
        // The cone's width where it hits, through how many UVs a unit of the triangle is, widened by the slant.
        const float worldArea = length(face), uvArea = abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));
        const float slant = max(abs(dot(normalize(face), along)), 0.05);
        const float width = found.distance * spread * sqrt(uvArea / max(worldArea, 1e-12)) / slant;
        dx = float2(width, 0.0);
        dy = float2(0.0, width);
    }
    const float3 n0 = normalOf(found.pack, corners.x), n1 = normalOf(found.pack, corners.y), n2 = normalOf(found.pack, corners.z);
    float3 normal = turned(model, n0 * weights.x + n1 * weights.y + n2 * weights.z);
    flat = normal;
    if (dot(normal, -along) < 0.0) { normal = -normal; flat = -flat; }    // seen from behind, it faces the eye

    const float4 base = pictureAt(draw.albedoMap, uv, dx, dy);
    const float4 material = pictureAt(draw.roughnessMap, uv, dx, dy);
    const float3 albedo = pow(max(draw.albedo, 0.0), 2.2) * pow(base.rgb, 2.2);

    // A normal map, which way its u and v run over the triangle worked out from the triangle's corners: where they are in
    // the world, and in the picture.
    if (draw.normalMap != ~0u) {
        const float3 texel = pictureAt(draw.normalMap, uv, dx, dy).xyz;
        if (any(texel < 0.99)) {
            const float3 e1 = p1 - p0, e2 = p2 - p0;
            const float2 d1 = t1 - t0, d2 = t2 - t0;
            const float det = d1.x * d2.y - d1.y * d2.x;
            if (abs(det) > 1e-12) {
                const float3 u = (e1 * d2.y - e2 * d1.y) / det, v = (e2 * d1.x - e1 * d2.x) / det;
                const float3 tangent = normalize(u - normal * dot(normal, u));
                const float3 bitangent = cross(normal, tangent) * (dot(cross(normal, tangent), v) < 0.0 ? -1.0 : 1.0);
                const float3 n = unpackedNormal(texel);
                normal = normalize(tangent * n.x + bitangent * n.y + normal * n.z);
            }
        }
    }

    Surface surface;
    surface.normal = normal;
    surface.toEye = -along;
    surface.facingEye = max(dot(normal, -along), 1e-4);
    surface.metallic = saturate(draw.metallic * material.b);
    surface.roughness = saturate(draw.roughness * material.g);
    surface.alpha = max(surface.roughness * surface.roughness, 0.0001);
    surface.straightOn = lerp(float3(0.04, 0.04, 0.04), albedo, surface.metallic);
    surface.scattered = albedo * (1.0 - surface.metallic);
    const float3 emissive = pictureAt(draw.emissiveMap, uv, dx, dy).rgb;
    surface.emitted = draw.emission * pow(emissive, 2.2);
    surface.blur = found.pack == Glass ? 0.0 : saturate(instances[found.instance].reflectionBlur);
    surface.through = saturate(draw.translucency);
    // How opaque, as the forward pass works it out: what of the picture behind does not show through, and more of it
    // mirrored the more along it a pane is seen.
    const float seeThrough = saturate(draw.transparency);
    surface.opaque = base.a * (1.0 - seeThrough);
    if (seeThrough > 0.0) surface.opaque = saturate(surface.opaque + (1.0 - surface.opaque) * dot(fresnel(surface.facingEye, surface.straightOn), float3(0.2126, 0.7152, 0.0722)) * (1.0 - surface.roughness));
    return surface;
}

/**
 * Light falling on a thin surface's back, let through to its front by how translucent it is and scattered there
 * evenly, tinted by its colour: what a curtain with the sun behind it glows with.
 */
float3 throughFrom(Surface surface, float facingBack, float3 radiance)
{
    return surface.scattered / Pi * surface.through * facingBack * radiance;
}

/** The sun on a point, through a shadow ray — from its back, for one that lets light through. */
float3 sunOn(Surface surface, float3 point, float3 flat)
{
    if (sun.present <= 0.0) return 0.0;
    const float3 toSun = -sun.direction;
    const float facing = dot(surface.normal, toSun);
    if (facing > 0.0) return clear(point + flat * shadowsFrom, toSun, 1e4) ? directLighting(surface, toSun, sun.color) : 0.0;
    if (surface.through <= 0.0 || !clear(point - flat * shadowsFrom, toSun, 1e4)) return 0.0;
    return throughFrom(surface, -facing, sun.color);
}

/**
 * What gives off light — what emitters.slang listed, each an instance whose material's `emission` is more than nothing
 * — on a point, by `emitterSamples` rays. Each is to one of a few emitters picked at random (`emitterCandidates`), that
 * one picked again among them by how much light it would bring from where it is (resampled importance sampling): a ray
 * a way into the cone its sphere fills, at random, and what the emitter gives off where the ray finds it — nothing,
 * where something else is in the way (its own lamp's frame, say). It does not light itself (@p self). The antialiasing,
 * gathering frames, takes the noise out.
 */
float3 emittersOn(Surface surface, float3 point, float3 flat, uint2 pixel, uint use, uint self)
{
    uint listed, instancesLength, stride;
    emitters.GetDimensions(listed, stride);
    instances.GetDimensions(instancesLength, stride);
    const uint count = min(emitterCount[0], listed);
    if (count == 0 || emitterCandidates == 0) return 0.0;
    const uint frame = frameState[0].frame;
    const uint samples = max(emitterSamples, 1u);
    float3 lit = 0.0;
    for (uint sample = 0; sample < samples; ++sample) {
        const uint salt = use + sample * 97;
        // A few at random; of them, one by how much it would bring here: as bright as it is, the nearer the more.
        float total = 0.0, weight = 0.0;
        uint chosen = ~0u;
        for (uint candidate = 0; candidate < emitterCandidates; ++candidate) {
            const uint at = min(uint(random(pixel, frame, salt + candidate * 2) * float(count)), count - 1);
            const Emitter emitter = emitters[at];
            if (emitter.instance == self) continue;
            const float3 to = emitter.center - point;
            const float near = max(dot(to, to), emitter.radius * emitter.radius), reach = emitter.radius * rsqrt(near);
            // As much as its sphere is before the surface (or behind one that lets light through), as bright, the nearer the more.
            const float before = dot(to, surface.normal) * rsqrt(near);
            const float faces = surface.through > 0.0 ? 1.0 : saturate(before + reach);
            const float brings = emitter.power / near * faces;
            total += brings;
            if (brings > 0.0 && random(pixel, frame, salt + candidate * 2 + 1) * total < brings) { chosen = at; weight = brings; }
        }
        if (chosen == ~0u) continue;
        const Emitter emitter = emitters[chosen];
        if (emitter.instance >= instancesLength) continue;
        // A way into the cone its sphere fills as seen from here, at random — every way round, from inside the sphere —
        // and what a ray that way finds: it, where nothing is before it, and what it gives off there.
        const float3 to = emitter.center - point;
        const float near = dot(to, to);
        const float cosWidest = near > emitter.radius * emitter.radius ? sqrt(max(1.0 - emitter.radius * emitter.radius / near, 0.0)) : -1.0;
        const float3 axis = near > 1e-8 ? to * rsqrt(near) : surface.normal;
        const float cosTilt = 1.0 - random(pixel, frame, salt + 61) * (1.0 - cosWidest), turn = 2.0 * Pi * random(pixel, frame, salt + 62);
        const float sinTilt = sqrt(max(1.0 - cosTilt * cosTilt, 0.0));
        const float3 side = abs(axis.y) < 0.99 ? normalize(cross(axis, float3(0.0, 1.0, 0.0))) : float3(1.0, 0.0, 0.0);
        const float3 way = normalize(axis * cosTilt + (side * cos(turn) + cross(axis, side) * sin(turn)) * sinTilt);
        const float facing = dot(surface.normal, way);
        if (facing <= 0.0 && surface.through <= 0.0) continue;
        const Hit found = trace(facing > 0.0 ? point + flat * shadowsFrom : point - flat * shadowsFrom, way, sqrt(near) + emitter.radius);
        if (found.distance < 0.0 || found.instance != emitter.instance) continue;
        const RayMaterial material = instances[emitter.instance];
        float3 given = max(material.emission, 0.0);
        if (material.emissiveMap != ~0u)
            given *= pow(kor::texture2D(material.emissiveMap).SampleLevel(textureSampler, uvAt(Solid, cornersOf(Solid, found.instance, found.triangle), found.barycentrics), 0.0).rgb, 2.2);
        // Weighed: the emitter as likely as it was picked among the few, the way as likely as one of all its cone's.
        const float picked = total / (float(emitterCandidates) * weight) * float(count);
        const float3 radiance = given * (2.0 * Pi * (1.0 - cosWidest)) * picked;
        lit += facing > 0.0 ? directLighting(surface, way, radiance) : throughFrom(surface, -facing, radiance);
    }
    return lit / float(samples);
}

/**
 * @p behind as seen through the glass before it: each layer from the eye up to @p far, the nearest first, shaded as
 * the forward pass shades it — the sun and what gives off light through shadow rays, the sky round it and mirrored in it — and
 * laid over what is behind it by how opaque it is. Up to `glassLayers` of them.
 */
float3 throughGlass(float3 eye, float3 along, float far, float spread, uint2 pixel, float2 size, float3 alongX, float3 alongY, float3 behind)
{
    float3 seen = 0.0;
    float showing = 1.0;        // how much of what is further shows through what was found so far
    float near = 0.0;
    for (uint layer = 0; layer < glassLayers && showing > 0.01; ++layer) {
        const Hit found = traceGlass(eye, along, near, far);
        if (found.distance < 0.0) break;
        float3 point, flat;
        const Surface surface = surfaceOf(found, eye, along, spread, point, flat, true, alongX, alongY);
        const float3 lifted = point + flat * shadowsFrom;
        float3 lit = surface.emitted + sunOn(surface, point, flat) + emittersOn(surface, point, flat, pixel, 300 + layer * 1000, ~0u);
        lit += skyLight(sun, surface.normal) * surface.scattered;
        lit += skyLight(sun, reflect(along, surface.normal)) * fresnel(surface.facingEye, surface.straightOn) * (1.0 - surface.roughness);
        seen += showing * surface.opaque * lit;
        showing *= 1.0 - surface.opaque;
        near = found.distance + 1e-4;
    }
    return seen + showing * behind;
}

[shader("raygeneration")]
void raygen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const float2 size = float2(DispatchRaysDimensions().xy);
    const FrameState state = frameState[0];
    // Through the pixel, moved by the frame's jitter as the raster passes move what they draw: the antialiasing gathers it.
    float2 across = (float2(pixel) + 0.5) / size * 2.0 - 1.0;
    if (state.begun == Begun) across -= state.jitter;
    const float3 along = through(across);
    // And through the next pixel across and the next down: what the pictures where it hits are read over.
    const float3 alongX = through(across + float2(2.0 / size.x, 0.0)), alongY = through(across + float2(0.0, 2.0 / size.y));
    const float3 eye = camera.position;
    // How much wider a ray is for each unit it goes: a pixel's angle.
    const float spread = 2.0 / (size.y * abs(camera.projection[1][1]));

    // No scene made yet (its entities not there, or still being read): nothing to trace through.
    if (scene[0].x == ~0u) {
        picture[pixel] = float4(skyColor(sun, along), 1.0);
        positions[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const Hit found = trace(eye, along, camera.zFar);
    if (found.distance < 0.0) {
        picture[pixel] = float4(throughGlass(eye, along, camera.zFar, spread, pixel, size, alongX, alongY, skyColor(sun, along)), 1.0);
        positions[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float3 point, flat;
    const Surface surface = surfaceOf(found, eye, along, spread, point, flat, true, alongX, alongY);
    positions[pixel] = float4(point, 1.0);
    const float3 lifted = point + flat * shadowsFrom;

    float3 lit = surface.emitted + sunOn(surface, point, flat) + emittersOn(surface, point, flat, pixel, 200, found.instance);

    // The sky all round, as much of it as short rays out from the surface do not find something in the way.
    float open = 1.0;
    if (occlusionRays > 0) {
        uint reached = 0;
        for (uint r = 0; r < occlusionRays; ++r)
            reached += clear(lifted, around(flat, random(pixel, state.frame, r * 2), random(pixel, state.frame, r * 2 + 1)), occlusionRadius) ? 1 : 0;
        open = float(reached) / float(occlusionRays);
    }
    lit += skyLight(sun, surface.normal) * surface.scattered * open;
    lit += skyLight(sun, -surface.normal) * surface.scattered * surface.through;

    // What it mirrors: one more ray, its way spread by how rough it is; what that finds lit by the sun and the sky.
    float3 mirrored = 0.0;
    if (surface.roughness < reflectionRoughness) {
        float3 way = reflect(along, surface.normal);
        way = normalize(lerp(way, around(way, random(pixel, state.frame, 101), random(pixel, state.frame, 102)), saturate(surface.alpha + surface.blur)));
        const Hit bounced = trace(lifted, way, 200.0);
        if (bounced.distance < 0.0) mirrored = skyColor(sun, way);
        else {
            float3 there, thereFlat;
            const Surface other = surfaceOf(bounced, lifted, way, spread, there, thereFlat);
            mirrored = other.emitted + sunOn(other, there, thereFlat) + skyLight(sun, other.normal) * other.scattered;
        }
    } else {
        mirrored = skyLight(sun, reflect(along, surface.normal)) * open;
    }
    lit += mirrored * fresnel(surface.facingEye, surface.straightOn) * (1.0 - surface.roughness);
    picture[pixel] = float4(throughGlass(eye, along, found.distance, spread, pixel, size, alongX, alongY, lit), 1.0);
}
