// The camera-effects module: what a real camera's lens and film do to a picture — depth of field (how out of focus a
// point is, its circle of confusion), the camera's own motion blurring it, colours parted at the edges (chromatic
// aberration), the corners darkened (vignetting), grain, and a flare's ghosts. camera_effects_passes runs them over a
// picture of light, before it is tone mapped.
//
//     import camera_effects;
//
//     const float coc = KoralCircleOfConfusion(distance, focus, aperture, focalLength);

module camera_effects;

import koral_math;
import koral_color;
import koral_random;

/**
 * How wide, in the picture's height, a point @p distance away is blurred by a lens of @p focalLength (metres) at f-number
 * @p fStop focused at @p focus: a thin lens's circle of confusion on a sensor @p sensorHeight high (24 mm by default).
 */
public float KoralCircleOfConfusion(float distance, float focus, float fStop, float focalLength, float sensorHeight = 0.024)
{
    const float aperture = focalLength / max(fStop, 0.1);
    const float coc = aperture * focalLength * (distance - focus) / max(distance * (focus - focalLength), 1e-6);
    return coc / sensorHeight;
}

/** What chromatic aberration reads: the picture's red, green and blue, each from @p uv moved out from the middle by its own amount. */
public float3 KoralChromaticAberration(Texture2D<float4> picture, SamplerState linear, float2 uv, float strength)
{
    const float2 fromMiddle = uv - 0.5;
    const float2 shift = fromMiddle * dot(fromMiddle, fromMiddle) * strength;
    return float3(picture.SampleLevel(linear, uv - shift, 0.0).r, picture.SampleLevel(linear, uv, 0.0).g, picture.SampleLevel(linear, uv + shift, 0.0).b);
}

/** How much of the light reaches a point of the picture: less towards the corners, by @p strength, the cos⁴ law softened. */
public float KoralVignette(float2 uv, float strength, float aspect = 1.0)
{
    const float2 d = (uv - 0.5) * float2(aspect, 1.0);
    const float c = 1.0 / (1.0 + dot(d, d) * strength);
    return c * c;
}

/** Film's grain at a pixel in a frame: noise about 0, more in the middle tones than in black or white. */
public float KoralGrain(uint2 pixel, uint frame, float luminance)
{
    const float n = KoralRandom2(pixel, frame).x + KoralRandom2(pixel + 1031, frame).y - 1.0;
    return n * 4.0 * luminance * (1.0 - saturate(luminance));
}
