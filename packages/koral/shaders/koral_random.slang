// The engine's base library: numbers that look random, made from a number — the same every time from the same one, so
// a frame, a pixel or an instance always gets its own — and sequences that cover a range evenly.
//
//     import koral_random;
//
//     const float jitter = KoralRandom(instance, seed, 0);        // 0 to 1, a stream of its own for each use
//     const float2 u = KoralR2(frame);                            // the next of an even sequence over the square

module koral_random;

/** PCG's output function, of a step of its generator: a uint well mixed from any uint. */
public uint KoralPcgHash(uint x)
{
    x = x * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}

/** Three uints mixed together, each into all (Jarzynski and Olano's pcg3d): for a pixel and a frame at once. */
public uint3 KoralPcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

/** xxHash32 of one uint: the best mixed of these for its cost, as a key's hash. */
public uint KoralXxHash32(uint p)
{
    const uint prime2 = 2246822519u, prime3 = 3266489917u, prime4 = 668265263u, prime5 = 374761393u;
    uint h = p + prime5;
    h = prime4 * ((h << 17) | (h >> 15));
    h = prime2 * (h ^ (h >> 15));
    h = prime3 * (h ^ (h >> 13));
    return h ^ (h >> 16);
}

/** A uint's top 24 bits as a float from 0 up to 1: every value a float can hold exactly. */
public float KoralUnitFloat(uint bits) { return float(bits >> 8) / 16777216.0; }

/** A number from 0 up to 1 for @p index, of @p seed, from stream @p stream of up to four: one for each thing it decides. */
public float KoralRandom(uint index, uint seed, uint stream)
{
    return KoralUnitFloat(KoralPcgHash(index * 4u + stream + KoralPcgHash(seed)));
}

/** Two numbers from 0 up to 1 for a pixel in a frame. */
public float2 KoralRandom2(uint2 pixel, uint frame) { const uint3 h = KoralPcg3d(uint3(pixel, frame)); return float2(KoralUnitFloat(h.x), KoralUnitFloat(h.y)); }

// ---- even sequences ----------------------------------------------------------------------------------------------

/** The @p index'th of Halton's sequence in @p base: 0 to 1, each next one in the widest gap left. */
public float KoralHalton(uint index, uint base)
{
    float f = 1.0, r = 0.0;
    uint i = index;
    while (i > 0) {
        f /= float(base);
        r += f * float(i % base);
        i /= base;
    }
    return r;
}

/** The @p index'th of Roberts's R2 sequence over the unit square: as even as any, and one multiply each. */
public float2 KoralR2(uint index)
{
    return frac(0.5 + float(index) * float2(0.7548776662466927, 0.5698402909980532));
}

/** Jimenez's interleaved gradient noise at a pixel: a different number at each, with no two the same nearby. */
public float KoralInterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}
