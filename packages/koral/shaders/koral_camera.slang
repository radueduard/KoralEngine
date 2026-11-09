// The engine's base library: the camera a frame is seen through — where it is, and the view and lens that put the world
// in the picture and take a pixel back out of it. 144 bytes, as a frame's block holds it.
//
//     import koral_camera;
//
//     raster.position = frame.camera.ToClip(world);
//     const float3 direction = frame.camera.RayThrough(clip).direction;
//
// The matrices are the engine's: a vector goes first, mul(vector, matrix).

module koral_camera;

import koral_math;
__exported import koral_depth;

public struct KoralCamera {
    public float4x4 viewProjection;         // the lens after the view: the world into clip space
    public float4x4 inverseViewProjection;  // what undoes it
    public float3 position;                 // where it is, in the world
    public float unused;

    /** A camera known only by where it is — a preview's, a thumbnail's — seeing through nothing. */
    public __init(float3 position)
    {
        viewProjection = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
        inverseViewProjection = viewProjection;
        this.position = position;
        unused = 0.0;
    }
    public __init(float4x4 viewProjection, float4x4 inverseViewProjection, float3 position)
    {
        this.viewProjection = viewProjection;
        this.inverseViewProjection = inverseViewProjection;
        this.position = position;
        unused = 0.0;
    }

    /** Where a point of the world is in clip space. */
    public float4 ToClip(float3 world) { return mul(float4(world, 1.0), viewProjection); }
    /** The way from a point of the world to the camera, a unit long. */
    public float3 ToEye(float3 world) { return KoralSafeNormalize(position - world); }
    /** The ray from the camera through clip-space @p clip (x and y from -1 to 1). */
    public KoralRay RayThrough(float2 clip) { return KoralRayThrough(clip, inverseViewProjection); }
    /** Where in the world what a pixel at @p uv (0 to 1) of depth @p depth shows is. */
    public float3 WorldAt(float2 uv, float depth) { return KoralWorldFromDepth(uv, depth, inverseViewProjection); }
};
