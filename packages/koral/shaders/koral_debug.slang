// What a material shows of itself in an editor's scene view, in place of its lit colour: its albedo, its normal, how
// rough it is. A shader that imports this has the specialization constant `koralDebugView`, which the scene view's
// runtime builds it with (kgraph::Runtime::SetShaderDebugView); the game's, and a project that is run, build it with
// 0 — the lit colour — so what is not shown costs nothing.
//
//   import koral_debug;
//   ...
//   if (koralDebugShowing()) return float4(koralDebugColor(surface), 1.0);
//
// A pass after the one that draws — a tone mapping, a bloom — imports it too, and leaves the picture as it is while
// koralDebugShowing(): what is shown is the channel, not the channel exposed, blurred and tone mapped.

module koral_debug;

import koral_color;

/** Which channel is shown: kgraph::ShaderDebugChannel. 0 shows the lit colour. */
[vk::constant_id(64)] public const int koralDebugView = 0;

public static const int KoralDebugLit = 0;
public static const int KoralDebugAlbedo = 1;
public static const int KoralDebugNormal = 2;
public static const int KoralDebugRoughness = 3;
public static const int KoralDebugMetallic = 4;
public static const int KoralDebugEmissive = 5;
public static const int KoralDebugOcclusion = 6;

/** What a surface is made of, at a point: each in the terms light adds up in (albedo and emissive linear). */
public struct KoralSurface {
    public float3 albedo;
    public float3 normal;       ///< In the world, of unit length: after a normal map.
    public float roughness;
    public float metallic;
    public float3 emissive;
    public float occlusion;     ///< 1 where nothing hides it.

    public __init(float3 albedo, float3 normal, float roughness, float metallic, float3 emissive = float3(0.0), float occlusion = 1.0)
    {
        this.albedo = albedo;
        this.normal = normal;
        this.roughness = roughness;
        this.metallic = metallic;
        this.emissive = emissive;
        this.occlusion = occlusion;
    }
};

/** Whether a channel of the material is shown rather than its lit colour. */
public bool koralDebugShowing() { return koralDebugView != KoralDebugLit; }

/**
 * The channel asked for, as the display shows it: a colour as it is seen (its linear terms back on the display's
 * curve), a direction as a colour (-1 to 1 as 0 to 1), a number as a grey.
 */
public float3 koralDebugColor(KoralSurface surface)
{
    switch (koralDebugView) {
    case KoralDebugAlbedo: return KoralGamma22FromLinear(saturate(surface.albedo));
    case KoralDebugNormal: return normalize(surface.normal) * 0.5 + 0.5;
    case KoralDebugRoughness: return float3(saturate(surface.roughness));
    case KoralDebugMetallic: return float3(saturate(surface.metallic));
    case KoralDebugEmissive: return KoralGamma22FromLinear(saturate(surface.emissive));
    case KoralDebugOcclusion: return float3(saturate(surface.occlusion));
    default: return float3(1.0, 0.0, 1.0);     // a channel this does not know: magenta, so as to be seen
    }
}
