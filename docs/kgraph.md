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
of two passes runs first when nothing they use says (see [Order](#order)).

`Graph::overview` is where nodes are when every node is seen at once, by id — a layout apart from the
passes' pages (`"overview": { "color": [1200, 40] }`). It is only for the editor; nothing that runs reads it.

Values are JSON's own: numbers, booleans, strings, lists and objects. An enum is its name, flags are a
list of names, a vector is a list of numbers, and a struct is an object by field name. `Graph::ToJson`
and `Graph::FromJson` round-trip a graph exactly.

### Functions

A **Function** node is a call: the nodes of a function, as one. A function is a graph of its own — kept by
whoever has the graph (`Graph::functions`, by name; a project's `functions/<name>.kfunc`, for the editor) — whose
**FunctionInput** and **FunctionOutput** nodes are its edges. Each edge is a port of every call, by its `name`, in
the order the edges are on the function's page, top to bottom; its `kind` says what it carries: `eValue`,
`eEntities` (what a Filter keeps some of), or a resource — `eMesh`, `eBuffer`, `eImage`, `eResource`. An input a
call is given nothing at takes its `default`.

Before the graph runs, each call is written out into its function's nodes, `<call>/<node>`, in the call's pass —
before its loops are, so that a function can hold a loop and a call can be in one. What is wired to an input is
what the call is given there; what is wired from the call is what the output is wired from inside. So nothing new
is evaluated: a call wired from a ForEach follows it as its nodes would. A call fares as its nodes do: the first of
them that fails says so on it, by name (`times: 'b' is a number`). A function that calls itself is written out
eight deep, and no further. `Runtime::PortsOf` and `OutputsOf` give a call's ports as its edges are, an output as
the node inside gives it, and `ValueOf` and `ResourceOf` what is there.

A FunctionInput or FunctionOutput in a graph itself — a function opened in place, in the editor — stands for what
its `value` is wired from.

## Node types

`kgraph::Catalog()` lists them, with every input and output, its type and its description. This is what
the editor will build its node list and its nodes from.

- **One for each builder the framework describes** (`kor::builders`): Buffer, Image, ImageView, Sampler,
  BufferView, Shader, Framebuffer, Mesh, AccelerationStructure and the three pipelines. A builder setting
  with one argument is an input of that name. With several it is one input per argument:
  `extent.x`, `entryPoint.module`. A setting that can be given more than once is `color[].view`, written
  in a graph as `color[0].view`, `color[1].view`. Nothing is written by hand: a builder that gains a
  setting gains the input.
- **Values:** Number, Bool, String, Vector, Color, Constant (`ePi`, `eTau`, `eE`, `eSqrt2`, `ePhi`), Component,
  and Split (a vector's `x`, `y`, `z`, `w` as outputs of their own).
- **Math:** Add, Subtract, Multiply, Divide, Min, Max, Pow, Mod, MultiplyAdd, Negate, Abs, Floor, Ceil, Round,
  Trunc, Fract, Sign, Saturate, OneMinus, Reciprocal, Sqrt, InverseSqrt, Exp, Exp2, Log, Log2, Log10, Sin, Cos,
  Tan, Asin, Acos, Atan, Atan2, Sinh, Cosh, Tanh, Radians, Degrees, Clamp, Lerp, InverseLerp, Remap (`value` from
  `fromMin`–`fromMax` to `toMin`–`toMax`), Smoothstep, Step. They work on numbers and, a component at a time, on
  vectors; a number goes with every component.
- **Vectors:** Length, Normalize, Dot, Cross, Distance, Reflect, Refract (`eta`; all zero when it is all
  reflected), Project (`a` along `onto`). Matrices of four by four: MatrixMultiply, MatrixInverse, MatrixTranspose,
  MatrixDeterminant; MatrixTranslate, MatrixRotate (x, y, z in degrees, as an entity's rotation), MatrixAxisAngle,
  MatrixScale and MatrixCompose (position, rotation and scale, as the world composes an entity's); TransformPoint
  (divided by its w), TransformDirection and TransformPlane (a plane where a matrix that moves points puts it).
- **Cameras:** View, Perspective, Orthographic, Frustum, Oblique, Paraboloid, CubeFace, ShadowCascades and
  PointShadows. See [The scene](#the-scene).
- **Logic:** Less, LessOrEqual, Greater, GreaterOrEqual, Equal, NotEqual, Not, And, Or, Xor, and Select
  (`a` when its condition is true, `b` when not).
- **VertexLayout** describes a mesh's vertices: a list of `{ "semantic": "POSITION", "type": "eFloat",
  "count": 3 }`, packed in the order given. Its output goes to a pipeline's `vertexShader.layout` and to a
  Mesh node's `vertexLayout`.
- **Primitive** is a mesh of a simple shape, centred on the origin and about a unit across: `eCube`,
  `eSphere`, `ePlane` (facing up), `eCylinder`, `eCone`, `eCapsule`, `eTorus`, and `eQuad` (facing +z) and
  `eTriangle`. Each has positions, normals and texture coordinates, and faces outward. Its second output,
  `layout`, is what a pipeline that draws it takes as `vertex.layout`.
- **Entities** is every entity, a **Filter** those of them with and without components, an **Entity** one of them (the
  first with a component, or one by name), and a **ForEach** reads any
  component of the world — the scene's Camera and lights are the engine's package's scripts —
  every entity of it; **ComponentValue** and **EntityTransform** one field, or one place, of an entity.
  **View** and a lens — **Perspective**, **Orthographic**, **Frustum**, **Oblique**, **Paraboloid** — are what a
  camera or a light sees, **Linear** a colour as light adds it up, **Collect** every element's value as one list,
  and **ShadowCascades** and **PointShadows** the shadow maps' matrices. **DataBuffer**
  packs values into a buffer a shader reads. See [The scene](#the-scene).
- **ShaderBuffer** is a buffer a shader declares, with an input for each field of its struct, and **ForEach** is
  every entity that has a component, an element at a time: an output for each of its fields, and where it is. Wired together they are an element for each entity. See [A shader's buffer, from the world](#a-shaders-buffer-from-the-world).
- **BufferBuilder** is a buffer whose element is laid out on the node. See [A buffer laid out on its node](#a-buffer-laid-out-on-its-node).
- **A node for each component.** Every component of the world — the package's, a project's scripts', a pipeline's — is
  offered as two more: `PointLight`, a ForEach of it, and `PointLight Buffer`, a BufferBuilder laid out as that
  ForEach's struct is (`Runtime::Presets()`). Neither is a type of its own: what is saved is the ForEach and the
  BufferBuilder they are, and the editor offers them, and `Document::AddNode` makes them, by those names.
- **Screen** is the scene's window. Its `extent`, `size`, `width`, `height` and `aspect` outputs are the window's
  size: an image whose `extent` is wired to it is rebuilt when the window is resized, and so is whatever is
  made of that image. **Reference** stands for another node by id: how a pass brings in what an
  earlier one made.
- **ImageFile** is a picture from a file. See [Files](#files).
- **Repeat**, **Previous**, **Element** and **Switch** are what is done more than once, and one of several
  ways. See [Loops and branches](#loops-and-branches).
- **Passes:** Clear, Blit (scaled, a part or all, and how a picture reaches the screen), Resolve (a
  multisampled image made an ordinary one), Copy (image to image of one size), CopyBuffer, ClearBuffer,
  CopyBufferToImage, CopyImageToBuffer, GenerateMipmaps, Dispatch, TraceRays and Draw. Every command the
  framework's command buffer records has a node, and each of its arguments is an input.

Every pass node also takes `enabled`: false leaves the pass out, with the node still there and still ok.

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

## Pipelines

A pipeline names its shaders itself, a stage at a time: `<stage>.module` is a `.slang` file among the
project's shaders, and `<stage>.entry` the function in it (`<stage>Main` when not given).

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
  nothing is zero.

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
| `Camera` | `fieldOfView` (60, degrees, top to bottom), `near` (0.1), `far` (100). |
| `DirectionalLight` | `color` (1, 1, 1), `intensity` (1). It shines along its entity's forward (-Z). |
| `PointLight` | `color` (1, 0.85, 0.6), `intensity` (2), `range` (5). |
| `SpotLight` | `color`, `intensity` (3), `range` (8), `innerAngle` (20), `outerAngle` (30, degrees, from the middle of the cone). |
| `Sky` | `zenith`, `horizon`, `ground` (colours), `ambient` (0.35), `sunSize` (1.5, degrees). |

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
| **Linear** | A colour as picked, in the linear terms light adds up in: red, green and blue to the power 2.2. |
| **Collect** | `out`: a value worked out once an element of a ForEach, every element's one after another, as one list; `count`. |
| **ShadowCascades** | A light's shadow maps, fitted to what a camera sees: `matrices` (four, one after another), `splits` (how far from the camera each reaches), `worldTexel`, `tileTexel`, `direction`. Inputs: the camera's `view` and `projection` (where its near and far planes are is read from it), `direction` (the way the light travels: a DirectionalLight's entity's `forward`), `resolution` of one cascade, `distance`, `lambda`, and `boundsMin` and `boundsMax` of what casts. |
| **PointShadows** | See [Point lights' shadows](#point-lights-shadows-and-passes-that-need-not-run-every-frame). |
| **ComponentValue** | `out`: one field of any component, of the first entity with it or the one `entity` names; `present`. |
| **EntityTransform** | `matrix`, `worldPosition`, `forward`, `up`, `right`, `position`, `rotation`, `scale`, `present`, of an entity by name. |

Each lens gives `projection` (and `out`) and `inverseProjection`, with depth 0 at its near plane and 1 at its far
one and Y pointing down, as the device has it — `flipY` false for one as glm makes it. A lens and a View know nothing
of each other: the two as one is a **MatrixMultiply** of the lens (`a`) and the View's matrix (`b`) — the projection
after the view — and what undoes it, a MatrixInverse of that. A Perspective gives the `fieldOfView`, `aspect`, `near`
and `far` it went by; an Orthographic and a Frustum their `near` and `far`.

None of them knows of a camera or a light: they are numbers, and whatever holds the numbers is wired in. A camera is a
View from where its entity is towards a step along the way it faces, up as its up, and a Perspective of its
component's fields:

```json
{ "id": "eye", "type": "Entity", "inputs": { "component": "Camera" } },
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
view, near and far. That is how an
editor looks at a scene from where it likes; and what the editor's gizmo, `Runtime::ViewProjection()`, `CurrentView()`
and a DebugDraw node go by is the main View's and its lens's. A View that is not main is left alone.

The same graph can be seen from two places at once by a second runtime that follows the first:
`Runtime(frameGraph, world, source)` runs whatever it is `Set` (`source.Current()`, whenever `source.Revision()`
goes up) over the same world, into its own frame graph's screen, seen from where its own `SetView` says. Each
pass and each node's resource is its own — a picture the size of its screen, what is worked out from where it is
seen — but what is in files, the default meshes and pictures and the meshes made with `MakeMesh` are the
source's, so nothing is read or made twice. The editor's Game panel is one: the scene's camera, beside the
editor's own view.

These values are wired like any others: into Math and Vectors nodes (`Append`, `MatrixMultiply` and
`MatrixInverse` among them) and into a **DataBuffer**, whose `field[n].value` inputs are packed one after
another as a shader's block lays them out (std140: a number in four bytes, a vector of three or four
aligned to sixteen, so a `float3` followed by a `float` share one `float4`). `field[n].type` says what a
value is where its size doesn't: `eUInt`, or `eUVec4` for a `uint4` given one number. `usage` is `eUniform`
for a block or `eStorage` for an array. So a shader's

```slang
struct FrameData { float4x4 viewProjection; float4 cameraPosition; float4 skyZenith; /* w: ambient */ };
```

is a DataBuffer with `seen`, `camera.position` (as `eVec4`), `sky.zenith` and
`sky.ambient` wired to fields 0 to 3.

A node that reads the world, every value worked out from one, and every DataBuffer one reaches are brought
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
hierarchy's order, an element at a time. It gives `field.<name>` for each number or vector field of the component
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
Perspective of a ForEach of `Camera`. Anything else wired to a ForEach has the first element's values.

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

Values are as the component holds them: a colour is as it was picked, not converted to linear (a Linear node does
that), and an angle is in degrees. Pow, Radians and Cos nodes between the two do that for each entity.

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
`Runtime::Pick(u, v, then)` reads one pixel of it back, a few frames later, as the entity there.
**DebugDraw** draws the scene's `kor::DebugDraw` shapes over an image, seen as the graph's main View sees
(`Runtime::SetDebugDraw`).

### Files

A picture or a mesh that is in a file is a resource like any a node makes, found by where it is instead of by a
node, read the first time it is asked for, and saved by that name:

| `Runtime::ResourceOf(...)` | What it is |
|---|---|
| `@textures/wall.png` | That picture, with its smaller levels made. Any format the framework's image importer reads, `.hdr` included. |
| `@sponza/Sponza.gltf#3` | The fourth mesh of that model, laid out as the default meshes are: POSITION, NORMAL, UV. |
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

An entity's `kgraph::Mesh` can hold one of the default meshes without the graph having a node for it:
`runtime.ResourceOf("$cube")`, and likewise `$sphere`, `$plane`, `$cylinder`, `$cone`, `$capsule` and
`$torus` (`kgraph::DefaultMeshNames()`, `kgraph::DefaultMeshId(name)`). Each is made the first time it is
asked for, and is saved by that id. They are the Primitive node's shapes, laid out the same way, so a
pipeline that takes a Primitive's `layout` draws them. `<kgraph/meshes.h>` also gives the shapes as plain
numbers (`ShapeData`), for code that wants the vertices.

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

**Dispatch** runs a compute pipeline, once or once an element of the ForEach its inputs are wired from. `groups` (or `groups.x/y/z`) is how many groups
to run, rounded up: an image's `extent` divided by the pipeline's `numThreads`, wired straight in, covers
the image. Or `cover` names one of the pipeline's image parameters and the pass works the count out itself,
from `groupSize`, which is the pipeline's `numThreads` unless the node says otherwise. With an image of each
element's own (`param.<name>` wired from a ForEach), each gets its own count.

A Dispatch can also read its group counts from a buffer a pass before it wrote: wire it to `indirect`
(the buffer needs `eIndirect`), with `indirectOffset` for where in it.

**TraceRays** runs a ray tracing pipeline the same way: `size` (or `size.x/y/z`) rays, or as many as
`cover` an image parameter.

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

An indirect or count buffer needs `eIndirect` among its usage, and is a use of the pass: the pass that
writes it runs first.

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
  so otherwise.
  With nothing to pack yet — a Filter that keeps no one, or meshes still being read — it is an empty mesh laid out as the
  default meshes are (POSITION, NORMAL, UV), whose ranges draw nothing: with no meshes the IndirectDraws has a `drawCount` of 0, and the Draw clears
  and draws nothing, until there is something, when it is packed again.
- **IndirectDraws** takes the pack's `ranges` and the Filter's `groups` (grouped by Mesh, so the two are in one
  order) and writes a `VkDrawIndexedIndirectCommand` for each mesh into `commands`, with `drawCount` and
  `stride` (20). `mode` eCPU draws each group whole; eGPU writes every command with no instances each frame,
  and gives `visible` (a uint for every instance), for a compute pass of your own to count what it draws: it adds
  one to its mesh's `instanceCount` and writes the instance's number at the command's `firstInstance` plus what
  it added. Looked at (`Runtime::Watch`, the editor's *Look at what it holds*), its `out` is the commands, a line
  each — `indexCount`, `instanceCount`, `firstIndex`, `vertexOffset`, `firstInstance`: which meshes are batched, and
  how many of each.

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
entity's index) is where the entity is, or which. Any other — a number, a vector or a `float4x4` — is the entity's own:
a field of the pipeline's component, as its unwired `param.`s and `push.`s are, shown in the inspector and set from a
script (`world.set(cube, "<pipeline>", "color", Vec3(1f, 0f, 0f))`). An entity without the component has zeros there.
The component is not one of the Filter's `with`: the pipeline's vertex layout comes from the pack, which comes from the
Filter. The buffer has room for at least 64, twice what there was; past that, the Draw is built again with more.
A `param.` wired on the Draw is used as it is, and then nothing of the buffer is the component's.
Draws that would write the same into their buffers share one, written once a frame: of the same Filter, an element
laid out alike — field by field, where each is and what it is — and, when any field is the entity's own, the same
pipeline's component. A shadow pass's turns share theirs; a pass whose element also has its entity's colour has its
own. A Draw gives its buffer as `instances`, outgrown the buffer is made again for every Draw that shares it.

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

## Looking at what a node made

`runtime.Watch(id)` copies what a node makes — its image or its buffer — out each frame from then on, and
`runtime.Watched(id)` is what it held a frame or two ago: a `Peek` with an image's format, size and texels, or a
buffer's bytes, and for a ShaderBuffer its fields by name and offset. `TexelLayoutOf(format)` says how a format's
texels are laid out. An image needs `eTransferSrc` among its usage, and so does a Buffer node's buffer; when one
cannot be looked at, `WatchProblem(id)` says why. It costs a copy a frame, so `Unwatch` when done. The copying is
two nodes added to the graph that runs and to nothing that is saved.

`runtime.WatchEvery(id, frames)` copies a watched node out once in so many frames rather than every frame — each
watch on frames of its own, so that many of them are not all copied at once — at the cost of a picture that is that
many frames old. Its copy pass stays in the frame graph and records nothing between, so changing it builds nothing
again; a new watch is copied at once, whatever its turn.

`runtime.Watch(id, mip, layer)` copies one level and array layer of an image rather than its first; asked again with
others, the copy is of those from a frame or two on, and a level or layer the image has not is its last. Besides the
texels, a `Peek` of an image says all of it: `fullExtent`, `mips`, `layers`, `samples`, `type`, its `usage` by name,
`byteSize` (every level and layer), and which `mip` and `layer` were copied. A picture of a file's that no node is is
watched by the name it is found by (`@textures/wall.png`): an `ImageFile` of it is added to what runs to copy from.

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
does not say which is first.

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
- A DataBuffer holds numbers, vectors and matrices, not nested structs or arrays of anything but floats.
- A ShaderBuffer reads the struct of a buffer an entry point uses: a struct no entry point reaches is not in the
  compiled shader. Numbers of 16 or 64 bits, and arrays of arrays, are left as zero. Of a block with fields before
  its array (`buffer B { uint count; Light lights[]; }`) only the array is filled.
- What is worked out between a ForEach and a buffer is worked out again for each element, each frame: fine for
  lights, slow for tens of thousands of entities.
- No node gives a struct, or takes one: what a component holds, and what a View and a lens work out, is wired a
  value at a time. A graph saved with a View's `of`, `kind` or `face`, a ForEach's `struct`, a buffer's `instance` or a
  ShadowCascades' `camera` says so on the node: each is the View, the lens and the wires of [The scene](#the-scene).
- A Paraboloid gives what a shader needs to bend a picture, not a matrix that does: a lens's matrix is straight lines
  in, straight lines out, and a paraboloid's is not.
- A graph saved with the Camera, Sky, Sun or Lights node of before has a node of a type there is none of now: each is
  a ForEach of its component and what is worked out of it (`samples/forward-plus` shows how). An Entities node is
  still a ForEach.
- A script's component is read from its source as far as its fields go: numbers, true or false, and vectors,
  quaternions and matrices of numbers start as written; anything else starts at zero, and the document's notes say so.
- A BufferBuilder's layout is numbers, vectors and matrices, and fixed arrays of them: no struct in a struct.
- A cycle between passes is left to the frame graph. Reading a resource as it was last frame
  (`ReadPrevious`) can't be asked for in a graph yet.
- Tags are matched by component name. A C++ type in a namespace is named with it unless `kecs::Name` is
  specialised.
- When a shader edit removes a parameter the node wires, the framework logs one error for the old
  pipeline before the runtime replaces it.
- The framework keeps every shader it compiled, by module and entry point, for the life of the app.
- Initial data (an image's pixels, a buffer's bytes) can't be given in a graph.
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

