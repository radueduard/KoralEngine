// The engine's package. Light from far away, shining along its entity's forward (-Z): a sun.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

public sealed class DirectionalLight : Script
{
    public Vec3 color = new(1f, 1f, 1f);
    public float intensity = 1f;
}
