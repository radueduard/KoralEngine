// The engine's package. Light from its entity's position, every way, fading out to nothing at its range.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

public sealed class PointLight : Script
{
    public Vec3 color = new(1f, 0.85f, 0.6f);
    public float intensity = 2f;
    public float range = 5f;
}
