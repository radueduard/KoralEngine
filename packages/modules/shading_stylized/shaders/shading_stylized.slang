// The shading-stylized module: surfaces drawn as a hand would rather than as light falls — toon bands, a rim of light,
// Gooch's warm and cool, hatching — each an IKoralShading (koral_shading), lit by KoralLit as any is. The Outline function
// draws lines where depth or normals break (shading_stylized_passes).
//
//     import shading_stylized;
//
//     KoralToon toon = KoralToon(normal, colour, 3);       // three bands of light

module shading_stylized;

__exported import koral_shadings;
import koral_math;

/** How much light a band of @p bands takes of what reaches a surface: stepped, its edges @p softness wide. */
public float KoralBands(float light, uint bands, float softness = 0.02)
{
    const float scaled = saturate(light) * float(max(bands, 1u));
    const float step = floor(scaled), within = scaled - step;
    return (step + smoothstep(0.5 - softness * float(bands), 0.5 + softness * float(bands), within)) / float(max(bands, 1u));
}

/** A rim of light round a shape's edge as the eye sees it: 1 where it is seen edge on, @p power how thin. */
public float KoralRim(float3 normal, float3 toEye, float power = 4.0) { return pow(1.0 - saturate(dot(normal, toEye)), power); }

/** Toon: light in @p bands steps, a hard highlight, and a rim — a cartoon's surface. */
public struct KoralToon : IKoralShading {
    public float3 normal;
    public float3 color;
    public uint bands;
    public float highlight;         // how big the highlight is: 0 none, 1 half the surface
    public float3 rimColor;
    public __init(float3 normal, float3 color, uint bands)
    {
        this.normal = normal; this.color = color; this.bands = bands; this.highlight = 0.1; this.rimColor = float3(0.0);
    }

    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const float nl = dot(normal, toLight);
        const float lit = KoralBands(nl * 0.5 + 0.5, bands);
        const float nh = dot(normal, KoralSafeNormalize(toEye + toLight, normal));
        const float shine = highlight > 0.0 ? smoothstep(1.0 - highlight * 0.1 - 0.005, 1.0 - highlight * 0.1 + 0.005, nh) : 0.0;
        return (color * lit + shine) * light * KoralInvPi;
    }
    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye)
    {
        return color * environment.Diffuse(normal) + rimColor * KoralRim(normal, toEye);
    }
    public float3 Emitted() { return 0.0; }
    public float Opacity() { return 1.0; }
};

/** Gooch's: cool where it faces away from the light, warm where it faces it — shape shown without dark shadows, as a technical drawing. */
public struct KoralGooch : IKoralShading {
    public float3 normal;
    public float3 color;
    public float3 cool;
    public float3 warm;
    public __init(float3 normal, float3 color)
    {
        this.normal = normal; this.color = color; this.cool = float3(0.0, 0.0, 0.55); this.warm = float3(0.3, 0.3, 0.0);
    }
    public float3 Reflected(float3 toEye, float3 toLight, float3 light)
    {
        const float t = (1.0 + dot(normal, toLight)) * 0.5;
        const float3 c = lerp(cool + color * 0.25, warm + color * 0.5, t);
        return c * KoralLuminance(light) * KoralInvPi;
    }
    public float3 Ambient<E : IKoralEnvironment>(E environment, float3 toEye) { return 0.0; }
    public float3 Emitted() { return 0.0; }
    public float Opacity() { return 1.0; }
};

/**
 * Hatching at a pixel: lines drawn across the picture, @p spacing pixels apart, more of them crossing as @p light falls
 * (1 none, 0 all four ways) — 1 paper, 0 ink.
 */
public float KoralHatching(float2 pixel, float light, float spacing = 6.0, float width = 1.0)
{
    const float2 ways[4] = { float2(0.7071, 0.7071), float2(0.7071, -0.7071), float2(1.0, 0.0), float2(0.0, 1.0) };
    float ink = 0.0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        if (light > 1.0 - 0.22 * float(i + 1)) break;
        // How far the pixel is from the nearest line of this way, in pixels; a line is `width` wide, its edges smooth.
        const float apart = abs(frac(dot(pixel, ways[i]) / spacing) - 0.5) * spacing;
        ink = max(ink, 1.0 - smoothstep(width * 0.5 - 0.5, width * 0.5 + 0.5, apart));
    }
    return 1.0 - ink;
}
