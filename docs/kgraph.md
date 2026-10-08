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
  { "id": "shader", "type": "Shader", "inputs": { "entryPoint.module": "fill", "entryPoint.entry": "computeMain" } },
  { "id": "pipeline", "type": "ComputePipeline", "inputs": {
      "computeShader": { "$link": ["shader", "out"] },
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
- **Values:** Number, Bool, String, Vector, and Add, Subtract, Multiply, Divide, Min, Max, Negate, Abs,
  Floor, Ceil, Round, Component and ImageExtent. The operations work on numbers and, a component at a
  time, on vectors.
- **VertexLayout** describes a mesh's vertices: a list of `{ "semantic": "POSITION", "type": "eFloat",
  "count": 3 }`, packed in the order given. Its output goes to a pipeline's `vertexShader.layout` and to a
  Mesh node's `vertexLayout`.
- **Primitive** is a mesh of a simple shape (`eQuad`, `eCube` or `eTriangle`, centred on the origin) with
  positions, normals and texture coordinates. Its second output, `layout`, is what a pipeline that draws
  it takes as `vertexShader.layout`.
- **Screen** is the scene's window. **Reference** stands for another node by id, for a resource used far
  from where it is made.
- **Passes:** Clear, Blit, Dispatch and Draw.

## Pipelines

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

### Dispatch and Draw

**Dispatch** runs a compute pipeline, once or per entity. `groups.x/y/z` is the size, or `cover` names one
of the pipeline's image parameters and the pass uses as many groups of `groupSize.x/y/z` as cover it. With
a per-entity image, each entity gets its own count.

**Draw** draws every entity that has a `kgraph::Mesh` (and the pipeline's component, when it has one) into
a framebuffer: the pipeline's own, or the one wired to `framebuffer`. `command` is `eAuto` (indexed when
the mesh has indices), `eDraw` or `eDrawIndexed`. `instances` is how many of each, and `load` draws over
what the framebuffer holds, where the default clears it first.

Both take `with` and `without`: lists of component names, usually tags, that narrow which entities run. A
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

- No ray tracing pass yet, and no indirect draws. Draw goes into a framebuffer, not straight to the
  screen: Blit one of the framebuffer's images to show it.
- A cycle between passes is left to the frame graph. Reading a resource as it was last frame
  (`ReadPrevious`) can't be asked for in a graph yet.
- Tags are matched by component name. A C++ type in a namespace is named with it unless `kecs::Name` is
  specialised.
- When a shader edit removes a parameter the node wires, the framework logs one error for the old
  pipeline before the runtime replaces it.
- A struct input given some of its fields has the rest zeroed, not defaulted.
- The framework keeps every shader it compiled, by module and entry point, for the life of the app.
- Initial data (an image's pixels, a buffer's bytes) can't be given in a graph.
- Fields for C++ components are still declared with `KECS_FIELDS`. Nothing reads the framework's
  `KORAL_REFLECT` for them.
