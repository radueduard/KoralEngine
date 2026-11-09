// koral_shadows, checked on what the engine's PointLightShadows and SpotLightShadows functions drew: a unit cube at
// (0, 1, 0), and a spot and a point light at (0, 4, 0) over it, the spot shining down. The ground under the cube is in
// its shadow; two units off to the side is not. Each check as library.slang's: what it got, what it should be, how near.

import koral_shadows;

Texture2DArray<float> spotMaps;
StructuredBuffer<float4x4> spotMatrices;
Texture2DArray<float> pointMaps;
StructuredBuffer<float4x4> pointFaces;
SamplerComparisonState compare;
RWStructuredBuffer<float4> results;

static uint next = 0;
void Check(float got, float expected, float tolerance) { results[next++] = float4(got, expected, tolerance, 1.0); }

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    KoralLight spot = KoralDirectionalLight(float3(0.0, -1.0, 0.0), float3(1.0));
    spot.kind = KoralLightSpot;
    spot.position = float3(0.0, 4.0, 0.0);
    spot.range = 5.0;
    spot.cosInner = cos(radians(20.0));
    spot.cosOuter = cos(radians(30.0));
    KoralLight lamp = spot;
    lamp.kind = KoralLightPoint;

    const KoralSpotShadows spots = KoralSpotShadows(spotMaps, spotMatrices, compare, KoralShadowFilter());
    const KoralPointShadows lamps = KoralPointShadows(pointMaps, pointFaces, compare, KoralShadowFilter());
    const float3 up = float3(0.0, 1.0, 0.0);
    const float3 under = float3(0.0, 0.0, 0.0), aside = float3(2.0, 0.0, 0.0);

    Check(spots.Visibility(spot, KoralArrivalAt(spot, under), under, up), 0.0, 0.1);
    Check(spots.Visibility(spot, KoralArrivalAt(spot, aside), aside, up), 1.0, 0.1);
    Check(lamps.Visibility(lamp, KoralArrivalAt(lamp, under), under, up), 0.0, 0.1);
    Check(lamps.Visibility(lamp, KoralArrivalAt(lamp, aside), aside, up), 1.0, 0.1);
    // A shadow is only its own light's.
    Check(spots.Visibility(lamp, KoralArrivalAt(lamp, under), under, up), 1.0, 0.0);
    Check(KoralCombineShadows(spots, lamps).Visibility(spot, KoralArrivalAt(spot, under), under, up), 0.0, 0.1);
}
