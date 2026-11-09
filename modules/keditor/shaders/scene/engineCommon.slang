// What every pass of the lit graph sees of a frame — written once a frame by the graph's `frame data`: the camera, the
// sky's light from all round, and how many lights of each kind there are — and the scene's lights, a buffer of each kind
// (the engine's Lights function's). `frame` is wired to a pipeline's `param.frame`, and the Lights function's
// `directional`, `points` and `spots` to its `param.directionalLights`, `param.pointLights` and `param.spotLights`; none is
// given a binding here, as the engine numbers what a pipeline's shaders declare.
//
// Light is worked in linear terms: the colours here are linear already (the graph converts them), a surface's colour is
// read with KoralLinearFromGamma22, and what is worked out goes to the display through KoralDisplay.

module engineCommon;

__exported import koral_shadings;
__exported import koral_display;

public struct FrameData {
    public KoralCamera camera;          // where the frame is seen from, and through what
    public KoralAmbientLight ambient;   // the Sky's light from all round
    public float4 sky;                  // x: the cosine of the sun disc's radius (the Sky's sunSize)
    public uint4 counts;                // the Lights function's: x directional, y point, z spot lights
};

public ConstantBuffer<FrameData> frame;
public StructuredBuffer<KoralDirectionalLight> directionalLights;
public StructuredBuffer<KoralPointLight> pointLights;
public StructuredBuffer<KoralSpotLight> spotLights;

/** Where the frame is seen from. */
public KoralCamera FrameCamera() { return frame.camera; }

/** The scene's lights: every one the Lights function found, each kind by itself. */
public KoralLightBuffers FrameLights() { return KoralLightBuffers(directionalLights, pointLights, spotLights, frame.counts.xyz); }

/** The sky's light from all round. */
public KoralAmbientLight FrameAmbient() { return frame.ambient; }

/** The sky seen along @p direction, in linear light: its colours, and the sun's disc and glow — the first directional light's. */
public float3 SkyColor(float3 direction)
{
    float3 color = frame.ambient.Along(direction);
    if (frame.counts.x > 0) {
        const KoralDirectionalLight sun = directionalLights[0];
        const float facing = dot(direction, sun.ToLight());
        const float edge = frame.sky.x;
        const float disc = smoothstep(edge - 0.00002, edge + 0.00002, facing);
        const float glow = pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.12;
        color += sun.color * (disc * 8.0 + glow);
    }
    return color;
}
