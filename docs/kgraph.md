# kgraph: the pass graph

`modules/kgraph` is a scene's pass graph: the nodes the editor will draw, kept as data, and the runtime
that turns them into GPU objects and render passes. It links the framework and kecs. It is built only
when the Koral SDK is found (`-DCMAKE_PREFIX_PATH=<GFX>/stage/debug`).

```cpp
kgraph::Graph graph;
std::string error;
kgraph::Graph::FromJson(text, graph, error);

kgraph::Runtime runtime(scene.Graph(), &world);   // the scene's kor::FrameGraph, and its kecs::World
runtime.Set(graph);                               // builds what the nodes describe
for (const auto& problem : runtime.Problems()) kor::log::error("{}", problem);
// each frame, before the frame graph runs:
runtime.Update();
```

## The graph, as data

A `Graph` is a list of `Node`s. A node has an id, a type, a title and a position for the editor, and
inputs by name. An input is a value or a link to another node's output.

```json
{ "kgraph": 1, "nodes": [
  { "id": "target", "type": "Image", "inputs": {
      "format": "eRGBA8_UNORM", "extent.x": 64, "extent.y": 64,
      "usage": ["eStorage", "eTransferSrc", "eTransferDst"] } },
  { "id": "pipeline", "type": "ComputePipeline", "inputs": {
      "compute.module": "fill", "compute.entry": "computeMain",
      "param.target": { "$link": ["target", "out"] },
      "constant.level": 0.5 } },
  { "id": "run", "type": "Dispatch", "inputs": { "pipeline": { "$link": ["pipeline", "out"] }, "groups.x": 8, "groups.y": 8 } }
] }
```

### Passes

A graph is laid out in passes. `Graph::passes` is their names in the order they are drawn, and a node's
`pass` is the one whose page it is on:

```json
{ "kgraph": 1, "passes": ["frame", "scene", "show"], "nodes": [
  { "id": "color", "type": "Image", "pass": "scene", "inputs": { ... } },
  { "id": "color (in show)", "type": "Reference", "pass": "show", "inputs": { "node": "color" } }
] }
```

A pass holds what it makes for itself and what it runs. What it uses of an earlier pass it brings in with a
**Reference** node, which gives every output the node it stands for gives. Passes are a way of laying a
graph out, not a boundary: a wire may go from any node to any other. Their order decides one thing: which
of two passes runs first when nothing they use says — and that is said by links, not by where a page is in a list
(see [Order](#order)).

### How often a pass runs

Each pass runs at one of three rates (`kgraph::PassRate`), in this order: **setup** (once), **fixed** (on the fixed
step), and **every frame**, which a pass is when nothing says. `Graph::setup` and `Graph::fixed` name the pages of the
first two, each in its own order; the rest run every frame. `passes` holds them all in that order, and they are saved
apart (`"setup": [...]`, `"fixed": [...]`, `"passes": [...]`). `Graph::RateOf(pass)` says which a pass is, and
`Graph::SetRate` moves it.

#### Setup

Some passes are made once, not every frame: a noise texture, a table worked out on the GPU, a buffer cleared to start
with. `Graph::setup` names those pages. They are among `passes`, first and in the order they run, and are saved apart
from the rest:

```json
{ "kgraph": 1, "setup": ["noise"], "passes": ["frame", "scene", "show"], "nodes": [
  { "id": "noise", "type": "Image", "pass": "noise", "inputs": { "usage": ["eStorage", "eSampled"] } },
  { "id": "make noise", "type": "Dispatch", "pass": "noise", "inputs": { "pipeline": { "$link": ["noise gen", "out"] }, "cover": "target" } }
] }
```

A pass node on a setup page records on the frame after it is built, and rests from then on. It stays in the frame
graph while it rests, so whatever reads what it made still runs after it. It records again only when it is built
again, and it is built again whenever something it is wired to is: an image resized (by the Screen), a constant or a
shader edited, a picture arriving from its file in place of its stand-in. A per-frame resource it writes is one copy
for each frame in flight, so it records on that many frames, once into each copy. Setup is one sequence: when any pass
of it records again, every pass of it does, as many times as the one that records most — so a pass that adds into a
buffer another cleared before it never adds twice. What a setup page uses of a page of
every frame is not there yet when it runs: a setup page brings in only what earlier setup pages made, and what is in
no pass.

A value it is given that follows the world (see [The scene](#the-scene)) changes where it is, without building
anything, so it makes setup record nothing. A pass that should follow a value every frame belongs on a page of every
frame.

#### The fixed step

A **fixed** page is for what must not go faster or slower with the frame rate: a simulation. Whoever runs the scene
tells the runtime of each fixed step it takes (`Runtime::FixedStep(seconds)`: a scene's `FixedUpdate`, which the editor
and the player call while the scene plays, 60 a second by default). A pass node on a fixed page records on a frame that
has one or more steps in it, and rests on a frame that has none. `Runtime::FixedSteps()` is how many the frame has.

A **FixedStep** node gives it as numbers: `dt` (a step's length, in seconds), `steps` (how many this frame has) and
`count` (how many there have been, this frame's among them), `lost` (steps frames had beyond the most they run) and
`alpha` (how far the frame is from the last step to the next, 0 to 1: what drawing mixes the last two steps' states by,
so that what moves on the fixed step moves smoothly between steps; `Runtime::SetFixedFraction`, which a scene sets).

Setup runs again when the scene starts playing from the beginning, and when it stops (`Runtime::RunSetupAgain`): what
it made — a simulation's starting state — starts from the world as it is. Not when it is paused, or plays on.

#### State kept from step to step

A **Buffer** or an **Image** node is one copy, kept from frame to frame, unless it is `isPerFrame` (a copy for each
frame in flight): what a step writes into it is there for the next. A pass on a fixed page that writes a per-frame one
is said to (its node's note): each frame in flight would step its own copy.

A **Keep** is a copy of a buffer or an image (`of`, with `eTransferSrc`) as it is where the Keep is among the passes: a
pass node that makes its copy, like its `of`, and copies into it each time its page runs. What comes after it reads the
Keep's `out` as the state was before, while it writes `of` itself: the last step's positions, beside this step's. It
is ordered among what writes `of` — by page, then by its place in the node list — not after every writer, as a pass
that only reads is (see [Order](#order)): put it first on the step's first page.

#### Back into the entities

A **WriteBack** writes what the GPU worked out into the entities it was worked out for. `buffer` is read back (a frame or
two late, as a watch is: see [Looking at what a node made](#looking-at-what-a-node-made)); `forEach` names the ForEach
whose elements the buffer's are — its first element is that ForEach's first entity — and `stride` how many bytes an
element is. Each element's `position` (three floats, where the entity is relative to what it is under), `rotation` (a
quaternion, x y z w) and `fields` (`{ "Spin.velocity": 32 }`: each component field's bytes, copied as they are) are
read from where those say, in bytes, and written into the entity — but only when what is read back changes, so that a
scene that is not stepping is not written over while it is edited. Only the runtime a scene is run by writes; a view
that follows it does not.

An entity moved by something else since it was last written — the gizmo, a script, the inspector — makes setup run
again, from the world as it is: what setup made of the entities starts from where it was put, and the read-backs from
before that are let pass. A simulation's state goes round through the entities this way, its velocities too when they
are among `fields`. An element whose float at `follows` (bytes into it) is not 0 follows its entity instead — a kinematic
body a script turns every step: it is neither written back nor taken as moved, and its entity's place goes to the
simulation another way (a `driven` flag a simulation keeps). Taken as moved, it would make setup run again every step.

What is read back is of the steps before this frame's, while what a script moved this frame — that kinematic blade — is
drawn where it was put: a body the blade pushes would be drawn a step behind it, in it. Given `velocity` (three floats,
bytes into an element) and `spin` (three floats, radians a second about the world's axes), each element is sent on by
them over this frame's fixed steps before it is written, so that it is drawn with the rest. Nothing is written while
setup is still being recorded (once for each copy of what it makes): it is made from the entities, and what is read back
is of what it is about to make again.

A resource made of values wired from the world — a Buffer whose `instanceCount` is a ForEach's `count` — is made again
when they change: one more body is a bigger buffer. What setup wrote into it is written again, as it is built again.

A frame with several steps runs its fixed pages once for each, each step after the one before: the passes of the fixed
pages are in the frame `Graph::fixedStepsAtMost` times over (4 unless the graph says; `"fixedStepsAtMost"`), and those
beyond the steps the frame has rest. Within them, what a fixed pass reads that a fixed pass writes is ordered among the
writers, so that a step reads what the step before it wrote. A frame with more steps than that loses the rest — the
simulation slows rather than falling further behind — and FixedStep's `lost` counts them. Every step of a frame is
given the same values: FixedStep's `count` is the frame's, not each step's.

`Graph::overview` is where nodes are when every node is seen at once, by id — a layout apart from the
passes' pages (`"overview": { "color": [1200, 40] }`). It is only for the editor; nothing that runs reads it.

Values are JSON's own: numbers, booleans, strings, lists and objects. An enum is its name, flags are a
list of names, a vector is a list of numbers, and a struct is an object by field name. `Graph::ToJson`
and `Graph::FromJson` round-trip a graph exactly.

### Functions

A **Function** node is a call: the nodes of a function, as one. A function is a graph of its own — kept by
whoever has the graph (`Graph::functions`, by name; a project's `functions/<name>.kfunc`, for the editor) — whose
**FunctionInput** and **FunctionOutput** nodes are its edges. Each edge is a port of every call, by its `name`, in
the order its `order` says (from 0; an edge that says none comes after, as the edges are on the function's page, top to
bottom); its `kind` says what it carries: `eValue`,
`eEntities` (what a Filter keeps some of), or a resource — `eMesh`, `eBuffer`, `eImage`, `eResource`. An input a
call is given nothing at takes its `default`; an `eBuffer` given nothing is a small buffer of zeros made in the call's
place, so that a buffer a function can do without — with a count of 0 beside it — may be left unwired.

Before the graph runs, each call is written out into its function's nodes, `<call>/<node>`, in the call's pass —
before its loops are, so that a function can hold a loop and a call can be in one. What is wired to an input is
what the call is given there; what is wired from the call is what the output is wired from inside. So nothing new
is evaluated: a call wired from a ForEach follows it as its nodes would. A call fares as its nodes do: the first of
them that fails says so on it, by name (`times: 'b' is a number`). A function that calls itself is written out
eight deep, and no further. A loop in a function names its own nodes — a Previous's `of` and `first`, an Element's
`of` — and they are named as the call has them written out. `Runtime::PortsOf` and `OutputsOf` give a call's ports as its edges are, an output as
the node inside gives it, and `ValueOf` and `ResourceOf` what is there.

A FunctionInput or FunctionOutput in a graph itself — a function opened in place, in the editor — stands for what
its `value` is wired from.

A call's nodes are written out where the call is among the nodes, and a loop's a turn at a time — every node of the
turn, then the next turn's — where the loop's first node is: passes that nothing they use orders run in the order of
the nodes, so a function's passes run where it is called, and a loop's in turns.

A function's file says what it is for beside its graph, for the editor's list of nodes to offer it under and find it by
(`Document::AboutFunction`): `"purpose"` (`"Physics"`), `"description"`, and `"tags"` (`["prefix sum", "scan"]`). One
of the engine's saved as the project's own keeps them.

A call of a project's function also keeps it as its `file` (`function:Scale`), which a scene saves by the file's ID: the
file renamed or moved, the call is called what it is now when the scene is read again (an editor keeps `file`).

**The engine's functions** are there in every project (`packages/koral/functions`, beside its shaders in
`packages/koral/shaders`); a project's function of the same name is called in its place, and one of the engine's
opened and changed is saved as the project's own (`Document::EngineFunctionsDirectory`, `IsEngineFunction`). Each
works on 32-bit uints, in buffers with `eStorage`:

| Function | Given | Gives |
|---|---|---|
| **Scan** | `values`, `count` (up to 1,048,576) | `values`, each the sum of those before it (an exclusive prefix sum), in place; `total`, a buffer whose first uint is the sum of them all |
| **RadixSort** | `keys`, `values`, `count` (up to 16,777,216) | both sorted by key, in place: stable — the same keys keep their values in order — four bits a turn, eight turns |
| **BitonicSort** | `elements`, `type` (`uint`), `count` (up to 4,194,304), `passes` (253, given on the call) | `elements` sorted in place, of any type that is IComparable (see below): not stable, in log2(count) × (log2(count) + 1) / 2 passes, log2 rounded up — 153 for 2¹⁷. `passes` is how many are written out: 253 covers any count, those not needed cost a little each frame, and with too few the elements are left unsorted |
| **OddEvenSort** | `elements`, `type` (`uint`), `count`, `phases` (2, given on the call, up to 256), `start` | `elements` nearer sorted in place, of any type that is IComparable (see below), and stably: each phase meets each element with its neighbour. For what is nearly sorted already, a few phases a frame; sorted whole, it takes `count` phases. Wire `start` from a count of frames when `phases` is odd, or the same pairs are met each frame |
| **Compact** | `flags`, `values`, `count` | `kept`: the values whose flag is not 0, one after another, in their order; `count`, a buffer of how many |
| **IndirectArgs** | `counter` (a buffer), `offset` (which uint of it), `groupSize` | `args`: the groups that cover that count, for a Dispatch's `indirect` |
| **Camera** | — | the scene's camera — the first entity that is a Camera (a PerspectiveCamera or an OrthographicCamera), a View from where it is the way it faces and a Perspective of its fields — or, orthographic, an Orthographic of its height — at the window's aspect, both `main` — as `view`, `projection`, `viewProjection`, and `camera`: a Bundle of `view`, `projection`, `inverseView`, `inverseProjection`, `viewProjection`, `position`, `zNear` and `zFar`, the names a camera block has |
| **SunShadows** | `entities`, `cascades` (a buffer whose first four matrices are the cascades'), `matrices` (the same four, a ShadowCascades' `matrices`), `layout` (a mesh's vertex layout), `size` (4096), `without` (components whose entities cast none: `["PointLight"]`) | `atlas`: four cascades' depth, `size` each way, two by two, each entity with a Mesh drawn as each cascade sees it (`koral_shadow`), again only when what it would draw has changed |
| **Bloom** | `picture` (an image of light, `eStorage`), `sampler` (linear), `threshold` (1), `knee` (0.5), `strength` (1/12) | the picture with what is brighter than `threshold` bled round it, in place (`koral_bloom`): six levels down, five up; and `picture`, for what comes after |
| **Tonemap** | `picture`, `exposure` (1) | `picture`: an image of eight bits a channel the window's size, exposed, tone mapped (ACES) and encoded for the display (`koral_tonemap`) |
| **TAA** | `picture`, `depth`, `viewProjection` (this frame's, not jittered), `blend` (0.1) | `picture`: an image like it of the picture blended with what this made the frame before, where each pixel was then (from its depth and the frame before's viewProjection, a LastFrame's), held to the colours round it now in YCoCg so that what moved leaves no trail (`koral_taa`). Its history is a History of what it makes; a frame with none — the first, another size — is the picture as it is. Draw with a Jitter's `jittered` projection |
| **HiZ** | `depth`, `reversed` (0) | `pyramid`: 512 by 256, nine levels of R32F, each texel the farthest depth of what it covers (`koral_hiz`): of a reversed depth, the least |
| **InstanceCull** | `instances`, `stride`, `matrixOffset`, `ranges`, `drawCount`, `viewProjection` (as CullInSight's), `early`, `earlyVisible`, `late`, `lateVisible` (two IndirectDraws in eGPU), `depth` (what the early draw draws into), `reversed`, `count` (65536: how many instances at most) | two phases of an indirect draw, on the GPU (`koral_occlusion`): `early` draws what was seen the frame before and is in sight; a HiZ of what it drew; `late` what is in sight, was not seen and is not behind it. Whether each is seen is kept for the next frame. Two Draws of the commands — the late one `load` — and each draw's shader finds its instance through its own `visible`. Also `pyramid` |
| **Lights** | — | `lights`: every DirectionalLight, then PointLight, then SpotLight of the scene as koral_lights.slang's `KoralLight` (64 bytes: position and range, direction and kind, linear colour times intensity and the cosine of the spot's inner angle, of its outer); `count`, and `directional`, `points` and `spots` |

BitonicSort and OddEvenSort sort what a shader says how to compare, where RadixSort, which reads a key's bits, sorts
uints alone. `type` is `float`, `uint` and the like, or a struct as `module.Type`, conforming to Slang's `IComparable`:

```slang
// shaders/fluid.slang
struct Entry : IComparable {
    uint hash; uint index;
    bool equals(Entry other) { return hash == other.hash; }
    bool lessThan(Entry other) { return hash < other.hash; }
    bool lessThanOrEquals(Entry other) { return hash <= other.hash; }
};
```

```json
{ "id": "sort", "type": "Function", "inputs": { "function": "BitonicSort", "type": "fluid.Entry", "passes": 153,
    "elements": { "$link": ["entries", "out"] }, "count": 131072 } }
```

A comparison sees the two elements and nothing else, so what it goes by — a distance from the camera — is written into
them first. Each swap copies a whole element: what is large is better sorted as a small `{ key, index }`. `type`,
`passes` and `phases` are given on the call, not wired. Under them is a ComputePipeline's `compute.element`: with the
module `koral_compare_sort`, the type it sorts, for which a module (`koral_sort_of_<type>`) is generated beside the
material kinds' and built as any other, again when the type's own module is written. A type that does not conform is
said on the call, in the shader compiler's words.

A buffer read or written by one of the engine's shaders is read and written (`RWStructuredBuffer`), so that the passes
that use it run in the order of the nodes.

## Node types

Every node type is of a **level** (`NodeType::level`, `kgraph::Level`): what one works at with it, each level made of
the ones below.

| Level | What it is | Examples |
|---|---|---|
| 0 value | worked out on the CPU, and what routes it | Float, Math, Vectors, Logic, View, Perspective, Switch, Bundle, Reference |
| 1 resource | a GPU object | Image, Buffer, Sampler, ImageView, the pipelines, Primitive, ImageFile |
| 2 command | one thing done on the GPU | Draw, Dispatch, TraceRays, Blit, Copy, Clear |
| 3 scene | what is read of the entities, and assembled of them | Entities, Filter, ForEach, MeshPack, IndirectDraws, Meshlets, RayScene, ShaderBuffer, BufferBuilder, ComponentBuffer |
| 4 technique | a way of drawing or working, whole | a Function's call: Bloom, Tonemap, SunShadows, RadixSort |

`NodeType::composite` says it is made of other nodes: a Function's call, written out as its function's nodes before
the graph runs. An editor shows both on a node's card (see editor.md).


`kgraph::Catalog()` lists them, with every input and output, its type and its description. This is what
the editor will build its node list and its nodes from.

- **One for each builder the framework describes** (`kor::builders`): Buffer, Image, ImageView, Sampler,
  BufferView, Shader, Framebuffer, Mesh, AccelerationStructure and the three pipelines. A builder setting
  with one argument is an input of that name. With several it is one input per argument:
  `extent.x`, `entryPoint.module`. A setting that can be given more than once is `color[].view`, written
  in a graph as `color[0].view`, `color[1].view`. Nothing is written by hand: a builder that gains a
  setting gains the input. An Image is wired as it is where a view of all of it is wanted — an attachment, a
  pipeline's image parameter — and that takes the image's default view, of the shape the shader declares: an
  ImageView node is for part of an image (a mip level, a layer) or another way of seeing it.
- **Values:** numbers of a kind — Float (32 bits, as a shader's `float`), Double, Int and UInt (what they are given
  rounded toward zero and kept within 32 bits; a UInt none or more) — and vectors of a size and a kind: Vec2, Vec3 and
  Vec4 of floats, IVec2 to IVec4 of ints, UVec2 to UVec4 of uints, each component given (0 when not). Bool, String,
  Color, Constant (`ePi`, `eTau`, `eE`, `eSqrt2`, `ePhi`), and Split (a vector's `x`, `y`, `z`, `w` as outputs of their
  own). **ColorConvert** says a colour another way: `from` and `to` each of `eSrgb` (as a display shows it: a picked
  colour), `eLinear` (the terms light adds up in), `eGamma22` (the display's curve as the power 2.2), `eHsv` (hue 0 to 1,
  saturation and value, of an sRGB colour), and `from` `eKelvin` too (a temperature, as the colour it glows); alpha is
  passed on, and `luminance` is how bright it is in linear terms. **DeviceSupports** is whether the device has a
  `feature` — `eMeshShader`, `eRayTracing`, `eAsyncCompute`... — as true or false, for a Switch to go another way where it
  has not. Number, Vector and Component are no more: a Double, a Vec2 to Vec4 and a Split's output are what they were.
- **Random numbers** (their own section, Random): a node for each of `<random>`'s distributions — **RandomUniform** (`min`
  to `max`, 0 to 1), **RandomNormal** and **RandomLognormal** (`mean`, `stddev`), **RandomExponential** and
  **RandomPoisson** (`lambda`), **RandomGamma** and **RandomWeibull** (`shape`, `scale`), **RandomExtremeValue** and
  **RandomCauchy** (`location`, `scale`), **RandomChiSquared** and **RandomStudentT** (`degrees`), **RandomFisherF**
  (`degrees`, `denominatorDegrees`), **RandomBernoulli** and **RandomGeometric** (`p`), **RandomBinomial** (`trials`, `p`)
  and **RandomNegativeBinomial** (`successes`, `p`) — each with a `seed`, and a `type` that is Auto unless it is set to
  Float to UVec4. Auto is the shape of the input it is wired to — a Dispatch's `push.tint` of a `float4` is four numbers,
  an Int's `value` a whole one — through what takes any kind and gives what it is given (an Add, a Lerp) to what that is
  wired to; the first wire that says, and a Float when none does (`Runtime`: `WiredShape`). One built before what it is
  wired to says — a pipeline built after it — is drawn again once that does. Each of a distribution's inputs is a number for
  every component or a vector, one for each. A whole kind's numbers are whole: a uniform one each from `min` to `max`,
  both included, as likely; any other rounded to the nearest and kept within what the kind holds. It is drawn when it is
  built, and the same seed gives the same numbers on any machine: the generator is `std::mt19937_64`, whose output the
  standard fixes, and the distributions are the engine's own (`kgraph/random.h`), not a standard library's, whose
  algorithms differ. A **RandomDevice** is a seed no one chose, from `std::random_device`: a new one each time the graph is
  opened, kept while it runs whatever else is edited, and drawn again when its `draw` changes.
- **Math:** Add, Subtract, Multiply, Divide, Min, Max, Pow, Mod, MultiplyAdd, Negate, Abs, Floor, Ceil, Round,
  Trunc, Fract, Sign, Saturate, OneMinus, Reciprocal, Sqrt, InverseSqrt, Exp, Exp2, Log, Log2, Log10, Sin, Cos,
  Tan, Asin, Acos, Atan, Atan2, Sinh, Cosh, Tanh, Radians, Degrees, Clamp, Lerp, InverseLerp, Remap (`value` from
  `fromMin`–`fromMax` to `toMin`–`toMax`), Smoothstep, Step. They work on numbers and, a component at a time, on
  vectors; a number goes with every component.
- **Vectors:** Length, Normalize, Dot, Cross, Distance, Reflect, Refract (`eta`; all zero when it is all
  reflected), Project (`a` along `onto`), ColorConvert. Quaternions, x y z w as an entity's `orientation` is:
  QuatFromEuler (degrees, as an entity's rotation), QuatFromAxisAngle, QuatToEuler, QuatToMatrix, QuatMultiply (`a`
  after `b`), QuatInverse, QuatSlerp (the shorter way) and QuatRotate (a vector turned). Matrices of four by four: MatrixMultiply, MatrixInverse, MatrixTranspose,
  MatrixDeterminant; MatrixTranslate, MatrixRotate (x, y, z in degrees, as an entity's rotation), MatrixAxisAngle,
  MatrixScale and MatrixCompose (position, rotation and scale, as the world composes an entity's); TransformPoint
  (divided by its w), TransformDirection and TransformPlane (a plane where a matrix that moves points puts it).
- **Cameras:** View, Perspective, Orthographic, Frustum, Oblique, Paraboloid, CubeFace, ShadowCascades, PointShadows and
  Jitter. See [The scene](#the-scene).
- **Logic:** Less, LessOrEqual, Greater, GreaterOrEqual, Equal, NotEqual, Not, And, Or, Xor, Any, All and Select
  (`a` when its condition is true, `b` when not). Less to GreaterOrEqual compare vectors a component at a time — a
  number with every component — giving true or false for each; Any and All say those as one, and a Select given one
  for each component picks each component of `a` or `b` by its own. Equal and NotEqual compare whole values.
- **LastFrame** is a value as it was the frame before — last frame's viewProjection, for what moved since — and `valid`,
  false on the first frame.
- **VertexLayout** describes a mesh's vertices: a list of `{ "semantic": "POSITION", "type": "eFloat",
  "count": 3 }`, packed in the order given. Its output goes to a pipeline's `vertexShader.layout` and to a
  Mesh node's `vertexLayout`.
- **Primitive** is one of the default meshes (below), by its name: `eCube`, `eRoundedBox`, `eTeapot`, ...
  Each has positions, normals, texture coordinates and the tangents the graph asks for, and faces outward
  (the inside ones, in). Its second output,
  `layout`, is what a pipeline that draws it takes as `vertex.layout`.
- **Entities** is every entity, a **Filter** those of them with and without components, an **Entity** one of them (the
  first with a component, or one by name — of no component, the one named, wherever it is), and a **ForEach** reads any
  component of the world — the scene's Camera and lights are the engine's package's scripts —
  every entity of it. Each gives where each entity is: `matrix`, `normalMatrix`, `previousMatrix` (where it was the
  frame before: what motion vectors are worked out from), `worldPosition` and the rest.
  **SceneBounds** is the box round some of them (`entities`: an Entities node's, a Filter's) — each one's mesh where it
  is, or where it is when it has none — as `min`, `max`, `center`, `radius` and `count`: what ShadowCascades' bounds are.
  **WriteComponent** writes a `value` into a `component`'s `field` of an entity (`entity` by name, or the first that has
  it), when it holds something else: what the graph worked out, for a script or the inspector.
  **View** and a lens — **Perspective**, **Orthographic**, **Frustum**, **Oblique**, **Paraboloid** — are what a
  camera or a light sees, **ColorConvert** a colour as light adds it up, **Collect** every element's value as one list,
  and **ShadowCascades** and **PointShadows** the shadow maps' matrices. A BufferBuilder `eUniform` packs values into a
  block a shader reads. See [The scene](#the-scene).
- **DebugShapes** draws lines in the scene where values say, every frame — `kind` eLine or eArrow (`from`, `to`), eBox
  (`min`, `max`), eSphere (`center`, `radius`), eFrustum (`matrix`, a viewProjection), eAxes (`matrix`, `size`) or
  eTransformedBox (the cube -1 to 1 turned by `matrix`), in `color` — as many as its values have: ShadowCascades'
  four matrices are four frusta. A DebugDraw pass draws them, with the scene's other debug shapes.
- **ShaderBuffer** is a buffer a shader declares, with an input for each field of its struct, and **ForEach** is
  every entity that has a component, an element at a time: an output for each of its fields, and where it is. Wired together they are an element for each entity. See [A shader's buffer, from the world](#a-shaders-buffer-from-the-world).
- **BufferBuilder** is a buffer whose element is laid out on the node. See [A buffer laid out on its node](#a-buffer-laid-out-on-its-node).
- **ComponentBuffer** is every entity of a component as a buffer, laid out as the component is: a ForEach and a
  BufferBuilder as one node. See [A component's buffer](#a-components-buffer).
- **A node for each component.** Every component of the world — the package's, a project's scripts', a pipeline's — is
  offered as one more: `PointLight`, a ForEach of it (`Runtime::Presets()`). It is no type of its own: what is saved
  is the ForEach it is, and the editor offers it, and `Document::AddNode` makes it, by that name. Its buffer is a
  ComponentBuffer, told the component.
- **Screen** is the scene's window. Its `extent`, `size`, `width`, `height` and `aspect` outputs are the window's
  size: an image whose `extent` is wired to it is rebuilt when the window is resized, and so is whatever is
  made of that image. **Reference** stands for another node by id: how a pass brings in what an
  earlier one made.
- **ImageFile** is a picture from a file. See [Files](#files).
- **Script** runs one of the project's `ResourceScript`s (see [scripts](scripts.md#scripts-that-make-resources)), by
  its class, in `script`. Its outputs are what the script makes, each by its name — Images, Buffers, ImageViews,
  BufferViews, Samplers — and its inputs are the script's fields, `field.<name>`, and what each resource is made with,
  `<resource>.<input>`: an Image's `picture.format`, `picture.extent.x`, the inputs a node of its kind has, starting as
  the script's source says (`ScriptResource::settings`) and otherwise as that kind of node starts. A resource made of
  another — an ImageView's `image` — names another of the script's in its source, or is wired. The images and buffers
  that are of nothing else of the script's are made first; then the script fills them all at once — an image's first
  level, every layer, its smaller levels made from it; a buffer's every byte — and they are made again with what it
  wrote; then the views of them. It is all made and filled again whenever any of it is: a size changed, the script set
  again (edited, loaded, run), a field changed. Whoever runs the graph says which scripts there are
  (`Runtime::SetResourceScript`, a `ResourceScript` with its `resources` and a `fill` function); one declared and run by
  nothing makes what it makes empty, and the node's `Status(id)->note` says so. A per-frame resource, or an image of a
  format whose texel is no plain numbers, cannot be filled: it says why. An editor offers a Script node of each script
  among the nodes to add (`Runtime::Presets`).
- **Repeat**, **Previous**, **Element** and **Switch** are what is done more than once, and one of several
  ways. See [Loops and branches](#loops-and-branches).
- **Passes:** Clear, Blit (scaled, a part or all, and how a picture reaches the screen), Resolve (a
  multisampled image made an ordinary one), Copy (image to image of one size), CopyBuffer, ClearBuffer,
  CopyBufferToImage, CopyImageToBuffer, GenerateMipmaps, Dispatch, TraceRays and Draw. Every command the
  framework's command buffer records has a node, and each of its arguments is an input. A Clear clears every mip and
  layer of an image: a depth image to its `depth` (1) and `stencil` (0), an image of whole numbers (UINT, SINT) to the
  whole numbers its `color` says, as those bits rather than floats. See [Shorthands, and what comes back](#shorthands-and-what-comes-back)
  for ComputePass, RenderTarget, History and Readback.

Every pass node also takes `enabled`: false leaves the pass out, with the node still there and still ok.

A pass node that records compute dispatches and copies only — Dispatch, Copy, CopyBuffer, ClearBuffer, CopyBufferToImage,
CopyImageToBuffer (`kgraph::RunsAsync`) — also takes `async`: true runs it on the async compute queue, beside the graphics
passes it does not depend on, ordered against them by what it reads and writes (`kor::PassBuilder::AsyncCompute`). One
that uses the screen stays on the graphics queue, and on a device with no second queue it runs there in order, which is
always correct; its node's note says which. `FrameGraph::Schedule` says where each pass ran.

## Shorthands, and what comes back

Some nodes are one node to whoever writes the graph and several to what runs it: written out before anything runs
(`expand.cpp`, as a Function's call is), each under its own id, so that its outputs and its status are its own.

- A **ComputePass** is a compute shader run: a ComputePipeline (`<id>/pipeline`) of its `module`, `entry`, `param.<name>`
  and `constant.<name>`, and the node itself a Dispatch of it with the rest — `groups`, `cover`, `indirect`, `idle`,
  `push.<name>`, `enabled`, `async`. Its shader's parameters are its ports (`Runtime::PortsOf`). A pipeline more than
  one Dispatch runs is still a ComputePipeline and Dispatches.
- A **RenderTarget** is an Image of a `format`, sized by the window — or by the image wired to `sizeOf` — times `scale`
  (a number, or one each way), rounded up and at least a pixel: the Screen, a Multiply, a Ceil and a Max written out
  (`<id>/screen`, `<id>/scaled`, `<id>/rounded`, `<id>/extent`). `extent` given is the size instead. `mipLevels` 0 is
  every level down to a pixel. Its usage is what it is used as, as an Image's is; its `extent` out is what it worked out.
- A **History** is what a buffer or an image (`of`) was when the frame before was over: a Keep on a page of its own
  before every other (`@history`), copied when the frame begins, before anything writes `of`. What reads its `out`
  during a frame reads the frame before's, while it writes `of` itself. `valid` is false until a frame has been copied
  (the first, and after `of` is made anew). A Keep says `valid` as well, from the frame after it first copied. A kept
  copy is made to be read — sampled, or as storage — whatever `of` was made for.
- A **Readback** is a few numbers back on the CPU, as a value: of a `buffer`, `type` (eFloat, eUInt, eVec4...) at
  `offset`; of an `image`, the texel at `texel` (x, y, and z or a layer) of `mip`, as many numbers as its format has
  channels, each as a shader reads it (a half, a normalized integer). It is a pass: copied where it is among the passes,
  into the next of four buffers of its own, a frame each, and read from the oldest — what the GPU is done with, three
  frames late. `ready` is false until a copy has come back. What is wired from it follows it every frame.

A **Screen** also gives the window's `format` (eBGRA8_UNORM...) and `scale` — how many of its pixels a point of the
desktop is — as whoever runs the graph says (`Runtime::SetScreenLook`). A **Time** also gives `frame`: which frame this
is, from 0, one more each frame the graph runs — a Jitter's.

## Loops and branches

A **Repeat** is a count. Every node wired from one, straight or through other nodes, is in its loop: it is there
once for each turn, and what it takes from the Repeat is that turn's number — `index` (from 0), `reverse`
(counted from the last), `count`, `first` and `last` (true or false).

```json
{ "id": "levels", "type": "Repeat", "inputs": { "count": 6 } },
{ "id": "level", "type": "ImageView", "inputs": { "image": { "$link": ["chain", "out"] },
    "baseMipLevel": { "$link": ["levels", "index"] }, "mipLevelCount": 1 } },
{ "id": "above", "type": "Previous", "inputs": { "of": "level", "first": "picture view" } },
{ "id": "down", "type": "ComputePipeline", "inputs": { "compute.module": "bloom", "compute.entry": "downsample",
    "param.source": { "$link": ["above", "out"] }, "param.target": { "$link": ["level", "out"] } } },
{ "id": "run down", "type": "Dispatch", "inputs": { "pipeline": { "$link": ["down", "out"] }, "cover": "target" } }
```

is six views, six pipelines and six passes, each reading the level the turn before wrote.

- **Previous** is what a node of the loop (`of`, by id) made the turn before. On the first turn it is the node
  `first` names.
- **Element** is what a node of a loop made on one turn (`index`; the last, when not given). It is in no loop
  for naming one, so it is how what a loop made is used outside it, or by another loop. Of a function's call in a
  loop, it is one of the call's outputs — the first, or the one `output` names — when that is the `out` of a node in it.
- **Switch** is one of the things wired to its `option[n]` inputs, by `index`: a resource or a value. With a
  pass's `enabled` it is a branch: a picture to show or another, and the pass that makes the other left out
  while it is not shown.
- **DebugView** is a picture an editor's scene view can be switched to: its `picture`, listed by its `name`
  (`Runtime::DebugViews()`). `Runtime::ShowDebugView(id)` shows one: its `shown` is true — wired to the `enabled` of
  the passes that make its picture, they run only then — and its `out` is the picture. Which is shown is the
  runtime's, not the graph's: nothing is saved of it, and another runtime of the same graph (the editor's Game
  panel, the project run) shows none.

Every pass also has `views`: `all` (what it is when not given), `game` — run only where `Runtime::SetEditorView` is
false, as in the game and the editor's Game panel: a vignette, words on the screen — or `editor`, run only in an
editor's scene view. And a pipeline whose shaders declare the specialization constant `koralDebugView`
(`packages/koral/shaders/koral_debug.slang`) is built with what `Runtime::SetShaderDebugView` says — a channel of
its material to show in place of its lit colour (`kgraph::ShaderDebugChannel`) — unless its node gives it.
`Runtime::WithoutShaderDebugView()` lists the graphics pipelines whose shaders do not. Each of the three is taken
up at the start of the next frame, building again only what it changes.

The graph that is saved and edited has the loop as a loop. What runs has it written out, a node a turn, named
`down [0]`, `down [1]`: `Runtime::TurnsOf("down")` lists them, and each is asked about like any node. Asked
about by its own id, a node of a loop answers for its first turn — its ports, what it made — and its `Status` is
that of the first turn that failed. The passes of a loop run in the order of their turns.

## What a node gives

Besides what it makes (`out`), a node gives everything about itself as outputs to wire from:

- **What the object says of itself.** An image or a view: `size` (width and height), `extent`, `width`,
  `height`, `depth`, `mipLevels`, `arrayLayers`. A buffer: `bytes`. A framebuffer: `size`, `width`,
  `height`. A mesh: `vertexCount`, `indexCount`. A compute pipeline: `numThreads`, its shader's
  `[numthreads(x, y, z)]`.
- **Each value it was given**, under the input's name: an image's `format`, a Clear's `color`.
- **The parts of one thing as one:** `extent` for `extent.x`, `.y`, `.z`; `groups` for a Dispatch's.
- **What an input is when nobody set it**, where the node type says.

So a second image half the size of the first is `{"$link": ["first", "size"]}` into a Divide, and that
into the second image's `extent`. When the first is resized, the second follows. These outputs are optional
in the catalog (`Port::optional`): an editor shows one when it is asked for or wired.

## An input in parts, given whole

An input that is three numbers (`extent.x/.y/.z`, `groups.x/.y/.z`, `groupSize.x/.y/.z`) also has an input
named for the whole, taking a vector: `"groups": {"$link": ["count", "out"]}`. The runtime splits it into
its parts before the node is built, as many as the vector has numbers, so an image's `size`, of two, leaves
the third as it was. A node gives the whole or its parts: both is an error. `kgraph::PartsOf(type, name)`
says which inputs a whole stands for.

## What an input starts as

`Port::initial` is what each input is to begin with. A node an editor adds gets the ones it needs set to
it: an Image is 512 by 512, `eRGBA8_UNORM`, sampled and copyable; a Clear's colour is opaque black; a
Dispatch covers in groups of 8. For an input a node can do without, it is what shows before it is set. A
struct given some of its fields has the rest from it: a `rasterizationState` given only its cull mode
keeps a line width of 1 and fills its polygons, where it used to get zeros. So does a setting given some of its
arguments: a Framebuffer given only `depthStencil.view` has its depth attachment cleared to 1 (the farthest) and
called `depth`, and a `color[n].view` alone clears to opaque black — where the builder's own zeros would clear depth
to 0, which nothing drawn with a depth test passes.

`Port::min` and `Port::max` are what an editor keeps a number input within, each of a vector's numbers, and each part
of an input in parts the same as the whole: an Image's `extent` and `mipLevels` at least 1, a Repeat's `count` 1 to
256, a Perspective's `fieldOfView` 1 to 179, a CubeFace's `face` 0 to 5, a ShadowCascades' `lambda` 0 to 1, a group's
size at least 1, and any count, offset or size of a draw no less than 0; a copy's or a blit's levels 0 to 15 and its
layers 0 to 2047; a framebuffer's clear depth 0 to 1 and stencil 0 to 255; a sampler's level of detail bias −16 to 16.
An unsigned builder argument is at least 0. A Script node's `field.<name>` inputs have the script's `[Range]`, `[Min]`
and `[Logarithmic]`. Anything else is unbounded — a position, a matrix, what a math node works on. The graph does not
check them: a value from a wire, or a file, is used as it is.

`Port::shape` is exactly what a value input takes or a value output gives — `Shape`: the kind of its numbers (`Scalar`:
float, double, int, uint, bool) and how many, 1 to 4 — where that is said: each number and vector node's inputs and
output, a Color's, a builder's argument of a number's kind, a pipeline's `constant.<name>` and a draw's `push.<name>` as
their shaders declare them. It is not said of what takes any kind (a Math node's `a`), of what is not numbers, of a
matrix, or of a whole of parts. An editor draws an input by its shape, not by what it holds: a `float4` four drags (or a
colour) before anything is set, an int dragged a unit at a time, a uint at none or more, a bool a box to tick; and what is
set of an input is made its shape (`Conform`: rounded to a whole number, kept within what an int or a uint holds, cut to a
float's precision, as many numbers as it has) — so an Int holds 3, not 2.6, whatever was dragged or typed.

### What a shader says of its inputs

A Slang shader's push constants carry their defaults and, with `import koral_inspector;`
(`packages/koral/shaders/koral_inspector.slang`), what the editor keeps them within and draws them as:

```slang
import koral_inspector;

void computeMain(uint3 id : SV_DispatchThreadID,
                 [Range(0, 50)] uniform float windSpeed = 10.0,
                 [Color] uniform float4 tint = float4(1, 0.5, 0, 1),
                 [Min(0)] uniform int steps = 3)
```

`[Range(min, max)]`, `[Min(n)]`, `[Max(n)]`, `[Logarithmic]` and `[Color]` (a `float3` or `float4`) are a parameter's;
Slang lets an attribute be a parameter's or a struct field's, not both, so a field's are `[FieldRange]`, `[FieldMin]`,
`[FieldMax]`, `[FieldLogarithmic]` and `[FieldColor]`. A default becomes the input's `Port::initial`: what a draw given
nothing for it runs with, and what an editor shows it as. Slang gives a scalar's; a vector's is read from the source — the
entry point's parameters, or its struct's body — when it is written as numbers (`float4(1, 0.5, 0, 1)`, `float3(1)`); one
worked out (`= speed * 2`) is said in the input's doc, and is zero. A specialization constant's default is its input's
`initial` too. Koral carries both (`kor::Shader::Annotation`, on a `PushConstantField` and a pipeline's
`PushConstantMember`); what an attribute means is the engine's.

`Port::group` is the part of an inspector an input is in — a Draw's *Indirect*, *Depth*, *Stencil*; a builder's struct,
`rasterizationState`; what three or more inputs share the start of the name of, `color[]` — and empty for what is at
the top. `Port::when` is when it does anything: while another input is one of some values (a Draw's `indirectOffset`
while its `command` is an indirect one, a Sampler's `maxAnisotropy` while `anisotropyEnable` is true), with what that
is in words. `kgraph::Applies(port, node)` says whether it does as the node is set — by what it is given, or what that
input starts as; one wired, or neither given nor starting as anything, might, and does.

`Port::logarithmic` is a number dragged by so much of what it is rather than by so much: a size (an extent, a buffer's
bytes, a shadow map's resolution), a distance (a lens's near and far, a cascade's reach), a count that may be a few
or thousands (vertices, instances, groups). What is between two ends — a field of view, a `lambda`, a depth — is not.
`Port::integer` is a whole number, dragged and typed a unit at a time: a builder's integer argument, a count, an
index, a level or a layer, an Int's or a UVec's. `kgraph::LimitsOf(struct, field)` says the same of a field of one of
the builders' structs: a stencil's masks and reference 0 to 255, a depth range 0 to 1, `minSampleShading` 0 to 1, a
line 0 to 64 pixels wide, a vertex attribute of 1 to 4 numbers at location −1 (by its semantic) to 31.

## Pipelines

A pipeline names its shaders itself, a stage at a time: `<stage>.module` is a `.slang` file among the
project's shaders, and `<stage>.entry` the function in it (`<stage>Main` when not given: of a hit group's, its own stage's, `closestHitMain`).

| Pipeline | Stages |
|---|---|
| ComputePipeline | `compute` |
| GraphicsPipeline, `stages` `eVertex` | `vertex` (with `vertex.layout`, when the shader reads vertices), `tessellationControl`, `tessellationEvaluation` (with `patchControlPoints`), `geometry`, `fragment` |
| GraphicsPipeline, `stages` `eMesh` | `task`, `mesh`, `fragment` |
| RayTracingPipeline | `raygen`, `miss[n]`, `hitGroup[n].closestHit`, `.anyHit`, `.intersection`, `callable[n]` |

A graphics pipeline's `stages` says which way it is drawn, and it has the inputs of that way only: an input
of the other is an error on the node. A Shader node wired to a pipeline still works, for graphs made before
this, but an editor doesn't offer one.

Every input says whether a node can do without it (`Port::optional`), what it may be when it is one of a
few names (`choices`), and what kind of thing it is beyond its type (`editor`: a colour, a shader module,
an entry point, a node's id, component names, an image parameter). An editor uses these to show a node's
few necessary inputs, offer the rest, and give each the right control. `PortsFor(type, node)` is a node
type's inputs for a node as it is set up.

And each says where it is set (`Port::place`, a `Placement`):

| Placement | What it is for | Examples |
|---|---|---|
| `eNode` | Read where it is: a control in the input's row on the node, while nothing is wired to it, and a port to wire all the same. | What Values, Math, Vectors, Cameras and Logic nodes work on; a Repeat's `count`; a View's `main`; a Clear's colour. |
| `eInspector` | What takes room to set: a list, text, a choice of many, a struct. | An Image's format and usage, a pipeline's shaders, a VertexLayout's attributes. |
| `eWire` | What only a node makes: wired, never typed. `Port::companion` names the node type that sets it up, for an editor to make one beside it. | A pipeline's `vertex.layout` and a Mesh's `vertexLayout` (companion: VertexLayout); a matrix (a MatrixMultiply's `a` and `b`); a Filter's `entities`. |

A node can move an input between the first two (`Node::placed`, saved as `"placed": { "a": "inspector" }`);
one wired only stays so. `PlacementOf(port, node)` is where one is set for a node. What works values out
starts as something it can work out: an Add made is 0 + 0, a Multiply 1 × 1.

A pipeline node's shaders decide two more groups of inputs, listed by `Runtime::PortsOf(id)` once it has
built:

- `param.<name>` for each resource the shaders declare. Those that are wired go in descriptor set 0,
  which the pipeline owns and binds once. Those left unwired go in set 1 and are returned by
  `PerEntityParameters(id)`: they are what each entity supplies (see Entities). Bindings written in the
  shader are ignored. The runtime numbers them.
- `constant.<name>` for each specialization constant.

A parameter wired to a resource of the wrong kind, or a name the shaders don't have, is an error on the
node that lists what the shaders do declare.

## Entities

A pipeline takes what is the same for every draw of it: a parameter wired on its node is bound once (set 0). What
it leaves unwired, and every push constant its shaders have, is each draw's own, and is an input of the **Draw**
(or the Dispatch, or the TraceRays) that runs it:

- `param.<name>` for each parameter the pipeline's node leaves unwired, a resource (set 1). A sampled image whose
  sampler the pipeline does not wire has `param.<name>.sampler` beside it.
- `push.<name>` for each push constant, by the name the shader gives it: a number, a vector, a matrix. One given
  nothing is the default its shader declares it with (`uniform float speed = 10.0`), or zero for one declared with none;
  and a new entity's component, of what its draws take from nobody else, starts as those defaults. See [What a shader
  says of its inputs](#what-a-shader-says-of-its-inputs).

Nothing is given by the engine behind a draw's back: a model matrix is a push constant like any other, wired.

A Draw wired from a node that is entities an element at a time — an **Entities** node, a **Filter** of one, a
**ForEach** — draws once for each of its elements, through any of its inputs, straight or through Math, Vectors and
the like, and each element's values are its own; one wired from none of them draws once. So a scene is drawn in three
nodes: every entity, those of them with a Mesh, and a draw of each, wired its mesh and where it is:

```json
{ "id": "all", "type": "Entities" },
{ "id": "drawn", "type": "Filter", "inputs": { "entities": { "$link": ["all", "out"] }, "with": ["Mesh", "Lit"] } },
{ "id": "draw", "type": "Draw", "inputs": { "pipeline": { "$link": ["placed", "out"] },
    "mesh": { "$link": ["drawn", "Mesh.mesh"] }, "push.model": { "$link": ["drawn", "matrix"] },
    "push.normalMatrix": { "$link": ["drawn", "normalMatrix"] }, "push.entityId": { "$link": ["drawn", "entityId"] } } }
```

**Entities** is every entity in the world: where each is (`matrix`, `normalMatrix`, `worldPosition`, `forward`, `up`,
`right`, `position`, `rotation`, `scale`), which it is (`entityId`), `index` and `count`. Which entities they are is
found again only when something they are found from changes — an entity made or destroyed, a component given, taken
away or switched, a name or a place in the hierarchy, the list a Filter keeps some of, what its `groupBy` holds
(`kecs::World::StructureVersion`, `ChangesOf`): a frame of thousands that only move finds nobody again. A **Filter** keeps those of
the entities wired to it — an Entities node's, or another Filter's — that have every component of `with` and none of
`without`, and gives the same, and each `with` component's fields as `<Component>.<field>` (`Mesh.mesh`): a field that
holds a resource — a mesh, a picture — as a resource. A component switched off on an entity (`world.SetEnabled`) does
not count as had. A **ForEach** is the two at once, of one component (its fields as `field.<name>`). An **Entity** node
is one entity, and no loop: the first with a `component`, or the one `entity` names — what a camera, a sky or a sun
is read through. The entity that is picked is a **Selected**
node's `entityId`, for an outline pass's `push.selected`.

What a draw takes from no other node is the pipeline's **component**, which the runtime makes in the world, named
after the pipeline's node: the material each entity holds. It has a field for each `param.` and `push.` its draws
are given nothing for, or are wired from that component's field of the same name (a Filter's `<pipeline>.<name>`, a
ForEach's `field.<name>`); one given on a Draw, or wired from anything else — `push.model` from a `matrix` — is no field
of it. A resource is a shared
reference (`Image`, `Buffer`, `Sampler`, `BufferView`, `AccelerationStructure`); a push constant is `f32`, `i32`,
`u32`, `vec2` to `vec4`, `ivec`, `uvec`, `mat4`, or `bytes<N>` for anything else. A pipeline whose draws take all
of it from elsewhere has none. So a lit pass is a Filter of the entities with a Mesh and the pipeline's component:

```json
{ "id": "solids", "type": "Filter", "inputs": { "entities": { "$link": ["all", "out"] }, "with": ["Mesh", "lit"] } },
{ "id": "draw scene", "type": "Draw", "inputs": { "pipeline": { "$link": ["lit", "out"] },
    "mesh": { "$link": ["solids", "Mesh.mesh"] }, "push.model": { "$link": ["solids", "matrix"] },
    "param.albedoMap": { "$link": ["solids", "lit.albedoMap"] }, "push.albedo": { "$link": ["solids", "lit.albedo"] } } }
```

and `lit` has `albedoMap` and `albedo`, and no `model`. Wiring a parameter on the pipeline takes it off its draws
(`Document::Connect` does), and off the component: the entities keep the rest (`MigrationOf`).

`runtime.ComponentOf(id)` returns the component, or `NoComponent` when there is none. `<kgraph/entities.h>` is how
code fills one:

```cpp
const kecs::ComponentId blur = runtime.ComponentOf("blur");
world.Add(entity, blur);
kgraph::Parameters(world, entity, blur)
    .Set("target", kgraph::MakeResource(std::move(image)))   // a resource made in code
    .Set("source", runtime.ResourceOf("backdrop"))           // or one a node makes
    .Set("radius", 4.f);
world.Add<kgraph::Mesh>(entity, { kgraph::MakeResource(std::move(mesh)) });   // what a ForEach of Mesh gives a Draw
```

Resources are shared and counted: an image lives while a node or a component holds it.

### The scene

What a scene is made of besides meshes is no part of the graph: the engine's **package** (`packages/koral`,
`kscript::PackageDirectory()`, or where `KORAL_ENGINE_PACKAGE` says) has it as scripts, data and nothing else, in
C# and in Kotlin alike. Every document declares them from their sources (`kscript::DeclarePackage`) before anything
runs, so an entity can be given one and a file can name one in any project, scripts running or not; a host
compiles them with the project's own, so a script reads a light as it reads its own scripts' components:

| Component | Fields, and what each starts as |
|---|---|
| `Camera` (a kind) | `near` (0.1), `far` (100): what every camera has. No entity has one itself; an Entity node or a ForEach of `Camera` finds each that has one of the two below, these fields read from it. |
| `PerspectiveCamera` | A Camera's, and `fieldOfView` (60, degrees, top to bottom). |
| `OrthographicCamera` | A Camera's, and `height` (10): how much it sees, top to bottom, in a box. |
| `Light` (a kind) | `color` (1, 1, 1), `intensity` (1): what every light has, found as a Camera is. |
| `DirectionalLight` | A Light's. It shines along its entity's forward (-Z). |
| `LocalLight` (a kind) | A Light's, and `range` (5): what a point and a spot have. |
| `PointLight` | A LocalLight's. |
| `SpotLight` | A LocalLight's, and `innerAngle` (20), `outerAngle` (30, degrees, from the middle of the cone). |
| `Sky` | `zenith`, `horizon`, `ground` (colours), `ambient` (0.35), `sunSize` (1.5, degrees). |
| `Instancer` | `count` (100000), `extent` (100, 100), `seed` (1), `scale` (0.5, 1.5), `jitter` (1), `pointDistance` (60), `drawDistance` (100000), `color` (points'). Its entity's Mesh, that many times, made and drawn on the GPU: see [Many of one mesh, on the GPU](#many-of-one-mesh-on-the-gpu). |

Where a camera or a light is, and which way it faces, is its entity's transform. Nothing in the graph knows any of
them by name: a project's script of its own is read the same way, and a component edited in the package is a
different one everywhere.

What they hold is there as values through an **Entity** node of the component — the one camera, sky or sun — or a
**ForEach** of it, every light (see [A shader's buffer, from the world](#a-shaders-buffer-from-the-world)), and through
these, which work out what is not held:

| Node | What it gives |
|---|---|
| **View** | A view matrix, as glm's `lookAt` makes one: from `position`, towards `target` (a step along -z, when not given), with `up` as up ((0, 1, 0), when not given). Gives `view` (and `out`), `inverseView` (matrices: sixteen numbers, column after column), `position`, `forward`, `up` and `right`. |
| **Perspective** | `fieldOfView` (degrees, top to bottom), `aspect` (width over height: a Screen's `aspect`), `near`, `far` (0 for no far plane); `reversedZ` for depth 1 at the near plane and 0 at the far one. |
| **Orthographic** | A box: `left`, `right`, `bottom`, `top`, `near`, `far` — or, those four not given, `size` high and as wide as that times the aspect. |
| **Frustum** | A perspective off the middle of the picture, as glm's `frustum`: the near plane's `left`, `right`, `bottom`, `top`, and `near`, `far`. |
| **Oblique** | A `projection` whose near plane is moved to a `plane` (a, b, c, d) as it is seen: a TransformPlane of the world's plane by the View's matrix. What is behind a mirror or under water is left out; x and y are as they were. |
| **Paraboloid** | Half of all round a point, for a dual-paraboloid map. No matrix can bend a picture so: it gives a shader `params` (near, far, 1 / (far - near), 1 for the front or -1 for the back), and the `turn` that faces a view that half: a MatrixMultiply of it and the View's matrix. |
| **CubeFace** | One `face` (0 to 5) of a cube round a point: its `direction` (+x, -x, +y, -y, +z, -z) and its `up`, as a cube map's layers have them. |
| **ColorConvert** | A colour as picked (`eSrgb`, or `eGamma22` as the power 2.2), in the linear terms light adds up in — or any of the other ways it says: see [Node types](#node-types). |
| **Jitter** | A place within a pixel for each `frame` (a Time's), of a Halton (2, 3) sequence `phases` long (8): `pixels` (-0.5 to 0.5 each way), `clip` (2 * pixels / `size`), and `jittered`, a `projection` moved by it in clip space — what a temporal antialiasing draws with. |
| **Collect** | `out`: a value worked out once an element of a ForEach, every element's one after another, as one list; `count`. |
| **ShadowCascades** | A light's shadow maps, fitted to what a camera sees: `matrices` (four, one after another), `splits` (how far from the camera each reaches), `worldTexel`, `tileTexel`, `direction`. Inputs: the camera's `view` and `projection` (where its near and far planes are is read from it), `direction` (the way the light travels: a DirectionalLight's entity's `forward`), `resolution` of one cascade, `distance`, `lambda`, and `boundsMin` and `boundsMax` of what casts. |
| **PointShadows** | See [Point lights' shadows](#point-lights-shadows-and-passes-that-need-not-run-every-frame). |

Each lens gives `projection` (and `out`) and `inverseProjection`, with depth 0 at its near plane and 1 at its far
one and Y pointing down, as the device has it — `flipY` false for one as glm makes it. A lens and a View know nothing
of each other: the two as one is a **MatrixMultiply** of the lens (`a`) and the View's matrix (`b`) — the projection
after the view — and what undoes it, a MatrixInverse of that. A Perspective gives the `fieldOfView`, `aspect`, `near`
and `far` it went by; an Orthographic and a Frustum their `near` and `far`.

None of them knows of a camera or a light: they are numbers, and whatever holds the numbers is wired in. A camera is a
View from where its entity is towards a step along the way it faces, up as its up, and a Perspective of its
component's fields:

```json
{ "id": "eye", "type": "Entity", "inputs": { "component": "PerspectiveCamera" } },
{ "id": "looking at", "type": "Add", "inputs": { "a": { "$link": ["eye", "worldPosition"] }, "b": { "$link": ["eye", "forward"] } } },
{ "id": "camera", "type": "View", "inputs": { "main": true, "position": { "$link": ["eye", "worldPosition"] },
    "target": { "$link": ["looking at", "out"] }, "up": { "$link": ["eye", "up"] } } },
{ "id": "lens", "type": "Perspective", "inputs": { "main": true, "aspect": { "$link": ["screen", "aspect"] },
    "fieldOfView": { "$link": ["eye", "field.fieldOfView"] }, "near": { "$link": ["eye", "field.near"] }, "far": { "$link": ["eye", "field.far"] } } },
{ "id": "seen", "type": "MatrixMultiply", "inputs": { "a": { "$link": ["lens", "out"] }, "b": { "$link": ["camera", "out"] } } }
```

A spot's is a Perspective twice its `outerAngle` wide, as far as its `range`; a point light's face, a CubeFace of a
ForEach's `turn` (with `repeat` 6) — the View's target its place and the face's direction added, its up the face's
up — and a Perspective of 90 degrees, square; a sun's, an Orthographic, the View back along the way it shines. A
script's own camera component, with fields of its own, is wired the same way.

The **main** View and the main lens (`main` true on each) are the scene's view. Whoever runs the graph can put its own
in place of them (`Runtime::SetView`): the main View's matrix is then that, and the main Perspective has its field of
view, near and far — or, given an `orthographic` height, is a box that tall, its depth the same way round as the
lens's (an effect that works a position out of depth as a perspective would then sees it wrong). Entities it is
told to leave out (`Runtime::SetHidden`) are in no list of entities with a Mesh, in that runtime alone: what lights
them and the world are as they were. That is how an
editor looks at a scene from where it likes; and what the editor's gizmo, `Runtime::ViewProjection()`, `CurrentView()`
and a DebugDraw node go by is the main View's and its lens's. A View that is not main is left alone.

The same graph can be seen from two places at once by a second runtime that follows the first:
`Runtime(frameGraph, world, source)` runs whatever it is `Set` (`source.Current()`, whenever `source.Revision()`
goes up) over the same world, into its own frame graph's screen, seen from where its own `SetView` says. Each
pass and each node's resource is its own — a picture the size of its screen, what is worked out from where it is
seen — but what is in files, the default meshes and pictures and the meshes made with `MakeMesh` are the
source's, so nothing is read or made twice. The editor's Game panel is one: the scene's camera, beside the
editor's own view.

These values are wired like any others: into Math and Vectors nodes (`Append`, `Slice` — `count` numbers of a list from its `first` —, `MatrixMultiply` and
`MatrixInverse` among them) and into a **BufferBuilder** of `usage` `eUniform`, laid out as a shader's block is
(std140: a number in four bytes, a vector of three or four aligned to sixteen, so a `float3` followed by a `float`
share one `float4`). So a shader's

```slang
struct FrameData { float4x4 viewProjection; float4 cameraPosition; float3 skyZenith; float ambient; };
```

is a BufferBuilder laid out `viewProjection` eMat4, `cameraPosition` eVec4, `skyZenith` eVec3 and `ambient` eFloat, with
`seen`, `camera.position` (three numbers: the fourth is 0), `sky.zenith` and `sky.ambient` wired to them. See
[A buffer laid out on its node](#a-buffer-laid-out-on-its-node).

A node that reads the world, every value worked out from one, and every buffer one reaches are brought
up to date each frame without anything being built again: the value changes where it is, and the buffer is
written. Anything else wired to such a value (an image's extent, a pipeline's constant) has it as it was
when the graph was last set.

### A shader's buffer, from the world

A DataBuffer is laid out by hand, a field after a field. A **ShaderBuffer** is laid out by the shader: it names
a `module`, an `entry` point of it and the `parameter` the shader calls the buffer, and has an input for each
field of the buffer's struct, `field.<name>`, typed and placed as the compiled shader has it. For

```slang
struct Light { float3 position; float range; float4 color; uint kind; float2 cone; };
StructuredBuffer<Light> lights;
```

a node with `"module": "lit", "entry": "fragment", "parameter": "lights"` has `field.position`, `field.range`,
`field.color`, `field.kind` and `field.cone`. `Runtime::PortsOf(id)` lists them once it is built. A struct in the
struct is `field.material.albedo`; a fixed array of numbers is one input taking them all. A field not given is
zero. A value with fewer numbers than the field fills the first of them.

A **ForEach** node is every entity that has a `component` (and those of `with`, and none of `without`), in the
hierarchy's order, an element at a time — with `where` (`{ "field.kind": 2 }`, a Filter's too), only those whose fields
hold what it says, found again when those components change. It gives `field.<name>` for each number or vector field of the component
(`Runtime::OutputsOf(id)`), and where the entity is: `matrix`, `worldPosition`, `forward`, `up`, `right`, `position`,
`rotation`, `scale`. It gives no struct of them: each is wired where it goes. Also `count` and `entityId`, and — asked
for, where the order matters more than the entity does — `index` (the element, from 0). With `repeat` (1 to 256) each
entity is that many elements one after another — six for a point light's faces — and `turn` says which of them,
`entityIndex` which entity and `entities` how many there are. It is every one of them: one is an Entity node's.

The editor draws a loop as a node that holds its nodes: a ForEach, a Filter (and the Entities node it keeps some of,
when that is its alone) or a Repeat, with every node worked out once an element of it, the buffers they fill and the
Draws and Dispatches that draw each (`Runtime::EachOf(id)` says which a node follows). One loop inside another is
drawn inside it.

```json
{ "id": "lamps", "type": "ForEach", "inputs": { "component": "PointLight" } },
{ "id": "tinted", "type": "Multiply", "inputs": { "a": { "$link": ["lamps", "field.color"] }, "b": { "$link": ["lamps", "field.intensity"] } } },
{ "id": "light data", "type": "ShaderBuffer", "inputs": {
    "module": "lit", "entry": "fragment", "parameter": "lights",
    "field.position": { "$link": ["lamps", "worldPosition"] }, "field.range": { "$link": ["lamps", "field.range"] },
    "field.color": { "$link": ["tinted", "out"] }, "field.kind": 1 } }
```

How many elements the buffer has depends on what it is and what its fields are wired to:

| The shader declares | Fields wired from a ForEach | Elements |
|---|---|---|
| `StructuredBuffer<T>`, `T name[]` | yes, straight or through Math and Vectors nodes | One for each element. `count` says how many. |
| `StructuredBuffer<T>`, `T name[]` | no | One. |
| `ConstantBuffer<T>`, a uniform block | either | One: the first entity's. |

So a camera's block is a ShaderBuffer whose parameter is a `ConstantBuffer`, with fields wired from a View and a
Perspective of a ForEach of `PerspectiveCamera`. Anything else wired to a ForEach has the first element's values.

An Entity node is one element: no loop. A buffer wired from several of those — a camera, a sky and a sun — is one
element, as it is of values that follow no
ForEach; its `present` says whether it found one.

A field that is the same for every element is given on the node (`field.kind` above). Fields wired from two
ForEach nodes are an error: an element is one of a ForEach's. For several kinds of entity in one buffer, a second
ShaderBuffer of the same struct takes the first on its `after` input: its elements come after the first one's,
and its `out` and `count` are of both.

The buffer is written each frame its elements change. It has room for the next power of two of elements, and is
built again, with the pipeline that reads it, only when there are more than that. Its shader is watched: edit the
struct and the node's fields follow. An input for a field the struct no longer has is an error on the node that
lists the fields it does have.

Values are as the component holds them: a colour is as it was picked, not converted to linear (a ColorConvert does
that), and an angle is in degrees. Pow, Radians and Cos nodes between the two do that for each entity.

### Bundles

A **Bundle** is many values carried on one wire, each by its name. A ForEach, an Entity, an Entities node and a Filter
give each element as one, `element`: its component's fields and `with`'s, by their own names (its component's first),
and where it is — `matrix`, `normalMatrix`, `worldPosition`, `forward`, `up`, `right`, `position`, `rotation`,
`orientation`, `scale`, `entityId`. A **Bundle** node is one of what it is given: an input for each of its `names`,
`field.<name>`, a value or a wire, made on `base` (another Bundle, whose members of the same names its own replace).
An **Unbundle** takes one apart: an output for each member, by its name. A function's input or output may carry one.

A ShaderBuffer's or a BufferBuilder's `fields`, and a Draw's, a Dispatch's or a TraceRays' `push`, take a Bundle and fill
each `field.<name>` (or `push.<name>`) of the same name — names match exactly, nothing more:

```json
{ "id": "lamps", "type": "ForEach", "inputs": { "component": "PointLight" } },
{ "id": "light data", "type": "ShaderBuffer", "inputs": { "module": "lit", "entry": "fragment", "parameter": "lights",
    "fields": { "$link": ["lamps", "element"] }, "field.color": { "$link": ["tinted", "out"] }, "field.kind": 1 } }
```

is a buffer of the lights whose `position` and `range` are each lamp's, and whose `color` is wired by itself: an input
given by itself, a wire or a value, takes its own. What matches nothing, on either side, is said on the node
(`Status(id)->note`): the bundle's `intensity`, which the struct has not; the layout's `glow`, which the bundle has not.
Wired from what is no Bundle, `fields` is an error on the node.

Nothing new is worked out. Before the graph runs, after its calls are written out and before its loops are, each
Bundle is written out into the wires it stands for (`Runtime::BundleOf(id, output)` says what one holds), so a buffer
filled from an `element` is filled once an element, as one wired a field at a time is.

### A buffer laid out on its node

A **BufferBuilder** is a buffer of elements whose fields are said on the node, not read from a shader: its `layout` is
a list of them in order, each `{ "name": "position", "type": "eVec3" }`, with `"count"` for a fixed array of one. The
types are `eFloat`, `eInt`, `eUInt`, `eBool`, `eVec2` to `eVec4`, `eIVec2` to `eIVec4`, `eUVec2` to `eUVec4`, `eMat3`
and `eMat4`. Each is an input, `field.<name>`, wired or given like a ShaderBuffer's.

`usage` says how it is packed, as a shader's struct of the same fields is: `eStorage` (the default) is a
`StructuredBuffer`, by std430 — a number on four bytes, a vector of two on eight, of three or four on sixteen, a
matrix's columns on sixteen — and `eUniform` a `ConstantBuffer`, by std140, where every array element and the struct
are on sixteen as well. `stride` is an element's size, `bytes` the whole buffer's.

How many elements: one for each of the ForEach its fields follow, or one with none. `count` given,
it has room for that many: of a ForEach's elements, the first that many; with no ForEach, that many of the same.
`count` out is how many it holds, and `after` puts another BufferBuilder's elements of the same layout first.

```json
{ "id": "camera data", "type": "BufferBuilder", "inputs": { "usage": "eUniform",
    "layout": [ { "name": "viewProjection", "type": "eMat4" }, { "name": "position", "type": "eVec3" }, { "name": "far", "type": "eFloat" } ],
    "field.viewProjection": { "$link": ["seen", "out"] }, "field.position": { "$link": ["camera", "position"] },
    "field.far": { "$link": ["lens", "far"] } } },
{ "id": "spots", "type": "ForEach", "inputs": { "component": "SpotLight" } },
{ "id": "spot data", "type": "BufferBuilder", "inputs": {
    "layout": [ { "name": "position", "type": "eVec3" }, { "name": "range", "type": "eFloat" }, { "name": "color", "type": "eVec3" } ],
    "field.position": { "$link": ["spots", "worldPosition"] }, "field.range": { "$link": ["spots", "field.range"] },
    "field.color": { "$link": ["spots", "field.color"] } } }
```

is a camera's block — of the View and the Perspective of [The scene](#the-scene) — and the spots' array, an element
a spot. The editor's inspector edits a layout a line a field.

### A component's buffer

A **ComponentBuffer** is a buffer of every entity that has its `component`, an element each, laid out as the component
is. It is written out, before anything runs, as a ForEach of the component (`<id>/each`) and the node itself a
BufferBuilder whose `layout` is the fields it holds, each wired from the ForEach's output of it. `with`, `without` and
`where` are the ForEach's; `usage`, `count` and `after` the BufferBuilder's, and so are its outputs.

`fields` is which it holds, by name: the component's numbers, vectors and matrices (a string or an asset is no field
of a buffer), and where the entity is — `worldPosition`, `forward`, `up`, `right`, `position`, `rotation`, `scale`,
`orientation`, `matrix`, `normalMatrix`, `entityId`. Not given, it holds every field of the component, then
`worldPosition`, `forward` and `matrix`. However they are ticked, they are laid out the component's first, as it
declares them, then where the entity is in that order: the shader's struct is written so. A name the component has
not is left out and said on the node. The inspector ticks them; another component chosen starts them again.

A field wired by itself, `field.<name>`, takes its wire instead of the entity's: a camera's `viewProjection` worked
out by a lens, in a buffer of the rest of the Camera. One wired that is not ticked is left out, and said.

```json
{ "id": "spot data", "type": "ComponentBuffer", "inputs": { "component": "SpotLight",
    "fields": [ "color", "range", "outerAngle", "worldPosition", "forward" ] } }
```

is the spots, as `struct Spot { float3 color; float range; float outerAngle; float3 worldPosition; float3 forward; }`.

A light's camera is a View and a lens like the scene's, worked out like any value: wired from a ForEach of lights,
once an element — so a buffer of the lights' cameras is made as a buffer of the scene's camera is.

```json
{ "id": "lamps", "type": "ForEach", "inputs": { "component": "PointLight", "repeat": 6 } },
{ "id": "face", "type": "CubeFace", "inputs": { "face": { "$link": ["lamps", "turn"] } } },
{ "id": "face target", "type": "Add", "inputs": { "a": { "$link": ["lamps", "worldPosition"] }, "b": { "$link": ["face", "direction"] } } },
{ "id": "face view", "type": "View", "inputs": { "position": { "$link": ["lamps", "worldPosition"] },
    "target": { "$link": ["face target", "out"] }, "up": { "$link": ["face", "up"] } } },
{ "id": "face lens", "type": "Perspective", "inputs": { "fieldOfView": 90, "near": 0.05, "far": { "$link": ["lamps", "field.range"] } } },
{ "id": "face seen", "type": "MatrixMultiply", "inputs": { "a": { "$link": ["face lens", "out"] }, "b": { "$link": ["face view", "out"] } } },
{ "id": "faces", "type": "BufferBuilder", "inputs": { "layout": [ { "name": "viewProjection", "type": "eMat4" } ],
    "field.viewProjection": { "$link": ["face seen", "out"] } } }
```

is six cameras a point light, one after another: a buffer a shadow pass reads its matrices from.

A paraboloid is bent in the vertex shader, from the view a Paraboloid's `turn` faces (`view` below) and its `params`:

```slang
float4 Paraboloid(float3 world, float4x4 view, float4 params)   // params: near, far, 1 / (far - near), side
{
    float3 seen = mul(float4(world, 1.0), view).xyz;
    float distance = length(seen);
    float3 towards = seen / distance;
    towards.z = -towards.z;                                     // the half it faces looks down -z
    return float4(towards.xy / (1.0 + towards.z), (distance - params.x) * params.z, 1.0);
}
```

Two pass nodes go with it, for a graph that wants them — an editor has both of its own, and needs neither of a
scene's (see [the editor](editor.md)). **Pick** takes an image of ids — one a pipeline draws `entityId` into — and
`Runtime::Pick(u, v, then)` reads one pixel of it back, a few frames later, as the entity there. Given its `depth`
too — the depth image the ids were drawn with, with eTransferSrc — `Runtime::PickAt(u, v, then)` reads the depth of
that pixel and the eight round it, with the view as it was (`PickHit`): where on a surface the pixel is, and which way
the surface faces.
**DebugDraw** draws the scene's `kor::DebugDraw` shapes over an image, seen as the graph's main View sees
(`Runtime::SetDebugDraw`).

### Files

A picture or a mesh that is in a file is a resource like any a node makes, found by where it is instead of by a
node, read the first time it is asked for, and saved by that name:

| `Runtime::ResourceOf(...)` | What it is |
|---|---|
| `@textures/wall.png` | That picture, with its smaller levels made. Any format the framework's image importer reads, `.hdr` included. |
| `@sponza/Sponza.gltf#3` | The fourth mesh of that model, its attributes as the file has them (see below). |
| `$white`, `$black`, `$normal` | A picture of one texel. |

A path is the file's own, or one under `Runtime::AssetDirectory()` — a project's `assets` folder, which a document
sets from where its scene is. `Runtime::AssetId(file, mesh)` gives the name for a file, and
`Runtime::ModelOf(file)` what a model holds besides meshes (`<kgraph/assets.h>`): its things and where each is,
its materials and their pictures, its lights. So an entity's own pictures are fields of its pipeline's component
— parameters the node leaves unwired — holding `@...` names, and need no nodes. An **ImageFile** node is for a
picture the graph itself uses: a sky.

A file is read off the frame. What is asked for is there at once as a stand-in — a white texel for a picture,
nothing yet for a mesh — and whoever holds it is led to the real thing when it arrives, as an entity is led from
a node's resource to the one the node made in its place. So a scene of many files opens at once and fills in:
an entity whose mesh is still being read is passed by, and says so in its pass's report. `Runtime::Reading()`
counts the files still being read, `Unread()` says why any could not be, and `ModelOf(file)` is null until the
model is read. A model's meshes are one mesh on the device, each a run of its indices (`Resource::part`): a
pass that draws a hundred of them binds one.

`keditor::Document::ImportModel(file)` makes entities of a model with these: see [the editor](editor.md).

### Default meshes

An entity's `kgraph::Mesh` can hold a procedural mesh without the graph having a node for it:
`runtime.ResourceOf("$cube")`, and likewise each of `kgraph::DefaultMeshNames()` (`kgraph::DefaultMeshId(name)`).
Each shape is made from parameters of its own (`kgraph::ShapeParameters`), and its id says the ones that are not
at their initial value: `$sphere?radius=0.8&rings=24`. One set of numbers has one id (`kgraph::ShapeName`: in the
parameters' order, each within its limits, a length to four decimals), so entities with the same numbers hold the one
mesh, and are batched as such. A tuned mesh nothing holds is let go of; `$sphere` as it starts is kept. An old scene's
`$sphere` is the same mesh it always was.

At its initial values each is about a unit across and centred on the origin:

| Mesh | What it is |
|---|---|
| `$cube`, `$sphere`, `$plane` (facing up), `$cylinder`, `$cone`, `$capsule`, `$torus`, `$quad` (facing +z), `$triangle` | The Kotlin engine project's shapes, as it makes them. |
| `$icosphere` | A sphere of 1280 even triangles: no bunching at the poles, for what is displaced. |
| `$grid` | The plane cut 64 by 64: for water, ground, cloth — what is moved vertex by vertex. |
| `$insideCube`, `$insideSphere` | The cube and the sphere facing in, their pictures the right way round from inside: a sky box, a sky dome, a room. |
| `$roundedBox` | A cube with its edges and corners rounded, 0.1 round. |
| `$wedge` | A ramp: its back a unit tall at -Z, sloping to nothing at +Z. |
| `$pyramid`, `$hemisphere` (from -0.25 to 0.25, flat side down), `$pipe` (a tube, its wall 0.15 thick) | |
| `$arch`, `$stairs` | Blocking out a level: an arch half a unit deep, 0.6 wide through; five stairs climbing towards -Z. |
| `$teapot`, `$knot` | To try shading on: a teapot made of turned and swept shapes (not the Utah patches), a trefoil knot. |
| `$arrow` | Up +Y, a shaft and a cone. `$axes`: arrows along +X, +Y, +Z from the origin, a ball where they meet. |
| `$skinnedCylinder` | Of three bones (at its foot, middle and top), with `BONE_IDS` and `BONE_WEIGHTS`: laid out apart from the rest, so drawn by a Draw of its own. |
 Each is made the first time it is
asked for, and is saved by that id. They are the Primitive node's shapes, laid out the same way, so a
pipeline that takes a Primitive's `layout` draws them. `<kgraph/meshes.h>` also gives the shapes as plain
numbers (`ShapeData`), for code that wants the vertices.

**Their tangents.** Besides POSITION, NORMAL and UV they have the way their picture's u grows along the surface
(the tangent) and the way v does (the bitangent), for a normal map to be turned by, after the normal in one
buffer, as `Graph::defaultMeshTangents` says (saved with the graph as `"defaultMeshTangents"` when it is not the
first):

| `Tangents` | Saved as | Attributes | Bytes a vertex |
|---|---|---|---|
| `eFour` (the default) | `tangent` | POSITION, NORMAL, `TANGENT` (float4: xyz the tangent, w ±1), UV — bitangent = cross(normal, tangent.xyz) · w, as glTF has it | 48 |
| `eWithBitangent` | `tangentBitangent` | POSITION, NORMAL, `TANGENT` (float3), `BITANGENT` (float3), UV — as a model file's meshes are read, so that the two pack together | 56 |
| `eNone` | `none` | POSITION, NORMAL, UV | 32 |

Changed, the default meshes made are made again and what held them holds the new (`Resource::replacedBy`), and a
Primitive is built again. `kgraph::AddTangents(data, form)` works them out for any `VertexData` with a NORMAL and a
UV (per triangle, summed at each vertex, then made at right angles to the normal). Meshes drawn by one Draw are laid
out alike: a script's mesh drawn with default ones has the same attributes in the same order.

### Meshes of any attributes

A mesh's vertices are any attributes, each found by its semantic: what a shader names its input
(`float2 uv : UV`). `kgraph::VertexData` is a mesh as numbers — its attributes (`VertexAttribute`: a semantic,
float, int or uint, one to four of them a vertex), its indices, and its **buffers**: which vertex buffer each
attribute is in (`{ { "POSITION", "UV" }, { "NORMAL", "TANGENT", "BITANGENT" }, { "BONE_IDS", "BONE_WEIGHTS" } }`),
side by side in each vertex, with what none names in one more after them. Empty, all are in one.

**A model's meshes** are read with every attribute they have: `POSITION`, `NORMAL`, `TANGENT`, `BITANGENT`, each
set of UVs (`UV`, then `UV1`, ...) and of colours (`COLOR`, `COLOR1`, ...), `BONE_IDS` (uint4) and
`BONE_WEIGHTS`. `Graph::meshImports` says otherwise, file by file — by the file as its meshes' ids have it
(`sponza.gltf` of `@sponza.gltf#3`): `leaveOut` names attributes not to read (they are not copied, and nothing
of them is held; POSITION is always read), and `buffers` how the rest are put in buffers. It is saved with the
graph. Changed, the file is read again — after a read under way finishes, when one is — and everything that held
its meshes holds the new ones (`Resource::replacedBy`). A model's parts with the same attributes in the same
buffers are one mesh on the device, each a run of its indices; parts that differ are in one of their own.
`Model::attributes` lists what the file has, read or not.

**A mesh made in code**: `runtime.MakeMesh(id, data)` makes one of any attributes, found by `id` from then on —
a script's are `$script/<name>` — replacing one there was. `runtime.ReadMesh(id)` reads any mesh back from the
device as `VertexData`: every attribute, in its buffers, and its indices (a model's part, only its own).

### Dispatch and Draw

A Dispatch, a TraceRays, a Keep, a CopyBuffer and a ClearBuffer take `idle`: while what they are given is not zero (or
is true), they record nothing that frame — given a number, always or never; wired from a value worked out each frame (a
FixedStep's `steps` compared with 0, a count), on the frames it says. A Draw's `idle` is a number for each turn of its
`cullIndex` (see [Point lights' shadows](#point-lights-shadows-and-passes-that-need-not-run-every-frame)).

A pass nothing that runs reads rests by itself, with no `idle` of its own: when every pass that reads what it writes is
resting that frame — idle, or resting so in its turn — it records nothing, and what it made is left as it was until one
reads it again. Forward+'s shadows rest so while its rays are traced, however they are made: a Function's passes as
much as a page's. Kept running are a pass that writes the screen, memory the CPU can read, something no other pass
reads (what the graph is for: whoever runs it reads it), or nothing at all; a setup or fixed page's, whose state goes on
from one run to the next; and what a watch copies, on the frames between its copies too. A Draw `whenChanged` rested so is not taken as drawn: it draws once read again. The frame's `leftOut` names
each, "resting".

A **MeshBuffers** gives a mesh's buffers as buffers, for a compute shader to read its triangles: `vertices` (the buffer
its positions are in), `indices` (32 bits each), `stride` and `position` (where in a vertex its position is, in bytes),
`vertexCount`, `indexCount`, and a model's mesh's `firstIndex` and `vertexOffset` within the buffers its model shares.
Every mesh made of numbers — a default one, a Primitive's, a model's, a script's, a MeshPack's — has its buffers with
`eStorage`.

**Dispatch** runs a compute pipeline, once or once an element of the ForEach its inputs are wired from. `groups` (or `groups.x/y/z`) is how many groups
to run, rounded up: an image's `extent` divided by the pipeline's `numThreads`, wired straight in, covers
the image. Or `cover` names one of the pipeline's image parameters and the pass works the count out itself,
from `groupSize`, which is the pipeline's `numThreads` unless the node says otherwise. With an image of each
element's own (`param.<name>` wired from a ForEach), each gets its own count.

A Dispatch can also read its group counts from a buffer a pass before it wrote: wire it to `indirect`
(the buffer needs `eIndirect`), with `indirectOffset` for where in it.

**One Dispatch of every entity.** Wired to a Filter's entities (`entities`), a Dispatch runs its `groups` once for each
of them in one dispatch, one after another along z: an element is `SV_GroupID.z / groups.z`, in the Filter's order.
What each is to the shader is its `[entity("PER_INSTANCE")]` buffer's, as a batched Draw's — where it is, its own fields,
and the images it holds by their bindless indices: read, `[entity("TEXTURE")]`, or written, `[entity("STORAGE")]`
(`kor_RWTexture2D[index]`). Every element runs as many groups as `groups` says, so where their images differ in size it
is as many as the largest takes, and the shader looks at its own image's size. Nothing else of it can be each element's:
one wired so is an error that says which. More groups along z than a dispatch counts (65,535)
run as several dispatches, each from the group the last stopped at (`CommandBuffer::DispatchBase`), so that `SV_GroupID.z` is the whole grid's in each and the shader is none the wiser; past 32 bits of groups, they are not run, and its report says so. Ten thousand entities, an image each: a Dispatch each costs the CPU about 14 ms a
frame (readying 4.4, recording 9.6), one Dispatch of all of them 0.3 ms (Release,
`ScaleTest.OneDispatchOfTenThousandEntitiesCostsLess`), and stamps the same images
(`EntityTest.OneDispatchOfEveryEntityStampsWhatADispatchEachDoes`).

**TraceRays** runs a ray tracing pipeline the same way: `size` (or `size.x/y/z`) rays, or as many as
`cover` an image parameter. Wired to a Filter's entities (`entities`), its pipeline has a material as a Draw's has: a
buffer its shader marks `[entity("PER_INSTANCE")]` and nothing is wired to is an element for each of them, in the
Filter's order — the `InstanceID()` a RayScene of the same Filter gives each — filled from the pipeline's component
(see [Each instance's, said by the shader](#each-instances-said-by-the-shader)), which each entity holds as it holds a Draw's.
It gives that buffer as `instances`, as a Draw does, for a pass to read the materials too: Forward+ lists what gives off
light from it (an `emission` above nothing), and its rays are lit by that.

**Draw** draws with a graphics pipeline into a framebuffer: the pipeline's own, or the one wired to
`framebuffer`. What it draws is its `mesh`:

- **Each element's mesh,** wired from a Filter `with` Mesh (`Mesh.mesh`), or a ForEach of `Mesh` (`field.mesh`). A mesh
  is drawn when it has what the vertex shader reads, found by semantic in the shader's reflection: what else the
  mesh has, and in which buffers, is no matter. One laid out otherwise than the pipeline's `vertex.layout` is drawn
  with the same pipeline made again for its layout, the first time one of that layout is drawn, and kept while the
  node is.
- **One mesh,** wired from a node that makes one — a Primitive — or given as a default mesh's id.
- **No mesh.** A vertex shader that takes no vertex inputs makes its own vertices from `SV_VertexID`, and
  the pipeline needs no `vertex.layout`. `vertexCount` says how many, 3 when not given: a triangle. A vertex
  shader that reads vertices with no mesh wired is an error on the Draw.

`command` picks the draw:

| `command` | What it records | Inputs it reads |
|---|---|---|
| `eAuto` | Indexed when the mesh has indices, mesh tasks when the pipeline's `stages` is `eMesh`, plain vertices otherwise | as below |
| `eDraw` | `Draw` | `vertexCount`, `firstVertex`, `instances`, `firstInstance` |
| `eDrawIndexed` | `DrawIndexed` | the same (counting indices), and `vertexOffset` |
| `eDrawMeshTasks` | `DrawMeshTasks` | `tasks` (or `tasks.x/y/z`) |
| `eDrawIndirect`, `eDrawIndexedIndirect`, `eDrawMeshTasksIndirect` | the draws a buffer holds | `indirect`, `indirectOffset`, `drawCount`, `stride` |
| `eDrawIndirectCount`, `eDrawIndexedIndirectCount` | as many of them as another buffer says | those, and `count`, `countOffset` (`drawCount` is the most it runs) |

An indirect or count buffer needs `eIndirect` among its usage (a Buffer node's is made with it, wired so), and is a
use of the pass: the pass that writes it runs first.

### Many meshes in one indirect draw

Every entity of a few meshes can be drawn by one Draw: the meshes packed as one, the entities grouped by the mesh
they show, and a buffer of commands saying how many of each to draw, from where.

- **A Filter's `groupBy`** names one of its `with` components: the entities it keeps come in groups, each next to
  those whose component holds the same (the same mesh), each group where its first one was. Each element gives
  `group`, and the Filter `groups` (`{ first, count }` for each, in order) and `groupCount`. A BufferBuilder
  whose fields follow it writes each mesh's instances one after another.
- **MeshPack** takes the Filter's `Mesh.mesh` and packs the distinct meshes, in the order they come first: their
  vertices one after another in one buffer for each of the layout's, and their indices in another, each copied
  once on the device (a model's meshes are already one, so it is copied whole). It gives the `mesh`, its
  `layout`, each mesh's `ranges` (`firstIndex`, `indexCount`, `vertexOffset`, `boundsMin`, `boundsMax`), the
  same for a shader in `rangeBuffer` (48 bytes a mesh: `uint firstIndex, indexCount; int vertexOffset; uint;
  float4 boundsMin; float4 boundsMax`) and `count`. It is packed again when the meshes are other ones, or one of
  them arrives from its file. The meshes' vertices must be laid out alike, and drawn by 32-bit indices; it says
  so otherwise. With `positions` it packs only where their vertices are, three floats each, however each mesh lays them
  out — read back and packed on the CPU when it is built: for a compute shader that reads triangles (a mesh collider),
  not for drawing.
  With nothing to pack yet — a Filter that keeps no one, or meshes still being read — it is an empty mesh laid out as the
  default meshes are (POSITION, NORMAL, their tangents, UV), whose ranges draw nothing: with no meshes the IndirectDraws has a `drawCount` of 0, and the Draw clears
  and draws nothing, until there is something, when it is packed again.
- **IndirectDraws** takes the pack's `ranges` and the Filter's `groups` (grouped by Mesh, so the two are in one
  order) and writes a `VkDrawIndexedIndirectCommand` for each mesh into `commands`, with `drawCount` and
  `stride` (20). `mode` eCPU draws each group whole; eGPU writes every command with no instances each frame,
  and gives `visible` (a uint for every instance), for a compute pass of your own to count what it draws: it adds
  one to its mesh's `instanceCount` and writes the instance's number at the command's `firstInstance` plus what
  it added. Looked at (`Runtime::Watch`, the editor's *Look at what it holds*), its `out` is the commands, a line
  each — `indexCount`, `instanceCount`, `firstIndex`, `vertexOffset`, `firstInstance`: which meshes are batched, and
  how many of each.
- **CullInSight**, one of the engine's functions, is that compute pass for what is in sight: a thread an instance
  tests its mesh's box (the pack's `rangeBuffer`), placed by its matrix, against `viewProjection`, and counts what is in
  sight into the eGPU commands and `visible`. `instances` is the Draw's buffer of instances (its `instances` output),
  `stride` bytes an element (its `instanceStride`) with its `[entity("MATRIX")]` `matrixOffset` bytes in; how many there are it reads from
  `visible`'s size, on the GPU. The Draw is wired its `commands` as `indirect` and `visible` as a `param.`, and its shader
  finds its instance through that: `instances[visible[first + index]]`. A mesh whose box is not known is always drawn.
  (`ScaleTest.CullInSightDrawsOnlyWhatIsSeen`: of sixteen cubes, the eight in the picture are drawn, and one moved in or
  out is drawn or not the next frame.)

```json
{ "id": "meshes", "type": "Filter", "inputs": { "entities": { "$link": ["all", "out"] }, "with": ["Mesh", "lit"], "groupBy": "Mesh" } },
{ "id": "pack", "type": "MeshPack", "inputs": { "meshes": { "$link": ["meshes", "Mesh.mesh"] } } },
{ "id": "commands", "type": "IndirectDraws", "inputs": { "ranges": { "$link": ["pack", "ranges"] }, "groups": { "$link": ["meshes", "groups"] } } },
{ "id": "draw", "type": "Draw", "inputs": { "pipeline": { "$link": ["lit", "out"] }, "mesh": { "$link": ["pack", "mesh"] },
  "command": "eDrawIndexedIndirect", "indirect": { "$link": ["commands", "commands"] }, "drawCount": { "$link": ["commands", "drawCount"] },
  "param.instances": { "$link": ["instances", "out"] } } }
```

The vertex shader finds its instance at `SV_StartInstanceLocation + SV_InstanceID` in the buffer the Filter's
fields filled (with eGPU, through `visible` first). Textures that differ from one entity to another are not
handled yet: an indirect draw suits meshes that share their pictures, or use none.

#### Each instance's, said by the shader

The buffer of instances need not be wired at all: a shader that marks it `[entity("PER_INSTANCE")]` (the engine's
`entity` module, in `packages/koral/shaders`, on every scene's search path) has the Draw make it and fill it, an element
for each entity of the Filter whose `Mesh.mesh` its `mesh`'s MeshPack packs, in the order the commands draw them.

```slang
import entity;
struct Instance {
    [entity("MATRIX")] float4x4 model;      // where it is in the world
    float3 color;                           // its own
};
[entity("PER_INSTANCE")] StructuredBuffer<Instance> instances;
```

A field marked `MATRIX`, `NORMAL_MATRIX` (both `float4x4`), `POSITION` (`float3`) or `ID` (`uint`, one more than the
entity's index) is where the entity is, or which. One marked `TEXTURE` or `STORAGE` (`uint`) is an image the entity
holds — a field of the component, an Image — by its bindless index for reading, or for writing; ~0 for none. One
marked with a material kind's attribute — `[PbrMaterial("roughness")] float rough;`, or `[PbrMaterial] float roughness;`
for the field of the member's name (`import PbrMaterial;`, or `import koral_materials;` for every kind) — is that field
of the entity's material of that kind, a picture of it by its bindless index. Its type is the field's, but that a
colour's three fill a `float4` (w 1), a `float4`'s first three a `float3`, true or false a `uint`; anything else is
the pipeline's error, naming the member and the field. Every kind a shader names is one an entity must have to be
drawn by it: one without is laid out as no one's, and not drawn (`Runtime::MaterialsOf` lists them). Any other — a number, a vector or a `float4x4` — is the entity's own:
a field of the pipeline's component, as its unwired `param.`s and `push.`s are, shown in the inspector and set from a
script (`world.set(cube, "<pipeline>", "color", Vec3(1f, 0f, 0f))`). An entity without the component has zeros there.
The component is not one of the Filter's `with`: the pipeline's vertex layout comes from the pack, which comes from the
Filter. The buffer has room for at least 64, twice what there was; past that, the Draw is built again with more.
A `param.` wired on the Draw is used as it is, and then nothing of the buffer is the component's.
Draws that would write the same into their buffers share one, written once a frame: of the same Filter, an element
laid out alike — field by field, where each is and what it is — and, when any field is the entity's own, the same
pipeline's component. A shadow pass's turns share theirs; a pass whose element also has its entity's colour has its
own. A Draw gives its buffer as `instances`, and the bytes of an element of it as `instanceStride`; outgrown the buffer is made again for every Draw that shares it. It is
kept up to date whether the Draw draws or idles: another pass may read it while the Draw rests (Forward+'s meshlets
read the forward Draw's when the vertices are not drawn).

**Only what changed is written.** The buffer is laid out once, whole, into every copy of it a frame in flight reads.
From then on an element is laid out again only when another entity is in its place (the Filter's list changed), when
its entity moved — by itself, by what it is under, or by being put under something else (`kecs::World::MovedAt`) —
when its entity's own fields were written or it was given or rid of the pipeline's component (`ChangedTickOf`), or
when a picture it names was made again. Each copy is then written only the elements laid out again since it was last
written, in runs. A frame in which none of that happened looks at none of the elements, and a Draw `whenChanged`
compares a version of the buffer rather than what is in it. What is looked at and laid out is spread over every core,
but for what entities under another are placed by, which is worked out one at a time. A hundred thousand entities of
one mesh, in two buffers (a pass and its prepass), cost the runtime about 0.04 ms a frame when none of them moves, 0.5 ms
when a thousand do and 3.3 ms when all of them do (Release, `ScaleTest` in `test_scale.cpp`); it was 13 ms in every case
before. Nothing but the runtime writes the buffer: what else wrote into it would stay until its elements change.

#### Batched: a Draw of each element, as one

A Draw whose `mesh` is each element's — a Filter's `Mesh.mesh`, or a ForEach's, or a ForEach of Mesh's `field.mesh` —
draws once for each element. Given `batched`, it is written out before it runs as one indirect draw of all of them,
with no node in the graph for it: `<draw>/batch`, a Filter of the same entities grouped by mesh (of a ForEach, a
Filter of an Entities node, `<draw>/all`, with what the ForEach is of and its `with` and `without`); `<draw>/pack`, a
MeshPack of their meshes; and `<draw>/commands`, an IndirectDraws of them, which the Draw draws `eDrawIndexedIndirect`.
What each element is to the shader is then its `[entity("PER_INSTANCE")]` buffer's, filled in the batch's order:

```json
{ "id": "drawn", "type": "ForEach", "inputs": { "component": "lit", "with": ["Mesh"] } },
{ "id": "draw", "type": "Draw", "inputs": { "pipeline": { "$link": ["lit", "out"] }, "mesh": { "$link": ["drawn", "Mesh.mesh"] }, "batched": true } }
```

A Draw that is wired a value of each element besides its mesh — a `push.model` from the element's `matrix`, straight
or through Math — cannot be one draw: it draws one at a time, as it did, and its note says which inputs and why. So
does one whose ForEach says `where`, `select`, `entityId` or `groupBy` (make it a Filter), and one whose `command` is
another than eAuto or eDrawIndexed.

**Given `cull`, a batch is culled on the GPU.** Its IndirectDraws is in eGPU, and CullInSight's passes are written out
with it — `<draw>/cull size` and `<draw>/cull`, their pipelines and a buffer of groups — reading its buffer of
instances (`instances`, `instanceStride`, `instanceMatrixOffset`, which a Draw gives) and seeing as `cull` does. The
Draw is `compacted`: its shader reads, in place of the buffer it fills, another as big (`drawnInstances`) into which
the cull copies each element in sight, one after another as they are drawn. So a shader that reads its instance at
`SV_StartInstanceLocation + SV_InstanceID`, as any of a buffer of instances does, draws only what is in sight with no
change to it (`ScaleTest.ABatchedDrawGivenCullIsCulledOnTheGpu`). A `cull` of several matrices one after another —
a ShadowCascades' `matrices`, with `cullIndex` a Repeat's `index` — is the `cullIndex`th: sixteen numbers from sixteen
times it, a **Slice** of them (`<draw>/sight`). Batched Draws of the same Filter's or ForEach's meshes — a cascade's
turns — share the batch and the pack (the first's `<draw>/batch` and `<draw>/pack`), each with commands and a cull of its
own (`ScaleTest.CascadesOfABatchedDrawAreEachCulledByTheirOwnMatrix`).

One whose shader has no buffer of instances draws every element alike, and says so. Ten thousand entities of two
meshes draw the same picture, pixel for pixel, either way; a draw each costs the CPU about 7.8 ms a frame (readying 3.1,
recording 4.7), and batched 0.19 ms (Release, `ScaleTest.ABatchedDrawDrawsTheSamePictureForLess`).

The rest of a draw is the node's as well:

- **What the framebuffer starts as and keeps:** `load` (true draws over what is there), or each of
  `colorLoad`, `depthLoad`, `stencilLoad` (`eClear`, `eLoad`, `eDontCare`) and `colorStore`, `depthStore`,
  `stencilStore` (`eStore`, `eDontCare`). `clearColor`, `clearDepth` and `clearStencil` replace what the
  framebuffer says it clears to.
- **Where:** `viewport` and `scissor`, each x, y, width, height in pixels. The whole framebuffer when not
  given.
- **State the pipeline leaves to the draw:** `cullMode`, `frontFace`, `depthTest`, `depthWrite`,
  `depthCompare`, `depthBias`, `lineWidth`, `blendConstants`, `stencilTest`, `stencilReference`,
  `stencilCompareMask`, `stencilWriteMask`, `rasterizerDiscard`, `primitiveRestart`. One not given is what
  the pipeline says.

A Draw's `cull`, wired from what the picture is seen with — a MatrixMultiply of a lens and a View, or a ShadowCascades'
`matrices` with `cullIndex` saying which — leaves out every element whose mesh, placed as its entity is, is wholly out of its sight. Only a
mesh whose box is known is left out: a default mesh, a model's. `ReportOf(id)->culled` counts them.

Each frame, `Update()` walks the elements of the Filter or ForEach it is wired from. Its matrix, normal matrix, entity id and fields — what is wired
straight from the ForEach — are read from the world as they are; what is worked out of it between, a node at a
time for each element. An element gets a descriptor set the first time it is drawn and a new one only when a
resource it is given changes. With 500 entities and two passes, a frame in which nothing changed takes about 1.6 ms
of this in a debug build.

An element that can't be drawn is skipped and counted in `runtime.ReportOf(passId)`, with the reason for the
first one: a parameter with nothing in it, a resource of the wrong kind, a mesh that has not what the vertex
shader reads (`its mesh has not what the vertex shader reads: ...`). The node itself stays ok. `ReportOf` is null
for a Draw that draws once and drew.

### Many of one mesh, on the GPU

An entity each is right for thousands of things, and too much for millions: each is a name, a place in the hierarchy
and its components on the CPU, a couple of hundred bytes before anything is drawn. Grass, stones, a crowd seen from
afar are an **Instancer** instead (a component of the engine's package): one entity, its Mesh, and how many of it there
are to be, which the GPU makes, culls and draws without a number of them ever being on the CPU. Three of the engine's
functions do it, each made of the nodes any graph has (and shaders in `packages/koral/shaders`):

- **Instances** (`count`, `seed`, `extent`, `scale`, `jitter`: an Entity of the Instancer's fields) makes them into
  one buffer: a header of what they were made from, then a box round each cluster of 256, then the instances, 20
  bytes each — where it is, its scale, and its rotation as a quaternion packed into 32 bits. 100 million are 2 GB, in a
  storage buffer the device must let reach 4 GB. They are scattered over the square in tiles of 16 by 16, a cluster
  each, so a cluster's box is small. A pass of one thread compares the header with what it is given each frame and
  writes how many groups the making and the boxing run: none when they are the same. A frame in which nothing changed
  runs three passes that do nothing; one in which a field changed makes them all again. `instances` and `count` out.
- **CullInstances** (`instances`, `count`, `toClip` — the entity's matrix, then the view, then the lens — `height` in
  pixels, `meshRadius`, `pointDistance`, `drawDistance`, the mesh's `indexCount`, `firstIndex` and `vertexOffset`, a
  MeshBuffers', and `capacity`, `pointCapacity`) works out what is seen, every frame, on the GPU. A thread a cluster
  leaves out one whose box is out of sight or past `drawDistance`. One wholly past `pointDistance` that spans fewer
  pixels than it has instances is as many points as it spans, of instances spread over it: there are never many more
  points than pixels, however many instances. The rest are listed, and a thread each of their instances leaves out
  those out of sight, and keeps the rest as their mesh (nearer than `pointDistance`) or a point, counted a wave at a
  time. Distances are along the view, in the world's units. It gives `commands` (one indexed indirect draw, its
  instance count the GPU's), `visible` (which instance each drawn one is), and `points`, `counters` and `groups` for
  SplatPoints. What does not fit `capacity` or `pointCapacity` is left out, and counted in `counters`' fourth.
- **SplatPoints** (those, `target` with eStorage, `depth` eSampled, `color`, `width`, `height`, `reversedZ`) draws
  the points without triangles: each a square of pixels as wide as its instance looks (up to four), written into a
  buffer of the picture's pixels with an atomic max of how near it is and its shade, then laid over `target` wherever
  it is nearer than `depth` says the scene is.

The near ones are the project's own Draw: its pipeline, the entity's mesh, `eDrawIndexedIndirect` of `commands` with
`drawCount` 1, and a vertex shader that finds its instance through `visible`:

```slang
import koral_instances;
ByteAddressBuffer instances;            // Instances' `instances`
StructuredBuffer<uint> visible;         // CullInstances' `visible`
...
const float4x4 model = instanceMatrix(instances, visible[instanceIndex]);   // then the entity's matrix, as toClip has it
```

The CPU's frame is the same whatever the count: a hundred million cubes seen across their square, the near ones as
cubes and the rest as points, cost the runtime 0.08 ms a frame, moved or not, and the GPU 0.3 ms (Release, an RTX 5080
laptop GPU, a picture of 512 by 512: `ScaleTest.AnInstancersMillionsCostTheCpuNothing`, `KGRAPH_INSTANCES=100000000`).
Making them takes the first frames about a second. What is not done yet: an instance's own colour or picture, more than
one mesh between the near one and a point, occlusion, and instances placed other than as Instances scatters them — for
which the function is opened and its `generate` given another shader.

### When a pipeline changes

The component follows the pipeline and its draws. Wire a parameter — on the pipeline, or on a Draw from anything
but the component — and its field goes. Edit the shader and the fields change with it. The component keeps its id, and every entity keeps the fields whose name and type are the
same. `runtime.MigrationOf(id)` says which fields were kept, reset and removed.

After a shader edit, a node input for a parameter or constant the shader no longer has, or now has as
another kind, is taken off the node — and a Draw's or a Dispatch's `param.` or `push.` that it no longer has, off
that. `runtime.TakeReset()` lists them as `node.input`, and `Current()` is
the graph without them. The editor will show these.

### Saving

`kgraph::RegisterCodecs(codecs, &runtime)` teaches kecs's save and load the field types above. Vectors and
matrices are lists of numbers. A resource is saved as the id of the node that made it and found again on
load. One made in code is saved as null.

## What a pass costs

`Runtime::Timings()` is each pass node's cost, averaged over recent frames, by its id: its GPU time (arriving a few
frames late; `gpuMeasured` false on a device that cannot time), and the CPU's in recording and readying it — a fixed
page's passes for every step a frame can run added together (`runs`). The editor shows it in a pass node's inspector.
Each pass is a command buffer of its own in the frame graph, which costs some tens of microseconds of CPU and GPU time
whatever it does: a loop of hundreds of small dispatches costs that many times over.

`Runtime::Frame()` is the frame as the frame graph runs it: the graph's passes in the order they are scheduled, each with
the node it is, its dependency level (passes of one level depend on none of each other), whether it is on the async
compute queue, its times, and what it reads and writes — by the node that made each resource, or the entities' set it
is; then the passes left out, skipped or culled, and why; and the memory the frame graph made for itself.

## Looking at what a node made

`runtime.Watch(id)` copies what a node makes — its image or its buffer — out each frame from then on, and
`runtime.Watched(id)` is what it held a frame or two ago: a `Peek` with an image's format, size and texels, or a
buffer's bytes, and for a ShaderBuffer its fields by name and offset. `TexelLayoutOf(format)` says how a format's
texels are laid out. An image needs `eTransferSrc` among its usage, and so does a buffer (an Image or a Buffer node's
always has it); when one
cannot be looked at, `WatchProblem(id)` says why. It costs a copy a frame, so `Unwatch` when done. The copying is
two nodes added to the graph that runs and to nothing that is saved.

A **Reference** watched is what it stands for as it is when the Reference's page begins: its copy is made first of
that page, after what earlier pages did to it and before what its own page does. So a page that brings in a picture
shows the picture it starts from. A Reference on a setup page is copied while setup records, and the first watch of
one makes setup record again, so that the copy is of what setup made.

`runtime.UsesOf(id)` lists the pass nodes that use what a node made, each with its page and whether it reads it,
writes it or both, in the order of their pages. The ones before a page are what was done to it by the time that page
begins.

`runtime.WatchEvery(id, frames)` copies a watched node out once in so many frames rather than every frame — each
watch on frames of its own, so that many of them are not all copied at once — at the cost of a picture that is that
many frames old. Its copy pass stays in the frame graph and records nothing between, so changing it builds nothing
again; a new watch is copied at once, whatever its turn.

`runtime.Watch(id, mip, layer)` copies one level and array layer of an image rather than its first; asked again with
others, the copy is of those from a frame or two on, and a level or layer the image has not is its last. Besides the
texels, a `Peek` of an image says all of it: `fullExtent`, `mips`, `layers`, `samples`, `type`, its `usage` by name,
`byteSize` (every level and layer), and which `mip` and `layer` were copied. A picture of a file's that no node is is
watched by the name it is found by (`@textures/wall.png`): an `ImageFile` of it is added to what runs to copy from.

A node is watched — and `ResourceOf(id)` and `ImageOf(id)` are, given no output — by `runtime.ShownOutput(id)`: its
`out`, or, for a node with none, a Script node, the first image it makes, else its first resource.
`runtime.WatchOutput(id, output)` watches another of its outputs instead: a Script node's buffer, its other image.

## Evaluation

`Runtime::Set(graph)` evaluates the nodes in dependency order. Each node has a signature made from its
type, its values and which build of each node it is wired to. A node is built again only when its
signature changes, so editing one constant rebuilds the pipeline and the pass that runs it and leaves
the image, the shader and every other pass alone. Moving a node rebuilds nothing.
`Status(id)->builds` counts builds.

A node that fails keeps the error (`Status(id)->error`), and the framework's own report of the failure
is kept out of the log, which a graph being edited would otherwise fill. Nodes wired to it fail with "wired to 'x', which
failed", and everything else keeps running. A cycle is reported on the nodes in it.

A resource is shared by the nodes and passes that use it and freed when the last one lets go, so taking
a node out of the graph frees what it made.

When a shader file is edited, the framework reloads it, and `Update()` rebuilds the pipelines made from
it on the next frame.

## Order

Passes are installed in the scene's `kor::FrameGraph`, which orders them by the resources they read and
write. Where a pass sits in the node list doesn't matter. The frame graph runs two writers of the same
resource in the order they were added, so kgraph adds a pass that only overwrites a resource (Clear)
before one that reads and writes it (Dispatch on a read-write image). Two passes that both modify the
same resource run in the order of the passes they are in, and within a pass in node-list order: usage
does not say which is first. A pass that only overwrites goes before one that reads and writes only within one page,
or when the overwriting page is the earlier one: what a later page clears, it clears after what an earlier page drew.
Setup pages come first, so what setup does to a resource is done before anything a frame does to it.

Which of two pages of the same rate runs first, where what they use does not say, is a **link**: `Graph::after`, each
`{ page, before }` — `page` runs after `before` — saved as `"after": [["sky", "scene"]]`. `Graph::OrderByRate` puts
`passes` in an order that keeps every link (one by one in a row too: `RunsAfter`), each page the links say nothing of
keeping its place; a ring of links is broken where it closes. Only two pages that both *write* something need one: a
page that reads what another writes runs after it whatever, so a link between them says nothing the data does not.
Two that both write something with no link between them are a **tie**: they run in the order they happen to be in,
and an editor says so (`keditor::Document::OrderTies`) until one is linked.

A graph made in code, or saved before there were links, is **not settled** (`Graph::orderSettled` false): its pages
run in the order its list has them, as they always did. An editor makes links of that order where it matters — each
pair of the same rate that writes the same thing, the later after the earlier, less those others in a row say already
(`Document::SettleOrder`) — when the graph is saved or a link is drawn, and the order is the links' from then on. A
graph read with an `"after"` (even an empty one) is settled.

A pass that only reads a resource runs after every pass that writes it. The one exception is a reader whose own
output a later writer of that resource takes: a trace that reads the picture into an image which the next pass adds
back to the picture. After every writer would be after itself, so it takes its place among the writers, in the
graph's order.

Resources that entities hold count too. Each per-entity parameter of a pass is a set of resources (a
`kor::ResourceSet`), refilled only when its members change. Two uses that have a resource in common are
given the same name in the frame graph. So a Draw that samples the images entities hold runs after the
Dispatch that writes them, and a wired image that some entity also holds is ordered with both.

## Limits

- Draw goes into a framebuffer, not straight to the screen: Blit one of the framebuffer's images to show it.
- A draw's stencil operations (what happens on fail and pass) are the pipeline's `depthStencilState`
  only; the reference and masks can be set on the draw. One `clearColor` is for every colour attachment.
- A buffer holds no data of its own in a graph: an indirect buffer is written by a compute pass, or by an
  IndirectDraws.
- A debug label or a GPU timer round a pass can't be asked for yet.
- The view a DebugDraw node and `Runtime::ViewProjection()` go by is that of the main View. A light casts a shadow
  only when a graph draws one: ShadowCascades gives the sun's matrices, and
  `samples/forward-plus` draws with them.
- A Repeat's `count` is given on the node, not wired, and a node is in one loop: a loop in a loop is an error.
  An Element's `index` is given, or wired straight from a Repeat.
- A model's meshes are handed to the device in one go when the file is read: a very large one is a pause of a
  frame or so then. A model's meshes have no tangents; a shader that wants a normal map's frame works it out (as
  `samples/forward-plus` does).
- A ShaderBuffer reads the struct of a buffer an entry point uses: a struct no entry point reaches is not in the
  compiled shader. Numbers of 16 or 64 bits, and arrays of arrays, are left as zero. Of a block with fields before
  its array (`buffer B { uint count; Light lights[]; }`) only the array is filled.
- What is worked out between a ForEach and a buffer is worked out again for each element, each frame: fine for
  lights, slow for tens of thousands of entities.
- No node works on a struct: a Bundle (see [Bundles](#bundles)) is many wires as one, written out into them before
  anything runs, so what a component holds, and what a View and a lens work out, is still worked out a value at a
  time. A graph saved with a View's `of`, `kind` or `face`, a ForEach's `struct`, a buffer's `instance` or a
  ShadowCascades' `camera` says so on the node: each is the View, the lens and the wires of [The scene](#the-scene).
- A Paraboloid gives what a shader needs to bend a picture, not a matrix that does: a lens's matrix is straight lines
  in, straight lines out, and a paraboloid's is not.
- A graph saved with the Camera, Sky, Sun or Lights node of before has a node of a type there is none of now: each is
  a ForEach of its component and what is worked out of it (`samples/forward-plus` shows how). An Entities node is
  still a ForEach.
- A script's component is read from its source as far as its fields go: numbers, true or false, and vectors,
  quaternions and matrices of numbers start as written; anything else starts at zero, and the document's notes say so.
- A BufferBuilder's layout is numbers, vectors and matrices, and fixed arrays of them: no struct in a struct.
- A Buffer node with no `type` has the one that suits what reaches it: `eDeviceLocal` while only passes do — the GPU's
  own memory, where its reads, writes and atomics are fastest — and `eDynamic`, memory the CPU writes into, once the CPU
  is handed it (a script, through `kengine_node_resource`: `Runtime::ReachedFromTheCpu`), when it is made again so and
  stays so. Nothing else reaches a Buffer node's memory from the CPU: what is looked at, and what a WriteBack reads, is
  copied out on the GPU. A `type` given is the one it is made with. (Forward-plus's buffers were `eDynamic` before:
  its lights' clusters, an atomic a group over the bus, took 2.9 ms a frame; on the device, 0.02 ms.)
- An Image or a Buffer node is made for what uses it: its usage is what each node it is wired to uses it as — an
  attachment of a Framebuffer, a parameter a shader samples or writes, a Clear's, a copy's or a blit's source or
  destination, an indirect draw's commands, an entity's picture — and `eTransferSrc`, so that it can always be looked
  at. Its `usage` adds to that, and need not be given: a new node has none, and the inspector shows the rest under it
  (`Runtime::UsageOf`). What a node is found to use it as when it is built, it is made again with at once, in the
  same build, and what is wired to it after it; what an entity is found to use it as, the frame after. So a graph that
  gives no usage builds its images and buffers twice the first time, and again — losing what they held — when what
  uses one changes what it uses it as. A Buffer shared from another runtime's is that one's, as it was made there.
- A cycle between passes is left to the frame graph. A resource as it was before this frame's writers is a Keep's
  copy of it (see [State kept from step to step](#state-kept-from-step-to-step)).
- Tags are matched by component name. A C++ type in a namespace is named with it unless `kecs::Name` is
  specialised.
- When a shader edit removes a parameter the node wires, the framework logs one error for the old
  pipeline before the runtime replaces it.
- The framework keeps every shader it compiled, by module and entry point, for the life of the app.
- What a resource starts with is made by a pass on a setup page (on the GPU) or a Script node (on the CPU, by a
  script of the project's, while the project runs with its scripts). Bytes can't be typed into a graph, and a Script
  node fills only the first level of an image: the rest are made from it. A resource a script makes of another of its
  own — a view of its image — is not given to it to fill.
- Fields for C++ components are still declared with `KECS_FIELDS`. Nothing reads the framework's
  `KORAL_REFLECT` for them.

## Point lights' shadows, and passes that need not run every frame

- **PointShadows** takes `lights`, four numbers each — where it is and how far it reaches: a Collect of an Append of a
  ForEach of PointLight's `worldPosition` and `field.range` — and gives, for the first `most`, six matrices each —
  towards +x, -x, +y, -y, +z, -z, a quarter turn wide, from `near` to the light's range: `matrices` (a Draw's `cull`,
  with `cullIndex`, and the shader's), `count`, `faces` (six times `most`), and `idle`.
- With `each` given, only that many lights are due a frame, one after another — and any light that moved or
  changed its range. `idle` has a 0 for each face that is due and a 1 for each that is not.
- A **Draw's `idle`**, wired from such a list and counted by `cullIndex`, makes the pass draw nothing while its
  number is not zero: its target is not cleared, and stays as it was drawn last. Wired from one number, or true or
  false, it is that for every turn: an editor's ids are drawn only while something is picked (a **Selected**'s
  `present`) or a pick is asked of the frame (its `picking`).
- A **Repeat** has up to 256 turns.

