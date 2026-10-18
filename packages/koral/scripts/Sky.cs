// The engine's package. The sky: what is behind everything, and the light that comes from all of it — zenith from above, ground from below — in ambient measure.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

public sealed class Sky : Script
{
    public Vec3 zenith = new(0.22f, 0.42f, 0.78f);
    public Vec3 horizon = new(0.72f, 0.8f, 0.9f);
    public Vec3 ground = new(0.23f, 0.21f, 0.19f);
    public float ambient = 0.35f;
    public float sunSize = 1.5f;    // How wide the sun's disc is, in degrees.
}
