// The shading-stylized module's pass: lines drawn over `picture` where what is seen breaks — a step in depth (relative
// to how far, more than `depthEdge`) or a turn of the normal (more than `normalEdge`, 1 - the cosine) — `width` pixels
// wide, of `color`. The Outline function runs it.

import koral_math;

[[vk::image_format("rgba16f")]] RWTexture2D<float4> picture;
Texture2D<float> depth;
Texture2D<float4> normals;

[shader("compute")]
[numthreads(16, 16, 1)]
void outline(uint3 id : SV_DispatchThreadID, uniform float4 color, uniform float depthEdge, uniform float normalEdge, uniform int width)
{
    uint w, h;
    picture.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const int2 at = int2(id.xy), last = int2(w, h) - 1;
    const float centreDepth = depth.Load(int3(at, 0));
    const float3 centreNormal = normals.Load(int3(at, 0)).xyz;
    float edge = 0.0;
    for (int y = -width; y <= width; ++y)
        for (int x = -width; x <= width; ++x) {
            if (x * x + y * y > width * width || (x == 0 && y == 0)) continue;
            const int2 other = clamp(at + int2(x, y), int2(0, 0), last);
            const float d = depth.Load(int3(other, 0));
            const float3 n = normals.Load(int3(other, 0)).xyz;
            const float step = abs(d - centreDepth) / max(abs(centreDepth), 1e-6);
            if (step > depthEdge || 1.0 - dot(n, centreNormal) > normalEdge) edge = 1.0;
        }
    const float4 seen = picture[id.xy];
    picture[id.xy] = float4(lerp(seen.rgb, color.rgb, edge * color.a), seen.a);
}
