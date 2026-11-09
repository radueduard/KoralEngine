// The engine's modules' libraries, checked on the GPU as library.slang checks the base library's: each check writes what
// it got, what it should be and how near is near enough, and test_modules.cpp names them by their lines.

import noise;
import sdf;
import color_grading;
import shading_extended;
import shading_stylized;
import lights_area;
import animation;
import ibl_precise;
import shadows_soft;
import antialiasing;
import filters;
import camera_effects;
import koral_random;
import koral_sampling;

RWStructuredBuffer<float4> results;

static uint next = 0;
void Check(float got, float expected, float tolerance) { results[next++] = float4(got, expected, tolerance, 1.0); }

/** A sphere and a box apart, as a field to march. */
struct Shapes : IKoralField {
    float Distance(float3 p) { return KoralUnion(KoralSdfSphere(p - float3(0, 0, -5), 1.0), KoralSdfBox(p - float3(3, 0, -5), float3(0.5))); }
};

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    // ---- noise ---------------------------------------------------------------------------------------------------
    {
        float low = 1e9, high = -1e9, sum = 0.0, cellLow = 1e9, value = 0.0;
        for (uint i = 0; i < 512; ++i) {
            const float3 p = float3(i * 0.37, i * 0.13, i * 0.71);
            const float n = KoralPerlin(p);
            low = min(low, n); high = max(high, n); sum += n;
            cellLow = min(cellLow, KoralWorley(p).x);
            value += KoralValueNoise(p);
        }
        Check(float(low >= -1.5 && high <= 1.5), 1.0, 0.0);
        Check(sum / 512.0, 0.0, 0.1);                       // about 0 on average
        Check(KoralPerlin(float3(3.0, -2.0, 5.0)), 0.0, 1e-5);     // 0 at a lattice point
        Check(KoralPerlinTiled(float3(0.3, 0.6, 0.2), int3(4, 4, 4)) - KoralPerlinTiled(float3(4.3, 0.6, 0.2), int3(4, 4, 4)), 0.0, 1e-5);
        Check(float(cellLow >= 0.0), 1.0, 0.0);
        Check(value / 512.0, 0.5, 0.1);
        Check(float(abs(KoralSimplex(float3(1.3, 2.7, -0.4))) <= 1.2), 1.0, 0.0);
        const float3 c = KoralCurl(float3(0.4, 1.1, 2.3));
        Check(float(all(isfinite(c))), 1.0, 0.0);
    }

    // ---- sdf -----------------------------------------------------------------------------------------------------
    Check(KoralSdfSphere(float3(3, 0, 0), 1.0), 2.0, 1e-6);
    Check(KoralSdfBox(float3(2, 0, 0), float3(1.0)), 1.0, 1e-6);
    Check(KoralSdfBox(float3(0, 0, 0), float3(1.0)), -1.0, 1e-6);
    Check(KoralSmoothUnion(1.0, 1.0, 0.5), 0.875, 1e-6);
    {
        const Shapes shapes;
        const KoralMarchHit hit = KoralMarch(shapes, float3(0.0), float3(0, 0, -1), 100.0);
        Check(hit.distance, 4.0, 1e-2);
        Check(KoralFieldNormal(shapes, float3(0, 0, -4)).z, 1.0, 1e-2);
    }

    // ---- color grading -------------------------------------------------------------------------------------------
    Check(KoralAgX(float3(0.0)).r, 0.0, 0.03);
    Check(KoralAgX(float3(1000.0)).g, 1.0, 0.05);
    Check(KoralReinhard(float3(1.0), 4.0).r, 0.53125, 1e-4);
    Check(KoralContrast(float3(0.18), 2.0).r, 0.18, 1e-5);          // middle grey stays
    Check(KoralSaturation(float3(1.0, 0.0, 0.0), 0.0).g, KoralLuminance(float3(1.0, 0.0, 0.0)), 1e-6);
    Check(KoralWhiteBalance(float3(0.5), 0.0, 0.0).g, 0.5, 0.02);    // none: as it is
    Check(float(KoralWhiteBalance(float3(0.5), 1.0, 0.0).r > KoralWhiteBalance(float3(0.5), 1.0, 0.0).b), 1.0, 0.0);     // warmer
    Check(KoralLiftGammaGain(float3(0.5), float3(0.0), float3(1.0), float3(1.0)).r, 0.5, 1e-5);
    Check(float(KoralHistogramBin(1.0, -10.0, 6.0, 256)), 160.0, 1.0);

    // ---- lobes and shadings: no more light out than in -----------------------------------------------------------
    {
        const float3 n = float3(0, 0, 1);
        float orenNayar = 0.0, toon = 0.0, cloth = 0.0, coat = 0.0;
        for (uint i = 0; i < 1024; ++i) {
            const float3 l = KoralCosineHemisphere(KoralR2(i));      // pdf cos/π: a sum of f·cos/pdf is f·π
            const float3 v = normalize(float3(0.3, 0.1, 0.9));
            orenNayar += KoralOrenNayarLobe.Reflected(KoralAnglesOf(n, v, l), 1.0) * KoralPi;
            KoralClothPbr velvet;
            velvet.normal = n; velvet.baseColor = float3(1.0); velvet.sheenColor = float3(0.0); velvet.roughness = 0.5; velvet.occlusion = 1.0; velvet.opacity = 1.0;
            cloth += velvet.Reflected(v, l, float3(1.0)).r / max(l.z, 1e-4) * KoralPi;
            KoralClearCoatPbr paint;
            paint.base = KoralMatte(n, float3(1.0)); paint.coat = 1.0; paint.coatRoughness = 0.1; paint.coatNormal = n;
            coat += paint.Reflected(v, l, float3(1.0)).r / max(l.z, 1e-4) * KoralPi;
        }
        Check(orenNayar / 1024.0, 0.9, 0.12);               // a white rough surface keeps most of the light, no more than all
        Check(cloth / 1024.0, 1.0, 0.02);                    // a white cloth without sheen: Lambert's
        Check(float(coat / 1024.0 <= 1.05), 1.0, 0.0);
        const KoralToon t = KoralToon(n, float3(1.0), 3);
        Check(KoralBands(0.5, 4), 0.5, 0.05);
        Check(float(t.Reflected(float3(0, 0, 1), float3(0, 0, 1), float3(1.0)).r >= t.Reflected(float3(0, 0, 1), normalize(float3(1, 0, 0.2)), float3(1.0)).r), 1.0, 0.0);
        Check(KoralHatching(float2(10.0, 10.0), 1.0), 1.0, 0.0);      // all light: no ink
        Check(KoralThinFilmFresnel(1.0, 0.0).g, 0.04, 0.03);          // no film: the base's own
    }

    // ---- area lights ----------------------------------------------------------------------------------------------
    {
        KoralAreaLight bulb;
        bulb.position = float3(0, 2, 0); bulb.radius = 0.5; bulb.axis = float3(1, 0, 0); bulb.length = 2.0; bulb.up = float3(0, 0, 1);
        bulb.size = float2(1.0, 1.0); bulb.color = float3(1.0); bulb.range = 10.0;
        const KoralArrival sphere = KoralSphereLightArrival(bulb, float3(0.0), float3(0, 1, 0), float3(0, 1, 0), 0.5);
        Check(sphere.toLight.y, 1.0, 1e-4);
        Check(float(KoralSphereNormalization(0.1, 0.5, 2.0) < 1.0), 1.0, 0.0);
        const KoralArrival tube = KoralTubeLightArrival(bulb, float3(0.0), float3(0, 1, 0), normalize(float3(-1, 1, 0)), 0.5);
        Check(float(tube.toLight.x > 0.0), 1.0, 0.0);     // the end the mirrored way points to
        Check(float(all(isfinite(KoralRectLightArrival(bulb, float3(0.0), float3(0, 1, 0), float3(0, 1, 0), 0.5).radiance))), 1.0, 0.0);
    }

    // ---- animation -------------------------------------------------------------------------------------------------
    {
        const float3 swayed = KoralWindSway(float3(0, 1, 0), 0.0, float3(1, 0, 0), 3.0);
        Check(swayed.x, 0.0, 1e-6);                         // at the root: unmoved
        Check(KoralBillboard(float3(0.0), float2(1.0, 1.0), float2(2.0, 2.0), float3(1, 0, 0), float3(0, 1, 0)).x, 1.0, 1e-6);
        Check(KoralBillboardUpright(float3(0.0), float2(0.0, 1.0), float2(1.0, 2.0), float3(0, 0, 5)).y, 1.0, 1e-6);
    }

    // ---- ibl precise ---------------------------------------------------------------------------------------------
    {
        const float2 smooth = KoralIntegrateBrdf(0.0, 1.0, 256);
        Check(smooth.x + smooth.y, 1.0, 0.03);              // a mirror seen square on mirrors all of it
        const float2 rough = KoralIntegrateBrdf(1.0, 0.5, 256);
        Check(float(rough.x + rough.y < 1.0), 1.0, 0.0);
        Check(rough.x, KoralEnvironmentBrdf(1.0, 0.5).x, 0.1);         // the base library's fit agrees
        Check(KoralBoxProject(float3(1, 0, 0), float3(0.0), float3(-1.0), float3(1.0), float3(0.0)).x, 1.0, 1e-5);
        Check(KoralProbeWeight(float3(0.0), float3(-1.0), float3(1.0), 0.5), 1.0, 0.0);
    }

    // ---- soft shadows ----------------------------------------------------------------------------------------------
    {
        const float4 m = KoralEvsmMoments(0.3);
        Check(KoralEvsmVisibility(m, 0.3), 1.0, 1e-3);                 // at the occluder's depth: lit
        Check(KoralEvsmVisibility(m, 0.6), 0.0, 0.05);                 // well behind it: not
        Check(KoralChebyshev(float2(0.5, 0.26), 0.4, 1e-4, 0.0), 1.0, 0.0);
    }

    // ---- antialiasing, filters, camera ---------------------------------------------------------------------------
    Check(KoralLanczos2(0.0), 1.0, 1e-6);
    Check(KoralLanczos2(1.0), 0.0, 1e-5);
    Check(KoralAtrousWeight(0) + 2.0 * KoralAtrousWeight(1) + 2.0 * KoralAtrousWeight(2), 1.0, 1e-6);
    Check(KoralSharpen(float3(0.5), float3(0.5), float3(0.5), float3(0.5), float3(0.5), 1.0).r, 0.5, 1e-5);     // flat: unchanged
    Check(KoralCircleOfConfusion(5.0, 5.0, 2.8, 0.05), 0.0, 1e-6);     // in focus
    Check(KoralVignette(float2(0.5), 1.0), 1.0, 1e-6);
}
