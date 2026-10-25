// What of an Instancer's instances is seen this frame, and as what, worked out on the GPU: the engine's CullInstances
// function runs it every frame, and nothing of it is a number of instances on the CPU.
//
//   cullClusters   a thread a cluster: left out when its box is out of sight or past drawDistance; when all of it is past
//                  pointDistance and it spans fewer pixels than it has instances, as many points as it spans; else
//                  listed, for its instances to be looked at one by one
//   clusterArgs    how many groups cullInstances runs: one a listed cluster
//   cullInstances  a thread an instance of a listed cluster: left out when out of sight; its mesh when nearer than
//                  pointDistance, a point when further
//   finish         the indirect draw of the meshes, and how many groups the points are splatted by
//
// Distances are along the view, in the world's units: the w a position has in the picture.

import koral_instances;

struct CullPush {
    float4x4 toClip;            // the Instancer's entity's space to the picture's: its matrix, then the view, then the lens
    uint count;
    float pointDistance;
    float drawDistance;
    float meshRadius;           // how far the mesh reaches from its middle, at scale 1
    float halfHeight;           // half the picture's height, in pixels
    uint capacity;              // how many instances `visible` has room for
    uint pointCapacity;         // and `points`
    uint indexCount;            // the mesh's part of its buffers: what the draw draws of each instance
    uint firstIndex;
    int vertexOffset;
};
[[vk::push_constant]] ConstantBuffer<CullPush> push;

ByteAddressBuffer instances;
RWStructuredBuffer<uint> counters;  // clusters listed, instances as their mesh, points, more than there was room for
RWStructuredBuffer<uint> listed;    // which clusters cullInstances looks in
RWStructuredBuffer<uint> visible;   // which instance each drawn one is
RWStructuredBuffer<uint> points;    // which instance each point is
RWStructuredBuffer<uint> args;      // cullInstances' groups (x, y, z), then the points' (x, y, z)
RWStructuredBuffer<uint> commands;  // the meshes' draw: a VkDrawIndexedIndirectCommand

static const uint Wide = 65535;

uint3 groupsFor(uint groups)
{
    return groups == 0 ? uint3(0, 1, 1) : uint3(min(groups, Wide), (groups + Wide - 1) / Wide, 1);
}

/** A box, centre and half its size, as the picture sees it: whether it is wholly out of sight, how near its nearest part is, and how many pixels it spans. */
struct Seen {
    bool out;
    float nearest;
    float pixels;
};
Seen look(float3 centre, float3 half)
{
    const float4 c = mul(float4(centre, 1.0), push.toClip);
    const float4 e[3] = { mul(float4(half.x, 0.0, 0.0, 0.0), push.toClip), mul(float4(0.0, half.y, 0.0, 0.0), push.toClip),
                          mul(float4(0.0, 0.0, half.z, 0.0), push.toClip) };
    int past[6] = { 0, 0, 0, 0, 0, 0 };
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        float4 at = c;
        [unroll] for (uint axis = 0; axis < 3; ++axis) at += (corner & (1u << axis)) != 0 ? e[axis] : -e[axis];
        past[0] += at.x < -at.w; past[1] += at.x > at.w; past[2] += at.y < -at.w; past[3] += at.y > at.w; past[4] += at.z < 0.0; past[5] += at.z > at.w;
    }
    Seen seen;
    seen.out = false;
    [unroll] for (uint side = 0; side < 6; ++side) seen.out = seen.out || past[side] == 8;
    const float reach = abs(e[0].w) + abs(e[1].w) + abs(e[2].w);
    seen.nearest = c.w - reach;
    const float across = max(abs(e[0].x) + abs(e[1].x) + abs(e[2].x), abs(e[0].y) + abs(e[1].y) + abs(e[2].y));
    seen.pixels = seen.nearest > 0.0 ? across / seen.nearest * push.halfHeight : 1e30;
    return seen;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void cullClusters(uint3 id : SV_DispatchThreadID)
{
    const uint cluster = id.x + id.y * Wide * 256;
    if (cluster >= clusterCount(push.count)) return;
    const uint4 low = instances.Load4(clusterAddress(cluster));
    const float3 high = asfloat(instances.Load3(clusterAddress(cluster) + 16));
    const float3 lowest = asfloat(low.xyz);
    const float reach = asfloat(low.w) * push.meshRadius;
    const Seen seen = look((lowest + high) * 0.5, (high - lowest) * 0.5 + reach);
    if (seen.out || seen.nearest > push.drawDistance) return;
    const uint first = cluster * InstancesPerCluster, many = min(InstancesPerCluster, push.count - first);
    // All of it past pointDistance, and over fewer pixels than it has instances: as many points as it has pixels, of
    // instances spread over it — no more points than can be seen, however many there are.
    const uint covered = uint(ceil(seen.pixels * seen.pixels));
    if (seen.nearest >= push.pointDistance && covered < many) {
        uint at;
        InterlockedAdd(counters[2], covered, at);
        for (uint j = 0; j < covered; ++j) {
            // 157 has nothing in common with 256: one step after another, every one of them in turn, spread over its tile.
            const uint which = first + (j * 157u) % many;
            if (at + j < push.pointCapacity) points[at + j] = which;
            else { InterlockedAdd(counters[3], covered - j); break; }
        }
        return;
    }
    uint at;
    InterlockedAdd(counters[0], 1u, at);
    listed[at] = cluster;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void clusterArgs()
{
    const uint3 groups = groupsFor(counters[0]);
    args[0] = groups.x; args[1] = groups.y; args[2] = groups.z;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void cullInstances(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint which = group.x + group.y * Wide;
    if (which >= counters[0]) return;
    const uint i = listed[which] * InstancesPerCluster + lane;
    bool asMesh = false, asPoint = false;
    if (i < push.count) {
        const Instance instance = loadInstance(instances, push.count, i);
        const Seen seen = look(instance.position, instance.scale * push.meshRadius);
        if (!seen.out && seen.nearest <= push.drawDistance) {
            asMesh = seen.nearest < push.pointDistance;
            asPoint = !asMesh;
        }
    }
    // Counted a wave at a time: one atomic for many.
    const uint meshes = WaveActiveCountBits(asMesh), dots = WaveActiveCountBits(asPoint);
    uint meshFirst = 0, dotFirst = 0;
    if (WaveIsFirstLane()) {
        if (meshes > 0) InterlockedAdd(counters[1], meshes, meshFirst);
        if (dots > 0) InterlockedAdd(counters[2], dots, dotFirst);
    }
    meshFirst = WaveReadLaneFirst(meshFirst);
    dotFirst = WaveReadLaneFirst(dotFirst);
    if (asMesh) {
        const uint at = meshFirst + WavePrefixCountBits(asMesh);
        if (at < push.capacity) visible[at] = i;
        else InterlockedAdd(counters[3], 1u);
    }
    if (asPoint) {
        const uint at = dotFirst + WavePrefixCountBits(asPoint);
        if (at < push.pointCapacity) points[at] = i;
        else InterlockedAdd(counters[3], 1u);
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void finish()
{
    commands[0] = push.indexCount;
    commands[1] = min(counters[1], push.capacity);
    commands[2] = push.firstIndex;
    commands[3] = asuint(push.vertexOffset);
    commands[4] = 0;
    const uint3 groups = groupsFor((min(counters[2], push.pointCapacity) + 255) / 256);
    args[3] = groups.x; args[4] = groups.y; args[5] = groups.z;
}
