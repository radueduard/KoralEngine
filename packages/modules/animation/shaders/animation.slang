// The animation module: meshes moved on the GPU — skinned by joints (linear blend and dual quaternions, which keep a
// twisted joint's volume), blended between morph targets, swayed by wind, turned to face the camera, and played from a
// vertex-animation texture.
//
//     import animation;
//
//     const KoralSkinned s = KoralSkinLinear(joints, vertex.joints, vertex.weights, vertex.position, vertex.normal);

module animation;

import koral_math;

/** A vertex after skinning: where it is, and which way it faces. */
public struct KoralSkinned {
    public float3 position;
    public float3 normal;
};

/** Linear blend skinning: the vertex placed by each of its four joints' matrices (each the joint's world after its inverse bind), weighed. */
public KoralSkinned KoralSkinLinear(StructuredBuffer<float4x4> joints, uint4 index, float4 weight, float3 position, float3 normal)
{
    float4x4 m = joints[index.x] * weight.x + joints[index.y] * weight.y + joints[index.z] * weight.z + joints[index.w] * weight.w;
    KoralSkinned s;
    s.position = KoralTransformPoint(position, m);
    s.normal = KoralTransformNormal(normal, m);
    return s;
}

/** A dual quaternion: a rotation (real) and a translation (dual) together. As joints for KoralSkinDualQuaternion keeps them. */
public struct KoralDualQuaternion {
    public float4 real;
    public float4 dual;
};

/** Dual quaternion skinning (Kavan's): the joints' turns blended as turns, not as matrices — no candy-wrapper at a twisted joint. */
public KoralSkinned KoralSkinDualQuaternion(StructuredBuffer<KoralDualQuaternion> joints, uint4 index, float4 weight, float3 position, float3 normal)
{
    const KoralDualQuaternion first = joints[index.x];
    float4 real = 0.0, dual = 0.0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        const KoralDualQuaternion q = joints[index[i]];
        const float w = weight[i] * (dot(q.real, first.real) < 0.0 ? -1.0 : 1.0);     // the shorter way round
        real += q.real * w;
        dual += q.dual * w;
    }
    const float length = max(sqrt(dot(real, real)), 1e-8);
    real /= length;
    dual /= length;
    const float3 translation = 2.0 * (real.w * dual.xyz - dual.w * real.xyz + cross(real.xyz, dual.xyz));
    KoralSkinned s;
    s.position = KoralRotate(real, position) + translation;
    s.normal = KoralRotate(real, normal);
    return s;
}

/** A vertex moved by its morph targets: each target's offset at this vertex (@p offsets, vertexCount of them a target), weighed. */
public float3 KoralMorph(StructuredBuffer<float4> offsets, StructuredBuffer<float> weights, uint targets, uint vertexCount, uint vertex, float3 position)
{
    for (uint t = 0; t < targets; ++t) position += offsets[t * vertexCount + vertex].xyz * weights[t];
    return position;
}

/**
 * A vertex swayed by wind: moved along @p wind (a direction times a strength) by as much as it is high above its
 * object's root (@p height, 0 at the root), waving at @p frequency, a little out of step with its neighbours (@p phase).
 */
public float3 KoralWindSway(float3 position, float height, float3 wind, float time, float frequency = 1.5, float phase = 0.0)
{
    const float bend = height * height;
    const float wave = sin(time * frequency + phase) * 0.6 + sin(time * frequency * 2.3 + phase * 1.7) * 0.4;
    return position + wind * bend * (0.6 + 0.4 * wave);
}

/** A quad's corner turned to face the camera: its middle @p centre, @p corner (-1 to 1 each way) times @p size, along the view's right and up. */
public float3 KoralBillboard(float3 centre, float2 corner, float2 size, float3 cameraRight, float3 cameraUp)
{
    return centre + cameraRight * corner.x * size.x * 0.5 + cameraUp * corner.y * size.y * 0.5;
}

/** The same, kept upright: turned about y only, as a tree's impostor is. */
public float3 KoralBillboardUpright(float3 centre, float2 corner, float2 size, float3 cameraPosition)
{
    const float3 toCamera = KoralSafeNormalize(float3(cameraPosition.x - centre.x, 0.0, cameraPosition.z - centre.z), float3(0.0, 0.0, 1.0));
    const float3 right = cross(float3(0.0, 1.0, 0.0), toCamera);
    return centre + right * corner.x * size.x * 0.5 + float3(0.0, corner.y * size.y * 0.5, 0.0);
}

/**
 * Where a vertex is at @p time of an animation baked into a picture (vertex animation texture): a texel a vertex across,
 * a row a frame down, its rgb the position, played @p framesPerSecond and looped; blended between the two frames.
 */
public float3 KoralVertexAnimation(Texture2D<float4> baked, uint vertex, float time, float framesPerSecond)
{
    uint width, frames;
    baked.GetDimensions(width, frames);
    const float at = frac(time * framesPerSecond / float(frames)) * float(frames);
    const uint first = uint(at) % frames, second = (first + 1) % frames;
    return lerp(baked.Load(int3(vertex, first, 0)).xyz, baked.Load(int3(vertex, second, 0)).xyz, frac(at));
}
