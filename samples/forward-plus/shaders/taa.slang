// Temporal antialiasing. The pre-pass and the forward pass move the picture by a different fraction of a pixel each
// frame (FrameState's jitter); this blends each frame into what the ones before it came to, found where it was last
// frame by the camera's matrices this frame and the last — kept in the frame's state, which `advance` writes.

import forwardCommon;

// ---- advance: the frame's state, once a frame, before anything is drawn ---------------------------------------------

RWStructuredBuffer<FrameState> state;

[numthreads(1, 1, 1)]
void advance()
{
    FrameState s = state[0];
    const bool begun = s.begun == Begun;
    const float2 size = camera.screenSize;
    // Last frame's picture can be blended with when there was one, as big as this one.
    s.history = begun && all(s.size == size) ? 1 : 0;
    s.previousViewProjection = begun ? s.viewProjection : camera.viewProjection;
    s.viewProjection = camera.viewProjection;
    s.frame = begun ? s.frame + 1 : 0;
    s.size = size;
    // Eight places within the pixel, round and round: the 2 and 3 Halton sequences, about the pixel's middle.
    const uint index = s.frame % 8 + 1;
    const float2 offset = float2(KoralHalton(index, 2), KoralHalton(index, 3)) - 0.5;
    s.jitter = offset * 2.0 / max(size, float2(1.0, 1.0));
    s.begun = Begun;
    state[0] = s;
}

// ---- resolve: this frame's picture, blended into the history -----------------------------------------------------------

Texture2D<float4> current;                  // the picture as light, this frame: jittered
Texture2D<float4> positionTex;              // where each pixel's surface is in the world; w is 1 where there is one
Texture2D<float4> history;                  // what the frames before came to
SamplerState linearSampler;
StructuredBuffer<FrameState> frameState;
RWTexture2D<float4> resolved;
[[vk::push_constant]] cbuffer Settings { float blend; };   // how much of this frame goes in: 0.1 keeps about ten frames

// A highlight would otherwise outweigh everything round it and flicker: each weighed by one over one and its brightest.
float weight(float3 c) { return 1.0 / (1.0 + max(c.r, max(c.g, c.b))); }

[numthreads(16, 16, 1)]
void resolve(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    resolved.GetDimensions(width, height);
    const int2 px = int2(id.xy);
    if (id.x >= width || id.y >= height) return;
    const float2 size = float2(width, height);
    const float3 colour = current[px].rgb;
    const FrameState s = frameState[0];
    if (s.begun != Begun || s.history == 0) { resolved[px] = float4(colour, 1.0); return; }

    // Where this pixel's surface was last frame: its place in the world, through last frame's matrix. Where there is
    // none, the sky: far along the way the pixel looks, which only the camera's turning moves.
    const float2 uv = (float2(px) + 0.5) / size;
    const float4 at = positionTex[px];
    float3 world;
    if (at.w > 0.0) world = at.xyz;
    else {
        const float4 seen = mul(float4(uv * 2.0 - 1.0, 1.0, 1.0), camera.inverseProjection);
        const float3 way = normalize(mul(float4(seen.xyz / seen.w, 0.0), camera.inverseView).xyz);
        world = camera.position + way * camera.zFar * 0.5;
    }
    const float4 now = mul(float4(world, 1.0), s.viewProjection);
    const float4 then = mul(float4(world, 1.0), s.previousViewProjection);
    const float2 motion = (then.xy / then.w - now.xy / now.w) * 0.5;
    const float2 previousUv = uv + motion;
    if (any(previousUv < 0.0) || any(previousUv > 1.0)) { resolved[px] = float4(colour, 1.0); return; }   // came into sight this frame

    // The colours this pixel could have, from its 3x3 neighbours this frame: history outside them belongs to something
    // that has since moved, or been uncovered, and is clamped in.
    float3 low = 1e30, high = -1e30;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const float3 c = KoralToYCoCg(current[clamp(px + int2(x, y), int2(0, 0), int2(size) - 1)].rgb);
            low = min(low, c);
            high = max(high, c);
        }
    float3 previous = history.SampleLevel(linearSampler, previousUv, 0.0).rgb;
    if (any(isnan(previous)) || any(isinf(previous))) previous = colour;
    previous = KoralFromYCoCg(clamp(KoralToYCoCg(previous), low, high));
    const float a = weight(colour) * blend, b = weight(previous) * (1.0 - blend);
    resolved[px] = float4((colour * a + previous * b) / max(a + b, 1e-6), 1.0);
}

// ---- keep: the resolved picture, as the picture and as the next frame's history ----------------------------------------

Texture2D<float4> resolvedTex;
RWTexture2D<float4> hdrImage;
RWTexture2D<float4> historyImage;

[numthreads(16, 16, 1)]
void keep(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdrImage.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float4 c = resolvedTex[id.xy];
    hdrImage[id.xy] = c;
    historyImage[id.xy] = c;
}
