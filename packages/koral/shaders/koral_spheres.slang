// Spheres on the GPU: kphysics' sequential impulses for bodies that are spheres, against each other and a ground, one
// colour of contacts at a time — contacts of a colour share no body, so each is solved by a thread of its own, and the
// colours one after another are the order kphysics solves them in. The engine's SpheresStep function runs it, a fixed
// step at a time:
//
//   velocities   gravity and damping                          keys       each body's cell, hashed, for the sort
//   ranges       where each cell's bodies are, sorted         contacts   each pair that touches, and the ground
//   claim/settle a round of colouring: each contact proposes the lowest colour neither of its bodies has yet, and has it
//                when no other contact of either body proposed it with a higher priority (speculative greedy colouring:
//                contacts only contend over the same colour, so most are coloured in a few rounds)
//   rest         what no round coloured, one colour more
//   bucket       contacts put together by colour               args       an indirect dispatch for each colour
//   solve        a colour's contacts, an iteration             positions  where each body goes, and how it turns
//
// A body: float4 position and inverse mass, float4 velocity and radius, float4 spin (angular velocity) and inverse
// inertia, float4 orientation (a quaternion). Its material: friction, restitution, linear and angular damping.

static const uint None = 0xFFFFFFFFu;
static const uint Rounds = 12;              // colours a round can give (first-fit needs about as many as a body has neighbours); what is left after the rounds is one colour more
static const uint Colours = Rounds + 1;

struct Body {
    float4 positionInvMass;
    float4 velocityRadius;
    float4 spinInvInertia;
    float4 orientation;
};
struct Contact {
    uint a, b, colour, pad;
    float4 normalDepth;         // from a to b; how far they overlap
    float4 impulses;            // the normal's, the two frictions', and the bias velocity
};

struct StepPush {
    uint count;                 // bodies
    float dt;
    float cell;                 // a cell's size: the largest diameter
    float ground;               // the ground's height; below -1e30, none
    float4 gravity;             // xyz, and w: the round or colour a pass is for
};
[[vk::push_constant]] ConstantBuffer<StepPush> push;

RWStructuredBuffer<Body> bodies;
RWStructuredBuffer<float4> materials;
RWStructuredBuffer<uint> keys;              // a body's cell, hashed; then sorted
RWStructuredBuffer<uint> ids;               // the bodies, as the keys are sorted
RWStructuredBuffer<uint> starts;            // a table cell's first sorted index + 1, 0 for none
RWStructuredBuffer<uint> ends;              // ...its last + 1
RWStructuredBuffer<Contact> contacts;
RWStructuredBuffer<uint> counter;           // [0]: how many contacts; [1]: how many there is room for (eight a body)
RWStructuredBuffer<uint> claims;            // a round's highest priority on each body for each colour: [body * Rounds + colour]
RWStructuredBuffer<uint> masks;             // the colours each body's contacts have, a bit each
RWStructuredBuffer<uint> colourCounts;      // how many contacts each colour has
RWStructuredBuffer<uint> colourStarts;      // where each colour's start, in ordered: the Scan of the counts
RWStructuredBuffer<uint> colourFill;
RWStructuredBuffer<uint> ordered;           // contacts, colour after colour
RWStructuredBuffer<uint> args;              // three uints for each colour: its groups of 64

uint TableSize() { return max(push.count * 2, 1024u); }      // a power of two or not: keys are taken modulo it
int3 CellOf(float3 p) { return int3(floor(p / push.cell)); }
uint Hash3(int3 c) { return (uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u) % TableSize(); }
uint Hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

[shader("compute")] [numthreads(64, 1, 1)]
void velocities(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    Body body = bodies[id.x];
    if (body.positionInvMass.w == 0) return;
    const float4 material = materials[id.x];
    body.velocityRadius.xyz = (body.velocityRadius.xyz + push.gravity.xyz * push.dt) / (1 + push.dt * material.z);
    body.spinInvInertia.xyz /= 1 + push.dt * material.w;
    bodies[id.x] = body;
}

[shader("compute")] [numthreads(64, 1, 1)]
void cellKeys(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    keys[id.x] = Hash3(CellOf(bodies[id.x].positionInvMass.xyz));
    ids[id.x] = id.x;
    if (id.x == 0) { counter[0] = 0; counter[1] = push.count * 8; }     // room for eight contacts a body
}

[shader("compute")] [numthreads(64, 1, 1)]
void ranges(uint3 id : SV_DispatchThreadID)
{
    const uint k = id.x;
    if (k >= push.count) return;
    const uint key = keys[k];
    if (k == 0 || keys[k - 1] != key) starts[key] = k + 1;
    if (k + 1 == push.count || keys[k + 1] != key) ends[key] = k + 1;
}

void Add(uint a, uint b, float3 normal, float depth)
{
    uint at;
    InterlockedAdd(counter[0], 1, at);
    if (at >= counter[1]) return;               // no room: lost — the buffer is to be bigger
    Contact c;
    c.a = a; c.b = b; c.colour = None; c.pad = 0;
    c.normalDepth = float4(normal, depth);
    c.impulses = float4(0, 0, 0, 0);
    contacts[at] = c;
}

[shader("compute")] [numthreads(64, 1, 1)]
void findContacts(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= push.count) return;
    const Body me = bodies[i];
    const float3 p = me.positionInvMass.xyz;
    const float r = me.velocityRadius.w;
    if (push.ground > -1e30 && p.y - r < push.ground && me.positionInvMass.w > 0) Add(i, None, float3(0, -1, 0), push.ground - (p.y - r));
    const int3 cell = CellOf(p);
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        const uint key = Hash3(cell + int3(dx, dy, dz));
        const uint start = starts[key];
        if (start == 0) continue;
        for (uint k = start - 1; k < ends[key]; ++k) {
            const uint j = ids[k];
            if (j <= i) continue;               // each pair once, and other cells hashed the same are checked as they are
            const Body other = bodies[j];
            if (me.positionInvMass.w == 0 && other.positionInvMass.w == 0) continue;
            const float3 d = other.positionInvMass.xyz - p;
            const float reach = r + other.velocityRadius.w;
            const float far = dot(d, d);
            if (far >= reach * reach || far < 1e-12) continue;
            const float distance = sqrt(far);
            // Another cell of the 27 hashed to the same key would find it twice: only from its own cell.
            if (any(CellOf(other.positionInvMass.xyz) != cell + int3(dx, dy, dz))) continue;
            Add(i, j, d / distance, reach - distance);
        }
    }
}

uint Priority(uint c) { return (Hash(c * 2654435761u + uint(push.gravity.w) * 97u) & 0xFFF00000u) | (c & 0xFFFFFu) | 1u; }

[shader("compute")] [numthreads(64, 1, 1)]
void claim(uint3 id : SV_DispatchThreadID)
{
    const uint c = id.x;
    if (c >= min(counter[0], counter[1]) || contacts[c].colour != None) return;
    const Contact contact = contacts[c];
    const uint used = masks[contact.a] | (contact.b != None ? masks[contact.b] : 0u);
    const uint colour = firstbitlow(~used);
    contacts[c].pad = colour;                   // what it proposes this round
    if (colour >= Rounds) return;               // every colour taken: left for the last
    const uint p = Priority(c);
    uint was;
    InterlockedMax(claims[contact.a * Rounds + colour], p, was);
    if (contact.b != None) InterlockedMax(claims[contact.b * Rounds + colour], p, was);
}

[shader("compute")] [numthreads(64, 1, 1)]
void settle(uint3 id : SV_DispatchThreadID)
{
    const uint c = id.x;
    if (c >= min(counter[0], counter[1]) || contacts[c].colour != None) return;
    const Contact contact = contacts[c];
    const uint colour = contact.pad;
    if (colour >= Rounds) return;
    const uint p = Priority(c);
    if (claims[contact.a * Rounds + colour] != p || (contact.b != None && claims[contact.b * Rounds + colour] != p)) return;
    contacts[c].colour = colour;
    uint was;
    InterlockedOr(masks[contact.a], 1u << colour, was);
    if (contact.b != None) InterlockedOr(masks[contact.b], 1u << colour, was);
    InterlockedAdd(colourCounts[colour], 1);
}

[shader("compute")] [numthreads(64, 1, 1)]
void rest(uint3 id : SV_DispatchThreadID)
{
    const uint c = id.x;
    if (c >= min(counter[0], counter[1]) || contacts[c].colour != None) return;
    contacts[c].colour = Rounds;
    InterlockedAdd(colourCounts[Rounds], 1);
}

[shader("compute")] [numthreads(64, 1, 1)]
void bucket(uint3 id : SV_DispatchThreadID)
{
    const uint c = id.x;
    if (c >= min(counter[0], counter[1])) return;
    const uint colour = contacts[c].colour;
    uint at;
    InterlockedAdd(colourFill[colour], 1, at);
    ordered[colourStarts[colour] + at] = c;
}

[shader("compute")] [numthreads(1, 1, 1)]
void colourArgs()
{
    for (uint colour = 0; colour < Colours; ++colour) {
        // The last colour — what no round coloured, bodies shared — is solved by one thread, a contact after another.
        args[colour * 3 + 0] = colour == Rounds ? min(colourCounts[colour], 1u) : (colourCounts[colour] + 63) / 64;
        args[colour * 3 + 1] = 1;
        args[colour * 3 + 2] = 1;
    }
}

float3 VelocityAt(Body body, float3 offset) { return body.velocityRadius.xyz + cross(body.spinInvInertia.xyz, offset); }

void Push(inout Body body, float3 offset, float3 impulse)
{
    body.velocityRadius.xyz += impulse * body.positionInvMass.w;
    body.spinInvInertia.xyz += cross(offset, impulse) * body.spinInvInertia.w;
}

float Mass(Body a, Body b, bool hasB, float3 ra, float3 rb, float3 n)
{
    float k = a.positionInvMass.w + dot(cross(cross(ra, n), ra), n) * a.spinInvInertia.w;
    if (hasB) k += b.positionInvMass.w + dot(cross(cross(rb, n), rb), n) * b.spinInvInertia.w;
    return k > 0 ? 1 / k : 0;
}

void SolveOne(uint c)
{
    Contact contact = contacts[c];
    const bool hasB = contact.b != None;
    Body a = bodies[contact.a];
    Body b = hasB ? bodies[contact.b] : (Body)0;
    const float3 n = contact.normalDepth.xyz;
    const float3 ra = n * a.velocityRadius.w;
    const float3 rb = hasB ? -n * b.velocityRadius.w : float3(0, 0, 0);
    const float4 ma = materials[contact.a];
    const float4 mb = hasB ? materials[contact.b] : ma;
    const float friction = sqrt(ma.x * mb.x);
    const float restitution = max(ma.y, mb.y);

    float3 relative = (hasB ? VelocityAt(b, rb) : float3(0, 0, 0)) - VelocityAt(a, ra);
    const float approach = dot(relative, n);
    // Pushed apart a part of how far they overlap, each step, beyond a slop; and bounced when they met fast.
    if (contact.impulses.w == 0) contact.impulses.w = max(0.2 * max(contact.normalDepth.w - 0.005, 0) / push.dt, approach < -1 ? -restitution * approach : 0) + 1e-9;
    const float normalMass = Mass(a, b, hasB, ra, rb, n);
    float lambda = -(approach - contact.impulses.w) * normalMass;
    const float was = contact.impulses.x;
    contact.impulses.x = max(was + lambda, 0);
    lambda = contact.impulses.x - was;
    Push(a, ra, -n * lambda);
    if (hasB) Push(b, rb, n * lambda);

    // Friction, along the two ways across the normal, each within friction times the normal's impulse.
    const float3 t1 = normalize(abs(n.y) < 0.9 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    const float3 t2 = cross(n, t1);
    const float most = friction * contact.impulses.x;
    for (int k = 0; k < 2; ++k) {
        const float3 t = k == 0 ? t1 : t2;
        relative = (hasB ? VelocityAt(b, rb) : float3(0, 0, 0)) - VelocityAt(a, ra);
        float f = -dot(relative, t) * Mass(a, b, hasB, ra, rb, t);
        const float before = k == 0 ? contact.impulses.y : contact.impulses.z;
        const float now = clamp(before + f, -most, most);
        f = now - before;
        if (k == 0) contact.impulses.y = now; else contact.impulses.z = now;
        Push(a, ra, -t * f);
        if (hasB) Push(b, rb, t * f);
    }
    if (a.positionInvMass.w > 0) bodies[contact.a] = a;
    if (hasB && b.positionInvMass.w > 0) bodies[contact.b] = b;
    contacts[c] = contact;
}

[shader("compute")] [numthreads(64, 1, 1)]
void solve(uint3 id : SV_DispatchThreadID)
{
    const uint colour = uint(push.gravity.w);
    if (colour == Rounds) {
        // Bodies shared: one thread, one after another.
        if (id.x != 0) return;
        for (uint k = 0; k < colourCounts[colour]; ++k) SolveOne(ordered[colourStarts[colour] + k]);
        return;
    }
    if (id.x >= colourCounts[colour]) return;
    SolveOne(ordered[colourStarts[colour] + id.x]);
}

[shader("compute")] [numthreads(64, 1, 1)]
void positions(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    Body body = bodies[id.x];
    if (body.positionInvMass.w == 0) return;
    body.positionInvMass.xyz += body.velocityRadius.xyz * push.dt;
    const float3 w = body.spinInvInertia.xyz;
    float4 q = body.orientation;
    const float4 dq = float4(w * q.w + cross(w, q.xyz), -dot(w, q.xyz)) * (0.5 * push.dt);
    body.orientation = normalize(q + dq);
    bodies[id.x] = body;
}
