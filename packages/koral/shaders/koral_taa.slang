// Temporal antialiasing: each pixel this frame blended with where it was in the frames before — the history, a picture
// of what came out of this the frame before — so that a picture drawn a little moved each frame (a Jitter's projection)
// settles into one seen between the pixels. Where it was is worked out of its depth: this frame's place put back by
// `currentToPrevious` (the frame before's viewProjection after this frame's inverse); a history further from what is
// round the pixel now than that is held to it, so that what moved does not leave a trail. The engine's TAA function
// runs it.

Texture2D<float4> current;                              // this frame's picture
Texture2D<float> depth;                                 // its depth
Sampler2D<float4> history;                              // what this made the frame before
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;

float3 toYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 fromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

[shader("compute")]
[numthreads(8, 8, 1)]
void resolve(uint3 id : SV_DispatchThreadID, uniform float4x4 currentToPrevious, uniform float blend, uniform uint valid)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const int2 at = int2(id.xy);
    const float4 now = current.Load(int3(at, 0));
    // What is round it now, in terms where brightness and colour are apart: the box a history is held in.
    float3 low = 1e30, high = -1e30;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 near = clamp(at + int2(x, y), int2(0, 0), int2(width, height) - 1);
            const float3 c = toYCoCg(current.Load(int3(near, 0)).rgb);
            low = min(low, c);
            high = max(high, c);
        }
    // Where it was: its place in clip space this frame, put where the frame before had it.
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float4 clip = float4(uv * 2.0 - 1.0, depth.Load(int3(at, 0)), 1.0);
    const float4 before = mul(clip, currentToPrevious);
    const float2 was = before.w > 0.0 ? (before.xy / before.w) * 0.5 + 0.5 : float2(-1.0, -1.0);
    if (valid == 0 || any(was < 0.0) || any(was > 1.0)) { target[id.xy] = now; return; }
    const float3 held = clamp(toYCoCg(history.SampleLevel(was, 0.0).rgb), low, high);
    target[id.xy] = float4(lerp(fromYCoCg(held), now.rgb, saturate(blend)), now.a);
}
