// What every pass of the lit graph sees of a frame: the camera, the sky and the sun — written once a frame by the
// graph's `frame data` — and the scene's lights, the engine's Lights function's. `frame` is wired to a pipeline's
// `param.frame` and `lights` to its `param.lights`; neither is given a binding here, as the engine numbers what a
// pipeline's shaders declare.
//
// Light is worked in linear terms: the colours here are linear already (the graph converts them), a surface's colour is
// read with KoralLinearFromGamma22, and what is worked out goes to the display through KoralDisplay.

module engineCommon;

__exported import koral_shadings;
__exported import koral_display;

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

public ConstantBuffer<FrameData> frame;
public StructuredBuffer<KoralLight> lights;

/** Where the frame is seen from. */
public KoralCamera FrameCamera() { return KoralCamera(frame.cameraPosition.xyz); }

/** The scene's lights: every one the Lights function found. */
public KoralLightBuffer FrameLights() { return KoralLightBuffer(lights, frame.counts.x); }

/** The sky, as light from all round: the ground's from below, the zenith's from above, times how much light it gives. */
public KoralGradientEnvironment FrameSky() { return KoralGradientEnvironment(frame.skyZenith.rgb, frame.skyHorizon.rgb, frame.skyGround.rgb, frame.skyZenith.w); }

/** The sky seen along @p direction, in linear light: its colours, and the sun's disc and glow. */
public float3 SkyColor(float3 direction)
{
    float3 color = FrameSky().Along(direction);
    if (frame.sunDirection.w > 0.0) {
        const float facing = dot(direction, frame.sunDirection.xyz);
        const float edge = frame.skyHorizon.w;
        const float disc = smoothstep(edge - 0.00002, edge + 0.00002, facing);
        const float glow = pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.12;
        color += frame.sunColor.rgb * (disc * 8.0 + glow);
    }
    return color;
}
