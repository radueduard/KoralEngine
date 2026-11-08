// What of an Instancer's instances is too far for its mesh, drawn as points: each written into a buffer of the
// picture's pixels with an atomic max — the nearest wins, and its shade with it — then laid over the picture wherever
// it is nearer than what the scene drew there. No triangle, and no rasteriser: a point is a square of pixels as wide as
// its instance looks, up to four, and millions of them are as many atomics. The engine's SplatPoints function runs it,
// after CullInstances listed them.
//
// A pixel of `splat` is how near (24 bits: 1 at the near plane, 0 at the far one) and a shade (8 bits); 0 is none.

import koral_instances;
import koral_math;
import koral_random;

struct PointsPush {
    float4x4 toClip;            // the Instancer's entity's space to the picture's, as CullInstances had it
    float3 color;               // what the points are drawn in
    uint count;
    uint width;                 // the picture's size, in pixels
    uint height;
    uint pointCapacity;
    uint reversedZ;             // 1 when depth is 1 at the near plane (a Perspective's reversedZ)
    float meshRadius;           // how far the mesh reaches from its middle, at scale 1: how wide a point is drawn
};
[[vk::push_constant]] ConstantBuffer<PointsPush> push;

ByteAddressBuffer instances;
StructuredBuffer<uint> counters;    // CullInstances': how many points, third
StructuredBuffer<uint> points;      // which instance each point is
RWStructuredBuffer<uint> splat;     // a uint a pixel
RWTexture2D<float4> target;         // what they are laid over
Texture2D<float> depth;             // and how near what the scene drew is

[shader("compute")]
[numthreads(256, 1, 1)]
void splatPoints(uint3 id : SV_DispatchThreadID)
{
    const uint k = KoralThreadIndex(id, 256);
    if (k >= min(counters[2], push.pointCapacity)) return;
    const uint which = points[k];
    const uint4 words = instances.Load4(instanceAddress(push.count, which));
    const float3 position = asfloat(words.xyz);
    const float4 clip = mul(float4(position, 1.0), push.toClip);
    if (clip.w <= 0.0) return;
    const float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0) return;
    // How wide it looks: its reach along the picture's x and y, in pixels.
    const float reach = asfloat(words.w) * push.meshRadius;
    const float2 across = float2(length(push.toClip[0].xy), length(push.toClip[1].xy)) * reach / clip.w * float2(push.width, push.height);
    const uint side = clamp(uint(round(max(across.x, across.y))), 1u, 4u);
    const int2 middle = int2((ndc.xy * 0.5 + 0.5) * float2(push.width, push.height));
    const float near = push.reversedZ != 0 ? ndc.z : 1.0 - ndc.z;
    // Each a little lighter or darker than the next, as leaves and stones are.
    const uint shade = 150u + KoralPcgHash(which) % 106u;
    const uint value = (uint(saturate(near) * 16777215.0) << 8) | shade;
    const int low = -int(side / 2);
    for (int y = 0; y < int(side); ++y)
        for (int x = 0; x < int(side); ++x) {
            const int2 pixel = middle + int2(low + x, low + y);
            if (any(pixel < 0) || pixel.x >= int(push.width) || pixel.y >= int(push.height)) continue;
            InterlockedMax(splat[pixel.y * push.width + pixel.x], value);
        }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolvePoints(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.width || id.y >= push.height) return;
    const uint value = splat[id.y * push.width + id.x];
    if (value == 0) return;
    const float scene = depth.Load(int3(id.xy, 0));
    const float sceneNear = push.reversedZ != 0 ? scene : 1.0 - scene;
    if ((value >> 8) <= uint(saturate(sceneNear) * 16777215.0)) return;     // the scene is nearer there
    target[id.xy] = float4(push.color * (float(value & 255u) / 255.0), 1.0);
}
