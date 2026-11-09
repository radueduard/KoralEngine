// What gives off light, for the ray tracer: each instance of its scene whose material's `emission` is more than nothing,
// listed with how many triangles it has, how much light it gives off and where it is. Run after the rays are traced,
// from none each frame: the next frame's rays are lit by what this one listed.
//
// A module of its own: the ray tracer's materials read as a buffer like any other, wired from the TraceRays'.

import forwardCommon;

struct MeshRange {
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint unused;
    float4 boundsMin;
    float4 boundsMax;
};
StructuredBuffer<RayMaterial> instances;    // the ray tracer's: a row an entity of its scene
StructuredBuffer<uint> meshOf;              // the RayScene's: which mesh of the pack each instance is
StructuredBuffer<MeshRange> ranges;         // the pack's
StructuredBuffer<uint> meshIndices;         // the pack's indices
ByteAddressBuffer meshPositions;            // the pack's positions, 12 bytes each
RWStructuredBuffer<Emitter> emitters;
RWStructuredBuffer<uint> emitterCount;      // how many there are: counted into, from none

static const uint MostCornersLooked = 6144;     // a mesh of more corners than this is reached as far as its box says

[shader("compute")]
[numthreads(64, 1, 1)]
void gather(uint3 id : SV_DispatchThreadID)
{
    uint instancesLength, meshesLength, rangesLength, emittersLength, stride;
    instances.GetDimensions(instancesLength, stride);
    meshOf.GetDimensions(meshesLength, stride);
    ranges.GetDimensions(rangesLength, stride);
    emitters.GetDimensions(emittersLength, stride);
    const uint instance = id.x;
    if (instance >= min(instancesLength, meshesLength)) return;
    const RayMaterial material = instances[instance];
    // How bright what it gives off is: a row of no entity gives off nothing.
    const float bright = KoralLuminance(max(material.emission, 0.0));
    if (!(bright > 0.0)) return;
    const uint mesh = meshOf[instance];
    if (mesh >= rangesLength) return;
    const MeshRange range = ranges[mesh];
    if (range.indexCount < 3) return;
    // Its middle, its box's, where its entity puts it; its reach, as far from that as its farthest corner — that a ray
    // aimed into the sphere finds it as often as can be: a ball's is its radius, not its box's half-diagonal — and its
    // box's half-diagonal, for one of too many corners to look at each frame.
    const float3 middle = (range.boundsMin.xyz + range.boundsMax.xyz) * 0.5;
    const float4x4 model = material.model;
    const float scale = max(length(model[0].xyz), max(length(model[1].xyz), length(model[2].xyz)));
    float reachHere = length(range.boundsMax.xyz - range.boundsMin.xyz) * 0.5;
    uint indicesLength;
    meshIndices.GetDimensions(indicesLength, stride);
    if (range.indexCount <= MostCornersLooked && range.firstIndex + range.indexCount <= indicesLength) {
        float farthest = 0.0;
        for (uint i = 0; i < range.indexCount; ++i) {
            const uint vertex = meshIndices[range.firstIndex + i] + uint(range.vertexOffset);
            farthest = max(farthest, length(asfloat(meshPositions.Load3(vertex * 12)) - middle));
        }
        reachHere = farthest;
    }
    const float reach = max(reachHere * scale, 1e-3);
    uint slot;
    InterlockedAdd(emitterCount[0], 1, slot);
    if (slot >= emittersLength) return;
    Emitter emitter;
    emitter.instance = instance;
    emitter.triangles = range.indexCount / 3;
    emitter.power = bright * KoralPi * reach * reach;    // as bright as it is, by how big it looks from afar
    emitter.radius = reach;
    emitter.center = mul(float4(middle, 1.0), model).xyz;
    emitter.unused = 0.0;
    emitters[slot] = emitter;
}
