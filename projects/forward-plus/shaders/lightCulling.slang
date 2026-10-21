// Clustered light culling: sorts the point lights into a grid of clusters — across the picture, down it, and into
// it, deeper clusters longer — which the forward pass walks for each pixel. A work group is one cluster: its threads
// share the lights out among themselves, each tests its share against the cluster's box, and the group claims a run
// of the index list for those that touch it.

import forwardCommon;

StructuredBuffer<PointLight> lights;
RWStructuredBuffer<uint2> lightGrid;        // for each cluster: where its lights start in the list, and how many
RWStructuredBuffer<uint> lightIndices;      // the list: each cluster's lights, one run after another
RWStructuredBuffer<uint> lightCounter;      // how much of the list is claimed: cleared before this runs

static const uint MaxLightsPerCluster = 256;
static const uint Threads = 8 * 8 * 2;

static groupshared uint groupLightCount;
static groupshared uint groupOffset;
static groupshared uint groupLights[MaxLightsPerCluster];

/** How far from the camera slice [s] starts: each slice as many times deeper as the last. */
float sliceToDepth(uint s)
{
    return camera.zNear * pow(camera.zFar / camera.zNear, float(s) / float(camera.gridSize.z));
}

float viewDepthToNdcZ(float depth)
{
    float4 clip = mul(float4(0.0, 0.0, -depth, 1.0), camera.projection);
    return clip.z / clip.w;
}

[shader("compute")]
[numthreads(8, 8, 2)]
void computeMain(uint3 cluster : SV_GroupID, uint thread : SV_GroupIndex)
{
    if (thread == 0) groupLightCount = 0;
    GroupMemoryBarrierWithGroupSync();

    // The cluster's box in the world: its eight corners, from the picture back through the camera.
    float2 xyMin = float2(cluster.xy) / float2(camera.gridSize.xy) * 2.0 - 1.0;
    float2 xyMax = float2(cluster.xy + 1) / float2(camera.gridSize.xy) * 2.0 - 1.0;
    float z0 = viewDepthToNdcZ(sliceToDepth(cluster.z));
    float z1 = viewDepthToNdcZ(sliceToDepth(cluster.z + 1));
    float3 lower = 1e30, upper = -1e30;
    for (uint i = 0; i < 8; ++i) {
        float4 corner = float4((i & 1) != 0 ? xyMax.x : xyMin.x, (i & 2) != 0 ? xyMax.y : xyMin.y, (i & 4) != 0 ? z1 : z0, 1.0);
        float4 seen = mul(corner, camera.inverseProjection);
        float3 world = mul(float4(seen.xyz / seen.w, 1.0), camera.inverseView).xyz;
        lower = min(lower, world);
        upper = max(upper, world);
    }

    // This thread's share of the lights: a sphere touches the box when the box's nearest point is inside it.
    uint share = (camera.lightCount + Threads - 1) / Threads;
    uint first = thread * share;
    uint last = min(first + share, camera.lightCount);
    for (uint i = first; i < last; ++i) {
        PointLight light = lights[i];
        float3 nearest = clamp(light.position, lower, upper);
        float3 away = nearest - light.position;
        if (dot(away, away) < light.radius * light.radius) {
            uint slot;
            InterlockedAdd(groupLightCount, 1u, slot);
            if (slot < MaxLightsPerCluster) groupLights[slot] = i;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (thread == 0) {
        groupLightCount = min(groupLightCount, MaxLightsPerCluster);
        InterlockedAdd(lightCounter[0], groupLightCount, groupOffset);
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint i = thread; i < groupLightCount; i += Threads) lightIndices[groupOffset + i] = groupLights[i];
    if (thread == 0) {
        uint flat = cluster.x + cluster.y * camera.gridSize.x + cluster.z * camera.gridSize.x * camera.gridSize.y;
        lightGrid[flat] = uint2(groupOffset, groupLightCount);
    }
}
