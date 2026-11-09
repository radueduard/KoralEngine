// The noise module: patterns that look natural and are the same every time from the same place — value, gradient
// (Perlin's), simplex and cellular (Worley's) noise, layered (fractal Brownian motion), warped, and the curl of one for
// what flows. Each repeats nowhere unless asked (the Tiled ones repeat every `period`).
//
//     import noise;
//
//     const float h = KoralFbm(position * 0.1, 5);                // -1 to 1, roughly: a height, a cloud's density
//     const float2 cell = KoralWorley(position).xy;               // the nearest seed's distance and the next's

module noise;

import koral_math;
import koral_random;

// ---- hashes of a lattice point -----------------------------------------------------------------------------------

float3 Hash3(int3 p) { return float3(KoralPcg3d(uint3(p) + 0x9E3779B9u)) / 4294967295.0; }
float Hash1(int3 p) { return float(KoralPcg3d(uint3(p) + 0x9E3779B9u).x) / 4294967295.0; }

/** A gradient of unit length at a lattice point: one of twelve, the cube's edges' middles. */
float3 Gradient(int3 p)
{
    const uint h = KoralPcg3d(uint3(p) + 0x85EBCA6Bu).x % 12u;
    const float3 g[12] = { float3(1, 1, 0), float3(-1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0), float3(1, 0, 1), float3(-1, 0, 1),
                           float3(1, 0, -1), float3(-1, 0, -1), float3(0, 1, 1), float3(0, -1, 1), float3(0, 1, -1), float3(0, -1, -1) };
    return g[h] * 0.70710678;
}

/** Perlin's quintic: no kink at a lattice point, nor in its slope. */
float3 Fade(float3 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

int3 Wrap(int3 p, int3 period) { return period.x > 0 ? ((p % period) + period) % period : p; }

// ---- value and gradient noise -------------------------------------------------------------------------------------

/** Value noise: a number at each lattice point, blended between them. 0 to 1. */
public float KoralValueNoise(float3 p)
{
    const int3 i = int3(floor(p));
    const float3 f = Fade(frac(p));
    const float a = lerp(lerp(Hash1(i), Hash1(i + int3(1, 0, 0)), f.x), lerp(Hash1(i + int3(0, 1, 0)), Hash1(i + int3(1, 1, 0)), f.x), f.y);
    const float b = lerp(lerp(Hash1(i + int3(0, 0, 1)), Hash1(i + int3(1, 0, 1)), f.x), lerp(Hash1(i + int3(0, 1, 1)), Hash1(i + int3(1, 1, 1)), f.x), f.y);
    return lerp(a, b, f.z);
}

float PerlinAt(float3 p, int3 period)
{
    const int3 i = int3(floor(p));
    const float3 f = frac(p), u = Fade(f);
    float corner[8];
    [unroll] for (uint c = 0; c < 8; ++c) {
        const int3 o = int3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        corner[c] = dot(Gradient(Wrap(i + o, period)), f - float3(o));
    }
    const float x00 = lerp(corner[0], corner[1], u.x), x10 = lerp(corner[2], corner[3], u.x);
    const float x01 = lerp(corner[4], corner[5], u.x), x11 = lerp(corner[6], corner[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z) * 1.4142;
}

/** Perlin's gradient noise: smooth, its slope at each lattice point a gradient of its own. About -1 to 1, 0 at a lattice point. */
public float KoralPerlin(float3 p) { return PerlinAt(p, int3(0, 0, 0)); }
public float KoralPerlin(float2 p) { return PerlinAt(float3(p, 0.37), int3(0, 0, 0)); }

/** The same, repeating every @p period lattice cells (whole numbers): for a texture that tiles. */
public float KoralPerlinTiled(float3 p, int3 period) { return PerlinAt(p, max(period, int3(1, 1, 1))); }

// ---- simplex noise -----------------------------------------------------------------------------------------------

/** Simplex noise (Gustavson's form): gradient noise over tetrahedra, four corners rather than eight. About -1 to 1. */
public float KoralSimplex(float3 p)
{
    const float f3 = 1.0 / 3.0, g3 = 1.0 / 6.0;
    const float3 s = floor(p + dot(p, float3(f3)));
    const float3 x0 = p - s + dot(s, float3(g3));
    const float3 e = step(float3(0.0), x0 - x0.yzx);
    const float3 i1 = e * (1.0 - e.zxy), i2 = 1.0 - e.zxy * (1.0 - e);
    const float3 x1 = x0 - i1 + g3, x2 = x0 - i2 + 2.0 * g3, x3 = x0 - 0.5;
    const int3 base = int3(s);
    const float4 w = max(0.6 - float4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    const float4 d = float4(dot(Gradient(base), x0), dot(Gradient(base + int3(i1)), x1), dot(Gradient(base + int3(i2)), x2), dot(Gradient(base + 1), x3));
    const float4 w4 = w * w * w * w;
    return 32.0 * dot(w4, d);
}

public float KoralSimplex(float2 p) { return KoralSimplex(float3(p, 0.61)); }

// ---- cellular noise ----------------------------------------------------------------------------------------------

/**
 * Worley's cellular noise: a seed somewhere in each lattice cell, and how far @p p is from the nearest (x) and the
 * second nearest (y); z is the nearest's own number, 0 to 1 — a colour for each cell.
 */
public float3 KoralWorley(float3 p)
{
    const int3 i = int3(floor(p));
    const float3 f = frac(p);
    float first = 1e9, second = 1e9, which = 0.0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const int3 o = int3(x, y, z);
                const float3 seed = float3(o) + Hash3(i + o) - f;
                const float d = dot(seed, seed);
                if (d < first) { second = first; first = d; which = Hash1(i + o + 17); }
                else if (d < second) second = d;
            }
    return float3(sqrt(first), sqrt(second), which);
}

// ---- layered, warped, curled -------------------------------------------------------------------------------------

/** @p octaves of Perlin's noise, each @p lacunarity times as fine and @p gain times as strong: about -1 to 1. */
public float KoralFbm(float3 p, uint octaves, float lacunarity = 2.0, float gain = 0.5)
{
    float sum = 0.0, amplitude = 1.0, total = 0.0;
    for (uint i = 0; i < octaves; ++i) {
        sum += amplitude * KoralPerlin(p);
        total += amplitude;
        p = p * lacunarity + float3(17.0, -11.0, 5.0);
        amplitude *= gain;
    }
    return sum / max(total, 1e-6);
}

/** The same of cellular noise, inverted: billows, as clouds' tops are. 0 to 1. */
public float KoralWorleyFbm(float3 p, uint octaves, float lacunarity = 2.0, float gain = 0.5)
{
    float sum = 0.0, amplitude = 1.0, total = 0.0;
    for (uint i = 0; i < octaves; ++i) {
        sum += amplitude * (1.0 - saturate(KoralWorley(p).x));
        total += amplitude;
        p *= lacunarity;
        amplitude *= gain;
    }
    return sum / max(total, 1e-6);
}

/** @p p moved by noise of itself before it is looked up (Quílez's domain warping): a pattern folded, as marble is. */
public float3 KoralDomainWarp(float3 p, float strength, uint octaves = 4)
{
    const float3 q = float3(KoralFbm(p, octaves), KoralFbm(p + float3(5.2, 1.3, 2.8), octaves), KoralFbm(p + float3(1.7, 9.2, 4.1), octaves));
    return p + strength * q;
}

/** The curl of a field of noise: a flow with nowhere it gathers or thins (Bridson's), for smoke, sparks, wind. */
public float3 KoralCurl(float3 p)
{
    const float e = 0.01;
    const float3 dx = float3(e, 0.0, 0.0), dy = float3(0.0, e, 0.0), dz = float3(0.0, 0.0, e);
    const float3 o1 = float3(31.4, 12.7, 5.9), o2 = float3(-7.3, 22.1, 13.6);
    // Three fields of noise, each the curl's part about an axis.
    const float a_dy = KoralPerlin(p + dy) - KoralPerlin(p - dy), a_dz = KoralPerlin(p + dz) - KoralPerlin(p - dz);
    const float b_dx = KoralPerlin(p + o1 + dx) - KoralPerlin(p + o1 - dx), b_dz = KoralPerlin(p + o1 + dz) - KoralPerlin(p + o1 - dz);
    const float c_dx = KoralPerlin(p + o2 + dx) - KoralPerlin(p + o2 - dx), c_dy = KoralPerlin(p + o2 + dy) - KoralPerlin(p + o2 - dy);
    return float3(c_dy - b_dz, a_dz - c_dx, b_dx - a_dy) / (2.0 * e);
}
