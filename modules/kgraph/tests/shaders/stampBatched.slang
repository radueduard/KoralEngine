// stamp.slang, for every entity in one dispatch: what each element was given — its image, its colour, its gain — is
// its instance's, element SV_GroupID.z. Its image is written by its bindless storage index (STORAGE).

import entity;
import koral_bindless;

struct Instance {
    [entity("STORAGE")] uint target;
    float4 tint;
    float gain;
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;

Texture2D<float4> backdrop;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    const Instance instance = instances[group.z];
    if (instance.target == ~0u) return;
    RWTexture2D<float4> target = kor_RWTexture2D[NonUniformResourceIndex(instance.target)];
    uint width, height;
    target.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    target[id.xy] = instance.tint * instance.gain + backdrop.Load(int3(0, 0, 0));
}
