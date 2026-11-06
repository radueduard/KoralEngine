// The engine's package. A camera that sees through a lens: what is further away is smaller.
// Data and nothing else: what a camera does is the graph's, which reads it through a node of its name, or of Camera's.

public sealed class PerspectiveCamera : Camera
{
    [Range(1f, 179f)] public float fieldOfView = 60f;    // Top to bottom, in degrees.
}
