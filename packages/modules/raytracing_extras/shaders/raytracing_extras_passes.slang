// The raytracing-extras module's pass: a picture's ambient occlusion, traced — from each pixel's place (its depth, back
// through `inverseViewProjection`) and normal, `rays` rays within `radius` into `scene`. The TracedOcclusion function
// runs it; the filters module's Denoise takes its noise out.

import raytracing_extras;
import koral_depth;
import koral_math;

StructuredBuffer<uint4> scene;         // a RayScene's: its structure's bindless index (~0 while it has nothing)
Texture2D<float> depth;
Texture2D<float4> normals;
[[vk::image_format("r16f")]] RWTexture2D<float> occlusion;

[shader("compute")]
[numthreads(8, 8, 1)]
void occlude(uint3 id : SV_DispatchThreadID, uniform float4x4 inverseViewProjection, uniform float radius, uniform uint rays, uniform uint frame,
             uniform uint reversed)
{
    uint width, height;
    occlusion.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float d = depth.Load(int3(id.xy, 0));
    if ((reversed != 0 ? d <= 0.0 : d >= 1.0) || !KoralSceneTraceable(scene)) { occlusion[id.xy] = 1.0; return; }     // the sky, or nothing to hit
    const float3 position = KoralWorldFromDepth(KoralPixelUv(id.xy, uint2(width, height)), d, inverseViewProjection);
    const float3 normal = KoralSafeNormalize(normals.Load(int3(id.xy, 0)).xyz);
    occlusion[id.xy] = KoralTracedOcclusion(KoralSceneStructure(scene), position, normal, radius, rays, id.xy, frame);
}
