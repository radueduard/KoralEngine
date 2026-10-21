// Bodies on the GPU, the GPU's way: XPBD in small substeps (Macklin and others, "Small Steps in Physics Simulation"),
// each body solved by itself — Jacobi — from every contact it has, found by walking its cells of the grid: no colours,
// nothing waiting on anything else, and every body at once. Spheres, boxes and capsules (along their Y), against each
// other and a ground, collided as kphysics collides them (a box against a box by the separating axis test, then the
// incident face clipped by the reference face's sides). The engine's XpbdStep function runs it: the grid once a step,
// then each substep
//
//   contacts     every `contactsEvery` substeps: each body's contacts with the bodies near it, kept for those substeps
//   predict      where each body would go: gravity and damping, its velocity, its spin; where it was kept
//   solve        how far each is pushed out of what it touches, at each point it touches, and turned there; and held
//                by friction where it slid — its own share, the pushes averaged over its points, friction averaged over
//                each contact's points and whole across contacts — into next
//   apply        next made where it is
//   velocities   its velocity from how far it went, its spin from how far it turned; and what pushing apart turned
//                into speed taken off again, a bounce where it came fast
//
// A body, 96 bytes: float4 position and inverse mass; float4 velocity and how far from its middle it reaches (what the
// grid is made by); float4 spin and its shape (0 a sphere, 1 a box, 2 a capsule); float4 orientation; float4 size (a
// sphere's radius; a box's half extents; a capsule's radius, and half its length between the two ends' middles); float4
// inverse inertia, about its own axes. `prepare` works out the last from the shape, the size and the mass (in w of it,
// before), once, as setup is made. Its material: friction, restitution, linear and angular damping.

static const uint Sphere = 0, Box = 1, Capsule = 2;
static const uint Static = 0, Dynamic = 1, Kinematic = 2;      // a RigidBody's kind, kept in size.w
static const uint Plane = 3;            // a Collider's plane: an infinite one through its body, facing the body's +Y
static const uint Cone = 4;             // along Y, its point up: radius (size.x), height (size.y), how much more it is (size.z)
static const uint MeshShape = 5;        // a mesh collider's triangles (static or kinematic): in shared, not in the body
static const float StillSpeed = 0.05, StillSpin = 0.05;     // below these a body is still (m/s, rad/s), as kphysics has it
static const uint MostPlanes = 16;

struct Body {
    float4 positionInvMass;
    float4 velocityReach;
    float4 spinShape;
    float4 orientation;
    float4 size;
    float4 invInertia;
    float4 placed;              // where its entity is — its middle, a cone's above its centre of mass: what is written back;
                                // in w, a kinematic body's entity (its id from 1): it follows its entity, which says where it goes
};
struct Before {
    float4 position;
    float4 orientation;
    float4 velocity;
    float4 spin;
};
struct Next {
    float4 position;
    float4 orientation;
};


struct XpbdPush {
    uint count;
    float h;
    float cell;                 // a cell's size; 0 or less, twice the largest reach (what setup found: shared[0])
    float ground;               // a ground's height, besides the planes; below -1e30, none
    float4 gravity;
    float sleepAfter;           // how long a body still sleeps after, seconds; 0 or less, never
    uint joints;                // setup's: how many joints there are
    uint meshes;                // setup's: how many meshes the pack of mesh colliders' has (its ranges)
    uint triangles;             // setup's: how many triangles it has in all
    uint stride;                // setup's: the bytes of one of its vertices, and where in one its position is
    uint position;
    uint listed;                // setup's: how many entities the list of which mesh each has
    uint driven;                // the step's: how many kinematic bodies' entities say where they are (in `driven`)
    uint stepsThisFrame;        // and how many steps there are this frame to get there in
};
[[vk::push_constant]] ConstantBuffer<XpbdPush> push;

RWStructuredBuffer<Body> bodies;
RWStructuredBuffer<float4> materials;
RWStructuredBuffer<Before> before;
RWStructuredBuffer<Next> next;
RWStructuredBuffer<float4> meshHits;
RWStructuredBuffer<float4> driven;
RWStructuredBuffer<uint> bigs;              // the bodies too big for the grid's cells: how many, then which          // two float4 each kinematic body's entity: where it is and its id, how it is turned        // each body's contacts with the meshes, as meshContacts found them: HitWords float4
RWStructuredBuffer<uint> keys;
RWStructuredBuffer<uint> ids;
RWStructuredBuffer<uint> starts;
RWStructuredBuffer<uint> ends;
RWStructuredBuffer<int4> homes;
RWStructuredBuffer<float4> described;       // fromComponents': eight float4 an entity, as a RigidBody and a Collider have it
/// What setup found of the bodies for every step: [0] the largest reach (a float's bits), [1] how many planes, and from
/// [4] each plane, four at a time: its normal and how far along it from the origin it is.
RWStructuredBuffer<uint> shared;

/**
 * A joint, as resolved on setup: its two bodies (b None: the world), its kind (0 ball, 1 hinge, 2 distance, 3 rope)
 * and flags (1 limited, 2 colliding with each other all the same); where it is on each, in each one's own frame (the
 * world's, for none) and its limits' angles in w; the hinge's axis on each, and a way across it on each (what its angle
 * is measured from), the rest length and the motor's speed and torque in w.
 */
struct Joint {
    uint4 head;
    float4 anchorA;             // w: the lower limit, radians
    float4 anchorB;             // w: the upper limit
    float4 axisA;               // w: a distance's or a rope's length
    float4 axisB;               // w: the motor's speed, radians a second
    float4 acrossA;             // w: the motor's most torque
    float4 acrossB;
};
static const uint None = 0xFFFFFFFFu;
RWStructuredBuffer<float4> jointsDescribed; // five float4 a Joint: its entity's id and the other's, kind, limited; anchor, lower;
                                            // otherAnchor, upper; axis, motor speed; most torque, collide connected
RWStructuredBuffer<uint> entityIds;         // each body's entity, as a ForEach's entityId: index and one
RWStructuredBuffer<float4> meshesOf;        // which of the pack's meshes each entity with a Mesh shows: its entityId, its group
RWStructuredBuffer<uint> meshRanges;        // the pack's: twelve words a mesh — first index, index count, vertex offset; bounds
RWStructuredBuffer<uint> meshVertices;      // the pack's positions
RWStructuredBuffer<uint> meshIndices;

/*
 * The joints are in shared, after its planes, so that a step is given them with what else setup found: from JointsAt,
 * where each body's joints start in the list (the bodies' and one more: counted first, then summed), how many of each
 * are in it yet (as it is filled), the list — which joints are each body's — and the joints. shared[2]: how many.
 */
static const uint JointsAt = 68;
static const uint JointWords = 28;
uint JointStart(uint i) { return shared[JointsAt + i]; }
uint JointFillAt(uint i) { return JointsAt + push.count + 1 + i; }
uint JointListAt(uint k) { return JointsAt + 2 * push.count + 1 + k; }
uint JointAt(uint j, uint joints) { return JointsAt + 2 * push.count + 1 + 2 * joints + j * JointWords; }
/*
 * After the joints, the mesh colliders — shared[3] of them, twelve words each: the body, its first triangle, how many,
 * then its scale and thickness, then the middle of its bounds and how far they reach — and after room for one a body,
 * the triangles of every mesh of the pack, nine floats each, where they are in the mesh's own frame, unscaled.
 */
static const uint MeshWords = 12;
uint MeshesAt(uint joints) { return JointsAt + 2 * push.count + 1 + 30 * joints; }
uint TrianglesAt(uint joints) { return MeshesAt(joints) + MeshWords * push.count; }

float4 Float4At(uint at) { return asfloat(uint4(shared[at], shared[at + 1], shared[at + 2], shared[at + 3])); }
void StoreFloat4(uint at, float4 v) { const uint4 u = asuint(v); shared[at] = u.x; shared[at + 1] = u.y; shared[at + 2] = u.z; shared[at + 3] = u.w; }
Joint LoadJoint(uint j)
{
    const uint at = JointAt(j, shared[2]);
    Joint joint;
    joint.head = uint4(shared[at], shared[at + 1], shared[at + 2], shared[at + 3]);
    joint.anchorA = Float4At(at + 4); joint.anchorB = Float4At(at + 8);
    joint.axisA = Float4At(at + 12); joint.axisB = Float4At(at + 16);
    joint.acrossA = Float4At(at + 20); joint.acrossB = Float4At(at + 24);
    return joint;
}
void StoreJoint(uint j, uint joints, Joint joint)
{
    const uint at = JointAt(j, joints);
    shared[at] = joint.head.x; shared[at + 1] = joint.head.y; shared[at + 2] = joint.head.z; shared[at + 3] = joint.head.w;
    StoreFloat4(at + 4, joint.anchorA); StoreFloat4(at + 8, joint.anchorB);
    StoreFloat4(at + 12, joint.axisA); StoreFloat4(at + 16, joint.axisB);
    StoreFloat4(at + 20, joint.acrossA); StoreFloat4(at + 24, joint.acrossB);
}

uint TableSize() { return max(push.count * 2, 1024u); }
float CellSize() { return push.cell > 0 ? push.cell : max(2 * asfloat(shared[0]), 0.05); }
int3 CellOf(float3 p) { return int3(floor(p / CellSize())); }
uint Hash3(int3 c) { return (uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u) % TableSize(); }

// ---- quaternions and frames --------------------------------------------------------------------------------------

float4 Multiply(float4 a, float4 b) { return float4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz)); }
float4 Conjugate(float4 q) { return float4(-q.xyz, q.w); }
float4 Turned(float4 q, float3 by) { return normalize(q + Multiply(float4(by, 0), q) * 0.5); }
float3 TurnBetween(float4 from, float4 to) { float4 d = Multiply(to, Conjugate(from)); return (d.w < 0 ? -2.0 : 2.0) * d.xyz; }
float3 Rotate(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float3 Unrotate(float4 q, float3 v) { return Rotate(Conjugate(q), v); }
float3 Axis(float4 q, int k) { return Rotate(q, k == 0 ? float3(1, 0, 0) : k == 1 ? float3(0, 1, 0) : float3(0, 0, 1)); }
/** I⁻¹ v, in the world: turned into the body's own axes, scaled, and back. */
float3 InverseInertia(Body b, float3 v) { return Rotate(b.orientation, b.invInertia.xyz * Unrotate(b.orientation, v)); }
/** How hard it is to move a point r from its middle along n: its inverse mass, and what turning it about its middle takes. */
float Generalized(Body b, float3 r, float3 n) { const float3 rn = cross(r, n); return b.positionInvMass.w + dot(rn, InverseInertia(b, rn)); }
uint ShapeOf(Body b) { return uint(b.spinShape.w + 0.5); }
/** A body XpbdStep collides with another — a sphere, a box, a capsule, a cone — and not a plane or a mesh, met another way. */
bool Solid(Body b) { const uint shape = ShapeOf(b); return shape <= Capsule || shape == Cone; }
/**
 * A static or kinematic body too big for the grid's cells — a floor, a wall, a mixer's bar: kept out of the grid, and
 * met by every body from a list of its own (bigs), so that one of them does not make every cell as big as it is.
 */
bool Big(Body b) { return Solid(b) && b.positionInvMass.w == 0 && b.velocityReach.w > 0.5 * CellSize(); }
/** Where its body is from its entity, in its frame: a cone's centre of mass is a quarter of its height above its base, below its middle. */
float3 Offset(Body b) { return ShapeOf(b) == Cone ? float3(0, -0.25 * b.size.y, 0) : float3(0, 0, 0); }
uint KindOf(Body b) { return uint(b.size.w + 0.5); }
/** How long a dynamic body has been still, in invInertia.w (its mass there only while it is prepared); and whether it sleeps. */
float StillFor(Body b) { return b.invInertia.w; }
bool Asleep(Body b) { return push.sleepAfter > 0 && StillFor(b) >= push.sleepAfter; }
/** Whether a body wakes what it touches: a dynamic one that moved this substep, or a kinematic one going. */
bool Stirring(Body b)
{
    if (b.positionInvMass.w > 0) return StillFor(b) == 0;
    return KindOf(b) == Kinematic && (dot(b.velocityReach.xyz, b.velocityReach.xyz) > 0 || dot(b.spinShape.xyz, b.spinShape.xyz) > 0);
}

// ---- a contact's points ------------------------------------------------------------------------------------------

/** What two bodies touch at: up to eight points (four kept), each where they meet and how deep, the normal from A to B. */
struct Manifold {
    float3 normal;
    uint count;
    float4 points[8];           // xyz where, w how deep
};
void Add(inout Manifold m, float3 at, float depth)
{
    if (m.count < 8) { m.points[m.count] = float4(at, depth); ++m.count; return; }
}

float3 ClosestOnSegment(float3 p, float3 a, float3 b, out float t)
{
    const float3 d = b - a;
    const float l = dot(d, d);
    t = l > 1e-12 ? clamp(dot(p - a, d) / l, 0.0, 1.0) : 0.0;
    return a + d * t;
}
void ClosestBetweenSegments(float3 p1, float3 q1, float3 p2, float3 q2, out float s, out float t)
{
    const float3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    if (a <= 1e-12 && e <= 1e-12) { s = 0; t = 0; return; }
    if (a <= 1e-12) { s = 0; t = clamp(f / e, 0.0, 1.0); return; }
    const float c = dot(d1, r);
    if (e <= 1e-12) { t = 0; s = clamp(-c / a, 0.0, 1.0); return; }
    const float b = dot(d1, d2), denom = a * e - b * b;
    s = denom > 1e-12 ? clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
    t = (b * s + f) / e;
    if (t < 0) { t = 0; s = clamp(-c / a, 0.0, 1.0); }
    else if (t > 1) { t = 1; s = clamp((b - c) / a, 0.0, 1.0); }
}
void CapsuleEnds(Body b, out float3 lower, out float3 upper)
{
    const float3 up = Axis(b.orientation, 1) * b.size.y;
    lower = b.positionInvMass.xyz - up;
    upper = b.positionInvMass.xyz + up;
}

/** Two spheres: true when they overlap; the normal from A's centre to B's, the point between their surfaces. */
bool SphereSphere(float3 a, float ra, float3 b, float rb, out float3 normal, out float3 at, out float depth)
{
    const float3 d = b - a;
    const float distance = length(d);
    normal = distance < 1e-6 ? float3(0, 1, 0) : d / distance;
    at = ((a + normal * ra) + (b - normal * rb)) * 0.5;
    depth = ra + rb - distance;
    return depth > 0;
}

/** A sphere (centre, radius) against box B: the normal out of the box towards the sphere, as kphysics has it. */
bool SphereBox(Body box, float3 centre, float radius, out float3 normal, out float3 at, out float depth)
{
    const float3 local = Unrotate(box.orientation, centre - box.positionInvMass.xyz);
    const float3 h = box.size.xyz;
    const float3 clamped = clamp(local, -h, h);
    const bool inside = all(clamped == local);
    if (!inside) {
        const float3 closest = box.positionInvMass.xyz + Rotate(box.orientation, clamped);
        const float3 d = centre - closest;
        const float distance = length(d);
        if (distance > radius || distance < 1e-9) { normal = float3(0, 1, 0); at = centre; depth = 0; return false; }
        normal = d / distance;
        at = (closest + (centre - normal * radius)) * 0.5;
        depth = radius - distance;
        return true;
    }
    int axis = 0; float gap = 1e30; float side = 1;
    for (int k = 0; k < 3; ++k) {
        const float g = h[k] - abs(local[k]);
        if (g < gap) { gap = g; axis = k; side = local[k] >= 0 ? 1.0 : -1.0; }
    }
    normal = Axis(box.orientation, axis) * side;
    at = ((centre + normal * gap) + (centre - normal * radius)) * 0.5;
    depth = radius + gap;
    return true;
}

float BoxSeparation(Body a, Body b, float3 d, float3 axis)
{
    float ra = 0, rb = 0;
    for (int k = 0; k < 3; ++k) {
        ra += a.size[k] * abs(dot(Axis(a.orientation, k), axis));
        rb += b.size[k] * abs(dot(Axis(b.orientation, k), axis));
    }
    return abs(dot(d, axis)) - (ra + rb);
}

/** A face of box ref (0 +X, 1 −X, ...) facing box inc, normal n out of it: the incident face clipped by its sides. */
void BoxBoxFace(Body ref, Body inc, int face, float3 n, bool refIsA, inout Manifold m)
{
    int incident = 0; float lowest = 1e30;
    for (int f = 0; f < 6; ++f) {
        const float along = (f % 2 == 0 ? 1.0 : -1.0) * dot(Axis(inc.orientation, f / 2), n);
        if (along < lowest) { lowest = along; incident = f; }
    }
    const int fi = incident / 2, fu = (fi + 1) % 3, fw = (fi + 2) % 3;
    float3 poly[8], clipped[8];
    for (int q = 0; q < 4; ++q) {
        float3 local = float3(0, 0, 0);
        local[fi] = (incident % 2 == 0 ? 1.0 : -1.0) * inc.size[fi];
        local[fu] = (q == 1 || q == 2 ? 1.0 : -1.0) * inc.size[fu];
        local[fw] = (q >= 2 ? 1.0 : -1.0) * inc.size[fw];
        poly[q] = inc.positionInvMass.xyz + Rotate(inc.orientation, local);
    }
    int count = 4;
    const int k = face / 2;
    for (int side = 0; side < 4; ++side) {
        const int t = side < 2 ? (k + 1) % 3 : (k + 2) % 3;
        const float3 plane = Axis(ref.orientation, t) * (side % 2 == 0 ? 1.0 : -1.0);
        const float offset = dot(plane, ref.positionInvMass.xyz) + ref.size[t] + 0.001;
        int kept = 0;
        for (int q = 0; q < count; ++q) {
            const float3 p = poly[q], r = poly[(q + 1) % count];
            const float sp = dot(plane, p) - offset, sr = dot(plane, r) - offset;
            if (sp <= 0 && kept < 8) clipped[kept++] = p;
            if ((sp < 0) != (sr < 0) && kept < 8) clipped[kept++] = p + (r - p) * (sp / (sp - sr));
        }
        count = kept;
        for (int q = 0; q < count; ++q) poly[q] = clipped[q];
        if (count == 0) return;
    }
    const float faceOffset = dot(n, ref.positionInvMass.xyz) + ref.size[k];
    m.normal = refIsA ? n : -n;
    for (int q = 0; q < count; ++q) {
        const float s = dot(n, poly[q]) - faceOffset;
        if (s > 0) continue;
        Add(m, poly[q] + n * (-s / 2), -s);
    }
}

void BoxBox(Body a, Body b, inout Manifold m)
{
    const float3 d = b.positionInvMass.xyz - a.positionInvMass.xyz;
    float sA = -1e30, sB = -1e30; int faceA = 0, faceB = 0;
    for (int i = 0; i < 3; ++i) {
        const float s1 = BoxSeparation(a, b, d, Axis(a.orientation, i));
        if (s1 > 0) return;
        if (s1 > sA) { sA = s1; faceA = i; }
        const float s2 = BoxSeparation(a, b, d, Axis(b.orientation, i));
        if (s2 > 0) return;
        if (s2 > sB) { sB = s2; faceB = i; }
    }
    float sE = -1e30; int edgeA = 0, edgeB = 0; float3 edgeAxis = float3(0, 1, 0);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float3 axis = cross(Axis(a.orientation, i), Axis(b.orientation, j));
            const float l = length(axis);
            if (l < 1e-6) continue;
            axis /= l;
            const float s = BoxSeparation(a, b, d, axis);
            if (s > 0) return;
            if (s > sE) { sE = s; edgeA = i; edgeB = j; edgeAxis = axis; }
        }
    const bool referenceIsA = !(sB > 0.95 * sA + 0.005);
    const float sReference = referenceIsA ? sA : sB;
    if (sE > 0.95 * sReference + 0.005) {
        if (dot(d, edgeAxis) < 0) edgeAxis = -edgeAxis;
        float3 centre = a.positionInvMass.xyz;
        for (int k = 0; k < 3; ++k) if (k != edgeA) centre += Axis(a.orientation, k) * ((dot(Axis(a.orientation, k), edgeAxis) >= 0 ? 1.0 : -1.0) * a.size[k]);
        const float3 a0 = centre - Axis(a.orientation, edgeA) * a.size[edgeA], a1 = centre + Axis(a.orientation, edgeA) * a.size[edgeA];
        centre = b.positionInvMass.xyz;
        for (int k = 0; k < 3; ++k) if (k != edgeB) centre += Axis(b.orientation, k) * ((dot(Axis(b.orientation, k), edgeAxis) >= 0 ? -1.0 : 1.0) * b.size[k]);
        const float3 b0 = centre - Axis(b.orientation, edgeB) * b.size[edgeB], b1 = centre + Axis(b.orientation, edgeB) * b.size[edgeB];
        float s, t;
        ClosestBetweenSegments(a0, a1, b0, b1, s, t);
        m.normal = edgeAxis;
        Add(m, (a0 + (a1 - a0) * s + b0 + (b1 - b0) * t) * 0.5, -sE);
        return;
    }
    if (referenceIsA) {
        const float3 u = Axis(a.orientation, faceA);
        const float sign = dot(d, u) < 0 ? -1.0 : 1.0;
        BoxBoxFace(a, b, faceA * 2 + (sign > 0 ? 0 : 1), u * sign, true, m);
    } else {
        const float3 u = Axis(b.orientation, faceB);
        const float sign = dot(d, u) > 0 ? -1.0 : 1.0;
        BoxBoxFace(b, a, faceB * 2 + (sign > 0 ? 0 : 1), u * sign, false, m);
    }
}

/** A box A and a capsule B: each end as a sphere against the box, or — neither touching — the segment's point nearest the box. */
void BoxCapsule(Body a, Body b, inout Manifold m)
{
    float3 lower, upper; CapsuleEnds(b, lower, upper);
    const float r = b.size.x;
    float3 n1, at1, n2, at2; float d1, d2;
    const bool first = SphereBox(a, lower, r, n1, at1, d1);
    const bool second = SphereBox(a, upper, r, n2, at2, d2);
    if (!first && !second) {
        float3 q = (lower + upper) * 0.5;
        for (int k = 0; k < 4; ++k) {
            const float3 local = clamp(Unrotate(a.orientation, q - a.positionInvMass.xyz), -a.size.xyz, a.size.xyz);
            float t;
            q = ClosestOnSegment(a.positionInvMass.xyz + Rotate(a.orientation, local), lower, upper, t);
        }
        if (!SphereBox(a, q, r, n1, at1, d1)) return;
        m.normal = n1;
        Add(m, at1, d1);
        return;
    }
    m.normal = first && (!second || d1 >= d2) ? n1 : n2;
    if (first) Add(m, at1, d1);
    if (second) Add(m, at2, d2);
}

/** A and B — A's shape no later than B's (sphere, box, capsule) — and the points they touch at, the normal from A to B. */
// ---- by points: what a cone meets ---------------------------------------------------------------------------------

/**
 * How far point p is outside body b (less than 0: inside), and the way out of it there, n: the distance to its surface
 * as each shape has it. A cone's is its outline's — the base and the slant, turned about its axis — in its middle's frame.
 */
float Distance(Form b, float3 p, out float3 n)
{
    const float3 c = b.middleReach.xyz;
    const uint shape = uint(b.size.w + 0.5);
    if (shape == Sphere) {
        const float3 v = p - c;
        const float l = length(v);
        n = l > 1e-7 ? v / l : float3(0, 1, 0);
        return l - b.size.x;
    }
    if (shape == Capsule) {
        const float3 up = Axis(b.orientation, 1) * b.size.y;
        const float3 lower = c - up, upper = c + up;
        float t;
        const float3 v = p - ClosestOnSegment(p, lower, upper, t);
        const float l = length(v);
        n = l > 1e-7 ? v / l : Axis(b.orientation, 0);
        return l - b.size.x;
    }
    const float3 q = Unrotate(b.orientation, p - c);
    if (shape == Box) {
        const float3 d = abs(q) - b.size.xyz;
        const float3 side = float3(q.x < 0 ? -1 : 1, q.y < 0 ? -1 : 1, q.z < 0 ? -1 : 1);
        if (any(d > 0)) {
            const float3 out = max(d, 0);
            const float l = length(out);
            n = Rotate(b.orientation, out * side / l);
            return l;
        }
        const int k = d.x > d.y ? (d.x > d.z ? 0 : 2) : (d.y > d.z ? 1 : 2);
        float3 face = float3(0, 0, 0);
        face[k] = side[k];
        n = Rotate(b.orientation, face);
        return d[k];
    }
    // A cone: (across, up) about its axis, from its middle; its outline the base, (0..R, −h) and the slant, (R, −h) to (0, h).
    const float R = b.size.x, h = 0.5 * b.size.y;
    const float rho = length(q.xz);
    const float2 radial = rho > 1e-7 ? q.xz / rho : float2(1, 0);
    const float2 P = float2(rho, q.y - 0.25 * b.size.y);
    const float2 slant = normalize(float2(2 * h, R));          // out of the slant
    const float2 B = float2(R, -h), apex = float2(0, h);
    float2 n2;
    float d;
    const float under = -h - P.y, beyond = dot(P - B, slant);
    if (under <= 0 && beyond <= 0) {
        // Inside: out by the nearer of the base and the slant.
        if (-under < -beyond) { d = under; n2 = float2(0, -1); } else { d = beyond; n2 = slant; }
    } else {
        const float2 onBase = float2(clamp(P.x, 0.0, R), -h);
        const float2 along = apex - B;
        const float2 onSlant = B + along * clamp(dot(P - B, along) / dot(along, along), 0.0, 1.0);
        const float2 nearest = dot(P - onBase, P - onBase) < dot(P - onSlant, P - onSlant) ? onBase : onSlant;
        d = length(P - nearest);
        n2 = d > 1e-7 ? (P - nearest) / d : slant;
    }
    n = Rotate(b.orientation, float3(n2.x * radial.x, n2.y, n2.x * radial.y));
    return d - b.size.z;
}
float Distance(Body b, float3 p, out float3 n) { return Distance(FormOf(b), p, n); }

static const float RimCos[12] = { 1, 0.8660254, 0.5, 0, -0.5, -0.8660254, -1, -0.8660254, -0.5, 0, 0.5, 0.8660254 };
static const float RimSin[12] = { 0, 0.5, 0.8660254, 1, 0.8660254, 0.5, 0, -0.5, -0.8660254, -1, -0.8660254, -0.5 };

/** What of a body its points and its distance are of — where it is and how far it reaches, its turn, its size and shape: less to carry. */
struct Form {
    float4 middleReach;
    float4 orientation;
    float4 size;            // w its shape
};
Form FormOf(Body b)
{
    Form f;
    f.middleReach = float4(b.positionInvMass.xyz, b.velocityReach.w);
    f.orientation = b.orientation;
    f.size = float4(b.size.xyz, b.spinShape.w);
    return f;
}
uint PointCount(Form f) { const uint shape = uint(f.size.w + 0.5); return shape == Sphere ? 1 : shape == Capsule ? 2 : shape == Box ? 8 : shape == Cone ? 13 : 0; }

/** How many points a body is met by: a sphere's middle, a capsule's two ends, a box's corners, a cone's point and rim. */
uint PointCount(Body b)
{
    const uint shape = ShapeOf(b);
    return shape == Sphere ? 1 : shape == Capsule ? 2 : shape == Box ? 8 : shape == Cone ? 13 : 0;
}
/** Its point k, and how far about it the body is (w): a sphere's or a capsule's radius; a corner's none. */
float4 PointOf(Form b, uint k)
{
    const float3 c = b.middleReach.xyz;
    const uint shape = uint(b.size.w + 0.5);
    if (shape == Sphere) return float4(c, b.size.x);
    if (shape == Capsule) return float4(c + Axis(b.orientation, 1) * (k == 0 ? -b.size.y : b.size.y), b.size.x);
    if (shape == Box) return float4(c + Rotate(b.orientation, float3((k & 1) ? 1 : -1, (k & 2) ? 1 : -1, (k & 4) ? 1 : -1) * b.size.xyz), 0);
    // A cone's, from its centre of mass: its point three quarters of its height up, its rim a quarter down.
    if (k == 12) return float4(c + Rotate(b.orientation, float3(0, 0.75 * b.size.y, 0)), b.size.z);
    return float4(c + Rotate(b.orientation, float3(b.size.x * RimCos[k], -0.25 * b.size.y, b.size.x * RimSin[k])), b.size.z);
}
float4 PointOf(Body b, uint k) { return PointOf(FormOf(b), k); }

/**
 * Whether segment p0–p1 runs through a box (clipped to its slabs, in its frame), and the middle of where it does.
 */
bool ThroughBox(Form box, float3 p0, float3 p1, out float3 inside)
{
    inside = p0;
    const float3 a = Unrotate(box.orientation, p0 - box.middleReach.xyz), d = Unrotate(box.orientation, p1 - p0);
    float t0 = 0, t1 = 1;
    for (uint k = 0; k < 3; ++k) {
        if (abs(d[k]) < 1e-9) { if (abs(a[k]) > box.size[k]) return false; continue; }
        float near = (-box.size[k] - a[k]) / d[k], far = (box.size[k] - a[k]) / d[k];
        if (near > far) { const float t = near; near = far; far = t; }
        t0 = max(t0, near);
        t1 = min(t1, far);
        if (t0 >= t1) return false;
    }
    inside = p0 + (p1 - p0) * (0.5 * (t0 + t1));
    return true;
}

/**
 * Two bodies met by points, either of them a cone: each point of A inside B, and each of B inside A, as deep as it is —
 * measured once, a point far from the other's bounding sphere passed over; the normal from A to B their ways out
 * weighed by how deep, and each depth along it.
 */
void ByPoints(Body a, Body b, inout Manifold m)
{
    float4 where[8];        // the point, and in w its radius: plus one for A's, less one for B's (so that 0 is neither)
    float4 out[8];          // its way out of the other, from A to B, and how deep
    uint found = 0;
    float3 sum = float3(0, 0, 0);
    const Form fa = FormOf(a), fb = FormOf(b);
    for (uint side = 0; side < 2; ++side) {
        const bool ofA = side == 0;
        const uint count = PointCount(ofA ? fa : fb);
        for (uint k = 0; k < count; ++k) {
            const float4 p = PointOf(ofA ? fa : fb, k);
            const float4 other = ofA ? fb.middleReach : fa.middleReach;
            const float3 off = p.xyz - other.xyz;
            if (dot(off, off) >= (other.w + p.w) * (other.w + p.w)) continue;
            float3 way;
            const float depth = p.w - Distance(ofA ? fb : fa, p.xyz, way);
            if (depth <= 0) continue;
            const float3 aToB = ofA ? -way : way;           // a point of A goes out of B against B's way out
            sum += aToB * depth;
            if (found < 8) {
                where[found] = float4(p.xyz, ofA ? p.w + 1 : -(p.w + 1));
                out[found] = float4(aToB, depth);
                ++found;
            }
        }
    }
    // A cone and a box: what neither's points find — a blade through a cone's slant, its edge across it. The cone's lines
    // from its point to its rim, each where it runs through the box (its middle there), and the box's edges, each at its
    // point nearest the cone's middle.
    const uint shapeA = uint(fa.size.w + 0.5), shapeB = uint(fb.size.w + 0.5);
    if ((shapeA == Cone && shapeB == Box) || (shapeA == Box && shapeB == Cone)) {
        const bool coneIsA = shapeA == Cone;
        const Form cone = coneIsA ? fa : fb, box = coneIsA ? fb : fa;
        const float4 apex = PointOf(cone, 12);
        for (uint k = 0; k < 12; ++k) {
            const float4 rim = PointOf(cone, k);
            float3 inside;
            if (!ThroughBox(box, apex.xyz, rim.xyz, inside)) continue;
            float3 way;
            const float depth = -Distance(box, inside, way);
            if (depth <= 0) continue;
            const float3 aToB = coneIsA ? -way : way;
            sum += aToB * depth;
            if (found < 8) { where[found] = float4(inside, coneIsA ? 1 : -1); out[found] = float4(aToB, depth); ++found; }
        }
        for (uint e = 0; e < 12; ++e) {
            // Edge e: along axis e / 4, at the corner given by the other two axes' signs.
            const uint along = e / 4;
            float3 from = float3((e & 1) ? 1 : -1, (e & 2) ? 1 : -1, 0);
            from = along == 0 ? float3(0, from.x, from.y) : along == 1 ? float3(from.x, 0, from.y) : from;
            float3 to = from;
            from[along] = -1; to[along] = 1;
            const float3 a = box.middleReach.xyz + Rotate(box.orientation, from * box.size.xyz);
            const float3 b = box.middleReach.xyz + Rotate(box.orientation, to * box.size.xyz);
            float u;
            const float3 p = ClosestOnSegment(cone.middleReach.xyz, a, b, u);
            float3 way;
            const float depth = -Distance(cone, p, way);
            if (depth <= 0) continue;
            const float3 aToB = coneIsA ? way : -way;     // the box's point in the cone: the cone goes out of the way it is pushed
            sum += aToB * depth;
            if (found < 8) { where[found] = float4(p, coneIsA ? -1 : 1); out[found] = float4(aToB, depth); ++found; }
        }
    }
    const float l = length(sum);
    if (l < 1e-9) return;
    m.normal = sum / l;
    for (uint q = 0; q < found; ++q) {
        const float along = out[q].w * dot(out[q].xyz, m.normal);
        if (along <= 0) continue;
        const bool ofA = where[q].w > 0;
        const float radius = abs(where[q].w) - 1;
        // Where they meet: halfway between the point's surface and the other's.
        const float3 toward = ofA ? m.normal : -m.normal;
        const float3 surface = where[q].xyz + toward * radius;
        Add(m, surface - toward * (along / 2), along);
    }
}

// ---- a mesh's triangles -------------------------------------------------------------------------------------------

/** The point of triangle (a, b, c) nearest p (Ericson's, by its regions). */
float3 ClosestOnTriangle(float3 p, float3 a, float3 b, float3 c)
{
    const float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const float3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const float3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denominator = 1 / (va + vb + vc);
    return a + ab * (vb * denominator) + ac * (vc * denominator);
}

/**
 * A point p, r about it, against a mesh's triangle (counter-clockwise about the way it faces), of a body whose middle is
 * not too far behind it (OnTriangle says): in front, it touches the nearest point of it within r; straight behind its
 * face, it is pushed out through the face. `out` is the way out, from the triangle to the point.
 */
bool TrianglePoint(float3 p, float r, float3 a, float3 b, float3 c, out float3 out, out float depth, out float3 at, out bool behind)
{
    out = float3(0, 1, 0); depth = 0; at = p; behind = false;
    float3 n = cross(b - a, c - a);
    const float area = length(n);
    if (area < 1e-12) return false;
    n /= area;
    const float s = dot(p - a, n);
    const float3 nearest = ClosestOnTriangle(p, a, b, c);
    const float3 v = p - nearest;
    if (s >= 0) {
        const float d = length(v);
        if (d >= r) return false;
        out = d > 1e-7 ? v / d : n;
        depth = r - d;
    } else {
        const bool facing = dot(v - n * s, v - n * s) < 1e-8 * max(1.0, dot(p, p));    // straight behind the face
        if (!facing) return false;
        out = n;
        depth = r - s;
        behind = true;
    }
    at = (nearest + p - out * r) * 0.5;
    return true;
}

/** A body's least size across: a quarter of it is how far its middle may go behind a mesh's face and still come back out. */
float LeastAcross(Body b)
{
    const uint shape = ShapeOf(b);
    if (shape == Box) return 2 * min(b.size.x, min(b.size.y, b.size.z));
    if (shape == Cone) return min(2 * b.size.x, b.size.y);
    return 2 * b.size.x;
}

/** Triangle t of mesh collider k, where it is in its mesh's own frame, unscaled. */
void MeshTriangleLocal(uint k, uint t, out float3 a, out float3 b, out float3 c)
{
    const uint first = shared[MeshesAt(shared[2]) + k * MeshWords + 1];
    const uint at = TrianglesAt(shared[2]) + (first + t) * 9;
    a = asfloat(uint3(shared[at], shared[at + 1], shared[at + 2]));
    b = asfloat(uint3(shared[at + 3], shared[at + 4], shared[at + 5]));
    c = asfloat(uint3(shared[at + 6], shared[at + 7], shared[at + 8]));
}
/** A point of a mesh's own frame, unscaled, where it is in the world now. */
float3 OfMesh(Body mesh, float3 scale, float3 local) { return mesh.positionInvMass.xyz + Rotate(mesh.orientation, scale * local); }
uint MeshTriangleCount(uint k) { return shared[MeshesAt(shared[2]) + k * MeshWords + 2]; }

static const uint MostPoints = 13;      // a cone's
static const uint MostPatches = 4;
static const float MostPushSpeed = 5;   // a mesh's push out of a body, at most, m/s: the velocity solve stops what comes in, and this is what is left

/** A contact of me with a mesh into its patches, by its normal (from me into the mesh): a new one, or the likest. */
void IntoPatch(inout Manifold patches[MostPatches], inout uint made, float3 normal, float3 at, float depth)
{
    uint into = made;
    for (uint g = 0; g < made; ++g) if (dot(patches[g].normal, normal) > 0.98) { into = g; break; }
    if (into == made) {
        if (made == MostPatches) {
            into = 0;
            for (uint g = 1; g < made; ++g) if (dot(patches[g].normal, normal) > dot(patches[into].normal, normal)) into = g;
        } else {
            patches[made].count = 0;
            patches[made].normal = normal;
            ++made;
        }
    }
    const float along = min(depth * max(dot(normal, patches[into].normal), 0.0), MostPushSpeed * push.h);
    if (along > 0) Add(patches[into], at, along);
}

/** Mesh collider k's body, and whether body me is near enough its bounds to meet any of its triangles. */
bool NearMesh(Body me, uint k, out uint body, out float4 scaleThickness)
{
    const uint at = MeshesAt(shared[2]) + k * MeshWords;
    body = shared[at];
    scaleThickness = Float4At(at + 4);
    const float4 bounds = Float4At(at + 8);
    const Body mesh = bodies[body];
    const float3 middle = mesh.positionInvMass.xyz + Rotate(mesh.orientation, bounds.xyz);
    const float reach = bounds.w + me.velocityReach.w + scaleThickness.w;
    return dot(me.positionInvMass.xyz - middle, me.positionInvMass.xyz - middle) < reach * reach;
}


Manifold Collide(Body a, Body b)
{
    Manifold m;
    m.count = 0;
    m.normal = float3(0, 1, 0);
    const uint sa = ShapeOf(a), sb = ShapeOf(b);
    float3 n, at; float depth;
    if (sa == Cone || sb == Cone) {
        ByPoints(a, b, m);
    } else if (sa == Sphere && sb == Sphere) {
        if (SphereSphere(a.positionInvMass.xyz, a.size.x, b.positionInvMass.xyz, b.size.x, n, at, depth)) { m.normal = n; Add(m, at, depth); }
    } else if (sa == Sphere && sb == Box) {
        if (SphereBox(b, a.positionInvMass.xyz, a.size.x, n, at, depth)) { m.normal = -n; Add(m, at, depth); }
    } else if (sa == Sphere && sb == Capsule) {
        float3 lower, upper; CapsuleEnds(b, lower, upper);
        float t;
        const float3 q = ClosestOnSegment(a.positionInvMass.xyz, lower, upper, t);
        if (SphereSphere(a.positionInvMass.xyz, a.size.x, q, b.size.x, n, at, depth)) { m.normal = n; Add(m, at, depth); }
    } else if (sa == Box && sb == Box) {
        BoxBox(a, b, m);
    } else if (sa == Box && sb == Capsule) {
        BoxCapsule(a, b, m);
    } else {
        float3 a0, a1, b0, b1; CapsuleEnds(a, a0, a1); CapsuleEnds(b, b0, b1);
        float s, t;
        ClosestBetweenSegments(a0, a1, b0, b1, s, t);
        const float3 da = a1 - a0, db = b1 - b0;
        // Side by side: a point at each end of where they overlap, so that one lying on another does not rock.
        const float la = dot(da, da), lb = dot(db, db);
        const float3 crossed = cross(da, db);
        if (la > 1e-12 && lb > 1e-12 && dot(crossed, crossed) <= 1e-6 * la * lb) {
            const float t0 = dot(b0 - a0, da) / la, t1 = dot(b1 - a0, da) / la;
            const float lo = max(0.0, min(t0, t1)), hi = min(1.0, max(t0, t1));
            if (hi - lo > 1e-4) {
                for (int e = 0; e < 2; ++e) {
                    const float3 p = a0 + da * (e == 0 ? lo : hi);
                    float u;
                    const float3 q = ClosestOnSegment(p, b0, b1, u);
                    if (SphereSphere(p, a.size.x, q, b.size.x, n, at, depth)) { m.normal = n; Add(m, at, depth); }
                }
                return m;
            }
        }
        if (SphereSphere(a0 + da * s, a.size.x, b0 + db * t, b.size.x, n, at, depth)) { m.normal = n; Add(m, at, depth); }
    }
    return m;
}

/**
 * A body on a plane (its normal n, d along it from the origin): a sphere's lowest point; a box's corners behind it; a
 * capsule's two ends. The normal from the body into the plane.
 */
Manifold OnPlane(Body a, float3 n, float d)
{
    Manifold m;
    m.count = 0;
    m.normal = -n;
    const uint shape = ShapeOf(a);
    const float3 p = a.positionInvMass.xyz;
    if (shape == Sphere) {
        const float s = dot(n, p) - a.size.x - d;
        if (s < 0) Add(m, p - n * (a.size.x + s / 2), -s);
    } else if (shape == Box) {
        for (int c = 0; c < 8; ++c) {
            const float3 corner = p + Rotate(a.orientation, float3((c & 1) ? 1 : -1, (c & 2) ? 1 : -1, (c & 4) ? 1 : -1) * a.size.xyz);
            const float s = dot(n, corner) - d;
            if (s < 0) Add(m, corner - n * (s / 2), -s);
        }
    } else {
        // A capsule's two ends, a cone's point and rim: each a point with a radius.
        for (uint k = 0; k < PointCount(a); ++k) {
            const float4 point = PointOf(a, k);
            const float s = dot(n, point.xyz) - point.w - d;
            if (s < 0) Add(m, point.xyz - n * (point.w + s / 2), -s);
        }
    }
    return m;
}
uint Planes() { return min(shared[1], MostPlanes); }
float4 PlaneAt(uint k) { const uint at = 4 + k * 4; return float4(asfloat(shared[at]), asfloat(shared[at + 1]), asfloat(shared[at + 2]), asfloat(shared[at + 3])); }

// ---- the passes --------------------------------------------------------------------------------------------------

/** What it moves by and turns by, and how far it reaches, from its shape, its size and its mass (0: nothing moves it). */
Body Finished(Body b, float mass)
{
    const float3 s = b.size.xyz;
    const uint shape = ShapeOf(b);
    float3 inertia;
    float reach;
    if (shape == Sphere) { inertia = 0.4 * mass * s.x * s.x; reach = s.x; }
    else if (shape == Box) { inertia = mass / 3.0 * float3(s.y * s.y + s.z * s.z, s.x * s.x + s.z * s.z, s.x * s.x + s.y * s.y); reach = length(s); }
    else if (shape == Cone) {
        // About its centre of mass, as kphysics has it: its point three quarters of its height above, its rim a quarter below.
        const float across = 3.0 / 20.0 * mass * s.x * s.x + 3.0 / 80.0 * mass * s.y * s.y;
        inertia = float3(across, 3.0 / 10.0 * mass * s.x * s.x, across);
        reach = max(0.75 * s.y, sqrt(s.x * s.x + 0.0625 * s.y * s.y)) + s.z;
    } else if (shape > Cone || shape == Plane) {
        inertia = float3(1, 1, 1);
        reach = 0;
    } else {
        // A cylinder between two half spheres, along Y: their masses in proportion to their volumes.
        const float r = s.x, half = s.y, cylinder = 3.14159265 * r * r * 2 * half, ball = 4.0 / 3.0 * 3.14159265 * r * r * r;
        const float mc = mass * cylinder / (cylinder + ball), mb = mass - mc;
        const float along = mc * r * r / 2 + mb * 0.4 * r * r;
        const float across = mc * (r * r / 4 + half * half / 3) + mb * (0.4 * r * r + half * half + 0.75 * half * r);
        inertia = float3(across, along, across);
        reach = half + r;
    }
    b.positionInvMass.w = mass > 0 ? 1 / mass : 0;
    b.invInertia = float4(mass > 0 ? 1 / inertia : float3(0, 0, 0), 0);
    b.velocityReach.w = Solid(b) ? reach : 0;
    // The grid's cells are sized by what moves: a floor, a wall or a bar too big for them is met another way (Big).
    if (Solid(b) && mass > 0) { uint was; InterlockedMax(shared[0], asuint(b.velocityReach.w), was); }   // a positive float's bits order as it does
    b.placed = float4(b.positionInvMass.xyz - Rotate(b.orientation, Offset(b)), 0);
    return b;
}

[shader("compute")] [numthreads(64, 1, 1)]
void prepare(uint3 id : SV_DispatchThreadID)
{
    // Its mass (in invInertia.w, as setup gives it; 0 for one nothing moves) made what it moves by and turns by.
    if (id.x >= push.count) return;
    Body b = bodies[id.x];
    bodies[id.x] = Finished(b, b.invInertia.w);
}

/**
 * A body of what an entity's RigidBody and Collider say, as kphysics' bridge reads them: its place, its turn and how
 * fast it goes; its shape and its size as its scale makes it (a sphere the largest scale; a capsule's radius across, its
 * length along Y); its mass, given or of its density and its volume — none for a static or kinematic one, which nothing
 * pushes; its material.
 *
 *   0 position, kind     1 orientation        2 velocity, shape       3 spin, radius
 *   4 halfExtents, height                     5 scale, density        6 mass, friction, restitution, angular damping
 *   7 linear damping
 */
[shader("compute")] [numthreads(64, 1, 1)]
void fromComponents(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    const uint at = id.x * 8;
    const float4 d0 = described[at], d1 = described[at + 1], d2 = described[at + 2], d3 = described[at + 3];
    const float4 d4 = described[at + 4], d5 = described[at + 5], d6 = described[at + 6], d7 = described[at + 7];
    const uint kind = uint(d0.w + 0.5), shape = uint(d2.w + 0.5);
    const float3 scale = abs(d5.xyz);
    Body b;
    b.positionInvMass = float4(d0.xyz, 0);
    b.orientation = dot(d1, d1) > 0 ? normalize(d1) : float4(0, 0, 0, 1);
    b.velocityReach = float4(d2.xyz, 0);
    b.spinShape = float4(d3.xyz, float(shape));
    float volume = 0;
    if (shape == Sphere) {
        const float r = d3.w * max(scale.x, max(scale.y, scale.z));
        b.size = float4(r, 0, 0, float(kind));
        volume = 4.0 / 3.0 * 3.14159265 * r * r * r;
    } else if (shape == Box) {
        b.size = float4(d4.xyz * scale, float(kind));
        volume = 8 * b.size.x * b.size.y * b.size.z;
    } else if (shape == Capsule) {
        const float r = d3.w * max(scale.x, scale.z);
        const float half = max(d4.w * scale.y / 2 - r, 0.0);
        b.size = float4(r, half, 0, float(kind));
        volume = 3.14159265 * r * r * 2 * half + 4.0 / 3.0 * 3.14159265 * r * r * r;
    } else if (shape == Cone) {
        // Its base's radius by the larger scale across, its height by the scale along; its body at its centre of mass.
        const float R = d3.w * max(scale.x, scale.z), H = d4.w * scale.y;
        b.size = float4(R, H, 0, float(kind));
        volume = 3.14159265 * R * R * H / 3.0;
        b.positionInvMass.xyz += Rotate(b.orientation, Offset(b));
    } else {
        b.size = float4(0, 0, 0, float(kind));
        if (shape == Plane) {
            // Every step's, as a plane: its +Y, through it.
            uint k;
            InterlockedAdd(shared[1], 1, k);
            if (k < MostPlanes) {
                const float3 n = Axis(b.orientation, 1);
                const uint at = 4 + k * 4;
                shared[at] = asuint(n.x); shared[at + 1] = asuint(n.y); shared[at + 2] = asuint(n.z); shared[at + 3] = asuint(dot(n, d0.xyz));
            }
        }
    }
    const float mass = kind == Dynamic && (shape <= Capsule || shape == Cone) ? (d6.x > 0 ? d6.x : d5.w * volume) : 0;
    b.invInertia = float4(0, 0, 0, 0);
    Body made = Finished(b, mass);
    if (kind == Kinematic) made.placed.w = d7.y;    // its entity's id: it follows its entity
    bodies[id.x] = made;
    materials[id.x] = float4(d6.y, d6.z, d7.x, d6.w);
    entityIds[id.x] = uint(d7.y + 0.5);
}

uint BodyOf(uint entity)
{
    if (entity == 0) return None;
    for (uint k = 0; k < push.count; ++k) if (entityIds[k] == entity) return k;
    return None;
}
float3 AnyAcross(float3 n) { return normalize(abs(n.y) < 0.9 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0))); }

/**
 * Each Joint resolved, as kphysics' bridge reads it: its bodies found by their entities; where it is on its own body
 * (anchor, in that body's frame), and so in the world, and that same point on the other's — or the world, fixed there; a
 * distance's or a rope's other end (otherAnchor, on the other or in the world) and the length between them now; a hinge's
 * axis (in its own body's frame) and a way across it, each as the other has them. And each body's joints counted.
 * push.count the bodies, push.joints the joints.
 */
[shader("compute")] [numthreads(64, 1, 1)]
void resolveJoints(uint3 id : SV_DispatchThreadID)
{
    const uint j = id.x, count = push.joints;
    if (j >= count) return;
    const float4 j0 = jointsDescribed[j * 5], j1 = jointsDescribed[j * 5 + 1], j2 = jointsDescribed[j * 5 + 2];
    const float4 j3 = jointsDescribed[j * 5 + 3], j4 = jointsDescribed[j * 5 + 4];
    Joint joint;
    const uint a = BodyOf(uint(j0.x + 0.5)), b = BodyOf(uint(j0.y + 0.5));
    const uint kind = uint(j0.z + 0.5);
    joint.head = uint4(a, b, a == None ? 99 : kind, (j0.w > 0.5 ? 1u : 0u) | (j4.y > 0.5 ? 2u : 0u));
    StoreJoint(j, count, joint);
    if (a == None) return;
    const Body A = bodies[a];
    // Anchors are where they are on the entity: on the body, from its centre of mass.
    const float3 anchor = j1.xyz - Offset(A);
    const float3 at = A.positionInvMass.xyz + Rotate(A.orientation, anchor);
    const float radians = 3.14159265 / 180.0;
    joint.anchorA = float4(anchor, j1.w * radians);
    float3 end = at;
    if (kind == 2 || kind == 3) end = b != None ? bodies[b].positionInvMass.xyz + Rotate(bodies[b].orientation, j2.xyz - Offset(bodies[b])) : j2.xyz;
    const float3 onB = b != None ? Unrotate(bodies[b].orientation, end - bodies[b].positionInvMass.xyz) : end;
    joint.anchorB = float4(onB, j2.w * radians);
    const float3 axisWorld = Rotate(A.orientation, normalize(dot(j3.xyz, j3.xyz) > 0 ? j3.xyz : float3(0, 1, 0)));
    const float3 acrossWorld = AnyAcross(axisWorld);
    joint.axisA = float4(Unrotate(A.orientation, axisWorld), length(end - at));
    joint.axisB = float4(b != None ? Unrotate(bodies[b].orientation, axisWorld) : axisWorld, j3.w);
    joint.acrossA = float4(Unrotate(A.orientation, acrossWorld), j4.x);
    joint.acrossB = float4(b != None ? Unrotate(bodies[b].orientation, acrossWorld) : acrossWorld, 0);
    StoreJoint(j, count, joint);
    uint was;
    InterlockedAdd(shared[JointsAt + a], 1, was);
    if (b != None) InterlockedAdd(shared[JointsAt + b], 1, was);
}

/** The bodies' counts of joints summed into where each one's start, by one thread: once, as setup is made. */
[shader("compute")] [numthreads(1, 1, 1)]
void sumJoints(uint3 id : SV_DispatchThreadID)
{
    uint sum = 0;
    for (uint i = 0; i <= push.count; ++i) { const uint n = shared[JointsAt + i]; shared[JointsAt + i] = sum; sum += n; }
    shared[2] = push.joints;
}

/** Each joint put in the lists of the bodies it is of, once the counts are summed into where each body's start. push.count the bodies. */
[shader("compute")] [numthreads(64, 1, 1)]
void fillJoints(uint3 id : SV_DispatchThreadID)
{
    const uint j = id.x;
    if (j >= shared[2]) return;
    const Joint joint = LoadJoint(j);
    if (joint.head.x == None) return;
    uint k;
    InterlockedAdd(shared[JointFillAt(joint.head.x)], 1, k);
    shared[JointListAt(JointStart(joint.head.x) + k)] = j;
    if (joint.head.y != None) { InterlockedAdd(shared[JointFillAt(joint.head.y)], 1, k); shared[JointListAt(JointStart(joint.head.y) + k)] = j; }
}

/**
 * Each mesh collider's line in shared: its body, which of the pack's triangles are its mesh's (found by its entity in
 * the list of which mesh each shows), its scale and its thickness, and its bounds. push.count the bodies, push.listed
 * the list's, push.joints the joints. `described` d5 is its scale, d7.z its thickness.
 */
[shader("compute")] [numthreads(64, 1, 1)]
void meshEntries(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= push.count || ShapeOf(bodies[i]) != MeshShape) return;
    const uint entity = entityIds[i];
    uint group = None;
    for (uint k = 0; k < push.listed; ++k) if (uint(meshesOf[k].x + 0.5) == entity) { group = uint(meshesOf[k].y + 0.5); break; }
    if (group == None || group >= push.meshes) return;
    const uint range = group * 12;
    const uint firstIndex = meshRanges[range], indexCount = meshRanges[range + 1];
    if (indexCount < 3) return;
    const float3 low = asfloat(uint3(meshRanges[range + 4], meshRanges[range + 5], meshRanges[range + 6]));
    const float3 high = asfloat(uint3(meshRanges[range + 8], meshRanges[range + 9], meshRanges[range + 10]));
    const float3 scale = abs(described[i * 8 + 5].xyz);
    const float thickness = max(described[i * 8 + 7].z, 0.0);
    uint k;
    InterlockedAdd(shared[3], 1, k);
    const uint at = MeshesAt(push.joints) + k * MeshWords;
    shared[at] = i; shared[at + 1] = firstIndex / 3; shared[at + 2] = indexCount / 3; shared[at + 3] = 0;
    StoreFloat4(at + 4, float4(scale, thickness));
    // Its bounds, as far as they reach from their middle — none known (a mesh with no bounds), as far as any reach.
    const bool bounded = any(high != low);
    StoreFloat4(at + 8, float4((low + high) * 0.5 * scale, bounded ? length((high - low) * 0.5 * scale) : 1e30));
}

/** Each of the pack's triangles, where its corners are in its mesh's own frame. push.triangles of them, push.meshes ranges. */
[shader("compute")] [numthreads(64, 1, 1)]
void meshTriangles(uint3 id : SV_DispatchThreadID)
{
    const uint t = id.x;
    if (t >= push.triangles) return;
    // Its mesh's vertices are counted from that mesh's own.
    int vertexOffset = 0;
    for (uint m = 0; m < push.meshes; ++m) {
        const uint first = meshRanges[m * 12], count = meshRanges[m * 12 + 1];
        if (t * 3 >= first && t * 3 < first + count) { vertexOffset = asint(meshRanges[m * 12 + 2]); break; }
    }
    const uint at = TrianglesAt(push.joints) + t * 9;
    for (uint c = 0; c < 3; ++c) {
        const uint v = uint(int(meshIndices[t * 3 + c]) + vertexOffset);
        const uint word = (v * push.stride + push.position) / 4;
        shared[at + c * 3] = meshVertices[word]; shared[at + c * 3 + 1] = meshVertices[word + 1]; shared[at + c * 3 + 2] = meshVertices[word + 2];
    }
}

/** Where its entity is, as its body is now: the body's place less its offset. */
float4 Placed(Body b) { return float4(b.positionInvMass.xyz - Rotate(b.orientation, Offset(b)), 0); }

/** Each entity's place, as the step left its body: what a WriteBack writes. */
[shader("compute")] [numthreads(64, 1, 1)]
void place(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    bodies[id.x].placed.xyz = Placed(bodies[id.x]).xyz;
}

/**
 * Each kinematic body that follows its entity, sent where the entity is now (a script turned it, the gizmo moved it): the
 * velocity and spin that take it there over this frame's steps, as kphysics' bridge gives one. push.h the step.
 */
[shader("compute")] [numthreads(64, 1, 1)]
void drive(uint3 id : SV_DispatchThreadID)
{
    if (id.x == 0) bigs[0] = 0;         // listed again this step, by cellKeys
    if (id.x >= push.count) return;
    Body b = bodies[id.x];
    if (KindOf(b) != Kinematic || b.placed.w < 0.5) return;
    for (uint j = 0; j < push.driven; ++j) {
        const float4 at = driven[j * 2];
        if (abs(at.w - b.placed.w) > 0.25) continue;
        const float4 turn = dot(driven[j * 2 + 1], driven[j * 2 + 1]) > 0 ? normalize(driven[j * 2 + 1]) : float4(0, 0, 0, 1);
        const float over = push.h * float(max(push.stepsThisFrame, 1u));
        const float3 target = at.xyz + Rotate(turn, Offset(b));
        b.velocityReach.xyz = (target - b.positionInvMass.xyz) / over;
        b.spinShape.xyz = TurnBetween(b.orientation, turn) / over;
        bodies[id.x] = b;
        return;
    }
}

[shader("compute")] [numthreads(64, 1, 1)]
void cellKeys(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    if (Big(bodies[id.x])) { uint k; InterlockedAdd(bigs[0], 1, k); bigs[1 + k] = id.x; }
    const int3 cell = CellOf(bodies[id.x].positionInvMass.xyz);
    keys[id.x] = Hash3(cell);
    ids[id.x] = id.x;
    homes[id.x] = int4(cell, 0);
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

[shader("compute")] [numthreads(64, 1, 1)]
void predict(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    Body body = bodies[id.x];
    Before was;
    was.position = body.positionInvMass;
    was.orientation = body.orientation;
    was.velocity = float4(body.velocityReach.xyz, 0);
    was.spin = float4(body.spinShape.xyz, 0);
    before[id.x] = was;
    if (!Solid(body) && ShapeOf(body) != MeshShape) return;
    if (body.positionInvMass.w == 0) {
        // Static stays; kinematic goes as its velocity says — pushing, never pushed.
        if (KindOf(body) != Kinematic) return;
        body.positionInvMass.xyz += body.velocityReach.xyz * push.h;
        body.orientation = Turned(body.orientation, body.spinShape.xyz * push.h);
        bodies[id.x] = body;
        return;
    }
    if (Asleep(body)) return;
    const float4 material = materials[id.x];
    body.velocityReach.xyz = (body.velocityReach.xyz + push.gravity.xyz * push.h) / (1 + push.h * material.z);
    body.spinShape.xyz /= 1 + push.h * material.w;
    body.positionInvMass.xyz += body.velocityReach.xyz * push.h;
    body.orientation = Turned(body.orientation, body.spinShape.xyz * push.h);
    bodies[id.x] = body;
}

/** How far a body's point r from its middle went this substep: its move, and its turn about its middle. */
float3 Slipped(Body b, Before was, float3 r) { return (b.positionInvMass.xyz - was.position.xyz) + cross(TurnBetween(was.orientation, b.orientation), r); }

/**
 * Body me's share of one contact with other: each point pushes them apart along the normal, as much as each moves and
 * turns there; friction holds the points — what they slid across the normal, all of it within static friction, the
 * rest by dynamic — averaged over the contact's points. `meIsA`: the normal is from me to other, or from other to me.
 */
void Share(Body me, Before meWas, Body other, Before otherWas, bool hasOther, Manifold m, bool meIsA, float friction,
           inout float3 move, inout float3 turn, inout float3 held, inout float3 heldTurn, inout uint points)
{
    const float3 n = meIsA ? m.normal : -m.normal;             // from me into the other
    float3 hold = float3(0, 0, 0), holdTurn = float3(0, 0, 0);
    for (uint q = 0; q < m.count; ++q) {
        const float3 at = m.points[q].xyz;
        const float depth = m.points[q].w;
        const float3 rMe = at - me.positionInvMass.xyz;
        const float3 rOther = hasOther ? at - other.positionInvMass.xyz : float3(0, 0, 0);
        const float wMe = Generalized(me, rMe, n), wOther = hasOther ? Generalized(other, rOther, n) : 0;
        if (wMe + wOther <= 0) continue;
        // Pushed apart: me against the normal.
        const float3 p = n * (depth / (wMe + wOther));
        move -= p * me.positionInvMass.w;
        turn -= InverseInertia(me, cross(rMe, p));
        ++points;
        // Held where it slid.
        float3 slip = Slipped(me, meWas, rMe) - (hasOther ? Slipped(other, otherWas, rOther) : float3(0, 0, 0));
        slip -= dot(slip, n) * n;
        const float slid = length(slip);
        if (slid > 1e-9) {
            const float3 t = slip / slid;
            const float wT = Generalized(me, rMe, t) + (hasOther ? Generalized(other, rOther, t) : 0);
            const float amount = slid < friction * depth ? slid : min(friction * depth, slid);
            const float3 f = -t * (amount / wT);
            hold += f * me.positionInvMass.w;
            holdTurn += InverseInertia(me, cross(rMe, f));
        }
    }
    if (m.count > 0) { held += hold / float(m.count); heldTurn += holdTurn / float(m.count); }
}

// ---- the meshes' contacts, found by sixteen lanes a body -----------------------------------------------------------

/*
 * Each body's contacts with the mesh colliders, worked out before a solve: the triangles of each mesh it is near shared
 * among sixteen lanes, each point's best of them reduced across the lanes, and up to MostPatches patches written to
 * meshHits — HitWords float4 a body: how many, then each patch's normal and count, its mesh's body, its eight points.
 * One-sided as kphysics has it: a triangle left out when the body's middle is further behind its face than a quarter of
 * its least size, or the mesh's thickness (a solid wall that deep). A point inside the mesh, behind faces, goes out
 * through the nearest of them; one in front touches the triangle it is deepest in; an edge of the mesh into the body, at
 * its point nearest the body's middle, pushes the body off it.
 */
static const uint Lanes = 16, BodiesAGroup = 4, MostEdges = 16;
static const uint PatchWords = 10, HitWords = 1 + MostPatches * PatchWords;
groupshared float4 laneHit[BodiesAGroup * Lanes * MostPoints];      // xyz the way out, w how deep
groupshared float4 laneWhere[BodiesAGroup * Lanes * MostPoints];    // xyz where, w how: 0 untouched, 1 in front, 2 behind
groupshared float4 edgeHit[BodiesAGroup * MostEdges];
groupshared float4 edgeWhere[BodiesAGroup * MostEdges];
groupshared uint edgeCount[BodiesAGroup];

/**
 * Whether a point's touch of rank `rank`, `depth` deep, is to be taken over the one it has: a higher rank first — 3,
 * behind a face the body's middle is behind too; 2, behind one it is in front of; 1, in front — and of one rank, the
 * least deep behind a face (the nearest way out), the deepest in front.
 */
bool Better(uint rank, float depth, uint hadRank, float had)
{
    if (rank != hadRank) return rank > hadRank;
    return rank == 1 ? depth > had : depth < had;
}

void MeshContactsOf(uint3 group, uint3 thread, float inflated)
{
    const uint slot = thread.x / Lanes, lane = thread.x % Lanes;
    const uint i = group.x * BodiesAGroup + slot;
    Body me = (Body)0;
    bool active = i < push.count;
    if (active) {
        me = bodies[i];
        active = Solid(me) && me.positionInvMass.w > 0;       // asleep too: a moving mesh touching it wakes it
        if (active && inflated > 0) me = Inflated(me, inflated);
    }
    if (lane == 0) edgeCount[slot] = 0;
    uint patchesMade = 0;                       // lane 0's
    const uint count = active ? PointCount(me) : 0;
    const float allowed = active ? max(0.25 * LeastAcross(me), 0.0) : 0;
    const uint meshes = shared[3];
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0; k < meshes; ++k) {
        uint body = 0; float4 scaleThickness = float4(1, 1, 1, 0);
        const bool near = active && NearMesh(me, k, body, scaleThickness);
        const Body mesh = bodies[body];
        float4 hit[MostPoints];
        float4 where[MostPoints];
        for (uint q = 0; q < MostPoints; ++q) { hit[q] = float4(0, 1, 0, 0); where[q] = float4(0, 0, 0, 0); }
        if (near) {
            const float behindAllowed = max(allowed, scaleThickness.w);
            const float3 scale = scaleThickness.xyz;
            const float3 mine = Unrotate(mesh.orientation, me.positionInvMass.xyz - mesh.positionInvMass.xyz) / max(scale, 1e-6);
            const float reach = (me.velocityReach.w + behindAllowed) / max(min(scale.x, min(scale.y, scale.z)), 1e-6);
            const uint triangles = MeshTriangleCount(k);
            for (uint t = lane; t < triangles; t += Lanes) {
                float3 a, b, c;
                MeshTriangleLocal(k, t, a, b, c);
                const float3 nearest = ClosestOnTriangle(mine, a, b, c);
                if (dot(nearest - mine, nearest - mine) > reach * reach) continue;
                a = OfMesh(mesh, scale, a); b = OfMesh(mesh, scale, b); c = OfMesh(mesh, scale, c);
                const float3 face = cross(b - a, c - a);
                const float area = length(face);
                if (area < 1e-12) continue;
                const float side = dot(face / area, me.positionInvMass.xyz - a);
                if (side < -behindAllowed || side > me.velocityReach.w) continue;
                // Its edges into a body in front of its face — pushing it off them, never back through the face.
                for (uint e = 0; e < 3 && side >= 0; ++e) {
                    const float3 from = e == 0 ? a : e == 1 ? b : c, to = e == 0 ? b : e == 1 ? c : a;
                    float u;
                    const float3 p = ClosestOnSegment(me.positionInvMass.xyz, from, to, u);
                    float3 out;
                    const float d = Distance(me, p, out);
                    if (d >= 0 || dot(out, face) > 0) continue;     // `out` is the way me is pushed against: not into the face
                    uint at;
                    InterlockedAdd(edgeCount[slot], 1, at);
                    if (at < MostEdges) { edgeHit[slot * MostEdges + at] = float4(out, -d); edgeWhere[slot * MostEdges + at] = float4(p + out * (-d * 0.5), 0); }
                }
                for (uint q = 0; q < count; ++q) {
                    const float4 p = PointOf(me, q);
                    float3 out, at; float depth; bool behind;
                    if (!TrianglePoint(p.xyz, p.w, a, b, c, out, depth, at, behind)) continue;
                    // Behind a face with the body's middle behind it too — in the wall — it goes out that way; behind
                    // others, through the nearest; in front of all, the deepest it touches.
                    const uint rank = behind ? (side < 0 ? 3 : 2) : 1;
                    if (Better(rank, depth, uint(where[q].w + 0.5), hit[q].w)) { hit[q] = float4(out, depth); where[q] = float4(at, rank); }
                }
            }
        }
        for (uint q = 0; q < MostPoints; ++q) {
            laneHit[(slot * Lanes + lane) * MostPoints + q] = hit[q];
            laneWhere[(slot * Lanes + lane) * MostPoints + q] = where[q];
        }
        GroupMemoryBarrierWithGroupSync();
        if (near && lane == 0) {
            // Each point's best of the lanes', and the edges: into patches, after those of the meshes before.
            Manifold patches[MostPatches];
            uint made = 0;
            for (uint q = 0; q < count; ++q) {
                float4 best = float4(0, 1, 0, 0), bestWhere = float4(0, 0, 0, 0);
                for (uint l = 0; l < Lanes; ++l) {
                    const float4 h = laneHit[(slot * Lanes + l) * MostPoints + q], w = laneWhere[(slot * Lanes + l) * MostPoints + q];
                    const uint how = uint(w.w + 0.5), bestHow = uint(bestWhere.w + 0.5);
                    if (how != 0 && Better(how, h.w, bestHow, best.w)) { best = h; bestWhere = w; }
                }
                if (bestWhere.w > 0.5) IntoPatch(patches, made, -best.xyz, bestWhere.xyz, best.w);
            }
            for (uint e = 0; e < min(edgeCount[slot], MostEdges); ++e)
                IntoPatch(patches, made, edgeHit[slot * MostEdges + e].xyz, edgeWhere[slot * MostEdges + e].xyz, edgeHit[slot * MostEdges + e].w);
            for (uint g = 0; g < made && patchesMade < MostPatches; ++g, ++patchesMade) {
                const uint at = i * HitWords + 1 + patchesMade * PatchWords;
                meshHits[at] = float4(patches[g].normal, float(patches[g].count));
                meshHits[at + 1] = float4(asfloat(body), 0, 0, 0);
                for (uint p = 0; p < 8; ++p) meshHits[at + 2 + p] = p < patches[g].count ? patches[g].points[p] : float4(0, 0, 0, 0);
            }
            edgeCount[slot] = 0;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0 && i < push.count) meshHits[i * HitWords] = float4(float(patchesMade), 0, 0, 0);
}

[shader("compute")] [numthreads(64, 1, 1)]
void meshContacts(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) { MeshContactsOf(group, thread, 0); }
/** The same for the velocity solve: each body a little bigger, as what touches is after its position is solved. */
[shader("compute")] [numthreads(64, 1, 1)]
void meshTouching(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) { MeshContactsOf(group, thread, Touching); }

/** Body i's patch g with the meshes, as meshContacts left it, and its mesh's body. */
Manifold MeshPatch(uint i, uint g, out uint body)
{
    const uint at = i * HitWords + 1 + g * PatchWords;
    const float4 head = meshHits[at];
    Manifold m;
    m.normal = head.xyz;
    m.count = uint(head.w + 0.5);
    body = asuint(meshHits[at + 1].x);
    for (uint p = 0; p < 8; ++p) m.points[p] = meshHits[at + 2 + p];
    return m;
}
uint MeshPatches(uint i) { return uint(meshHits[i * HitWords].x + 0.5); }

/** Whether bodies i and j are joined, and are not to collide for it. */
bool Joined(uint i, uint j)
{
    for (uint k = JointStart(i); k < JointStart(i + 1); ++k) {
        const Joint joint = LoadJoint(shared[JointListAt(k)]);
        if ((joint.head.w & 2u) != 0) continue;
        if ((joint.head.x == i && joint.head.y == j) || (joint.head.x == j && joint.head.y == i)) return true;
    }
    return false;
}

/** A hinge's angle about its axis aA: the entity's turn against its other's, as kphysics has it — A's way across from B's. */
float HingeAngle(Joint joint, Body A, Body B, bool hasB, float3 aA)
{
    const float3 cA = Rotate(A.orientation, joint.acrossA.xyz);
    const float3 cB = hasB ? Rotate(B.orientation, joint.acrossB.xyz) : joint.acrossB.xyz;
    return atan2(dot(aA, cross(cB, cA)), dot(cA, cB));
}

/** How easily a body turns about n through its middle. */
float Turning(Body b, float3 n) { return dot(n, InverseInertia(b, n)); }

/**
 * How easily a body turns about n through a point r from its middle: a hinge's way of turning, held at its pin — turned
 * about its middle, the pin would carry it as far back as it was turned.
 */
float AboutPin(Body b, float3 n, float3 r)
{
    const float t = Turning(b, n);
    if (b.positionInvMass.w <= 0 || t <= 0) return 0;
    const float3 across = cross(n, r);
    return 1.0 / (1.0 / t + dot(across, across) / b.positionInvMass.w);
}

/**
 * Body me's share of its joints: each holds its two points together (ball, hinge), or apart by its length (distance;
 * a rope only when stretched past it), and a hinge its two axes along each other, its angle within its limits, and
 * turns at its motor's speed as far as its torque will. Each joint's whole, and the joints that held averaged: not
 * averaged with the contacts, as those are — a joint held a part of the way each substep springs back from its limit.
 */
void Joints(uint i, Body me, Before meWas, inout float3 move, inout float3 turn, inout uint joints)
{
    for (uint k = JointStart(i); k < JointStart(i + 1); ++k) {
        const Joint joint = LoadJoint(shared[JointListAt(k)]);
        const uint kind = joint.head.z;
        if (kind > 3) continue;
        const bool meIsA = joint.head.x == i;
        const bool hasB = joint.head.y != None;
        const Body A = meIsA ? me : bodies[joint.head.x];
        const Body B = hasB ? (meIsA ? bodies[joint.head.y] : me) : (Body)0;
        const float3 pA = A.positionInvMass.xyz + Rotate(A.orientation, joint.anchorA.xyz);
        const float3 pB = hasB ? B.positionInvMass.xyz + Rotate(B.orientation, joint.anchorB.xyz) : joint.anchorB.xyz;
        const float3 rA = pA - A.positionInvMass.xyz, rB = hasB ? pB - B.positionInvMass.xyz : float3(0, 0, 0);
        const float sign = meIsA ? 1.0 : -1.0;
        const float3 rMe = meIsA ? rA : rB;
        // Where: together, or a length apart.
        const float3 d = pB - pA;
        const float length = sqrt(dot(d, d));
        float error = length;
        if (kind == 2 || kind == 3) error = length - joint.axisA.w;
        if (length > 1e-7 && !(kind == 3 && error <= 0)) {
            const float3 n = d / length;
            const float w = Generalized(A, rA, n) + (hasB ? Generalized(B, rB, n) : 0);
            if (w > 0) {
                const float3 p = n * (error / w) * sign;
                move += p * me.positionInvMass.w;
                turn += InverseInertia(me, cross(rMe, p));
            }
        }
        ++joints;
        if (kind != 1) continue;
        // A hinge, each body turned about its pin (A by `by`, B the other way, as each turns there): its axes along each other.
        const float3 aA = Rotate(A.orientation, joint.axisA.xyz);
        const float3 aB = hasB ? Rotate(B.orientation, joint.axisB.xyz) : joint.axisB.xyz;
        const float3 e = cross(aA, aB);
        const float sine = sqrt(dot(e, e));
        if (sine > 1e-7) {
            const float3 n = e / sine;
            const float wA = AboutPin(A, n, rA), wB = hasB ? AboutPin(B, n, rB) : 0;
            if (wA + wB > 0) {
                const float by = sine * (meIsA ? wA : -wB) / (wA + wB);
                turn += n * by;
                move += by * cross(rMe, n);
            }
        }
        // Its angle — the entity's turn against its other's, as kphysics has it: A's way across from B's — within its
        // limits, and its motor.
        const float wA = AboutPin(A, aA, rA), wB = hasB ? AboutPin(B, aA, rB) : 0, wAxis = wA + wB;
        if (wAxis <= 0) continue;
        const float wMe = meIsA ? wA : -wB;
        const float angle = HingeAngle(joint, A, B, hasB, aA);
        if ((joint.head.w & 1u) != 0) {
            const float over = angle < joint.anchorA.w ? angle - joint.anchorA.w : angle > joint.anchorB.w ? angle - joint.anchorB.w : 0;
            if (over != 0) { const float by = -over * wMe / wAxis; turn += aA * by; move += by * cross(rMe, aA); }
        }
        if (joint.acrossA.w > 0) {
            // How far A turned about the axis this substep, against B: what the motor makes its speed, as its torque allows.
            const Before wasA = meIsA ? meWas : before[joint.head.x];
            const float3 turnedA = TurnBetween(wasA.orientation, A.orientation);
            const float3 turnedB = hasB ? TurnBetween((meIsA ? before[joint.head.y] : meWas).orientation, B.orientation) : float3(0, 0, 0);
            const float wanted = joint.axisB.w * push.h - dot(turnedA - turnedB, aA);
            const float most = joint.acrossA.w * push.h * push.h;
            const float by = clamp(wanted / wAxis, -most, most) * wMe;
            turn += aA * by;
            move += by * cross(rMe, aA);
        }
    }
}

/** A move, less what of it goes against `out` (the world's push out of itself): it is not to undo that. */
float3 OutOfTheWorld(float3 move, float3 out)
{
    const float l = length(out);
    if (l < 1e-9) return move;
    const float3 d = out / l;
    const float into = dot(move, d);
    return into < 0 ? move - d * into : move;
}

/**
 * What a body's contacts with what nothing moves add up to — a plane, a mesh, a static or a kinematic body — kept apart
 * from those with what moves and averaged by themselves: averaged with all the rest, a floor's or a blade's push out is
 * a share of what its neighbours push it back in with, and a crowd presses it through.
 */
struct Pushes {
    float3 move;
    float3 turn;
    float3 held;
    float3 heldTurn;
    uint points;
};
Pushes NoPushes() { Pushes p; p.move = float3(0, 0, 0); p.turn = float3(0, 0, 0); p.held = float3(0, 0, 0); p.heldTurn = float3(0, 0, 0); p.points = 0; return p; }

/**
 * Whether body me is further from a big one than it reaches, and `by` more: measured from me's middle to the big one's
 * surface — a blade across the whole drum is far from most of what is in it, though its middle is not.
 */
bool Apart(Body big, Body me, float by)
{
    float3 n;
    return Distance(big, me.positionInvMass.xyz, n) > me.velocityReach.w + by;
}

/**
 * Body me's share of its contact with body j: collided in one order whichever of the two asks — the lower shape first,
 * then the lower index. `fromBigs`: j is of the list of big ones, which the grid leaves out.
 */
void Meet(uint i, Body me, Before meWas, float myFriction, uint j, bool fromBigs, inout float3 move, inout float3 turn,
          inout float3 held, inout float3 heldTurn, inout uint points, inout Pushes fixed)
{
    const Body other = bodies[j];
    if (!Solid(other) || (!fromBigs && Big(other)) || Joined(i, j)) return;
    const float3 d = other.positionInvMass.xyz - me.positionInvMass.xyz;
    const float reach = me.velocityReach.w + other.velocityReach.w;
    if (dot(d, d) >= reach * reach || (fromBigs && Apart(other, me, 0))) return;
    const uint mine = ShapeOf(me), theirs = ShapeOf(other);
    const bool meIsA = mine < theirs || (mine == theirs && i < j);
    const Manifold m = meIsA ? Collide(me, other) : Collide(other, me);
    if (m.count == 0) return;
    if (other.positionInvMass.w == 0)
        Share(me, meWas, other, before[j], true, m, meIsA, sqrt(myFriction * materials[j].x), fixed.move, fixed.turn, fixed.held, fixed.heldTurn, fixed.points);
    else Share(me, meWas, other, before[j], true, m, meIsA, sqrt(myFriction * materials[j].x), move, turn, held, heldTurn, points);
}

/** The same for the velocity solve: each a little bigger, as what touches is after its position is solved. */
void Touch(uint i, Body me, float3 v, float3 w, Before meWas, float myRestitution, uint j, bool fromBigs, inout float3 dv, inout float3 dw,
           inout uint points, inout bool woken, inout Pushes fixed)
{
    const Body other = bodies[j];
    if (!Solid(other) || (!fromBigs && Big(other)) || Joined(i, j)) return;
    const float3 d = other.positionInvMass.xyz - me.positionInvMass.xyz;
    const float reach = me.velocityReach.w + other.velocityReach.w;
    if (dot(d, d) >= reach * reach || (fromBigs && Apart(other, me, Touching))) return;
    const uint mine = ShapeOf(me), theirs = ShapeOf(other);
    const bool meIsA = mine < theirs || (mine == theirs && i < j);
    const Manifold m = meIsA ? Collide(Inflated(me, Touching / 2), Inflated(other, Touching / 2))
                             : Collide(Inflated(other, Touching / 2), Inflated(me, Touching / 2));
    if (m.count == 0) return;
    woken = woken || Stirring(other);
    const Before otherWas = before[j];
    const float3 otherV = (other.positionInvMass.xyz - otherWas.position.xyz) / push.h;
    const float3 otherW = TurnBetween(otherWas.orientation, other.orientation) / push.h;
    if (other.positionInvMass.w == 0) Settle(me, v, w, meWas, other, otherV, otherW, otherWas, true, m, meIsA, max(myRestitution, materials[j].y), fixed.move, fixed.turn, fixed.points);
    else Settle(me, v, w, meWas, other, otherV, otherW, otherWas, true, m, meIsA, max(myRestitution, materials[j].y), dv, dw, points);
}

// ---- the contacts of a step, found once ------------------------------------------------------------------------

/** A body a little bigger every way: what still touches when pushed out to just touching, for the velocity solve. */
Body Inflated(Body b, float by)
{
    if (ShapeOf(b) == Box) b.size.xyz += by;
    else if (ShapeOf(b) == Cone) b.size.z += by;
    else b.size.x += by;
    b.velocityReach.w += by;
    return b;
}
static const float Touching = 1e-3;     // how far apart two bodies still touch, after their positions are solved

/*
 * Each dynamic body's contacts with the bodies near it (the grid's: not the big ones, the meshes or the planes, which each
 * substep meets as they are), found every few substeps and kept for them (as Müller and others keep a step's, "Detailed
 * Rigid Body Simulation with Extended Position Based Dynamics"): collided with both a little bigger — as far as each goes
 * in those substeps (push.h, all of them) — so that what comes to touch meanwhile is there too. Each point is kept on each
 * body, in its own frame, with the normal from me into the other: a substep finds how deep it is from where the two are
 * then. A round body's point is kept as its core's (a sphere's middle, a capsule's axis) and its radius along the normal,
 * so that a ball that rolls goes on touching at its bottom, and two round ones meet along their cores as they are then.
 * Found every substep, a body's narrowphase is done once a substep, not by each of solve and velocities; found once a
 * step, a crowd a blade throws about is met as it was, and pressed into floors: every second substep is XpbdStep's
 * default (`contactsEvery`).
 *
 * CacheWords float4 a body: in x of the first, how many are used (asuint); then a record a pair — the normal and, in w,
 * the other (low 24 bits) and how many points (high 8) — and two float4 a point: where it is on me, my radius; where on
 * the other, its radius. What does not fit is left out.
 */
static const uint CacheWords = 128;
RWStructuredBuffer<float4> cached;
groupshared uint cacheFill[BodiesAGroup];
groupshared uint cacheEnd[BodiesAGroup];

/** How much bigger a body is collided with, for the contacts kept: as far as it goes until they are found again (push.h), and a little. */
float MarginOf(Body b) { return length(b.velocityReach.xyz) * push.h * 0.5; }
/** How round a body is about its core: what of its point is not turned with it. */
float RoundOf(Body b) { const uint shape = ShapeOf(b); return shape == Sphere || shape == Capsule ? b.size.x : shape == Cone ? b.size.z : 0; }

/**
 * A point of a body, in its own frame — a box's kept within it: made bigger, a box's corners go out further than its
 * faces, and what was taken back along the normal alone would be outside it still, touching what it does not.
 */
float3 OnItself(Body b, float3 p)
{
    const float3 local = Unrotate(b.orientation, p - b.positionInvMass.xyz);
    return ShapeOf(b) == Box ? clamp(local, -b.size.xyz, b.size.xyz) : local;
}

void Cache(uint i, uint slot, Body me, float myMargin, uint j)
{
    const Body other = bodies[j];
    if (!Solid(other) || Big(other) || Joined(i, j)) return;
    const float otherMargin = MarginOf(other);
    const float3 d = other.positionInvMass.xyz - me.positionInvMass.xyz;
    const float reach = me.velocityReach.w + myMargin + other.velocityReach.w + otherMargin;
    if (dot(d, d) >= reach * reach) return;
    const uint mine = ShapeOf(me), theirs = ShapeOf(other);
    const bool meIsA = mine < theirs || (mine == theirs && i < j);
    const Body wideMe = Inflated(me, myMargin), wideOther = Inflated(other, otherMargin);
    const Manifold m = meIsA ? Collide(wideMe, wideOther) : Collide(wideOther, wideMe);
    if (m.count == 0) return;
    const float3 n = meIsA ? m.normal : -m.normal;
    const uint words = 1 + 2 * m.count;
    uint at;
    InterlockedAdd(cacheFill[slot], words, at);
    if (at + words > CacheWords) { InterlockedMin(cacheEnd[slot], at); return; }
    const uint base = i * CacheWords + at;
    cached[base] = float4(n, asfloat(j | (m.count << 24)));
    const float myRound = RoundOf(me), otherRound = RoundOf(other);
    for (uint q = 0; q < m.count; ++q) {
        // Where they meet is halfway between the two surfaces, each into the other: each made as big as it is again.
        const float3 middle = m.points[q].xyz;
        const float half = 0.5 * m.points[q].w;
        const float3 onMe = middle + n * (half - myMargin), onOther = middle - n * (half - otherMargin);
        cached[base + 1 + 2 * q] = float4(OnItself(me, onMe - n * myRound), myRound);
        cached[base + 2 + 2 * q] = float4(OnItself(other, onOther + n * otherRound), otherRound);
    }
}

[shader("compute")] [numthreads(64, 1, 1)]
void contacts(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint slot = thread.x / Lanes, lane = thread.x % Lanes;
    const uint i = group.x * BodiesAGroup + slot;
    if (lane == 0) { cacheFill[slot] = 1; cacheEnd[slot] = CacheWords; }
    GroupMemoryBarrierWithGroupSync();
    if (i < push.count) {
        const Body me = bodies[i];
        if (me.positionInvMass.w > 0 && Solid(me)) {
            const float myMargin = MarginOf(me);
            uint taken = 0;
            const int3 cell = homes[i].xyz;
            for (int dz = -1; dz <= 1; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                // Every body near me, as the grid has it: from its own cell, once, whatever else hashed the same.
                const uint key = Hash3(cell + int3(dx, dy, dz));
                const uint start = starts[key];
                if (start == 0) continue;
                for (uint k = start - 1; k < ends[key]; ++k) {
                    const uint j = ids[k];
                    if (j == i || any(homes[j].xyz != cell + int3(dx, dy, dz))) continue;
                    if (taken++ % Lanes == lane) Cache(i, slot, me, myMargin, j);
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && i < push.count) cached[i * CacheWords] = float4(asfloat(min(cacheFill[slot], cacheEnd[slot])), 0, 0, 0);
}

/** How many of body i's cache's float4 are used: from 1, its first a record. */
uint CacheUsed(uint i) { return asuint(cached[i * CacheWords].x); }
/** Where the record after the one at `at` is. */
uint NextCached(uint i, uint at) { return at + 1 + 2 * (asuint(cached[i * CacheWords + at].w) >> 24); }

/**
 * The record at `at` in body i's cache, as a contact now: each point as deep as it is from where the two are now, and
 * `more` (the velocity solve's Touching) — those not, left out. The normal from me into the other; its `other` and the
 * float4 the next record is at.
 */
Manifold Cached(uint i, Body me, inout uint at, out uint other, out Body them, float more)
{
    const float4 head = cached[i * CacheWords + at];
    const uint word = asuint(head.w), count = word >> 24;
    other = word & 0xFFFFFFu;
    them = bodies[other];
    Manifold m;
    m.count = 0;
    m.normal = head.xyz;
    // A sphere meets a sphere, or a capsule, along the line from its middle to the other's core as it is now — the other's
    // middle, or the point of its axis nearest — not the way it once did: one sliding past is pushed out the way it is.
    const uint mine = ShapeOf(me), theirs = ShapeOf(them);
    if (count == 1 && (mine == Sphere || theirs == Sphere) && (mine == Sphere || mine == Capsule) && (theirs == Sphere || theirs == Capsule)) {
        float3 myCore = me.positionInvMass.xyz, theirCore = them.positionInvMass.xyz, lower, upper;
        float t;
        if (mine == Capsule) { CapsuleEnds(me, lower, upper); myCore = ClosestOnSegment(theirCore, lower, upper, t); }
        if (theirs == Capsule) { CapsuleEnds(them, lower, upper); theirCore = ClosestOnSegment(myCore, lower, upper, t); }
        const float3 between = theirCore - myCore;
        if (dot(between, between) > 1e-12) {
            m.normal = normalize(between);
            const float depth = me.size.x + them.size.x - length(between) + more;
            if (depth > 0) Add(m, myCore + m.normal * (me.size.x - 0.5 * depth), depth);
            at += 3;
            return m;
        }
    }
    for (uint q = 0; q < count; ++q) {
        const float4 a = cached[i * CacheWords + at + 1 + 2 * q], b = cached[i * CacheWords + at + 2 + 2 * q];
        const float3 onMe = me.positionInvMass.xyz + Rotate(me.orientation, a.xyz) + m.normal * a.w;
        const float3 onThem = them.positionInvMass.xyz + Rotate(them.orientation, b.xyz) - m.normal * b.w;
        const float depth = dot(onMe - onThem, m.normal) + more;
        if (depth > 0) Add(m, (onMe + onThem) * 0.5, depth);
    }
    at += 1 + 2 * count;
    return m;
}

// What each lane of a body's sixteen found: its share of the body's contacts, summed by lane 0.
groupshared float4 laneMove[BodiesAGroup * Lanes];         // w how many points
groupshared float4 laneTurn[BodiesAGroup * Lanes];         // w whether something stirring touches it (the velocity solve's)
groupshared float4 laneHeld[BodiesAGroup * Lanes];
groupshared float4 laneHeldTurn[BodiesAGroup * Lanes];
groupshared float4 laneFixedMove[BodiesAGroup * Lanes];    // with what nothing moves; w how many points
groupshared float4 laneFixedTurn[BodiesAGroup * Lanes];

/**
 * Each body's contacts and joints, pushed apart and held: sixteen lanes a body, the bodies near it shared among them
 * (every sixteenth to each), lane 0 its joints, the planes and the meshes, and lane 0 what they come to — so that a
 * few hundred bodies each near many keep the GPU busy, not a few of its threads.
 */
[shader("compute")] [numthreads(64, 1, 1)]
void solve(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint slot = thread.x / Lanes, lane = thread.x % Lanes;
    const uint i = group.x * BodiesAGroup + slot;
    Body me = (Body)0;
    bool active = false;
    if (i < push.count) {
        me = bodies[i];
        active = me.positionInvMass.w > 0 && Solid(me) && !Asleep(me);
    }
    float3 move = float3(0, 0, 0), turn = float3(0, 0, 0), held = float3(0, 0, 0), heldTurn = float3(0, 0, 0);
    uint points = 0, joints = 0;
    Pushes fixed = NoPushes();
    Pushes world = NoPushes();          // lane 0's: the planes' and the meshes', which have the last word
    float3 jointMove = float3(0, 0, 0), jointTurn = float3(0, 0, 0);
    if (active) {
        const Before meWas = before[i];
        const float myFriction = materials[i].x;
        if (lane == 0) {
            Joints(i, me, meWas, jointMove, jointTurn, joints);
            for (uint k = 0; k < Planes(); ++k) {
                const float4 plane = PlaneAt(k);
                Share(me, meWas, me, meWas, false, OnPlane(me, plane.xyz, plane.w), true, myFriction, world.move, world.turn, world.held, world.heldTurn, world.points);
            }
            if (push.ground > -1e30)
                Share(me, meWas, me, meWas, false, OnPlane(me, float3(0, 1, 0), push.ground), true, myFriction, world.move, world.turn, world.held, world.heldTurn, world.points);
            // The mesh colliders it touches, as meshContacts found them.
            for (uint g = 0; g < MeshPatches(i); ++g) {
                uint body;
                const Manifold m = MeshPatch(i, g, body);
                Share(me, meWas, bodies[body], before[body], true, m, true, sqrt(myFriction * materials[body].x), world.move, world.turn, world.held, world.heldTurn, world.points);
            }
        }
        // Every body near me, as the step found them: a pair a lane.
        uint taken = 0;
        const uint used = CacheUsed(i);
        for (uint at = 1; at < used; ++taken) {
            if (taken % Lanes != lane) { at = NextCached(i, at); continue; }
            uint j; Body other;
            const Manifold m = Cached(i, me, at, j, other, 0);
            if (m.count == 0) continue;
            const float friction = sqrt(myFriction * materials[j].x);
            if (other.positionInvMass.w == 0) Share(me, meWas, other, before[j], true, m, true, friction, fixed.move, fixed.turn, fixed.held, fixed.heldTurn, fixed.points);
            else Share(me, meWas, other, before[j], true, m, true, friction, move, turn, held, heldTurn, points);
        }
        for (uint b = 0; b < bigs[0]; ++b)
            if (bigs[1 + b] != i && taken++ % Lanes == lane) Meet(i, me, meWas, myFriction, bigs[1 + b], true, move, turn, held, heldTurn, points, fixed);
    }
    laneMove[thread.x] = float4(move, float(points));
    laneTurn[thread.x] = float4(turn, 0);
    laneHeld[thread.x] = float4(held + fixed.held, 0);
    laneHeldTurn[thread.x] = float4(heldTurn + fixed.heldTurn, 0);
    laneFixedMove[thread.x] = float4(fixed.move, float(fixed.points));
    laneFixedTurn[thread.x] = float4(fixed.turn, 0);
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0 || i >= push.count) return;
    Next out;
    out.position = me.positionInvMass;
    out.orientation = me.orientation;
    if (active) {
        float4 moveAll = float4(0, 0, 0, 0), fixedMoveAll = float4(0, 0, 0, 0);
        float3 turnAll = float3(0, 0, 0), heldAll = float3(0, 0, 0), heldTurnAll = float3(0, 0, 0), fixedTurnAll = float3(0, 0, 0);
        for (uint l = 0; l < Lanes; ++l) {
            const uint at = slot * Lanes + l;
            moveAll += laneMove[at];
            turnAll += laneTurn[at].xyz;
            heldAll += laneHeld[at].xyz;
            heldTurnAll += laneHeldTurn[at].xyz;
            fixedMoveAll += laneFixedMove[at];
            fixedTurnAll += laneFixedTurn[at].xyz;
        }
        const uint all = uint(moveAll.w + 0.5), fixedAll = uint(fixedMoveAll.w + 0.5);
        if (all > 0 || fixedAll > 0 || world.points > 0 || joints > 0) {
            // The contacts' pushes averaged — those with what moves, those with what does not, and the world's, each apart
            // — and the joints' apart. The world's have the last word: what the others would push me further into it
            // with is let go, so that a blade sweeping by a wall, or a crowd, does not press me through it.
            float3 moved = heldAll + world.held, turned = heldTurnAll + world.heldTurn;
            float3 shift = float3(0, 0, 0);
            // Those with what moves, as damped as ever: shared by every point it has, so that a crowd does not overshoot.
            const uint every = all + fixedAll + world.points;
            if (all > 0) {
                const float3 apart = moveAll.xyz / float(every);
                const float most = MostPushSpeed * push.h, far = length(apart);
                shift += far > most ? apart * (most / far) : apart;
                const float3 turn = turnAll / float(every);
                const float mostTurn = most / max(me.velocityReach.w, 0.05), turning = length(turn);
                turned += turning > mostTurn ? turn * (mostTurn / turning) : turn;
            }
            if (fixedAll > 0) { shift += fixedMoveAll.xyz / float(fixedAll); turned += fixedTurnAll / float(fixedAll); }
            if (world.points > 0) {
                const float3 out = world.move / float(world.points);
                turned += world.turn / float(world.points);
                shift = OutOfTheWorld(shift, out);
                moved += out;
            }
            moved += shift;
            if (joints > 0) { moved += jointMove / float(joints); turned += jointTurn / float(joints); }
            out.position.xyz += moved;
            out.orientation = Turned(me.orientation, turned);
        }
    }
    next[i] = out;
}

[shader("compute")] [numthreads(64, 1, 1)]
void apply(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.count) return;
    const Next moved = next[id.x];
    bodies[id.x].positionInvMass.xyz = moved.position.xyz;
    bodies[id.x].orientation = moved.orientation;
}


/** A point's velocity: the body's, and its spin about its middle. */
float3 PointVelocity(float3 v, float3 w, float3 r) { return v + cross(w, r); }

/** Me's share of one contact's velocity solve: how fast each point closes along the normal, made what a bounce makes of it. */
void Settle(Body me, float3 v, float3 w, Before meWas, Body other, float3 otherV, float3 otherW, Before otherWas, bool hasOther,
            Manifold m, bool meIsA, float restitution, inout float3 dv, inout float3 dw, inout uint points)
{
    const float3 n = meIsA ? m.normal : -m.normal;
    for (uint q = 0; q < m.count; ++q) {
        const float3 at = m.points[q].xyz;
        const float3 rMe = at - me.positionInvMass.xyz;
        const float3 rOther = hasOther ? at - other.positionInvMass.xyz : float3(0, 0, 0);
        const float wMe = Generalized(me, rMe, n), wOther = hasOther ? Generalized(other, rOther, n) : 0;
        if (wMe + wOther <= 0) continue;
        const float u = dot(PointVelocity(v, w, rMe) - (hasOther ? PointVelocity(otherV, otherW, rOther) : float3(0, 0, 0)), n);
        const float came = dot(PointVelocity(meWas.velocity.xyz, meWas.spin.xyz, rMe)
                             - (hasOther ? PointVelocity(otherWas.velocity.xyz, otherWas.spin.xyz, rOther) : float3(0, 0, 0)), n);
        const float wanted = came > 2 * 9.81 * push.h ? -restitution * came : 0;
        const float3 p = n * ((wanted - u) / (wMe + wOther));
        dv += p * me.positionInvMass.w;
        dw += InverseInertia(me, cross(rMe, p));
        ++points;
    }
}

/**
 * Body me's share of its hinges' stops: one at a limit it was turning into this substep stops there — its turn about its
 * axis against its other's is none, about its pin — rather than leave it as fast as the position solve put it back.
 */
void Stops(uint i, Body me, float3 w, Before meWas, inout float3 dv, inout float3 dw, inout uint joints)
{
    for (uint k = JointStart(i); k < JointStart(i + 1); ++k) {
        const Joint joint = LoadJoint(shared[JointListAt(k)]);
        if (joint.head.z != 1 || (joint.head.w & 1u) == 0) continue;
        const bool meIsA = joint.head.x == i;
        const bool hasB = joint.head.y != None;
        const uint other = meIsA ? joint.head.y : joint.head.x;
        const Body A = meIsA ? me : bodies[other];
        const Body B = hasB ? (meIsA ? bodies[other] : me) : (Body)0;
        const Before wasA = meIsA ? meWas : before[other];
        const Before wasB = hasB ? (meIsA ? before[other] : meWas) : (Before)0;
        const float3 aA = Rotate(A.orientation, joint.axisA.xyz);
        const float angle = HingeAngle(joint, A, B, hasB, aA);
        const float came = dot(wasA.spin.xyz - wasB.spin.xyz, aA);
        if (!(angle >= joint.anchorB.w - 1e-3 && came > 0) && !(angle <= joint.anchorA.w + 1e-3 && came < 0)) continue;
        const float3 spinA = meIsA ? w : TurnBetween(wasA.orientation, A.orientation) / push.h;
        const float3 spinB = hasB ? (meIsA ? TurnBetween(wasB.orientation, B.orientation) / push.h : w) : float3(0, 0, 0);
        const float3 rA = Rotate(A.orientation, joint.anchorA.xyz), rB = hasB ? Rotate(B.orientation, joint.anchorB.xyz) : float3(0, 0, 0);
        const float wA = AboutPin(A, aA, rA), wB = hasB ? AboutPin(B, aA, rB) : 0;
        if (wA + wB <= 0) continue;
        const float3 by = aA * (-dot(spinA - spinB, aA) * (meIsA ? wA : -wB) / (wA + wB));
        dw += by;
        dv += cross(meIsA ? rA : rB, by);
        ++joints;
    }
}

/** The velocity solve, sixteen lanes a body as the position solve is. */
[shader("compute")] [numthreads(64, 1, 1)]
void velocities(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint slot = thread.x / Lanes, lane = thread.x % Lanes;
    const uint i = group.x * BodiesAGroup + slot;
    Body me = (Body)0;
    bool active = false;
    if (i < push.count) {
        me = bodies[i];
        active = me.positionInvMass.w > 0 && Solid(me);
    }
    float3 dv = float3(0, 0, 0), dw = float3(0, 0, 0), v = float3(0, 0, 0), w = float3(0, 0, 0);
    uint points = 0;
    bool woken = false;
    Pushes fixed = NoPushes();         // move and turn its velocity's and spin's
    Pushes world = NoPushes();         // lane 0's: the planes' and the meshes'
    Before meWas = (Before)0;
    if (active) {
        meWas = before[i];
        v = (me.positionInvMass.xyz - meWas.position.xyz) / push.h;
        w = TurnBetween(meWas.orientation, me.orientation) / push.h;
        const float myRestitution = materials[i].y;
        if (lane == 0) {
            const Body near = Inflated(me, Touching);
            for (uint k = 0; k < Planes(); ++k) {
                const float4 plane = PlaneAt(k);
                Settle(me, v, w, meWas, me, v, w, meWas, false, OnPlane(near, plane.xyz, plane.w), true, myRestitution, world.move, world.turn, world.points);
            }
            if (push.ground > -1e30)
                Settle(me, v, w, meWas, me, v, w, meWas, false, OnPlane(near, float3(0, 1, 0), push.ground), true, myRestitution, world.move, world.turn, world.points);
            for (uint g = 0; g < MeshPatches(i); ++g) {
                uint body;
                const Manifold m = MeshPatch(i, g, body);
                const Body mesh = bodies[body];
                const Before meshWas = before[body];
                const float3 meshV = (mesh.positionInvMass.xyz - meshWas.position.xyz) / push.h;
                const float3 meshW = TurnBetween(meshWas.orientation, mesh.orientation) / push.h;
                woken = woken || Stirring(mesh);
                Settle(me, v, w, meWas, mesh, meshV, meshW, meshWas, true, m, true, max(myRestitution, materials[body].y), world.move, world.turn, world.points);
            }
        }
        // Every body near me, as the step found them, touching now: a pair a lane.
        uint taken = 0;
        const uint used = CacheUsed(i);
        for (uint at = 1; at < used; ++taken) {
            if (taken % Lanes != lane) { at = NextCached(i, at); continue; }
            uint j; Body other;
            const Manifold m = Cached(i, me, at, j, other, Touching);
            if (m.count == 0) continue;
            woken = woken || Stirring(other);
            const Before otherWas = before[j];
            const float3 otherV = (other.positionInvMass.xyz - otherWas.position.xyz) / push.h;
            const float3 otherW = TurnBetween(otherWas.orientation, other.orientation) / push.h;
            const float restitution = max(myRestitution, materials[j].y);
            if (other.positionInvMass.w == 0) Settle(me, v, w, meWas, other, otherV, otherW, otherWas, true, m, true, restitution, fixed.move, fixed.turn, fixed.points);
            else Settle(me, v, w, meWas, other, otherV, otherW, otherWas, true, m, true, restitution, dv, dw, points);
        }
        for (uint b = 0; b < bigs[0]; ++b)
            if (bigs[1 + b] != i && taken++ % Lanes == lane) Touch(i, me, v, w, meWas, myRestitution, bigs[1 + b], true, dv, dw, points, woken, fixed);
    }
    laneMove[thread.x] = float4(dv, float(points));
    laneTurn[thread.x] = float4(dw, woken ? 1 : 0);
    laneFixedMove[thread.x] = float4(fixed.move, float(fixed.points));
    laneFixedTurn[thread.x] = float4(fixed.turn, 0);
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0 || !active) return;
    float4 dvAll = float4(0, 0, 0, 0), dwAll = float4(0, 0, 0, 0), fixedDv = float4(0, 0, 0, 0);
    float3 fixedDw = float3(0, 0, 0);
    for (uint l = 0; l < Lanes; ++l) {
        dvAll += laneMove[slot * Lanes + l];
        dwAll += laneTurn[slot * Lanes + l];
        fixedDv += laneFixedMove[slot * Lanes + l];
        fixedDw += laneFixedTurn[slot * Lanes + l].xyz;
    }
    const uint all = uint(dvAll.w + 0.5), fixedAll = uint(fixedDv.w + 0.5);
    // The velocity solve's averaged over every point together: apart, a body between the ground and a crowd is given
    // both whole, and a pile gains what neither took off.
    const uint every = all + fixedAll + world.points;
    if (every > 0) {
        v += (dvAll.xyz + fixedDv.xyz + world.move) / float(every);
        w += (dwAll.xyz + fixedDw + world.turn) / float(every);
    }
    woken = dwAll.w > 0.5;
    float3 stopV = float3(0, 0, 0), stopW = float3(0, 0, 0);
    uint stops = 0;
    Stops(i, me, w, meWas, stopV, stopW, stops);
    for (uint k = JointStart(i); k < JointStart(i + 1); ++k) {
        const Joint joint = LoadJoint(shared[JointListAt(k)]);
        const uint other = joint.head.x == i ? joint.head.y : joint.head.x;
        woken = woken || (other != None && Stirring(bodies[other]));
    }
    float3 going = v + (stops > 0 ? stopV / float(stops) : float3(0, 0, 0));
    float3 spinning = w + (stops > 0 ? stopW / float(stops) : float3(0, 0, 0));
    // Still for as long again, or awake: a sleeping one goes nowhere until something stirring touches it.
    const bool still = dot(going, going) < StillSpeed * StillSpeed && dot(spinning, spinning) < StillSpin * StillSpin;
    me.invInertia.w = push.sleepAfter > 0 && still && !woken ? StillFor(me) + push.h : 0;
    if (Asleep(me)) { going = float3(0, 0, 0); spinning = float3(0, 0, 0); }
    me.velocityReach.xyz = going;
    me.spinShape.xyz = spinning;
    bodies[i] = me;
}
