// The engine's package. A camera that sees in a box: as big however far — a plan, an elevation, a 2D game's view.
// Data and nothing else: what a camera does is the graph's, which reads it through a node of its name, or of Camera's.

public sealed class OrthographicCamera : Camera
{
    [Min(0.001f), Logarithmic] public float height = 10f; // How much it sees, top to bottom.
}
