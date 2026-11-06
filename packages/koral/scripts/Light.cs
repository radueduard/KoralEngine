// The engine's package. Light: how much of it, and of what colour. A kind — no entity has a Light itself, but one of
// what derives from it — so that a query for Light finds every light, the colour and intensity read from whichever it is.

public abstract class Light : Data
{
    [Min(0f)] public Vec3 color = new(1f, 1f, 1f);
    [Min(0f), Logarithmic] public float intensity = 1f;
}
