// The lit pass: the world seen from straight above, each point white where the light reaches it and dark where
// something is in the way.

struct Light {
    float4x4 viewProjection;
};
ConstantBuffer<Light> light;
Texture2D<float> shadowMap;

struct Object {
    float3 offset;
    float scale;
};
[[vk::push_constant]] ConstantBuffer<Object> object;

struct Lit {
    float4 position : SV_Position;
    float3 world : WORLD;
};

[shader("vertex")]
Lit litVertex(float3 position : POSITION)
{
    Lit output;
    output.world = position * object.scale + object.offset;
    output.position = float4(output.world.xy, 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 litFragment(Lit input) : SV_Target
{
    // The vector first: see shadow.slang.
    const float4 seen = mul(float4(input.world, 1.0), light.viewProjection);
    uint width, height;
    shadowMap.GetDimensions(width, height);
    const int2 texel = clamp(int2((seen.xy * 0.5 + 0.5) * float2(width, height)), int2(0, 0), int2(width - 1, height - 1));
    const float nearest = shadowMap.Load(int3(texel, 0));
    return seen.z <= nearest + 0.01 ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.2, 0.2, 0.2, 1.0);
}
