// A buffer of lights and a block of one view, as a ShaderBuffer node fills them: laid out as written here, with
// nothing said of bindings or offsets. It adds up the point lights' colours, each times its range, so that a test can
// tell that every field is where the shader reads it.

struct Light {
    float3 position;
    float range;
    float4 color;
    uint kind;          // 1: a point light, 2: a spot
    float2 cone;
};

struct View {
    float4x4 viewProjection;
    float3 eye;
    uint lightCount;
};

StructuredBuffer<Light> lights;
ConstantBuffer<View> view;
[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    float3 sum = 0.0;
    for (uint i = 0; i < view.lightCount; ++i)
        if (lights[i].kind == 1) sum += lights[i].color.rgb * lights[i].range;
    target[id.xy] = float4(sum, 1.0);
}
