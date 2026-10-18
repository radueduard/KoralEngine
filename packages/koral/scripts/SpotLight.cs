// The engine's package. Light from its entity's position along its forward (-Z), in a cone: full inside innerAngle, none past outerAngle.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

public sealed class SpotLight : Script
{
    public Vec3 color = new(1f, 1f, 1f);
    public float intensity = 3f;
    public float range = 8f;
    public float innerAngle = 20f;    // From the middle of the cone to where it starts fading, in degrees.
    public float outerAngle = 30f;    // From the middle of the cone to its edge, in degrees.
}
