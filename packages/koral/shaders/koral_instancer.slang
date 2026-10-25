// The instances an Instancer says, made on the GPU into one buffer (koral_instances' layout), and the box round each
// cluster of them: only when what they are made from is not what the buffer's header says they were made from. The
// engine's Instances function runs it every frame: `check` decides, in one thread, and writes how many groups the
// other two run — none, when nothing changed — so a frame in which nothing changed costs three passes that do nothing.

import koral_instances;

struct MakePush {
    uint count;
    uint seed;
    float jitter;               // 0 a grid, 1 anywhere in its cell
    uint unused;
    float2 extent;              // the square they are scattered over, along x and z
    float2 scale;               // from, to
};
[[vk::push_constant]] ConstantBuffer<MakePush> push;

RWByteAddressBuffer instances;
RWStructuredBuffer<uint> args;  // generate's groups (x, y, z), then bounds' (x, y, z)

// The most groups along x a device must take: more go along y.
static const uint Wide = 65535;

uint3 groupsFor(uint groups)
{
    return groups == 0 ? uint3(0, 1, 1) : uint3(min(groups, Wide), (groups + Wide - 1) / Wide, 1);
}

uint linearIndex(uint3 id) { return id.x + id.y * Wide * 256; }

uint hash(uint x)
{
    // PCG's output function, of a step of its generator: well mixed for any input.
    x = x * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}
float random(uint index, uint seed, uint stream) { return float(hash(index * 4u + stream + hash(seed)) >> 8) / 16777216.0; }

[shader("compute")]
[numthreads(1, 1, 1)]
void check()
{
    const uint header[HeaderWords] = { InstancesMagic, push.count, push.seed, asuint(push.jitter),
                                       asuint(push.extent.x), asuint(push.extent.y), asuint(push.scale.x), asuint(push.scale.y) };
    bool same = true;
    [unroll] for (uint i = 0; i < HeaderWords; ++i) same = same && instances.Load(i * 4) == header[i];
    if (same) {
        // As they were made: nothing to make, nor to bound.
        [unroll] for (uint i = 0; i < 6; ++i) args[i] = i % 3 == 0 ? 0 : 1;
        return;
    }
    [unroll] for (uint i = 0; i < HeaderWords; ++i) instances.Store(i * 4, header[i]);
    const uint3 made = groupsFor((push.count + 255) / 256);
    const uint3 bounded = groupsFor((clusterCount(push.count) + 255) / 256);
    args[0] = made.x; args[1] = made.y; args[2] = made.z;
    args[3] = bounded.x; args[4] = bounded.y; args[5] = bounded.z;
}

/**
 * Instance i, scattered: the square is cut into tiles of 16 by 16 cells, a cluster each — so a cluster's 256 are near one
 * another and its box is small — and each instance is somewhere in its cell, as big as it is, turned about y as it is.
 */
[shader("compute")]
[numthreads(256, 1, 1)]
void generate(uint3 id : SV_DispatchThreadID)
{
    const uint i = linearIndex(id);
    if (i >= push.count) return;
    const uint clusters = clusterCount(push.count);
    const uint across = max(1u, uint(ceil(sqrt(float(clusters)))));
    const uint down = (clusters + across - 1) / across;
    const uint tile = i / InstancesPerCluster, within = i % InstancesPerCluster;
    const float2 cells = float2(across, down) * 16.0;
    const float2 cell = float2(tile % across * 16 + within % 16, tile / across * 16 + within / 16);
    const float2 strayed = cell + 0.5 + (float2(random(i, push.seed, 0), random(i, push.seed, 1)) - 0.5) * push.jitter;
    const float2 at = strayed / cells * push.extent - push.extent * 0.5;
    const float scale = lerp(push.scale.x, push.scale.y, random(i, push.seed, 2));
    const float yaw = random(i, push.seed, 3) * 6.28318530718;
    const uint address = instanceAddress(push.count, i);
    instances.Store4(address, uint4(asuint(at.x), asuint(0.0), asuint(at.y), asuint(scale)));
    instances.Store(address + 16, packRotation(float4(0.0, sin(yaw * 0.5), 0.0, cos(yaw * 0.5))));
}

/** The box round each cluster's instances, and the largest of their scales: what culling tests before any of them. */
[shader("compute")]
[numthreads(256, 1, 1)]
void bounds(uint3 id : SV_DispatchThreadID)
{
    const uint cluster = linearIndex(id);
    if (cluster >= clusterCount(push.count)) return;
    float3 low = 1e30, high = -1e30;
    float largest = 0.0;
    const uint first = cluster * InstancesPerCluster, last = min(first + InstancesPerCluster, push.count);
    for (uint i = first; i < last; ++i) {
        const uint4 words = instances.Load4(instanceAddress(push.count, i));
        const float3 position = asfloat(words.xyz);
        low = min(low, position);
        high = max(high, position);
        largest = max(largest, asfloat(words.w));
    }
    const uint address = clusterAddress(cluster);
    instances.Store4(address, uint4(asuint(low), asuint(largest)));
    instances.Store4(address + 16, uint4(asuint(high), 0u));
}
