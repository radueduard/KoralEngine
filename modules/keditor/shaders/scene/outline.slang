module outline;

// The selected entity's outline: a pixel that is not the entity's, but is within [thickness] pixels of one that is,
// is painted the outline's colour. Read from the image of which entity is where that the lit pass writes.
//
// `selected` is the engine's to give: the entity that is picked, 0 for none. `outlineColor` and `thickness` are
// given on the pipeline's node (push.outlineColor, push.thickness), so the pass runs once and no entity has them.

Texture2D<uint> ids;

struct RasterInfo {
    float4 position : SV_Position;
};

[shader("vertex")]
RasterInfo vertex(uint index : SV_VertexID)
{
    float2 corner = float2((index << 1) & 2, index & 2);
    RasterInfo raster;
    raster.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return raster;
}

[shader("fragment")]
float4 fragment(RasterInfo raster, uniform uint selected, uniform float4 outlineColor, uniform int thickness) : SV_Target
{
    if (selected == 0) discard;        // nothing is picked
    int2 pixel = int2(raster.position.xy);
    uint width, height;
    ids.GetDimensions(width, height);
    if (ids.Load(int3(pixel, 0)) == selected) discard;

    // The nearest of the entity's pixels in a disc round this one: full colour beside it, fading to the edge.
    float nearest = 1e9;
    for (int y = -thickness; y <= thickness; y++) {
        for (int x = -thickness; x <= thickness; x++) {
            float distance = length(float2(x, y));
            if (distance > thickness + 0.5 || distance >= nearest) continue;
            int2 at = clamp(pixel + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
            if (ids.Load(int3(at, 0)) == selected) nearest = distance;
        }
    }
    if (nearest > thickness + 0.5) discard;
    float alpha = saturate(thickness + 0.5 - nearest);
    return float4(outlineColor.rgb, outlineColor.a * alpha);
}
