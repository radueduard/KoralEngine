// Forward+: what every pass of it shares. The camera's block, the lights, and the sun with its cascades — each
// declared here once and filled by a ShaderBuffer node of the graph, a field at a time by name.
//
// Koral's matrices are a column after another and Slang reads a row after another, so a vector goes first:
// mul(vector, matrix). A matrix's fourth row is then where it puts the origin.

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
    public uint lightCount;         // how many point lights there are
    public uint3 gridSize;          // the clusters lights are sorted into: across, down, and into the picture
};
public ConstantBuffer<Camera> camera;

public struct PointLight {
    public float3 position;
    public float radius;            // where its light ends
    public float3 color;
    public float intensity;
};

public static const uint CascadeCount = 4;
public static const uint LampShadowMost = 24;       // how many point lights have a shadow map: the first that many

// The point lights' shadow maps: a cube round each, its six faces one after another in an image of as many layers.
public struct LampShadows {
    public float4x4 faces[144];     // the world, as each face of each light's cube has it: towards +x, -x, +y, -y, +z, -z
    public float count;             // how many lights have a cube
    public float texel;             // one over a face's resolution
    public float bias;              // what is nearer the light by less than this, as the map counts depth, casts no shadow on itself
    public float softness;          // how soft a shadow's edge is, in pixels of a face
}

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

/** The sky seen along a direction: the ground below the horizon, the zenith above it, and the sun. */
public float3 skyColor(Sun sun, float3 direction)
{
    float y = direction.y;
    float3 color = y >= 0.0
        ? lerp(sun.skyHorizon, sun.skyZenith, pow(saturate(y), 0.45))
        : lerp(sun.skyHorizon, sun.skyGround, pow(saturate(-y * 4.0), 0.6));
    if (sun.present > 0.0) {
        float facing = dot(direction, -sun.direction);
        float disc = smoothstep(sun.sunCosine - 0.00002, sun.sunCosine + 0.00002, facing);
        float glow = pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.12;
        color += sun.color * (disc * 8.0 + glow);
    }
    return color;
}

/** The light the sky gives a surface facing [normal]: the ground's from below, the zenith's from above. */
public float3 skyLight(Sun sun, float3 normal)
{
    float up = normal.y * 0.5 + 0.5;
    float3 sky = lerp(sun.skyHorizon, sun.skyZenith, saturate(normal.y));
    return lerp(sun.skyGround, sky, smoothstep(0.0, 1.0, up)) * sun.ambient;
}
