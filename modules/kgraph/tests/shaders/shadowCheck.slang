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
    const KoralSpotLight spot = KoralSpotLight(float3(0.0, 4.0, 0.0), 5.0, float3(0.0, -1.0, 0.0), float3(1.0), cos(radians(20.0)), cos(radians(30.0)));
    const KoralPointLight lamp = KoralPointLight(float3(0.0, 4.0, 0.0), 5.0, float3(1.0));

    const KoralSpotShadows spots = KoralSpotShadows(spotMaps, spotMatrices, compare, KoralShadowFilter());
    const KoralPointShadows lamps = KoralPointShadows(pointMaps, pointFaces, compare, KoralShadowFilter());
    const float3 up = float3(0.0, 1.0, 0.0);
    const float3 under = float3(0.0, 0.0, 0.0), aside = float3(2.0, 0.0, 0.0);

    Check(spots.SpotVisibility(0, spot, spot.ArrivalAt(under), under, up), 0.0, 0.1);
    Check(spots.SpotVisibility(0, spot, spot.ArrivalAt(aside), aside, up), 1.0, 0.1);
    Check(lamps.PointVisibility(0, lamp, lamp.ArrivalAt(under), under, up), 0.0, 0.1);
    Check(lamps.PointVisibility(0, lamp, lamp.ArrivalAt(aside), aside, up), 1.0, 0.1);
    // A shadow is only its own kind's.
    Check(spots.PointVisibility(0, lamp, lamp.ArrivalAt(under), under, up), 1.0, 0.0);
    Check(KoralCombineShadows(spots, lamps).SpotVisibility(0, spot, spot.ArrivalAt(under), under, up), 0.0, 0.1);
    // What reaches: the spot's cone, the lamp's range.
    Check(float(spot.Reaches(under)), 1.0, 0.0);
    Check(float(spot.Reaches(aside + float3(0.0, 3.5, 0.0))), 0.0, 0.0);     // beside it, out of its cone
    Check(float(lamp.Touches(float3(-1.0, -1.0, -1.0), float3(1.0, 0.0, 1.0))), 1.0, 0.0);
    Check(float(lamp.Touches(float3(10.0, 0.0, 0.0), float3(11.0, 1.0, 1.0))), 0.0, 0.0);
}
