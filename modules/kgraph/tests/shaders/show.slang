// Draws a unit square a quarter of the picture in size, where the entity says, in the colour of the far corner of
// the entity's image: a corner only an image covered whole has anything in.

struct Place {
    float2 offset;
};
[[vk::push_constant]] ConstantBuffer<Place> place;

Texture2D<float4> source;

struct VertexOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOut vertexMain(float3 position : POSITION)
{
    VertexOut output;
    output.position = float4(position.xy + place.offset, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    uint width, height;
    source.GetDimensions(width, height);
    return source.Load(int3(width - 1, height - 1, 0));
}
