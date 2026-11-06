// The engine's package. Light from a place, fading out to nothing at its range: what is bounded, and culled by it.
// A kind, as Light is: a PointLight or a SpotLight is one.

public abstract class LocalLight : Light
{
    [Min(0f), Logarithmic] public float range = 5f;
}
