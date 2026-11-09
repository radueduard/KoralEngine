// Forward shading: each pixel lit by the point lights of its cluster and by the sun through its cascaded shadows,
// as a rough or polished, metal or not surface. Into the picture as light, and beside it where the surface is, which
// way it faces and how it mirrors — what the ambient occlusion and the reflections after it read.
//
// The depth pre-pass has already laid the depth down, so this shades only what is seen.

import forwardCommon;
import koral_debug;     // what the editor's scene view shows of a material: its albedo, its normal after the map...

ConstantBuffer<Sun> sun;
StructuredBuffer<KoralPointLight> pointLights;     // the Lights function's `points`
StructuredBuffer<KoralSpotLight> spotLights;       // and `spots`: a cluster lists one as the point lights' count and its place
StructuredBuffer<uint2> lightGrid;          // for each cluster: where its lights start in the list, and how many
StructuredBuffer<uint> lightIndices;
Texture2D<float> shadowMap;                 // the four cascades, two by two
Texture2DArray<float> lampShadowMaps;       // each point light's cube: six layers a light, a face each
SamplerComparisonState lampCompare;
StructuredBuffer<float4x4> lampFaces;       // each face's matrix, six a light

// Every entity, in one indirect draw: each instance's matrix, and the fields of its component — the pictures by their
// bindless indices (Instance). Each picture names a file (`@textures/wall.png`), or is `$white` for one it has not,
// which leaves what it multiplies as it is. Their sampler is the node's, the same for all.
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;
SamplerState textureSampler;
// Which instances the occlusion cull kept, a command's from its first instance on — read when `culled` is 1. Without
// a cull (the blended pass) each command draws its instances as they are.
StructuredBuffer<uint> drawnInstances;
StructuredBuffer<FrameState> frameState;
[[vk::push_constant]] cbuffer Culling { uint culled; };

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
    nointerpolation uint instance : INSTANCE;
};

struct Output {
    float4 color : SV_Target0;
    float4 position : SV_Target1;       // where the surface is in the world; w is 1 where there is one
    float4 normal : SV_Target2;         // which way it faces
    float4 specular : SV_Target3;       // rgb: what it mirrors straight on; a: how rough it is
};

[shader("vertex")]
RasterInfo vertex(Vertex vertex, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    RasterInfo raster;
    // How many instances there are: culled, the list has room for each and no more (the buffer of them has room for
    // more); not, each command draws its own, all of them there.
    uint length, stride, listed;
    instances.GetDimensions(length, stride);
    drawnInstances.GetDimensions(listed, stride);
    if (culled != 0) length = min(length, listed);
    raster.instance = culled == 0 ? first + index : first + index < listed ? drawnInstances[first + index] : length;
    // One the cull did not write — a list not counted into yet — is nowhere.
    const bool known = raster.instance < length;
    raster.instance = min(raster.instance, max(length, 1) - 1);
    float4x4 model = instances[raster.instance].model;
    raster.worldPosition = mul(float4(vertex.position, 1.0), model).xyz;
    raster.position = known ? toPicture(raster.worldPosition, frameState[0]) : float4(0.0, 0.0, 0.0, 0.0);
    // The normal through the cofactors of the model's 3x3: square to the surface however unevenly it is scaled.
    float3 a0 = model[0].xyz, a1 = model[1].xyz, a2 = model[2].xyz;
    float3 normal = vertex.normal.x * cross(a1, a2) + vertex.normal.y * cross(a2, a0) + vertex.normal.z * cross(a0, a1);
    raster.normal = normal * sign(dot(a0, cross(a1, a2)));
    raster.uv = vertex.uv;
    return raster;
}

// ---- the same, drawn by meshlets: a task shader's group a task, a mesh shader's group a meshlet ---------------------
// The pack's vertices, an attribute a buffer, tightly packed (a MeshPack given `attributes`): a position and a normal
// 12 bytes, a UV 8. Each meshlet's vertices name the pack's, its mesh's vertexOffset in them.

struct Meshlet {
    uint vertexOffset, vertexCount, triangleOffset, triangleCount;
    float4 sphere;      // its middle and how far it reaches, in its mesh's own space
    float4 cone;        // not used: every surface is seen from both sides
};
StructuredBuffer<Meshlet> meshlets;
StructuredBuffer<uint> meshletVertices;
StructuredBuffer<uint> meshletTriangles;    // a triangle's corners among its meshlet's vertices, a byte each
StructuredBuffer<uint4> meshletTasks;       // meshletTasks.slang's: instance, first meshlet, how many
ByteAddressBuffer meshPositions;
ByteAddressBuffer meshNormals;
ByteAddressBuffer meshUVs;

struct MeshletPayload {
    uint instance;
    uint meshlets[32];
};
groupshared MeshletPayload meshletPayload;
groupshared uint meshletsSeen;

/** Whether a sphere, moved by @p model, is wholly outside one of the camera's planes. */
bool sphereOutOfSight(float3 middle, float reach, float4x4 model, float4x4 viewProjection)
{
    const float scale = max(length(model[0].xyz), max(length(model[1].xyz), length(model[2].xyz)));
    return KoralSphereOutOfSight(KoralTransformPoint(middle, model), reach * scale, viewProjection);
}

// ---- occlusion: what is behind what was drawn is not drawn ---------------------------------------------------------
// Two draws a frame. The early one draws what last frame's depth pyramid shows, as last frame's camera saw it, and notes
// which it drew; the pyramid is made again of what it drew (meshletTasks.slang's `reduce`), and the late one draws what
// the early one left out that this frame's pyramid shows. What came into sight this frame is found by the late one: no
// hole where something stops hiding it. The glass, drawn after both, is tested against this frame's pyramid.

Texture2D<float> depthPyramid;              // each texel the farthest depth of what it covers: 1024 by 512, then halves
RWStructuredBuffer<uint> meshletsDrawnEarly;    // a task's: which of its meshlets the early draw drew, a bit each
static const float2 PyramidSize = float2(1024.0, 512.0);
static const uint PyramidLevels = 10;

/**
 * Whether a sphere, moved by @p model and seen by @p viewProjection, is behind what the depth pyramid holds: its nearest
 * point farther than the farthest depth over the pixels it covers. One reaching behind the camera is not.
 */
bool sphereHidden(float3 middle, float reach, float4x4 model, float4x4 viewProjection, float2 pictureSize)
{
    const float3 at = mul(float4(middle, 1.0), model).xyz;
    const float r = reach * max(length(model[0].xyz), max(length(model[1].xyz), length(model[2].xyz)));
    float2 low = float2(1.0, 1.0), high = float2(0.0, 0.0);
    float nearest = 1.0;
    for (uint corner = 0; corner < 8; ++corner) {
        const float3 offset = float3(corner & 1 ? r : -r, corner & 2 ? r : -r, corner & 4 ? r : -r);
        const float4 clip = mul(float4(at + offset, 1.0), viewProjection);
        if (clip.w <= 1e-4) return false;
        const float3 seen = clip.xyz / clip.w;
        low = min(low, seen.xy * 0.5 + 0.5);
        high = max(high, seen.xy * 0.5 + 0.5);
        nearest = min(nearest, seen.z);
    }
    if (nearest <= 0.0) return false;
    // A pixel more each way: the depth was drawn jittered, by up to half of one.
    low = saturate(low - 1.0 / pictureSize);
    high = saturate(high + 1.0 / pictureSize);
    // The level where it covers at most a texel each way, so at most two of them: those four read.
    const float2 covered = (high - low) * PyramidSize;
    const uint level = min(PyramidLevels - 1, uint(ceil(log2(max(max(covered.x, covered.y), 1.0)))));
    const int2 size = int2(max(floor(PyramidSize / float(1u << level)), 1.0));
    const int2 first = min(int2(low * float2(size)), size - 1), last = min(int2(high * float2(size)), size - 1);
    float farthest = 0.0;
    for (int y = first.y; y <= last.y; ++y)
        for (int x = first.x; x <= last.x; ++x) farthest = max(farthest, depthPyramid.Load(int3(x, y, level)));
    return nearest > farthest;
}

static const uint NoOcclusion = 0, EarlyDraw = 1, LateDraw = 2, BehindDrawn = 3;
groupshared uint drawnEarly;

/** A task's meshlets in sight, handed to the mesh shader: each tested as @p occlusion says (see above). */
void taskOf(uint thread, uint group, uint occlusion)
{
    if (thread == 0) { meshletsSeen = 0; drawnEarly = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint tasksLength, drawnLength, stride;
    meshletTasks.GetDimensions(tasksLength, stride);
    meshletsDrawnEarly.GetDimensions(drawnLength, stride);
    const uint4 task = group < tasksLength ? meshletTasks[group] : uint4(0, 0, 0, 0);
    if (thread < task.z) {
        const Meshlet meshlet = meshlets[task.y + thread];
        const FrameState state = frameState[0];
        const float4x4 model = instances[task.x].model;
        const uint bit = 1u << thread;
        const bool drewEarly = occlusion == LateDraw && group < drawnLength && (meshletsDrawnEarly[group] & bit) != 0;
        bool drawn = !drewEarly && (state.begun != Begun || !sphereOutOfSight(meshlet.sphere.xyz, meshlet.sphere.w, model, state.viewProjection));
        if (drawn && state.begun == Begun) {
            // Early, by last frame's pyramid and camera — every one, when there was no last frame to have drawn it.
            if (occlusion == EarlyDraw) drawn = state.history == 0 || !sphereHidden(meshlet.sphere.xyz, meshlet.sphere.w, model, state.previousViewProjection, state.size);
            else if (occlusion != NoOcclusion) drawn = !sphereHidden(meshlet.sphere.xyz, meshlet.sphere.w, model, state.viewProjection, state.size);
        }
        if (drawn) {
            uint slot;
            InterlockedAdd(meshletsSeen, 1, slot);
            meshletPayload.meshlets[slot] = task.y + thread;
            if (occlusion == EarlyDraw) InterlockedOr(drawnEarly, bit);
        }
    }
    if (thread == 0) meshletPayload.instance = task.x;
    GroupMemoryBarrierWithGroupSync();
    if (occlusion == EarlyDraw && thread == 0 && group < drawnLength) meshletsDrawnEarly[group] = drawnEarly;
    DispatchMesh(meshletsSeen, 1, 1, meshletPayload);
}

/** Every meshlet in the camera's sight. */
[shader("amplification")]
[numthreads(32, 1, 1)]
void meshletTask(uint thread : SV_GroupThreadID, uint3 group : SV_GroupID) { taskOf(thread, group.x, NoOcclusion); }

/** Those last frame's depth shows, noted in meshletsDrawnEarly. */
[shader("amplification")]
[numthreads(32, 1, 1)]
void meshletTaskEarly(uint thread : SV_GroupThreadID, uint3 group : SV_GroupID) { taskOf(thread, group.x, EarlyDraw); }

/** Those the early draw did not draw that this frame's depth shows. */
[shader("amplification")]
[numthreads(32, 1, 1)]
void meshletTaskLate(uint thread : SV_GroupThreadID, uint3 group : SV_GroupID) { taskOf(thread, group.x, LateDraw); }

/** Those this frame's depth shows, without a draw before them to leave out: the glass. */
[shader("amplification")]
[numthreads(32, 1, 1)]
void meshletTaskBehind(uint thread : SV_GroupThreadID, uint3 group : SV_GroupID) { taskOf(thread, group.x, BehindDrawn); }

[shader("mesh")]
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void meshletMesh(uint thread : SV_GroupThreadID, uint3 group : SV_GroupID, in payload MeshletPayload task,
                 out vertices RasterInfo corners[64], out indices uint3 triangles[124])
{
    const Meshlet meshlet = meshlets[task.meshlets[group.x]];
    const float4x4 model = instances[task.instance].model;
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);
    if (thread < meshlet.vertexCount) {
        const uint v = meshletVertices[meshlet.vertexOffset + thread];
        const float3 position = asfloat(meshPositions.Load3(v * 12));
        const float3 normal = asfloat(meshNormals.Load3(v * 12));
        RasterInfo raster;
        raster.worldPosition = mul(float4(position, 1.0), model).xyz;
        raster.position = toPicture(raster.worldPosition, frameState[0]);
        float3 a0 = model[0].xyz, a1 = model[1].xyz, a2 = model[2].xyz;
        raster.normal = (normal.x * cross(a1, a2) + normal.y * cross(a2, a0) + normal.z * cross(a0, a1)) * sign(dot(a0, cross(a1, a2)));
        raster.uv = asfloat(meshUVs.Load2(v * 8));
        raster.instance = task.instance;
        corners[thread] = raster;
    }
    for (uint t = thread; t < meshlet.triangleCount; t += 64) {
        const uint packed = meshletTriangles[meshlet.triangleOffset + t];
        triangles[t] = uint3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    }
}

struct Surface {
    float3 normal;
    float3 toEye;
    float facingEye;
    float alpha;            // roughness, squared
    float3 straightOn;      // what it mirrors straight on
    float3 scattered;       // what it scatters: none, for a metal
    float metallic;
};

/** What a surface sends to the eye of light arriving along [toLight]: the library's lobes (koral_brdf) — GGX,
 *  height-correlated Smith and Schlick's Fresnel over Lambert. */
float3 directLighting(Surface surface, float3 toLight, float3 radiance)
{
    const KoralAngles a = KoralAnglesOf(surface.normal, surface.toEye, toLight);
    if (dot(surface.normal, toLight) <= 0.0) return 0.0;
    const float3 mirrored = KoralFresnelSchlick(surface.straightOn, a.vh);
    const float3 highlight = KoralGgxDistribution(a.nh, surface.alpha) * KoralSmithGgxCorrelated(a.nv, a.nl, surface.alpha) * mirrored;
    const float3 matte = (1.0 - mirrored) * (1.0 - surface.metallic) * surface.scattered * KoralInvPi;
    return (matte + highlight) * a.nl * radiance;
}

/**
 * Light falling on a thin surface's back, let through to its front by @p through and scattered there evenly, tinted by
 * its colour: what a curtain with the sun behind it glows with. @p facingBack is how squarely its back faces the light.
 */
float3 throughFrom(Surface surface, float through, float facingBack, float3 radiance)
{
    return surface.scattered * KoralInvPi * through * facingBack * radiance;
}

/** How much of point light @p index reaches a point: through the face of its cube that looks the point's way. A light
 *  past those that have a cube casts no shadow. */
float lampShadow(uint index, KoralPointLight light, float3 world, float3 normal, float3 toLight)
{
    if (index >= LampShadowMost) return 1.0;
    return KoralPointVisibility(lampShadowMaps, lampCompare, lampFaces, index, light.position, world, normal, toLight, lampFilter());
}

[shader("fragment")]
Output fragment(RasterInfo raster)
{
    const Instance draw = instances[raster.instance];
    float4 base = pictureOf(draw.albedoMap, textureSampler, raster.uv);
    if (isCutOut(draw) && base.a < draw.alphaCutoff) discard;
    float4 material = pictureOf(draw.roughnessMap, textureSampler, raster.uv);
    float3 albedo = KoralLinearFromGamma22(draw.baseColor.rgb) * KoralLinearFromGamma22(base.rgb);
    float roughness = saturate(draw.roughness * material.g);
    float3 world = raster.worldPosition;

    Surface surface;
    surface.normal = normalize(raster.normal);
    surface.toEye = normalize(camera.position - world);
    if (dot(surface.normal, surface.toEye) < 0.0) surface.normal = -surface.normal;    // seen from behind, it faces the eye
    float3 texel = pictureOf(draw.normalMap, textureSampler, raster.uv).xyz;
    if (any(texel < 0.99))     // (white: it has none) — its directions along the surface from how its uv changes
        surface.normal = KoralMappedNormal(KoralSurfaceAt(world, surface.normal, raster.uv, textureSampler), KoralNormalTexelOf(texel.xy));
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

    // A thin thing lit from behind lets some of it through: its back's light, scattered out of its front.
    const float through = saturate(draw.translucency);
    float3 lit = 0.0;
    for (uint i = run.x; i < run.x + run.y; ++i) {
        // A point light, with its shadow's cube, or past their count a spot (which has none).
        const uint index = lightIndices[i];
        const bool lamp = index < camera.pointCount;
        const KoralArrival arrival = lamp ? pointLights[index].ArrivalAt(world) : spotLights[index - camera.pointCount].ArrivalAt(world);
        if (all(arrival.radiance <= 0.0)) continue;
        const float3 toLight = arrival.toLight;
        const float facing = dot(surface.normal, toLight);
        const float3 side = facing > 0.0 ? surface.normal : -surface.normal;
        const float shadow = lamp ? lampShadow(index, pointLights[index], world, side, toLight) : 1.0;
        if (facing > 0.0) lit += directLighting(surface, toLight, arrival.radiance) * shadow;
        else if (through > 0.0) lit += throughFrom(surface, through, -facing, arrival.radiance) * shadow;
    }

    if (sun.present > 0.0) {
        float3 toSun = -sun.direction;
        float facing = dot(surface.normal, toSun);
        if (facing > 0.0) lit += directLighting(surface, toSun, sun.color * KoralSunVisibility(shadowMap, cascadesOf(sun), world, surface.normal, toSun, depth, sunFilterOf(sun)));
        else if (through > 0.0) lit += throughFrom(surface, through, -facing, sun.color * KoralSunVisibility(shadowMap, cascadesOf(sun), world, -surface.normal, toSun, depth, sunFilterOf(sun)));
    }

    // The sky's light from all round, scattered; and what of the sky it mirrors, the less the rougher it is.
    lit += skyLight(sun, surface.normal) * surface.scattered;
    lit += skyLight(sun, -surface.normal) * surface.scattered * through;
    lit += skyLight(sun, reflect(-surface.toEye, surface.normal)) * KoralFresnelSchlick(surface.straightOn, surface.facingEye) * (1.0 - roughness);
    const float3 emitted = draw.emission * KoralLinearFromGamma22(pictureOf(draw.emissiveMap, textureSampler, raster.uv).rgb);
    lit += emitted;
    if (koralDebugShowing()) lit = koralDebugColor(KoralDebugSurface(albedo, surface.normal, roughness, surface.metallic, emitted));

    Output output;
    // How opaque it is: what the blended pipeline draws it over the picture by. What it mirrors is on it however
    // clear it is: a pane seen along itself shows more of the sky than of what is behind it.
    float clear = transparencyOf(draw);
    float opaque = base.a * (1.0 - clear);
    if (clear > 0.0) opaque = saturate(opaque + (1.0 - opaque) * KoralLuminance(KoralFresnelSchlick(surface.straightOn, surface.facingEye)) * (1.0 - roughness));
    output.color = float4(lit, opaque);
    output.position = float4(world, 1.0);
    output.normal = float4(surface.normal, 1.0);
    output.specular = float4(surface.straightOn, sqrt(surface.alpha));
    return output;
}
