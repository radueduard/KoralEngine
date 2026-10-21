# Physics

Rigid bodies on the CPU (`modules/kphysics`) or on the GPU (the engine's XPBD functions, in the graph), and four scripts
the engine has of its own that put them in a scene. Give an entity a `RigidBody` and a `Collider`, put a
`PhysicsSimulation` on any one entity, and press Play: each fixed step, the bodies are simulated and their positions and
orientations written back.

| Script | What it is |
|---|---|
| `RigidBody` | A body physics moves. `kind` Static (nothing moves it), Dynamic, or Kinematic (moved by you: it pushes and is never pushed). `density` (kg/m³) or `mass` (kg, above 0 it wins), `friction`, `restitution`, `linearDamping`, `angularDamping`, `gravityScale`; `velocity` and `angularVelocity`, where it starts and, while it plays, what it is doing. |
| `Collider` | Its shape about the entity's position, before its scale: `shape` Sphere (`radius`), Box (`halfExtents`), Capsule along Y (`radius`, overall `height`), Plane on XZ facing +Y (static only), Cone along Y, its point up, about its middle as `$cone` is (`radius` of its base, `height`), or Mesh: the triangles of the entity's `Mesh`, a default one, a model's or a script's (static or kinematic only), as a solid wall `thickness` deep behind its faces. The defaults fit `$sphere`, `$cube`, `$capsule`, `$plane` and `$cone`. |
| `Joint` | A joint from this body to `other`'s, or to the world when there is none: `kind` Ball, Hinge (about `axis`, with `limited` angles and a motor), Distance (a rod), or Rope (only pulls). `anchor` is in this entity's frame, `otherAnchor` in the other's. |
| `PhysicsSimulation` | Steps the simulation on the fixed step: `mode` (CPU: kphysics; GPU: the graph, see below), `gravity`, `substeps` (2 keep a stack standing), `iterations`, `allowSleep`, `broadphase` (Every pair, Sweep and prune, Grid, Dynamic AABB tree), `gyroscopic` (Off, Explicit, Implicit), `threads` (0 as many as the contacts are worth, a thread for every 500, up to the cores and 8; 1 the main thread alone; n, n); what to draw (`drawColliders`, `drawVelocities`, `drawContacts`, `drawJoints`); and, while it plays, `bodyCount`, `contactCount`, `sleepingCount` and the last step's milliseconds. |

The kinds, shapes and broadphases are chosen by name in the inspector. They are kept and saved as numbers (in the
order listed: `Static` is 0, `Mesh` is 5), so a script sets them either way: `world.set(e, "RigidBody", "kind",
"Kinematic")` or `2`. A field of any component is so named by `kecs::FieldDescription::choices`.

**Seeing and sizing a collider.** The Collider's header in the inspector has two buttons, for the picked entity only:
the eye draws its collider in the scene view as the physics will have it (from the entity's position, rotation and
scale; grey static, green dynamic, blue kinematic; a mesh by its sharp edges), seen through what is in front of it;
the arrows draw it yellow and put a scaling gizmo on it in place of the one that moves the entity. Dragging a handle
changes the field that sizes it that way — a box's `halfExtents`; a capsule's or a cone's `radius` across and `height`
along; a sphere's `radius` — never the entity's scale. A drag is one change to undo, and while the scene plays the
bodies are built again with it. A plane or a mesh has no size to drag: it is shown only. They stay on as other
entities are picked, until pressed again.

They are registered on every scene that plays, as `FlyCamera` is (`modules/keditor/src/physics.cpp`), so they need no
project of scripts. Only bodies at the top of the hierarchy are simulated. The simulation is built from the scene
when Play is pressed, and again when a body is added or removed; a body moved by something else (the gizmo, a script,
the inspector) is put where it was put and stopped, and a kinematic body is given the velocity that takes it where it
is told to be in one step.

## On the GPU

A PhysicsSimulation whose `mode` is GPU runs nothing on the CPU. On the first step it is wanted, the graph is given three
pages of its own (when it has no `gpu physics` node already), and from then on they are the graph's like any other:
seen, changed, saved with it.

- **physics: bodies** (setup) — `gpu physics bodies`, a call of XpbdRigidBodies: every RigidBody with a Collider, and every
  Joint on one, as XPBD bodies.
- **physics: step** (the fixed step) — `gpu physics settings` (the PhysicsSimulation), `gpu physics step` (a FixedStep) and
  `gpu physics`, a call of XpbdStep with the settings' gravity, and its bodies sleeping as `allowSleep` says.
- **physics: written** (every frame) — `gpu physics written back`, a WriteBack of the bodies to their entities.

All of it idles while the mode is CPU, so the mode can change while the scene plays: to the GPU, setup reads the bodies
where kphysics left them; to the CPU, kphysics is built from where the GPU left them. On the GPU the bodies are spheres,
boxes, capsules and cones, against each other, against planes and against mesh colliders (static or kinematic, one-sided,
a solid wall `thickness` deep, as kphysics has them); joints are all four kinds, with a hinge's limits and motor.
`substeps`, `iterations`, `broadphase`, `gyroscopic`, `threads` and the drawing are kphysics', and `bodyCount` and the
other counts stay at 0. A kinematic body goes where its entity is put, as on the CPU: the step page's `gpu physics
kinematic` (a ForEach of the kinematic bodies) gives XpbdStep where each entity is as each step begins, and XpbdStep gives
the body the velocity and spin that take it there; the WriteBack leaves it to its entity, and sends the rest on by their
velocities over the frame's steps, so that what the blade pushes is not drawn a step behind it, in it. See
[the engine's functions](kgraph.md) for XpbdStep, and `PhysicsTest` in `modules/keditor/tests/test_physics.cpp` for the
two modes side by side.

## kphysics

Depends on nothing, as kecs depends on nothing, and is tested on its own (`kphysics_tests`).

- **Shapes**: spheres, boxes, capsules, planes; every pair by its own function (box–box by the separating axis test,
  then clipping), up to four points a manifold, each with a feature id.
- **Cones** are collided as pyramids of 16 sides (`ConeSides`), with mass and inertia of the round cone: against boxes,
  cones and triangles by the separating axis test over faces and edge pairs, then clipping, so a cone stands on its
  base; against spheres and capsules by the closest points of their core (GJK); against planes by their corners. Their
  body is at their centre of mass, a quarter of their height above the base; the Collider script offsets it from the
  entity's middle.
- **Meshes** (`TriangleMesh`): static or kinematic triangles with a tree over them, each one-sided. Each edge is known
  to be sharp (a convex fold, or the mesh's edge) or not; a body pushed out across an edge that is not sharp takes the
  face's normal, so nothing catches on the flat joins between triangles. A body touching triangles that face different
  ways (a floor and a wall) has a manifold for each way. Against a mesh, contacts are looked for as far as a body can go
  in the step, so a fast one is stopped at the surface; and a mesh given a thickness holds, and pushes back out along
  the face, a body pressed up to that far behind its faces, as a solid wall would.
- **Solver**: sequential impulses, warm-started by feature id; cone friction; restitution above 1 m/s; speculative
  contacts within a 2 cm margin; split impulse for penetration; substeps.
- **Joints**: ball, hinge (limits and a motor), distance and rope, solved with the contacts, their drift closed by
  position passes.
- **Islands and sleep**: union–find each step; an island still for half a second sleeps and costs nothing.
- **Broadphase**: every pair, sweep and prune, a uniform grid, or a dynamic AABB tree whose leaves reach ahead of
  bodies moving steadily and which is built again at once when most of it moves.
- **Threads**: the narrowphase's pairs are split into chunks, each collided into a list of its own and the lists joined
  in order; the contacts are coloured so that no two of a colour share a body that moves, and each colour is solved on
  the threads at once (`Simulation::threads`, a pool that spins between the step's jobs and sleeps between steps). With
  `threads` 0 each part takes a thread per 500 manifolds (or pairs): handing a colour over costs more than a small one
  saves. On threads the contacts are always solved by colour, so a run is the same to the bit whatever the number of
  threads, and on any machine.

It is the port of the Kotlin core of the physics project (`EngineProjects/physics`), and does what it does to the
bit: the reference runs that project recorded (`tests/reference/*.kprr`) are made again by the tests and compared
step by step, and no position differs at all. `kphysics_bench` measures it as the project's own benchmarks did.
