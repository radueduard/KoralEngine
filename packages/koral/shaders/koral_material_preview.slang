// The editor's: a material on a mesh, in a cube map's light with two lights of its own. What the material inspector and
// the small pictures of materials draw with, whatever the material's kind: for each kind the editor generates a module
// of a few lines that names it and its shading module, and everything else is here. No project's graph is needed.

module koral_material_preview;

__exported import koral_shading;
__exported import koral_bindless;
import koral_display;
import koral_math;

public struct KoralPreviewView {
    public float4x4 viewProjection;
    public float4x4 inverseViewProjection;
    public float4x4 inverseView;
    public uint environment;    // the cube map's bindless index
    public float mips;          // how many mip levels it has
    public uint showAround;     // 1: what is behind the mesh is the cube map; 0: nothing
};

public ConstantBuffer<KoralPreviewView> view;
public SamplerState smooth;

public float3 KoralPreviewEye() { return view.inverseView[3].xyz; }

public struct KoralPreviewRaster {
    public float4 position : SV_Position;
    public float3 world;
    public float3 normal;
    public float2 uv;
    public nointerpolation uint instance;
};

public KoralPreviewRaster KoralPreviewVertex(float3 position, float3 normal, float2 uv, uint instance)
{
    KoralPreviewRaster raster;
    raster.position = mul(float4(position, 1.0), view.viewProjection);
    raster.world = position;
    raster.normal = normal;
    raster.uv = uv;
    raster.instance = instance;
    return raster;
}

public KoralCubeEnvironment KoralPreviewAround()
{
    return KoralCubeEnvironment(kor_TextureCube[NonUniformResourceIndex(view.environment)], smooth, view.mips);
}

/** Light as the picture shows it: exposed a little under, then as the engine shows light. */
float3 Shown(float3 light) { return KoralDisplay(light * 0.8); }

public float4 KoralPreviewColour<M : IKoralMaterial>(M material, KoralPreviewRaster raster, bool front)
{
    KoralSurface surface = KoralSurfaceAt(raster.world, front ? raster.normal : -raster.normal, raster.uv, smooth);
    const M.Shading shading = material.Read(surface);
    const float opacity = shading.Opacity();
    if (opacity <= 0.0) discard;
    float3 light = KoralLit(shading, surface, KoralCamera(KoralPreviewEye()), KoralStudioLights(), KoralPreviewAround());
    // Over what is behind it, as it is where it stands; with nothing behind it, by its own alpha.
    if (view.showAround != 0) {
        light = lerp(KoralPreviewAround().Specular(normalize(raster.world - KoralPreviewEye()), 0.0), light, opacity);
        return float4(Shown(light), 1.0);
    }
    return float4(Shown(light), opacity);
}

// ---- what is behind it ------------------------------------------------------------------------------------------------

public struct KoralPreviewSkyRaster {
    public float4 position : SV_Position;
    public float2 at;
};

[shader("vertex")]
public KoralPreviewSkyRaster skyVertex(uint index : SV_VertexID)
{
    KoralPreviewSkyRaster raster;
    raster.at = KoralFullscreenCorner(index);
    raster.position = float4(raster.at, 0.99999, 1.0);
    return raster;
}

[shader("fragment")]
public float4 skyFragment(KoralPreviewSkyRaster raster) : SV_Target
{
    const float4 far = mul(float4(raster.at, 1.0, 1.0), view.inverseViewProjection);
    const float3 direction = normalize(far.xyz / far.w - KoralPreviewEye());
    return float4(Shown(KoralPreviewAround().Specular(direction, 0.0)), 1.0);
}
