// The color-grading module's passes. `grade`: a picture of light white balanced, its contrast and saturation set, lifted,
// gammaed and gained, through a curve, and through a 3D table when given one — to a picture the display shows (the Grade
// function). `histogram`, `average`: how bright a picture is, as a histogram of its log brightness, and the exposure that
// shows its middle as middle grey, moved towards it a little each frame (the AutoExposure function).

import color_grading;
import koral_debug;

[[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrImage;
[[vk::image_format("rgba8")]] RWTexture2D<float4> ldrImage;
Texture3D<float4> table;
SamplerState tableSampler;

struct GradePush {
    float4 lift;            // rgb, about 0
    float4 gamma;           // rgb, about 1
    float4 gain;            // rgb, about 1
    float exposure;
    float temperature;
    float tint;
    float contrast;
    float saturation;
    uint curve;             // KoralCurveAces...
    float tableSize;        // 0: no table
    float tableAmount;
};
[[vk::push_constant]] ConstantBuffer<GradePush> push;

StructuredBuffer<float> exposure;       // AutoExposure's, when it is wired: one number

/** A pixel graded, with @p scale times push.exposure: what the display is given, before a table. */
float3 Graded(uint2 at, float scale)
{
    float3 c = hdrImage[at].rgb;
    if (koralDebugShowing()) return c;
    c *= push.exposure * scale;
    c = KoralWhiteBalance(c, push.temperature, push.tint);
    c = KoralContrast(c, push.contrast);
    c = max(KoralSaturation(c, push.saturation), 0.0);
    const float3 shown = KoralCurve(c, push.curve);
    return saturate(KoralLiftGammaGain(shown, push.lift.rgb, push.gamma.rgb, push.gain.rgb));
}

bool Inside(uint2 at) { uint width, height; ldrImage.GetDimensions(width, height); return at.x < width && at.y < height; }

/** The grade, of push.exposure alone. */
[shader("compute")]
[numthreads(16, 16, 1)]
void grade(uint3 id : SV_DispatchThreadID) { if (Inside(id.xy)) ldrImage[id.xy] = float4(Graded(id.xy, 1.0), 1.0); }

/** The grade, exposed as AutoExposure says too: `exposure` its buffer. */
[shader("compute")]
[numthreads(16, 16, 1)]
void gradeExposed(uint3 id : SV_DispatchThreadID) { if (Inside(id.xy)) ldrImage[id.xy] = float4(Graded(id.xy, exposure[0]), 1.0); }

/** The grade, and a 3D table after it: `table`, push.tableSize texels a side, push.tableAmount of it. */
[shader("compute")]
[numthreads(16, 16, 1)]
void gradeTabled(uint3 id : SV_DispatchThreadID)
{
    if (!Inside(id.xy)) return;
    const float3 shown = Graded(id.xy, 1.0);
    ldrImage[id.xy] = float4(saturate(lerp(shown, KoralLut3D(table, tableSampler, shown, push.tableSize), saturate(push.tableAmount))), 1.0);
}

// ---- auto exposure -----------------------------------------------------------------------------------------------

static const uint Bins = 256;
Texture2D<float4> picture;
RWStructuredBuffer<uint> bins;          // Bins counts: cleared each frame before `histogram`
RWStructuredBuffer<float> adapted;      // [0] the exposure, carried from frame to frame

struct ExposurePush {
    float lowest;           // log2 of the darkest brightness counted
    float highest;          // and the brightest
    float key;              // what the middle is shown as: 0.18, middle grey
    float speed;            // how far towards it a frame goes, 0 to 1
    float darkest;          // the least exposure, and the most
    float brightest;
};
[[vk::push_constant]] ConstantBuffer<ExposurePush> exposurePush;

groupshared uint local[Bins];

[shader("compute")]
[numthreads(16, 16, 1)]
void histogram(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    local[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint width, height;
    picture.GetDimensions(width, height);
    if (id.x < width && id.y < height) {
        const float y = KoralLuminance(KoralSane(picture.Load(int3(id.xy, 0)).rgb));
        InterlockedAdd(local[KoralHistogramBin(y, exposurePush.lowest, exposurePush.highest, Bins)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (local[index] > 0) InterlockedAdd(bins[index], local[index]);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void average(uint index : SV_GroupIndex)
{
    const uint count = bins[index];
    local[index] = count * index;       // weighed by its bin, black (bin 0) counting for nothing
    GroupMemoryBarrierWithGroupSync();
    for (uint step = Bins / 2; step > 0; step >>= 1) {
        if (index < step) local[index] += local[index + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        uint width, height;
        picture.GetDimensions(width, height);
        const uint lit = max(width * height - bins[0], 1u);
        const float bin = float(local[0]) / float(lit);
        const float middle = KoralHistogramLuminance(max(bin, 1.0), exposurePush.lowest, exposurePush.highest, Bins);
        const float wanted = clamp(exposurePush.key / max(middle, 1e-6), exposurePush.darkest, exposurePush.brightest);
        const float before = adapted[0];
        adapted[0] = before > 0.0 && !isnan(before) ? lerp(before, wanted, saturate(exposurePush.speed)) : wanted;
    }
}
