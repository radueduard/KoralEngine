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

Values are JSON's own: numbers, booleans, strings, lists and objects. An enum is its name, flags are a
list of names, a vector is a list of numbers, and a struct is an object by field name. `Graph::ToJson`
and `Graph::FromJson` round-trip a graph exactly.

## Node types

`kgraph::Catalog()` lists them, with every input and output, its type and its description. This is what
the editor will build its node list and its nodes from.

- **One for each builder the framework describes** (`kor::builders`): Buffer, Image, ImageView, Sampler,
  BufferView, Shader, Framebuffer, Mesh, AccelerationStructure and the three pipelines. A builder setting
  with one argument is an input of that name. With several it is one input per argument:
  `extent.x`, `entryPoint.module`. A setting that can be given more than once is `color[].view`, written
  in a graph as `color[0].view`, `color[1].view`. Nothing is written by hand: a builder that gains a
  setting gains the input.
- **Values:** Number, Bool, String, Vector, Color, Component, and Split (a vector's `x`, `y`, `z`, `w` as
  outputs of their own).
- **Math:** Add, Subtract, Multiply, Divide, Min, Max, Pow, Mod, Negate, Abs, Floor, Ceil, Round, Fract,
  Sign, Saturate, Sqrt, Exp, Log, Sin, Cos, Tan, Radians, Degrees, Clamp, Lerp, Smoothstep, Step, Atan2.
  They work on numbers and, a component at a time, on vectors; a number goes with every component.
- **Vectors:** Length, Normalize, Dot, Cross, Distance.
- **Logic:** Less, LessOrEqual, Greater, GreaterOrEqual, Equal, NotEqual, Not, And, Or, Xor, and Select
  (`a` when its condition is true, `b` when not).
- **VertexLayout** describes a mesh's vertices: a list of `{ "semantic": "POSITION", "type": "eFloat",
  "count": 3 }`, packed in the order given. Its output goes to a pipeline's `vertexShader.layout` and to a
  Mesh node's `vertexLayout`.
- **Primitive** is a mesh of a simple shape, centred on the origin and about a unit across: `eCube`,
  `eSphere`, `ePlane` (facing up), `eCylinder`, `eCone`, `eCapsule`, `eTorus`, and `eQuad` (facing +z) and
  `eTriangle`. Each has positions, normals and texture coordinates, and faces outward. Its second output,
  `layout`, is what a pipeline that draws it takes as `vertex.layout`.
- **Screen** is the scene's window. **Reference** stands for another node by id, for a resource used far
  from where it is made.
- **Passes:** Clear, Blit (scaled, a part or all, and how a picture reaches the screen), Resolve (a
  multisampled image made an ordinary one), Copy (image to image of one size), CopyBuffer, ClearBuffer,
  CopyBufferToImage, CopyImageToBuffer, GenerateMipmaps, Dispatch, TraceRays and Draw. Every command the
  framework's command buffer records has a node, and each of its arguments is an input.

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
keeps a line width of 1 and fills its polygons, where it used to get zeros.

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

A pipeline with a parameter left unwired, or with push constants, runs once for each entity. The runtime
generates a component for it in the world, named after the pipeline's node:

- a field for each unwired parameter, holding a shared reference to a resource (type `Image`, `Buffer`,
  `Sampler`, `BufferView` or `AccelerationStructure`). A sampled image whose sampler the node doesn't wire
  gets a second field, `<name>.sampler`.
- a field for each push constant, by the name the shader gives it (type `f32`, `i32`, `u32`, `vec2` to
  `vec4`, `ivec`, `uvec`, `mat4`, and `bytes<N>` for anything else).

Where the entity is, is not one of those fields. A push constant that is a `float4x4` named `model` (or
`world`) is filled each frame with the entity's world matrix: its own transform under those of everything
it is under (see [kecs](kecs.md)). One named `normalMatrix` gets its inverse, transposed. In Slang,
`mul(float4(position, 1.0), object.model)`.

`runtime.ComponentOf(id)` returns the component, or `NoComponent` for a pipeline with everything wired and
no push constants, which runs once. `<kgraph/entities.h>` is how code fills one:

```cpp
const kecs::ComponentId blur = runtime.ComponentOf("blur");
world.Add(entity, blur);
kgraph::Parameters(world, entity, blur)
    .Set("target", kgraph::MakeResource(std::move(image)))   // a resource made in code
    .Set("source", runtime.ResourceOf("backdrop"))           // or one a node makes
    .Set("radius", 4.f);
world.Add<kgraph::Mesh>(entity, { kgraph::MakeResource(std::move(mesh)) });   // what a Draw node draws it with
```

Resources are shared and counted: an image lives while a node or a component holds it.

### Default meshes

An entity's `kgraph::Mesh` can hold one of the default meshes without the graph having a node for it:
`runtime.ResourceOf("$cube")`, and likewise `$sphere`, `$plane`, `$cylinder`, `$cone`, `$capsule` and
`$torus` (`kgraph::DefaultMeshNames()`, `kgraph::DefaultMeshId(name)`). Each is made the first time it is
asked for, and is saved by that id. They are the Primitive node's shapes, laid out the same way, so a
pipeline that takes a Primitive's `layout` draws them. `<kgraph/meshes.h>` also gives the shapes as plain
numbers (`ShapeData`), for code that wants the vertices.

### Dispatch and Draw

**Dispatch** runs a compute pipeline, once or per entity. `groups` (or `groups.x/y/z`) is how many groups
to run, rounded up: an image's `extent` divided by the pipeline's `numThreads`, wired straight in, covers
the image. Or `cover` names one of the pipeline's image parameters and the pass works the count out itself,
from `groupSize`, which is the pipeline's `numThreads` unless the node says otherwise. With a per-entity
image, each entity gets its own count.

A Dispatch can also read its group counts from a buffer a pass before it wrote: wire it to `indirect`
(the buffer needs `eIndirect`), with `indirectOffset` for where in it.

**TraceRays** runs a ray tracing pipeline the same way: `size` (or `size.x/y/z`) rays, or as many as
`cover` an image parameter.

**Draw** draws with a graphics pipeline into a framebuffer: the pipeline's own, or the one wired to
`framebuffer`. What it draws is one of three things.

- **Each entity's mesh.** Every entity that has a `kgraph::Mesh` (and the pipeline's component, when it has
  one). This is what happens when the vertex shader reads vertices and no mesh is wired.
- **One mesh,** wired to `mesh`. With nothing left for an entity to supply, it draws once.
- **No mesh.** A vertex shader that takes no vertex inputs makes its own vertices from `SV_VertexID`, and
  the pipeline needs no `vertex.layout`. `vertexCount` says how many, 3 when not given: a triangle. This
  too draws once, with no entities and no world.

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

All three take `with` and `without`: lists of component names, usually tags, that narrow which entities run. A
component switched off on an entity (`world.SetEnabled`) doesn't match, so the entity is passed by without
being moved or losing its values.

Each frame, `Update()` walks the matching entities. An entity gets a descriptor set the first time it
runs and a new one only when a resource it holds changes. Its push constants are read every frame. With
500 entities and two passes, a frame in which nothing changed takes 0.4 ms of this in a debug build.

An entity that can't be run is skipped and counted in `runtime.ReportOf(passId)`, with the reason for the
first one: a parameter with nothing in it, a resource of the wrong kind, a mesh whose layout doesn't match
the pipeline's. The node itself stays ok.

### When a pipeline changes

The component follows the pipeline. Wire a parameter and its field goes. Edit the shader and the fields
change with it. The component keeps its id, and every entity keeps the fields whose name and type are the
same. `runtime.MigrationOf(id)` says which fields were kept, reset and removed.

After a shader edit, a node input for a parameter or constant the shader no longer has, or now has as
another kind, is taken off the node. `runtime.TakeReset()` lists them as `node.input`, and `Current()` is
the graph without them. The editor will show these.

### Saving

`kgraph::RegisterCodecs(codecs, &runtime)` teaches kecs's save and load the field types above. Vectors and
matrices are lists of numbers. A resource is saved as the id of the node that made it and found again on
load. One made in code is saved as null.

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
same resource run in node-list order: usage does not say which is first.

Resources that entities hold count too. Each per-entity parameter of a pass is a set of resources (a
`kor::ResourceSet`), refilled only when its members change. Two uses that have a resource in common are
given the same name in the frame graph. So a Draw that samples the images entities hold runs after the
Dispatch that writes them, and a wired image that some entity also holds is ordered with both.

## Limits

- Draw goes into a framebuffer, not straight to the screen: Blit one of the framebuffer's images to show it.
- A draw's stencil operations (what happens on fail and pass) are the pipeline's `depthStencilState`
  only; the reference and masks can be set on the draw. One `clearColor` is for every colour attachment.
- A buffer holds no data of its own in a graph: an indirect buffer is written by a compute pass.
- A debug label or a GPU timer round a pass can't be asked for yet.
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
