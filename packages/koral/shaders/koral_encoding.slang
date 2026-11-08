// The engine's base library: numbers packed small, for what is kept or sent many times over — a normal in a G-buffer,
// an instance's rotation, a colour in a uint.
//
//     import koral_encoding;
//
//     const float2 stored = KoralOctEncode(normal);       // a direction as two numbers from -1 to 1
//     const uint turned = KoralPackQuaternion(q);         // a rotation in 32 bits

module koral_encoding;

// ---- directions ----------------------------------------------------------------------------------------------------

/** A direction of unit length as two numbers from -1 to 1: the octahedron round it, unfolded onto a square. */
public float2 KoralOctEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, float2(1.0), float2(-1.0));
    return n.xy;
}

/** What KoralOctEncode made, a direction again, of unit length. */
public float3 KoralOctDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, float2(-t), float2(t));
    return normalize(n);
}

// ---- rotations ---------------------------------------------------------------------------------------------------

/** A quaternion in 32 bits: its three smallest parts, 10 bits each, and which part was left out — the largest, made positive. */
public uint KoralPackQuaternion(float4 q)
{
    const float4 a = abs(q);
    uint largest = 0;
    if (a.y > a[largest]) largest = 1;
    if (a.z > a[largest]) largest = 2;
    if (a.w > a[largest]) largest = 3;
    if (q[largest] < 0.0) q = -q;
    uint packed = largest;
    uint shift = 2;
    [unroll] for (uint i = 0; i < 4; ++i) {
        if (i == largest) continue;
        // Each of the others is within ±1/√2.
        const float unit = saturate(q[i] * 0.70710678 + 0.5);
        packed |= uint(round(unit * 1023.0)) << shift;
        shift += 10;
    }
    return packed;
}

public float4 KoralUnpackQuaternion(uint packed)
{
    const uint largest = packed & 3u;
    float4 q = 0.0;
    float sum = 0.0;
    uint shift = 2;
    [unroll] for (uint i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float part = (float((packed >> shift) & 1023u) / 1023.0 - 0.5) * 1.41421356;
        q[i] = part;
        sum += part * part;
        shift += 10;
    }
    q[largest] = sqrt(max(1.0 - sum, 0.0));
    return q;
}

// ---- numbers in fewer bits ----------------------------------------------------------------------------------------

/** Two floats as halves in one uint: the first in the low 16 bits. */
public uint KoralPackHalf2(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
public float2 KoralUnpackHalf2(uint packed) { return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16)); }

/** Four numbers from 0 to 1 in 8 bits each: the first in the low byte. */
public uint KoralPackUnorm4x8(float4 v)
{
    const uint4 b = uint4(round(saturate(v) * 255.0));
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}
public float4 KoralUnpackUnorm4x8(uint packed)
{
    return float4(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u, packed >> 24) / 255.0;
}

/** Two numbers from -1 to 1 in 16 bits each. */
public uint KoralPackSnorm2x16(float2 v)
{
    const int2 s = int2(round(clamp(v, -1.0, 1.0) * 32767.0));
    return (uint(s.x) & 0xFFFFu) | (uint(s.y) << 16);
}
public float2 KoralUnpackSnorm2x16(uint packed)
{
    const int2 s = int2(int(packed << 16) >> 16, int(packed) >> 16);
    return max(float2(s) / 32767.0, -1.0);
}

/** A colour of any brightness in 32 bits: 8 bits each and an exponent they share (Ward's RGBE). */
public uint KoralPackRgbe(float3 c)
{
    const float largest = max(c.r, max(c.g, c.b));
    if (largest < 1e-32) return 0u;
    int exponent;
    const float mantissa = frexp(largest, exponent);
    const float3 scaled = c * (mantissa * 256.0 / largest);
    const uint3 b = uint3(min(scaled, 255.0));
    return b.r | (b.g << 8) | (b.b << 16) | (uint(exponent + 128) << 24);
}
public float3 KoralUnpackRgbe(uint packed)
{
    if ((packed >> 24) == 0u) return float3(0.0);
    const float scale = ldexp(1.0, int(packed >> 24) - 136);
    return float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u) * scale;
}
