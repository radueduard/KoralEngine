// Reads what an element of a buffer of PbrMaterials holds, as the module the engine wrote of the kind has it: so a test
// can tell that the module is there, and that a shader that imports it is built.

import koral_materials;

StructuredBuffer<PbrMaterial> materials;
StructuredBuffer<BlinnPhongMaterial> classic;

[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    target[id.xy] = materials[0].baseColor * materials[0].roughness + float4(classic[0].specular, 0.0) * classic[0].shininess;
}
