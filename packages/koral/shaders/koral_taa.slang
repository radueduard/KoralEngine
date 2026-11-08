// Temporal antialiasing: each pixel this frame blended with where it was in the frames before — the history, a picture
// of what came out of this the frame before — so that a picture drawn a little moved each frame (a Jitter's projection)
// settles into one seen between the pixels. Where it was is worked out of its depth: this frame's place put back by
// `currentToPrevious` (the frame before's viewProjection after this frame's inverse); a history further from what is
// round the pixel now than that is held to it, so that what moved does not leave a trail. The engine's TAA function
// runs it.

import koral_color;
import koral_depth;
import koral_math;

Texture2D<float4> current;                              // this frame's picture
Texture2D<float> depth;                                 // its depth
Sampler2D<float4> history;                              // what this made the frame before
[[vk::image_format("rgba16f")]] RWTexture2D<float4> target;

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
            const float3 c = KoralToYCoCg(current.Load(int3(near, 0)).rgb);
            low = min(low, c);
            high = max(high, c);
        }
    // Where it was: its place in clip space this frame, put where the frame before had it.
    const float2 was = KoralReproject(KoralPixelUv(id.xy, uint2(width, height)), depth.Load(int3(at, 0)), currentToPrevious);
    if (valid == 0 || !KoralInPicture(was)) { target[id.xy] = now; return; }
    const float3 held = clamp(KoralToYCoCg(history.SampleLevel(was, 0.0).rgb), low, high);
    target[id.xy] = float4(lerp(KoralFromYCoCg(held), now.rgb, saturate(blend)), now.a);
}
