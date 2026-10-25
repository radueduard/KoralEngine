// show.slang, for every entity in one draw: where each square goes, and the image whose far corner it is drawn in, are
// its instance's. The image is read by its bindless index (TEXTURE).

import entity;
import koral_bindless;

struct Instance {
    [entity("TEXTURE")] uint source;
    float2 offset;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

struct VertexOut {
    float4 position : SV_Position;
    nointerpolation uint source : SOURCE;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION, uint index : SV_InstanceID, uint first : SV_StartInstanceLocation)
{
    const Instance instance = instances[first + index];
    VertexOut output;
    output.position = float4(position.xy + instance.offset, 0.0, 1.0);
    output.source = instance.source;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    if (input.source == ~0u) return 0.0;
    Texture2D<float4> source = kor_Texture2D[NonUniformResourceIndex(input.source)];
    uint width, height;
    source.GetDimensions(width, height);
    return source.Load(int3(width - 1, height - 1, 0));
}
