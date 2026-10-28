// The scene's lights, as the engine's Lights function lays them out: every DirectionalLight, then every PointLight, then
// every SpotLight, one after another in one buffer. A shader that reads them imports this:
//
//     import koral_lights;
//     StructuredBuffer<KoralLight> lights;     // the Lights function's `lights`; its `count` how many there are

public static const uint KoralLightDirectional = 0;
public static const uint KoralLightPoint = 1;
public static const uint KoralLightSpot = 2;

public struct KoralLight {
    public float3 position;     // where it is: nothing for a directional light
    public float range;         // how far it reaches: nothing for a directional light
    public float3 direction;    // the way it shines: the light's entity's forward
    public uint kind;           // KoralLightDirectional, KoralLightPoint or KoralLightSpot
    public float3 color;        // its colour in linear terms, times its intensity
    public float cosInner;      // a spot's: the cosine of the angle within which it is at its brightest
    public float cosOuter;      // the cosine of the angle past which it gives nothing
};
