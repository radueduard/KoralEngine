// Depth only: the pre-pass, which lays down how far everything is from the camera before anything is shaded, so that
// the forward pass shades each pixel once; and a point light's shadows. The sun's are the engine's SunShadows function's.

import forwardCommon;

// The pre-pass draws every instance the forward pass would, in one indirect draw: its buffer of instances is the
// forward Draw's (param.instances, wired from it), so that an instance is the same number in both.
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;
StructuredBuffer<FrameState> frameState;
SamplerState textureSampler;                // what a cut-out's picture is read with
// Which instances are in sight (occlusionCull.slang's frustum), a command's from its first instance on.
StructuredBuffer<uint> drawnInstances;

struct Seen {
    float4 position : SV_Position;
    float2 uv : UV;
    nointerpolation uint instance : INSTANCE;
};

[shader("vertex")]
Seen prepassVertex(float3 position : POSITION, float2 uv : UV, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    Seen seen;
    // How many instances there are: the list has room for each, and no more. (The buffer of them has room for more.)
    uint length, stride, listed;
    drawnInstances.GetDimensions(listed, stride);
    instances.GetDimensions(length, stride);
    length = min(length, listed);
    seen.instance = first + index < listed ? drawnInstances[first + index] : length;
    // One the cull did not write — a list not counted into yet — is nowhere.
    seen.position = seen.instance < length ? toPicture(mul(float4(position, 1.0), instances[seen.instance].model).xyz, frameState[0]) : float4(0.0, 0.0, 0.0, 0.0);
    seen.instance = min(seen.instance, max(length, 1) - 1);
    seen.uv = uv;
    return seen;
}

/**
 * Which instance each pixel shows, one more than its number (0: none) — what the occlusion cull reads to draw only what
 * is seen. A cut-out leaves out where its picture is less opaque than its alphaCutoff, as the forward pass will.
 */
[shader("fragment")]
uint prepassFragment(Seen seen) : SV_Target0
{
    const Instance instance = instances[seen.instance];
    if (instance.alphaCutoff > 0.0 && pictureOf(instance.albedoMap, textureSampler, seen.uv).a < instance.alphaCutoff) discard;
    return seen.instance + 1;
}

ConstantBuffer<LampShadows> lampShadows;

// A point light's shadows: the same meshes as one face of the cube round the light sees them. `face` is which face of
// which light — six a light, one after another: the turn's number of the loop the pipeline is in (push.face).
[shader("vertex")]
float4 lampVertex(float3 position : POSITION, uniform float4x4 model, uniform uint face) : SV_Position
{
    float4 world = mul(float4(position, 1.0), model);
    return mul(float4(world.xyz, 1.0), lampShadows.faces[face]);
}

[shader("fragment")]
void lampFragment() {}
