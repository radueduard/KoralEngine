// The engine's package. A lens on its entity, looking along its forward (-Z): what the scene is seen through.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

public sealed class Camera : Script
{
    public float fieldOfView = 60f;    // Top to bottom, in degrees.
    public float near = 0.1f;
    public float far = 100f;
}
