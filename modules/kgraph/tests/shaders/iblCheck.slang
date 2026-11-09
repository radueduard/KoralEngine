// koral_ibl, checked on what the engine's PrefilterEnvironment baked of an environment of one colour everywhere: its
// harmonics give that colour facing any way, and every mip of its prefiltered cube is that colour. As library.slang's.

import koral_ibl;

static const float3 Colour = float3(0.5, 0.25, 1.0);

[[vk::image_format("rgba16f")]] RWTexture2DArray<float4> environmentFaces;
StructuredBuffer<float4> sh;
TextureCube<float4> prefiltered;
SamplerState prefilteredSampler;
RWStructuredBuffer<float4> results;

/** The environment: one colour, all six faces. */
[shader("compute")]
[numthreads(8, 8, 1)]
void fill(uint3 id : SV_DispatchThreadID)
{
    uint width, height, faces;
    environmentFaces.GetDimensions(width, height, faces);
    if (id.x >= width || id.y >= height) return;
    for (uint face = 0; face < 6; ++face) environmentFaces[uint3(id.xy, face)] = float4(Colour, 1.0);
}

static uint next = 0;
void Check(float got, float expected, float tolerance) { results[next++] = float4(got, expected, tolerance, 1.0); }

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    const KoralSh9 harmonics = KoralShOf(sh);
    Check(KoralShDiffuse(harmonics, float3(0.0, 1.0, 0.0)).r, Colour.r, 0.02);
    Check(KoralShDiffuse(harmonics, normalize(float3(1.0, -1.0, 0.3))).b, Colour.b, 0.02);
    Check(KoralShDiffuse(harmonics, float3(0.0, 0.0, -1.0)).g, Colour.g, 0.02);
    const KoralShEnvironment around = KoralShEnvironment(harmonics, prefiltered, prefilteredSampler, 4.0);
    Check(around.Specular(normalize(float3(0.2, 0.9, -0.1)), 0.0).b, Colour.b, 0.01);
    Check(around.Specular(float3(-1.0, 0.0, 0.0), 1.0).r, Colour.r, 0.02);
    Check(around.Diffuse(float3(1.0, 0.0, 0.0)).g, Colour.g, 0.02);
}
