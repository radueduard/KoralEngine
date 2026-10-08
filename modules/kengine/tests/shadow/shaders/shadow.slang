// Shadow mapping, as small as it goes. The ground is the picture; an occluder floats over it; a light leans.
//
// `light` is the light as a camera: the matrix that takes the world to where the light sees it. A graphics script
// writes it each frame from the light entity. The shadow pass draws what casts into a depth image with it, and the
// lit pass asks that image whether the light reached each point of the ground.

struct Light {
    float4x4 viewProjection;
};
ConstantBuffer<Light> light;

struct Object {
    float3 offset;      // where the entity is
    float scale;        // how big
};
[[vk::push_constant]] ConstantBuffer<Object> object;

float3 World(float3 position) { return position * object.scale + object.offset; }

// ---- the shadow pass: depth, as the light sees it --------------------------------------------------------------

[shader("vertex")]
float4 castVertex(float3 position : POSITION) : SV_Position
{
    // Koral's matrices are a column after another, and Slang reads a row after another: the vector goes first.
    return mul(float4(World(position), 1.0), light.viewProjection);
}

[shader("fragment")]
void castFragment() {}
