// The editor's grid: a plane through the world's origin, its lines a cell apart and every `major` cells a stronger
// one, the two axes in it in their colours (X red, Y green, Z blue), fading out by `fade` metres off and where the
// plane is seen edge on. Drawn over a whole picture, a ray a pixel: where the ray meets the plane behind what the scene
// has drawn there (`sceneDepth`, when `depthTested`), nothing is.

struct GridView {
    float4x4 viewProjection;
    float4x4 inverseViewProjection;
};
ConstantBuffer<GridView> gridView;
Texture2D<float> sceneDepth;

struct Raster {
    float4 position : SV_Position;
    float2 clip;
};

[shader("vertex")]
Raster vertex(uint index : SV_VertexID)
{
    float2 corner = float2((index << 1) & 2, index & 2);
    Raster raster;
    raster.clip = corner * 2.0 - 1.0;
    raster.position = float4(raster.clip, 0.0, 1.0);
    return raster;
}

/** How much of a line there is at `at` — lines at every whole number of it — `width` pixels wide. */
float lines(float2 at, float width)
{
    float2 step = max(fwidth(at), 1e-6);
    float2 off = abs(frac(at - 0.5) - 0.5) / step;
    return 1.0 - saturate(min(off.x, off.y) / width);
}

/** How much of a line there is where `at` is 0, `width` pixels wide. */
float axis(float at, float width) { return 1.0 - saturate(abs(at) / max(fwidth(at), 1e-6) / width); }

[shader("fragment")]
float4 fragment(Raster raster, uniform float cell, uniform float major, uniform float fade, uniform float opacity, uniform int plane, uniform int depthTested) : SV_Target
{
    float4 nearPoint = mul(float4(raster.clip, 0.0, 1.0), gridView.inverseViewProjection);
    float4 farPoint = mul(float4(raster.clip, 1.0, 1.0), gridView.inverseViewProjection);
    float3 from = nearPoint.xyz / nearPoint.w, to = farPoint.xyz / farPoint.w;
    float3 ray = to - from;

    // Which axis the plane is square to, and the two in it: XZ (Y up), XY, YZ.
    int across = plane == 1 ? 2 : plane == 2 ? 0 : 1;
    int u = plane == 2 ? 1 : 0, v = plane == 1 ? 1 : 2;
    if (abs(ray[across]) < 1e-7) discard;
    float t = -from[across] / ray[across];
    if (t <= 0.0 || t >= 1.0) discard;
    float3 at = from + ray * t;

    float4 clip = mul(float4(at, 1.0), gridView.viewProjection);
    float depth = clip.z / clip.w;
    if (depthTested != 0 && depth > sceneDepth.Load(int3(int2(raster.position.xy), 0)) + 1e-7) discard;

    float2 onPlane = float2(at[u], at[v]);
    float size = max(cell, 1e-4);
    float2 inCells = onPlane / size;
    // Lines closer together than a few pixels fade, before they shimmer.
    float crowded = 1.0 - smoothstep(0.15, 0.45, max(fwidth(inCells).x, fwidth(inCells).y));
    float minorLine = lines(inCells, 1.0) * crowded;
    float majorLine = lines(inCells / max(major, 1.0), 1.4);

    const float3 axisColor[3] = { float3(0.92, 0.32, 0.32), float3(0.45, 0.85, 0.35), float3(0.32, 0.55, 0.95) };
    float3 color = float3(0.6, 0.6, 0.62);
    float alpha = max(minorLine * 0.28, majorLine * 0.55);
    float alongU = axis(onPlane.y, 1.6), alongV = axis(onPlane.x, 1.6);     // the line where v is 0 runs along u
    if (alongU > 0.0 && alongU >= alongV) { color = axisColor[u]; alpha = max(alpha, alongU * 0.9); }
    else if (alongV > 0.0) { color = axisColor[v]; alpha = max(alpha, alongV * 0.9); }

    float distance = length(at - from);
    float far = max(fade, 1e-3);
    alpha *= 1.0 - smoothstep(far * 0.45, far, distance);
    alpha *= saturate(abs(normalize(ray)[across]) * 6.0);     // edge on, a plane is all lines
    alpha *= saturate(opacity);
    if (alpha < 0.004) discard;
    return float4(color, alpha);
}
