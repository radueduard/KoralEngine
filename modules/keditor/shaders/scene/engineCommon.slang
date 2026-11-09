// What every pass of the lit graph sees of a frame, each written once a frame by a node of the graph's `frame` page:
// the camera (`camera data`, to a pipeline's `param.camera`), the sky's light from all round (`ambient light`, to
// `param.ambient`), and the scene's lights, a buffer of each kind (the engine's Lights function's `directional`, `points`
// and `spots`, to `param.directionalLights`, `param.pointLights` and `param.spotLights`). How many lights of each kind
// there are is a draw's push constant: the Lights function's `counts`, to a Draw's `push.lightCounts`. None is given a
// binding here, as the engine numbers what a pipeline's shaders declare.
//
// Light is worked in linear terms: the colours here are linear already (the graph converts them), a surface's colour is
// read with KoralLinearFromGamma22, and what is worked out goes to the display through KoralDisplay.

module engineCommon;

__exported import koral_shadings;
__exported import koral_display;

public ConstantBuffer<KoralCamera> camera;
public ConstantBuffer<KoralAmbientLight> ambient;
public StructuredBuffer<KoralDirectionalLight> directionalLights;
public StructuredBuffer<KoralPointLight> pointLights;
public StructuredBuffer<KoralSpotLight> spotLights;

/** Where the frame is seen from. */
public KoralCamera FrameCamera() { return camera; }

/** The sky's light from all round. */
public KoralAmbientLight FrameAmbient() { return ambient; }

/** The scene's lights, each kind by itself: @p counts how many of each (x directional, y point, z spot), a draw's push constant. */
public KoralLightBuffers FrameLights(uint3 counts) { return KoralLightBuffers(directionalLights, pointLights, spotLights, counts); }

/**
 * The sky seen along @p direction, in linear light: its colours, and the sun's disc and glow — the first directional
 * light's, when @p directionalCount has one — @p sunDisc the cosine of the disc's radius.
 */
public float3 SkyColor(float3 direction, uint directionalCount, float sunDisc)
{
    float3 color = ambient.Along(direction);
    if (directionalCount > 0) {
        const KoralDirectionalLight sun = directionalLights[0];
        const float facing = dot(direction, sun.ToLight());
        const float disc = smoothstep(sunDisc - 0.00002, sunDisc + 0.00002, facing);
        const float glow = pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.12;
        color += sun.color * (disc * 8.0 + glow);
    }
    return color;
}
