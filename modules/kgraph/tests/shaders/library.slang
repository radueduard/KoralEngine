// The engine's base library, checked on the GPU: each check writes what it got, what it should be and how near is near
// enough, and test_library.cpp names them. The order of the checks here is the order of the names there.

import koral_math;
import koral_color;
import koral_display;
import koral_random;
import koral_encoding;
import koral_sampling;
import koral_bounds;
import koral_depth;

RWStructuredBuffer<float4> results;

static uint next = 0;
void Check(float got, float expected, float tolerance) { results[next++] = float4(got, expected, tolerance, 1.0); }

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    // ---- koral_math ------------------------------------------------------------------------------------------------
    Check(KoralSquare(3.0), 9.0, 0.0);
    Check(KoralRemap(5.0, 0.0, 10.0, 100.0, 200.0), 150.0, 1e-4);
    Check(KoralRemapClamped(20.0, 0.0, 10.0, 100.0, 200.0), 200.0, 1e-4);
    Check(length(KoralSafeNormalize(float3(0.0))), 1.0, 1e-6);
    {   // a normal stays square to a surface however unevenly it is scaled
        const float4x4 squashed = float4x4(float4(4, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, 0.5, 0), float4(1, 2, 3, 1));
        const float3 along = KoralTransformDirection(normalize(float3(1, -1, 0)), squashed);     // a direction in the surface x = y
        Check(dot(KoralTransformNormal(normalize(float3(1, 1, 0)), squashed), along), 0.0, 1e-5);
        Check(length(KoralTransformNormal(float3(0, 0, 1), squashed)), 1.0, 1e-5);
    }
    {   // a quaternion's matrix turns as the quaternion does, and places
        const float4 q = float4(0.0, sin(0.6), 0.0, cos(0.6));
        const float3 v = float3(1, 2, 3);
        const float3 turned = mul(float4(v, 1.0), KoralMatrixOf(q, 1.0, float3(0.0))).xyz;
        Check(distance(turned, KoralRotate(q, v)), 0.0, 1e-5);
        Check(KoralTransformPoint(float3(0.0), KoralMatrixOf(q, 2.0, float3(5, 6, 7))).y, 6.0, 1e-6);
    }
    {   // a matrix and what undoes it make nothing
        const float4x4 m = float4x4(float4(2, 0.5, 0, 0), float4(-0.25, 1, 0.3, 0), float4(0.1, 0, 3, 0), float4(4, -2, 1, 1));
        const float4x4 one = mul(m, KoralInverse(m));
        float off = 0.0;
        for (uint r = 0; r < 4; ++r) for (uint c = 0; c < 4; ++c) off = max(off, abs(one[r][c] - (r == c ? 1.0 : 0.0)));
        Check(off, 0.0, 1e-5);
    }
    Check(KoralFullscreenCorner(1).x, 3.0, 0.0);
    Check(KoralFullscreenCorner(2).y, 3.0, 0.0);
    Check(float(KoralGroupsFor(70000).y), 2.0, 0.0);

    // ---- koral_color -----------------------------------------------------------------------------------------------
    {
        float srgb = 0.0, gamma = 0.0, ycocg = 0.0, oklab = 0.0;
        for (uint i = 0; i <= 64; ++i) {
            const float3 c = float3(i / 64.0, (64 - i) / 64.0, (i % 7) / 6.0);
            srgb = max(srgb, KoralMax3(abs(KoralSrgbFromLinear(KoralLinearFromSrgb(c)) - c)));
            gamma = max(gamma, KoralMax3(abs(KoralGamma22FromLinear(KoralLinearFromGamma22(c)) - c)));
            ycocg = max(ycocg, KoralMax3(abs(KoralFromYCoCg(KoralToYCoCg(c)) - c)));
            oklab = max(oklab, KoralMax3(abs(KoralLinearFromOklab(KoralOklabFromLinear(c)) - c)));
        }
        Check(srgb, 0.0, 1e-5);
        Check(gamma, 0.0, 1e-5);
        Check(ycocg, 0.0, 1e-6);
        Check(oklab, 0.0, 1e-4);
    }
    Check(KoralLinearFromSrgb(float3(0.5)).x, 0.214041, 1e-5);
    Check(KoralLuminance(float3(1.0)), 1.0, 1e-6);
    Check(KoralOklabFromLinear(float3(1.0)).x, 1.0, 1e-3);      // white is lightness 1
    Check(KoralSane(float3(asfloat(0x7FC00000u), 1.0, 1.0)).y, 0.0, 0.0);
    Check(KoralRgbFromHsv(float3(1.0 / 3.0, 1.0, 1.0)).g, 1.0, 1e-5);     // a third round is green

    // ---- koral_display ---------------------------------------------------------------------------------------------
    Check(KoralAcesFitted(float3(0.0)).x, 0.0, 1e-6);
    Check(KoralAcesFitted(float3(1000.0)).x, 1.0, 1e-3);       // rolled off to white, not past it
    Check(KoralPbrNeutral(float3(0.5, 0.3, 0.2)).x - KoralPbrNeutral(float3(0.5, 0.3, 0.2)).y, 0.2, 1e-5);     // below white: as it is
    Check(KoralExposureFromEv100(0.0), 1.0 / 1.2, 1e-6);

    // ---- koral_random ----------------------------------------------------------------------------------------------
    {
        float low = 1.0, high = 0.0, sum = 0.0;
        float2 r2 = 0.0;
        for (uint i = 0; i < 4096; ++i) {
            const float x = KoralRandom(i, 7u, 1u);
            low = min(low, x);
            high = max(high, x);
            sum += x;
            r2 += KoralR2(i);
        }
        Check(low, 0.0, 0.01);
        Check(high, 1.0, 0.01);
        Check(sum / 4096.0, 0.5, 0.02);
        Check(r2.x / 4096.0, 0.5, 0.01);
        Check(KoralHalton(1, 2), 0.5, 0.0);
        Check(KoralHalton(3, 2), 0.75, 0.0);
        Check(float(KoralPcgHash(1u) != KoralPcgHash(2u)), 1.0, 0.0);
    }

    // ---- koral_encoding --------------------------------------------------------------------------------------------
    {
        float oct = 0.0, turned = 0.0;
        for (uint i = 0; i < 256; ++i) {
            const float3 n = KoralFibonacciSphere(i, 256);
            oct = max(oct, distance(KoralOctDecode(KoralOctEncode(n)), n));
            const float4 q = normalize(float4(n, KoralRandom(i, 3u, 0u) - 0.5));
            const float4 back = KoralUnpackQuaternion(KoralPackQuaternion(q));
            turned = max(turned, 1.0 - abs(dot(back, q)));      // q and -q are the same turn
        }
        Check(oct, 0.0, 1e-5);
        Check(turned, 0.0, 1e-5);
    }
    Check(KoralUnpackHalf2(KoralPackHalf2(float2(1.5, -2.25))).y, -2.25, 0.0);
    Check(KoralUnpackUnorm4x8(KoralPackUnorm4x8(float4(0.0, 0.5, 1.0, 0.25))).z, 1.0, 0.0);
    Check(KoralUnpackSnorm2x16(KoralPackSnorm2x16(float2(-1.0, 0.5))).x, -1.0, 0.0);
    Check(KoralUnpackRgbe(KoralPackRgbe(float3(1000.0, 1.0, 0.5))).x, 1000.0, 4.0);

    // ---- koral_sampling --------------------------------------------------------------------------------------------
    {
        float cosine = 0.0, sphere = 0.0, cone = 1.0, disc = 0.0, visible = 1.0, basis = 0.0;
        for (uint i = 0; i < 4096; ++i) {
            const float2 u = KoralR2(i);
            cosine += KoralCosineHemisphere(u).z;
            sphere += KoralUniformSphere(u).z;
            cone = min(cone, KoralUniformCone(u, 0.9).z);
            disc = max(disc, length(KoralConcentricDisc(u)));
            visible = min(visible, KoralGgxVisibleNormal(normalize(float3(0.3, 0.1, 0.9)), 0.4, u).z);
            const KoralBasis b = KoralBasisOf(KoralFibonacciSphere(i, 4096));
            basis = max(basis, abs(dot(b.tangent, b.bitangent)) + abs(dot(b.tangent, b.normal)) + abs(length(b.bitangent) - 1.0));
        }
        Check(cosine / 4096.0, 2.0 / 3.0, 0.01);       // E[cos θ] under a cos θ / π density
        Check(sphere / 4096.0, 0.0, 0.01);
        Check(cone, 0.9, 1e-4);
        Check(disc, 1.0, 1e-3);
        Check(float(visible >= 0.0), 1.0, 0.0);         // a visible normal faces out
        Check(basis, 0.0, 1e-4);
    }

    // ---- koral_bounds ----------------------------------------------------------------------------------------------
    {
        // Seen from the origin along -z: a lens of 90 degrees, near 0.1, far 100.
        const float4x4 lens = float4x4(float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, -100.0 / 99.9, -1), float4(0, 0, -10.0 / 99.9, 0));
        const float4x4 none = float4x4(float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, 1, 0), float4(0, 0, 0, 1));
        Check(float(KoralBoxOutOfSight(float3(-1, -1, -6), float3(1, 1, -4), none, lens)), 0.0, 0.0);
        Check(float(KoralBoxOutOfSight(float3(-1, -1, 4), float3(1, 1, 6), none, lens)), 1.0, 0.0);      // behind the eye
        Check(float(KoralBoxOutOfSight(float3(20, -1, -6), float3(22, 1, -4), none, lens)), 1.0, 0.0);    // off to the side
        const KoralBoxInPicture placed = KoralPlaceBox(float3(-1, -1, -6), float3(1, 1, -4), none, lens, false);
        Check(placed.low.x, 0.375, 1e-4);               // its near face, 2 wide at 4 away: the middle quarter
        Check(float(placed.inFront), 1.0, 0.0);
        Check(float(KoralSphereOutOfSight(float3(0, 0, -5), 1.0, lens)), 0.0, 0.0);
        Check(float(KoralSphereOutOfSight(float3(0, 0, 5), 1.0, lens)), 1.0, 0.0);
        const KoralBoxSeen seen = KoralSeeBox(float3(0, 0, -10), float3(1.0), lens, 100.0);
        Check(seen.nearest, 9.0, 1e-3);
        Check(KoralRayBox(float3(0, 0, -10), float3(0, 0, 1), float3(-1), float3(1)).x, 9.0, 1e-5);
        Check(KoralRaySphere(float3(0, 0, -10), float3(0, 0, 2), float3(0.0), 1.0).x, 4.5, 1e-5);     // a direction 2 long: half as far
        Check(KoralRaySphere(float3(0, 5, -10), float3(0, 0, 1), float3(0.0), 1.0).x, -1e30, 0.0);
        Check(KoralRayPlane(float3(0, 5, 0), float3(0, -1, 0), float3(0.0), float3(0, 1, 0)), 5.0, 1e-6);
        const KoralTriangleHit hit = KoralRayTriangle(float3(0.25, 0.25, 1), float3(0, 0, -1), float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0));
        Check(hit.distance, 1.0, 1e-6);
        Check(hit.barycentrics.x, 0.25, 1e-6);
        Check(float(KoralRayTriangle(float3(2, 2, 1), float3(0, 0, -1), float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0)).hit), 0.0, 0.0);
    }

    // ---- koral_depth -----------------------------------------------------------------------------------------------
    {
        const float4x4 lens = float4x4(float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, -100.0 / 99.9, -1), float4(0, 0, -10.0 / 99.9, 0));
        const float4x4 inverseLens = KoralInverse(lens);
        const float3 point = float3(1.0, -0.5, -8.0);
        const float4 clip = mul(float4(point, 1.0), lens);
        const float2 uv = KoralUvFromClip(clip.xy / clip.w);
        Check(distance(KoralWorldFromDepth(uv, clip.z / clip.w, inverseLens), point), 0.0, 1e-3);
        Check(KoralLinearDepth(clip.z / clip.w, 0.1, 100.0, false), 8.0, 1e-2);
        const float4x4 none = float4x4(float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, 1, 0), float4(0, 0, 0, 1));
        Check(KoralReproject(float2(0.3, 0.7), 0.5, none).y, 0.7, 1e-6);
        const KoralRay ray = KoralRayThrough(float2(0.0), inverseLens);
        Check(ray.direction.z, -1.0, 1e-5);
        Check(ray.length, 99.9, 0.05);
    }
}
