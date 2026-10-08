// A ray generation shader that traces nothing: it writes where each ray is, so a test can tell there was one for
// every pixel.

[[vk::image_format("rgba8")]] RWTexture2D<float4> target;

[shader("raygeneration")]
void raygenMain()
{
    const uint2 at = DispatchRaysIndex().xy;
    target[at] = float4(0.0, float(at.x) / 31.0, 1.0, 1.0);
}
