// Forward+: what every pass of it shares. The camera's block and the sun with its cascades — each declared here once
// and filled by a ShaderBuffer node of the graph, a field at a time by name — and the engine's library, which has the
// rest: the lights (KoralLight, the Lights function's), the lobes, the shadows' filters, the sky's gradient.
//
// Koral's matrices are a column after another and Slang reads a row after another, so a vector goes first:
// mul(vector, matrix). A matrix's fourth row is then where it puts the origin.

__exported import entity;
__exported import koral_bindless;
__exported import PbrMaterial;     // what each thing drawn is made of: the engine writes it of the kind
__exported import koral_math;       // the library: numbers, colour, lights, lobes, environments, shadows...
__exported import koral_color;
__exported import koral_brdf;
__exported import koral_lights;
__exported import koral_shading;
__exported import koral_shadows;
__exported import koral_random;
__exported import koral_sampling;
__exported import koral_bounds;
__exported import koral_depth;

public struct Camera {
    public float4x4 view;
    public float4x4 projection;
    public float4x4 inverseView;
    public float4x4 inverseProjection;
    public float4x4 viewProjection;
    public float3 position;
    public float zNear;
    public float2 screenSize;
    public float zFar;
    public uint lightCount;         // how many lights there are: the Lights function's, the sun's first
    public uint3 gridSize;          // the clusters lights are sorted into: across, down, and into the picture
};
public ConstantBuffer<Camera> camera;

public static const uint CascadeCount = 4;
public static const uint LampShadowMost = 24;       // how many point lights have a shadow map: the first that many

public struct Sun {
    public float4x4 lightViewProj[4];   // the world, as each cascade's shadow map has it
    public float4 splits;               // how far from the camera each cascade reaches
    public float4 worldTexel;           // how much of the world a pixel of each cascade's map covers
    public float3 direction;            // the way the light travels
    public float present;               // 1 when there is a sun
    public float3 color;                // times its intensity
    public float bias;                  // what is nearer the light by less than this casts no shadow on itself
    public float pcfRadius;             // how soft a shadow's edge is, in pixels of the nearest cascade
    public float tileTexel;             // one over a cascade's resolution
    public float blend;                 // the part of each cascade, before its end, faded into the next
    public float ambient;               // how much light the sky gives all round
    public float3 skyZenith;
    public float sunCosine;             // the cosine of the sun disc's radius
    public float3 skyHorizon;
    public float3 skyGround;
};

/**
 * An entity the pre-pass and the forward pass draw, one indirect draw for them all: where it is, and the fields of its
 * `forward` component — its pictures by the bindless index the engine gives each (~0 for none).
 */
public struct Instance {
    [entity("MATRIX")] public float4x4 model;
    [PbrMaterial("baseColor")] public float4 baseColor;    // its colour, as picked; a: how opaque
    [PbrMaterial] public float metallic;
    [PbrMaterial] public float3 emission;     // the light it gives off itself, as light: above 1 it blooms
    [PbrMaterial] public float roughness;
    [PbrMaterial] public int alphaMode;       // 0 opaque; 1 cut out, where its albedoMap is less opaque than alphaCutoff; 2 drawn over what is behind it — glass
    [PbrMaterial] public float alphaCutoff;
    [PbrMaterial("baseColorMap")] public uint albedoMap;            // its colour; a: how opaque
    [PbrMaterial] public uint normalMap;                            // which way its surface faces, texel by texel, relative to the mesh's own
    [PbrMaterial("metallicRoughnessMap")] public uint roughnessMap; // g: how rough; b: how much a metal
    [PbrMaterial] public uint emissiveMap;
    public float translucency;  // how much light falling on its back shows through on its front, tinted by its colour: cloth, a leaf, paper
};

/** Whether it is cut out where its picture is less opaque than its alphaCutoff. */
public bool isCutOut(Instance instance) { return instance.alphaMode == 1; }
/** How much of what is behind it shows through, over what its albedoMap lets: 0 none, 1 all of it — glass. */
public float transparencyOf(Instance instance) { return instance.alphaMode == 2 ? saturate(1.0 - instance.baseColor.a) : 0.0; }

/**
 * What a surface is made of, as rays see it: the ray tracer's own material — its component, which each entity holds as
 * it holds the forward pass's — an element an entity of the TraceRays' `entities`, InstanceID() in the scene. Its fields
 * are the forward pass's of the same names, and one of its own.
 */
public struct RayMaterial {
    [entity("MATRIX")] public float4x4 model;
    [PbrMaterial("baseColor")] public float4 baseColor;    // its colour, as picked
    [PbrMaterial] public float metallic;
    [PbrMaterial] public float3 emission;     // the light it gives off itself
    [PbrMaterial] public float roughness;
    [PbrMaterial] public int alphaMode;       // 1: cut out — where its albedoMap is less opaque than alphaCutoff, rays pass through
    [PbrMaterial] public float alphaCutoff;
    public float reflectionBlur;       // how much more its reflection rays spread than its roughness says: 0 as rough as it is
    [PbrMaterial("baseColorMap")] public uint albedoMap;
    [PbrMaterial] public uint normalMap;
    [PbrMaterial("metallicRoughnessMap")] public uint roughnessMap;   // g: how rough; b: how much a metal
    [PbrMaterial] public uint emissiveMap;
    public float translucency;         // how much light falling on its back shows through on its front, tinted by its colour
};
/**
 * Something that gives off light, as the ray tracer finds it: an instance of its scene whose material's `emission` is
 * more than nothing — emitters.slang lists them each frame — with how many triangles it has to give it off from, how
 * much it gives off (its emission's brightness by how big it is) and the sphere it is in, to judge from afar.
 */
public struct Emitter {
    public uint instance;       // its row of the ray tracer's materials: InstanceID() in the scene
    public uint triangles;
    public float power;
    public float radius;
    public float3 center;
    public float unused;
};

/** One of an instance's pictures at @p uv: @p none where it has no picture there. */
public float4 pictureOf(uint index, SamplerState sampler, float2 uv, float4 none = float4(1.0, 1.0, 1.0, 1.0))
{
    return index == ~0u ? none : kor::texture2D(index).Sample(sampler, uv);
}

/**
 * What the frame carries over from the last one, for the temporal antialiasing: the camera's matrix without the
 * jitter this frame and the last, and the jitter — a different fraction of a pixel each frame, which the pre-pass and
 * the forward pass move the picture by. Written once a frame by taa.slang's `advance`.
 */
public struct FrameState {
    public float4x4 viewProjection;             // this frame's, without the jitter
    public float4x4 previousViewProjection;     // last frame's, without the jitter
    public float2 jitter;                       // this frame's, in clip space: two over the picture's size, times a fraction of a pixel
    public uint frame;                          // how many frames there have been
    public uint history;                        // 1 when last frame's picture is there to be blended with
    public float2 size;                         // the picture's size, in pixels
    public uint begun;                          // Begun once it has been written: before, what is in it is nothing
};
public static const uint Begun = 0x7A11u;

/** Where a point of the world is in the picture this frame: moved by the frame's jitter, for the antialiasing to gather. */
public float4 toPicture(float3 world, FrameState state)
{
    float4 clip = toClip(world);
    if (state.begun == Begun) clip.xy += state.jitter * clip.w;
    return clip;
}

/** Where a point of the world is in the picture. One function for every pass, so that each draws a thing at the very same depth. */
public float4 toClip(float3 world)
{
    return mul(float4(world, 1.0), camera.viewProjection);
}

/** How far in front of the camera a point of the world is. */
public float viewDepth(float3 world)
{
    return -mul(float4(world, 1.0), camera.view).z;
}

/** The sky: its three colours, as the library's gradient environment has them, giving `ambient` all round. */
public KoralGradientEnvironment skyOf(Sun sun) { return KoralGradientEnvironment(sun.skyZenith, sun.skyHorizon, sun.skyGround, sun.ambient); }

/** The sky seen along a direction: the ground below the horizon, the zenith above it, and the sun. */
public float3 skyColor(Sun sun, float3 direction)
{
    float3 color = skyOf(sun).Along(direction);
    if (sun.present > 0.0) {
        float facing = dot(direction, -sun.direction);
        float disc = smoothstep(sun.sunCosine - 0.00002, sun.sunCosine + 0.00002, facing);
        float glow = pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.12;
        color += sun.color * (disc * 8.0 + glow);
    }
    return color;
}

/** The light the sky gives a surface facing [normal]: the ground's from below, the zenith's from above. */
public float3 skyLight(Sun sun, float3 normal) { return skyOf(sun).Diffuse(normal); }

/** The sun's cascades as koral_shadows reads them, and how they are filtered: as the block says. */
public KoralSunCascades cascadesOf(Sun sun)
{
    KoralSunCascades c;
    c.cascades = sun.lightViewProj;
    c.splits = sun.splits;
    c.worldTexel = sun.worldTexel;
    return c;
}
public KoralShadowFilter sunFilterOf(Sun sun)
{
    KoralShadowFilter f = KoralShadowFilter();
    f.bias = sun.bias / 0.0015;         // the library's bias is in steps of 0.0015 of the map's depth
    f.softness = sun.pcfRadius;
    f.cascadeBlend = sun.blend;
    return f;
}

/** How the lamps' shadows are filtered: soft by a little more than a texel of a face. */
public KoralShadowFilter lampFilter() { KoralShadowFilter f = KoralShadowFilter(); f.softness = 1.25; return f; }
