// The engine's package. A lens on its entity, looking along its forward (-Z): what the scene is seen through.
// Data and nothing else: what a light or a camera does is the graph's, which reads it through a node of its name.

public sealed class Camera : Setup
{
    [Range(1f, 179f)] public float fieldOfView = 60f;    // Top to bottom, in degrees.
    [Min(0.001f), Logarithmic] public float near = 0.1f;
    [Min(0f)] public float far = 100f;                   // 0: as far as there is — a perspective's, no far plane.
    public bool orthographic = false;                    // Seen in a box, as big however far, rather than through a lens.
    [Min(0.001f), Logarithmic] public float height = 10f; // Orthographic: how much it sees, top to bottom.
}
