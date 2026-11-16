// A mesh bent by its joints: each vertex placed by the four joints it names, weighed — where each joint's matrices start
// for its entity an instance's JOINTS, the matrices the runtime's JOINT_MATRICES — then where its entity is.

import entity;

struct Instance {
    [entity("MATRIX")] float4x4 model;
    [entity("JOINTS")] uint firstJoint;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;
[entity("JOINT_MATRICES")] StructuredBuffer<float4x4> joints;

struct VertexOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, uint4 bones : BONE_IDS, float4 weights : BONE_WEIGHTS, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    const Instance instance = instances[first + index];
    const uint at = instance.firstJoint;
    const float4x4 skin = joints[at + bones.x] * weights.x + joints[at + bones.y] * weights.y + joints[at + bones.z] * weights.z + joints[at + bones.w] * weights.w;
    VertexOut output;
    const float4 world = mul(mul(float4(position, 1.0), skin), instance.model);
    output.position = float4(world.xy, 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
