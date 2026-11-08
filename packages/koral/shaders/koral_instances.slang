// The engine's instances: many of one mesh, made and kept on the GPU in one buffer — the Instances function's — that a
// vertex shader reads its own from:
//
//     import koral_instances;
//
//     ByteAddressBuffer instances;            // the Instances function's `instances`
//     StructuredBuffer<uint> visible;         // the CullInstances function's: which instance each drawn one is
//     struct Placed { float4x4 placed; };     // where the Instancer's entity is: what places all of them
//     [[vk::push_constant]] ConstantBuffer<Placed> push;
//
//     const float4x4 model = mul(instanceMatrix(instances, visible[instanceIndex]), push.placed);
//
// The buffer is laid out as:
//
//     a header        8 uints   what they were made from (InstancesHeader): made again when it is not that
//     the clusters    8 uints   each: a box round 256 instances one after another — min, the largest scale, max
//     the instances   5 uints   each: where it is (3 floats), its scale, and its rotation (a quaternion, packed)
//
// An instance is 20 bytes: 100 million are 2 GB, which one buffer holds where the device's storage buffers reach 4 GB.

module koral_instances;

import koral_math;
import koral_encoding;

public static const uint InstancesPerCluster = 256;
public static const uint HeaderWords = 8;
public static const uint ClusterWords = 8;
public static const uint InstanceWords = 5;
public static const uint InstancesMagic = 0x534E494Bu;     // "KINS"

/** How many clusters @p count instances are in. */
public uint clusterCount(uint count) { return (count + InstancesPerCluster - 1) / InstancesPerCluster; }
/** Where in the buffer instance @p index starts, in bytes, of @p count instances. */
public uint instanceAddress(uint count, uint index) { return (HeaderWords + clusterCount(count) * ClusterWords + index * InstanceWords) * 4; }
/** Where cluster @p cluster starts, in bytes. */
public uint clusterAddress(uint cluster) { return (HeaderWords + cluster * ClusterWords) * 4; }
/** How many bytes @p count instances need, with their header and clusters. */
public uint instancesBytes(uint count) { return instanceAddress(count, count); }

public struct Instance {
    public float3 position;     // where it is, in its Instancer's entity's space
    public float scale;
    public float4 rotation;     // a quaternion: x, y, z, w
};

public struct Cluster {
    public float3 min;          // the box round where its instances are
    public float scale;         // the largest of their scales
    public float3 max;
};

public Instance loadInstance(ByteAddressBuffer instances, uint count, uint index)
{
    const uint at = instanceAddress(count, index);
    const uint4 words = instances.Load4(at);
    Instance instance;
    instance.position = asfloat(words.xyz);
    instance.scale = asfloat(words.w);
    instance.rotation = KoralUnpackQuaternion(instances.Load(at + 16));
    return instance;
}

/** How many instances the buffer holds: what its header says they were made as. */
public uint instancesCount(ByteAddressBuffer instances) { return instances.Load(4); }

/** An instance as a matrix — its rotation, scale and place — in its Instancer's entity's space, row-vector convention (mul(v, m)). */
public float4x4 instanceMatrix(Instance instance) { return KoralMatrixOf(instance.rotation, instance.scale, instance.position); }

/** Instance @p index of the buffer as a matrix: what a vertex shader places its vertices by, before the entity's matrix. */
public float4x4 instanceMatrix(ByteAddressBuffer instances, uint index)
{
    return instanceMatrix(loadInstance(instances, instancesCount(instances), index));
}
