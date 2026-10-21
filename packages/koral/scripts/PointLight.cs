// The engine's package. Light from its entity's position, every way, fading out to nothing at its range.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

public sealed class PointLight : Script
{
    [Min(0f)] public Vec3 color = new(1f, 0.85f, 0.6f);
    [Min(0f), Logarithmic] public float intensity = 2f;
    [Min(0f), Logarithmic] public float range = 5f;
}
