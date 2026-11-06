// What every pass sees of a frame: the camera, the sky, and the lights. Written once a frame by the graph's
// SceneView node, whose `frame` and `lights` outputs are wired to a pipeline's `param.frame` and `param.lights`.
// Neither is given a binding here: the engine numbers what a pipeline's shaders declare.
//
// Light is worked in linear terms: the colours here are linear already (the SceneView converts them), a surface's
// colour is converted with toLinear, and what is worked out goes back to the display through present.

public struct FrameData {
    public float4x4 viewProjection;
    public float4x4 inverseViewProjection;
    public float4 cameraPosition;      // xyz
    public float4 skyZenith;           // rgb; w: how much light the sky gives all round
    public float4 skyHorizon;          // rgb; w: the cosine of the sun disc's radius
    public float4 skyGround;           // rgb
    public float4 sunDirection;        // xyz: towards the sun; w: 1 when there is a sun
    public float4 sunColor;            // rgb, times its intensity
    public uint4 counts;               // x: how many lights there are
};

public static const uint LightDirectional = 0;
public static const uint LightPoint = 1;
public static const uint LightSpot = 2;

public struct Light {
    public float4 position;            // xyz; w: range
    public float4 direction;           // xyz: the way it shines; w: its type
    public float4 color;               // rgb, times its intensity
    public float4 cone;                // x: cosine of the inner angle, y: of the outer
};

public ConstantBuffer<FrameData> frame;
public StructuredBuffer<Light> lights;

/** A colour as picked — as the display shows it — in linear terms, which light adds up in. */
public float3 toLinear(float3 color)
{
    return pow(max(color, 0.0), 2.2);
}

/**
 * Linear light as the display shows it: tone mapped (Narkowicz's fit of ACES), so that brighter than white rolls off
 * rather than clipping, then encoded for the display's curve.
 */
public float3 present(float3 light)
{
    float3 mapped = saturate((light * (2.51 * light + 0.03)) / (light * (2.43 * light + 0.59) + 0.14));
    return pow(mapped, 1.0 / 2.2);
}

/** The light the sky gives a surface facing [normal]: the ground's from below, the zenith's from above. */
public float3 skyLight(float3 normal)
{
    float up = normal.y * 0.5 + 0.5;
    float3 sky = lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, saturate(normal.y));
    return lerp(frame.skyGround.rgb, sky, smoothstep(0.0, 1.0, up)) * frame.skyZenith.w;
}

/** The sky seen along [direction], in linear light: the ground below the horizon, the zenith above it, and the sun. */
public float3 skyColor(float3 direction)
{
    float y = direction.y;
    float3 color = y >= 0.0
        ? lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, pow(saturate(y), 0.45))
        : lerp(frame.skyHorizon.rgb, frame.skyGround.rgb, pow(saturate(-y * 4.0), 0.6));
    if (frame.sunDirection.w > 0.0) {
        float facing = dot(direction, frame.sunDirection.xyz);
        float edge = frame.skyHorizon.w;
        float disc = smoothstep(edge - 0.00002, edge + 0.00002, facing);
        float glow = pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.12;
        color += frame.sunColor.rgb * (disc * 8.0 + glow);
    }
    return color;
}
