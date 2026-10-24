# Koral Engine — plan

Status: draft, 2026-10-08. This plan builds what [design.md](design.md) describes, in dependency order.
Milestones prefixed **F** go in the framework (GFX, branch `v2-development`). Milestones prefixed **E** go
in this repository. Each milestone ends at a gate: what has to work, and be tested, before it is done.

```
F1 shader reflection ─┐
F2 binding assignment ─┼──────────────┐
F3 builder descriptions ─► E3 graph model ─► E4 pipelines per entity ─► E5 editor ─► E6 scripts ─► E7 batching
F4 query-driven passes ───────────────────┘                    ▲            ▲
F5 node editor (koral-ui) ─────────────────────────────────────┘            │
E1 ECS core ─► E2 shape changes + JSON ─────────────────────────────────────┘
```

E1 and E2 depend on nothing in the framework and can start at once. The F milestones can be built
alongside them.

## Framework

**All five are done** (2026-10-08), on GFX `v2-development`, commits `3978bd9`, `fa211b6`, `8892a65` and
`7a1aefd`. Each section below says what was planned. Where what was built differs, a **Built** note says how.

### F1: Shader reflection for the editor

- Reflect **specialization constants**: id, name, type and default value.
- Reflect every parameter the editor puts on one side of the split, with its name and type: resources,
  the push-constant block and its members, and the constants above. Keep it independent of the set and
  binding numbers, which no longer come from the source.

**Gate:** a Slang shader with no bindings, two specialization constants and a push-constant block
reflects all of them, by name, from C++ and through the C API.

**Built:** as planned. `Shader::BlockLayout().specializationConstants`, `koral_shader_parameter` and its
siblings in C, `Shader.Parameters` in C# and Kotlin. `SetEntryPoint(module, entry)` now means Slang without
`SetLang`, and GLSL without `layout(binding)` compiles.

### F2: Bindings assigned from outside the shader

- `Shader::Builder` takes an assignment of parameters to sets and bindings, keyed by parameter name.
  The framework writes it into the SPIR-V (`OpDecorate DescriptorSet/Binding`) before the module is made,
  and any bindings in the source are overwritten.
- Descriptor set layouts are built from the assignment, so a pipeline gets exactly the two layouts of
  the split.
- Changing the assignment rebuilds the pipeline without recompiling the source.

**Gate:** one compute shader, run with every resource on the pipeline's set, then with some moved to the
per-entity set, gives the same result both ways with no validation errors, and isn't recompiled in
between.

**Built:** the assignment is on the **pipeline** builder, not the shader: `SetBinding(name, set, binding)`
on all three pipeline builders. One compiled shader then serves any number of pipelines that split it
differently, which is what one shader node feeding several pipeline nodes needs. Specialization constants
are set by name too (`SetSpecializationConstant("radius", 4)`). Sets a pipeline skips are empty layouts.

### F3: Builder descriptions, generated

- A generator reads the C API (`koral_c.h`, which already has a builder for every node type, and is what
  the C# and Kotlin generators already parse). It writes a description of each builder: its settings,
  their types and defaults, enum values, and which ones are required.
- It ships with the SDK (a JSON file, plus a C++ table), with a `--check` ctest like the other generators.
- The engine makes a node per builder from it, and calls the builder through it.

**Gate:** the description covers every builder in `koral_c.h`, and a test builds an image, a sampler and
a compute pipeline purely from descriptions plus values.

**Built:** `kor::builders` in `builderDescriptions.h` (`All()`, `Find("Image")`, `Build(description,
constructor values, calls)`), and `share/Koral/builders.json`. Two differences from the plan. Defaults are
not listed: a setting never given leaves the builder's own default, so nothing is restated. And "required"
is not listed: a build that lacks something comes back poisoned with the builder's own error, which is what
a node shows. Descriptor sets and frame-graph pass builders are left out on purpose.

### F4: Frame-graph passes whose resources come from a query

- A pass can declare a **set of resources supplied at run time** (by the engine, from an ECS query)
  instead of naming each one. The graph orders passes by them and synchronises them, as it does named
  resources.
- The graph is reordered only when the set's membership changes, not every frame.

**Gate:** two passes, one writing per-entity images and one sampling them, order themselves correctly
as entities are added and removed, with synchronisation validation clean.

**Built:** `kor::ResourceSet` and `FrameGraph::ImportSet(name, set)`. Passes `Read` and `Write` the set's
name and look its members up as they record (`PassResources::SetNamed`). Because the order depends only on
the name, a membership change rebuilds nothing at all. Sets change between frames; a change while passes
record is refused. On a device whose async compute queue is a separate family, a pass using a set runs on
the graphics queue. In C, C# and Kotlin too.

### F5: A node editor in koral-ui

- Nodes with typed input and output ports, wires, pan and zoom, selecting and moving several nodes (with
  a selection rectangle), deleting, copying and pasting, and comments or groups.
- Ports refuse wires of the wrong type, and the editor can show a node as invalid and say why.
- In C++ first, since the editor is C++. C, C# and Kotlin follow koral-ui's usual layers.

**Gate:** a graph of a few hundred nodes edits smoothly, and every interaction above is covered by
tests that drive the pointer.

**Built:** `kui::NodeEditor(graph, options)` in `kui/nodes.h`: the graph is the caller's and every change
comes back as a callback. Dragging in a 300-node graph costs under a millisecond of the view's work a frame
in Release. Copy, paste and duplicate are callbacks (the engine serialises). Still to do: the C, C# and
Kotlin layers, and popups opened from controls inside a zoomed node are placed as if it were not zoomed.

## Engine

### E1: ECS core

kecs, from the earlier plan, adapted:

- Component types defined at run time: size, alignment, and copy, move and destroy operations, so
  components holding GPU resources keep their reference counts right. Types are named by reflection.
- Archetypes, entities (index plus generation), tags (components without data), and a per-entity enable
  bit on every component that doesn't move the entity to another archetype.
- Queries with required, excluded and optional components, tags, and enabled-only matching. Change
  ticks, deferred commands, and a parent/child hierarchy.

**Gate:** unit tests for all of the above, clean under ASan and UBSan, plus a million entities created,
churned and iterated, with timings recorded.

**Built** (2026-10-08): `modules/kecs`, described in [kecs.md](kecs.md). 25 tests pass in Debug, in
Release and under ASan and UBSan. A million entities: created with two components in 36 ms, a third
component added to half in 21 ms, one removed from a quarter in 10 ms, a third destroyed in 21 ms, and
half a million walked with two components in 0.3 ms (Release). kecs depends on nothing in the framework,
so it is tested without a GPU. Observers are in. Two things are left for when they are needed: a way to
list archetypes for a tool, and a C API (E6).

### E2: Component types that change shape, and JSON

- A component type can be replaced by a new version. Entities are migrated: fields keeping their name
  and type keep their values, and the others reset.
- Worlds save to and load from JSON through reflection, with run-time component types included.

**Gate:** a component edited three times (a field added, retyped, removed) keeps the values it should
across the edits, and a world round-trips through JSON unchanged.

**Built** (2026-10-08): `World::Reshape` and `kecs::Save`/`kecs::Load`, described in [kecs.md](kecs.md).
35 tests pass in Debug, in Release and under ASan and UBSan. kecs stays free of the framework: it has
its own small JSON reader and writer, and values are written by codecs looked up by field type name.
Numbers, `bool`, `string` and `entity` are built in. **Still to do in the engine:** an adapter that
registers codecs for the framework's types (vectors, matrices, resource references) and declares fields
for types that use the framework's reflection. It moved to E4, where the generated component is the first
thing to need it.

### E3: The graph model

- Engine-owned resources: named, shared and refcounted.
- The scene's pass graph as data. Nodes come from F3's descriptions: object nodes, shader nodes (module
  plus entry point), mesh nodes, value nodes with their operations, command nodes, dependency nodes,
  and clear-only and blit-only passes.
- Evaluating it builds the objects from their nodes' inputs, and rebuilds only what changed. It saves
  to and loads from JSON.

**Gate:** a graph loaded from JSON makes an image, runs a compute shader once (everything bound, so no
component), and clears and blits to a window, without the editor.

**Built** (2026-10-08): `modules/kgraph`, described in [kgraph.md](kgraph.md). The gate is the test
`AGraphLoadedFromJsonClearsComputesAndShows`, run offscreen: the window is the scene's screen image, read
back. 10 tests pass, clean under validation and sync validation. What differs from the plan:

- Draw commands are not in E3. They need a pipeline run per entity, so they come with E4. The command
  nodes here are Clear, Blit and Dispatch. The mesh node exists, as the Mesh builder's node.
- A pipeline with unwired parameters builds and lists them, but nothing runs it yet (E4).
- Between two passes that write the same resource, usage alone orders a pure overwrite before a
  read-and-write. Two that both modify it run in node-list order.
- The kecs adapter for the framework's types moved to E4.
- The framework gained `FrameGraph::Remove`, `kor::capi::*Of` for every resource kind, and the SDK now
  installs kmath's `.inl` headers (without them no SDK consumer could include a vector).

### E4: Pipelines run per entity

- The split: parameters left unset on the pipeline become a generated component (E1 type, E2 migration).
  Pipeline-wide constants are specialization constants, and per-entity constants are push constants.
- Command nodes run per entity over a query on that component, narrowed by tags, skipping disabled
  components. Compute first, then graphics (mesh node plus the user's choice of draw, into a framebuffer),
  then ray tracing.
- Pass order from resource usage, per-entity resources included (F4). A cycle is an error unless one
  side reads the previous frame.
- When a shader changes, wires and components keep what keeps its name and type.

**Gate:** a scene where many entities each own an image that one compute node processes and a graphics
node draws; tags and disabling narrow both; editing the shader keeps what it should.

**Built** (2026-10-08): in `modules/kgraph`, described in [kgraph.md](kgraph.md) under Entities. The gate
is the tests in `test_entities.cpp`: entities each with an image, stamped by a compute node and drawn by
a graphics node; tags and switched-off components narrowing both; a wire added and a shader edited under
them; 500 entities. 18 tests pass in all, clean under validation and sync validation. What differs from
the plan:

- Compute and graphics are built. Ray tracing passes and indirect draws are not.
- The mesh is a component of its own, `kgraph::Mesh`, not a field of the generated one: an entity has one
  mesh however many pipelines draw it.
- Per-entity ordering goes further than F4's one set per name: uses that share a resource are merged into
  one set, so no wire has to say that two passes touch the same entity images.
- A cycle is left to the frame graph, and a graph can't yet ask to read last frame's value.
- The kecs adapter is the codecs (`kgraph::RegisterCodecs`). Declaring a C++ component's fields from
  `KORAL_REFLECT` is not done.
- The framework gained a descriptor pool that grows (it was fixed at 1,000 sets, which per-entity sets
  exhaust), `CommandBuffer::PushConstantBytes`, and `kor::capi::Adopt`.

### E5: The editor

- The node editor (F5) over the scene's graph, an inspector for nodes and entities, an scene tree, and the
  scene's view (an offscreen scene in a panel), all on koral-ui.
- Editing is live: a change to the graph, a node, an entity or a shader shows in the next frame.
- Projects open, save (JSON) and run from the editor and from the Hub.

**Gate:** E4's scene is built from nothing in the editor, saved, reopened, and runs the same outside it.

**Built** (2026-10-08): `modules/keditor` and the programs `koral-editor` and `koral-play`, described in
[editor.md](editor.md). The gate is `DocumentTest.ASceneBuiltFromNothingIsSavedAndRunsTheSameFromItsFile`:
E4's scene made one edit at a time through the same calls the panels make, saved, and run again from the
file by the scene `koral-play` runs, with the same picture to the pixel. The interface is tested offscreen
(what it shows, and clicks on the scene tree, a node and the add-node dialog) and was looked at as rendered.
8 tests, clean under validation and sync validation. What differs from the plan:

- **The Hub is not done.** Projects open, save and run from the editor and from `koral-play` only.
- No undo, no copy and paste, no renaming a node, a flat scene tree, one scene a project in the interface.
- kgraph gained a Primitive node, since a graph could not make a mesh with vertices in it.
- The framework gained `kor::log::Quiet`, so a half-made node failing after each edit isn't logged.

### E6: Scripts

- A C API for the ECS, and C# and Kotlin layers over it.
- `Script`: a class whose fields are a component type (data in the ECS) and whose lifecycle methods are
  called on each component (created, started, update, late and fixed update, enabled, disabled,
  destroyed).
- `GraphicsScript`: makes input resources that nodes take, such as turning lights into cameras.
- Hot reload in both languages, migrating changed component types by the E2 rule.

**Gate:** shadow mapping where a `GraphicsScript` turns light entities into cameras and the graph draws
their shadow maps, and a `Script` moves the lights, all edited and reloaded while it runs, in C# and in
Kotlin.

**Built** (2026-10-08): described in [scripts.md](scripts.md).

- `modules/kscript`: a script type as the engine sees it, a component with functions called on each one.
- `modules/kengine`: the engine as one shared library with its scenes and its C interface, `koral_engine.h`.
- `bindings/csharp`: `Koral.Engine` and the runner `koral-engine-dotnet`.
- `bindings/kotlin`: the Gradle library `koral:koral-engine`.

The gate runs three times on one scene: with the scripts as C functions (which also saves the project),
in C# and in Kotlin. Each checks the picture: the shadow under the occluder, following the light as a
script moves it, and following four edits made while the scene runs. 75 tests in ctest, clean under
validation and sync validation. What differs from the plan:

- **One light.** The graphics script writes one light's camera into a buffer node. Shadow maps for many
  lights need a pass that runs per light and per mesh, which a graph can't yet say.
- **No play and stop.** Scripts run whenever the scene does.
- **Kotlin has no runner or project template**, and was not run in a window.
- Script fields are numbers, `bool`, vectors, `Mat4`, `Quat` and `Entity`. No text, lists or resources.
- The framework gained public handles in its Kotlin binding, a `NativeScene` there, and `Koral.Engine` in
  the C# binding's friends.

### E7: Batching (opt in)

- Rewriting per-entity resources into arrays indexed by an entity number from the GPU. Compute uses an
  extra dispatch dimension. Graphics uses multi-draw-indirect over shared mesh buffers, and per-entity
  push constants become a buffer.
- The editor says when a node can't be batched and why, and runs it per entity instead.

**Gate:** a batched node gives the same picture as unbatched, and 10,000 entities are measurably faster.

**Begun** (2026-10-24). Multi-draw-indirect over shared mesh buffers, and push constants as a buffer, were already
there before E7 was begun: MeshPack, IndirectDraws and the Draw-filled `[entity("PER_INSTANCE")]` buffer (see
[kgraph.md](kgraph.md#each-instances-said-by-the-shader)). What E7 has added so far is what made them cheap, and a way
past entities:

- **A buffer of instances is written only where something changed.** kecs stamps each entity when its world matrix
  changes (`MovedAt`, `MoveClock`) and says when a component was last written (`ChangedTickOf`). The runtime keeps the
  buffer's elements, lays out again only those whose entity changed, writes into each frame's copy only what changed
  since that copy was written, and spreads what it looks at and lays out over every core. MeshPack no longer walks every
  entity each frame to know its meshes are the same. `ScaleTest` (`modules/kgraph/tests/test_scale.cpp`): 100,000
  entities of one mesh in a pass and its prepass took 13 ms of `Runtime::Update` a frame whatever moved; now 0.04 ms
  when nothing moves, 0.5 ms when 1% do, 3.3 ms when all do.
- **Many of one mesh, with no entity each:** the `Instancer` component and the engine's Instances, CullInstances and
  SplatPoints functions (see [kgraph.md](kgraph.md#many-of-one-mesh-on-the-gpu)). Made, culled, given a level of detail —
  the mesh near, points far, never many more points than pixels — and drawn on the GPU. 100 million cost the CPU 0.08
  ms a frame and the GPU 0.3 ms. 100 million entities could not be: kecs alone would need some 20 GB for them.

- **A Draw `batched`** of a Filter's or a ForEach's meshes is written out as one indirect draw of them all (a grouped
  Filter, a MeshPack and an IndirectDraws, `<draw>/…`), its shader opting in with a `[entity("PER_INSTANCE")]` buffer;
  one wired a value of each element draws one at a time and its note says why (see
  [kgraph.md](kgraph.md#batched-a-draw-of-each-element-as-one)). **The gate, for graphics:**
  `ScaleTest.ABatchedDrawDrawsTheSamePictureForLess` — ten thousand entities of two meshes, the same picture pixel for
  pixel, 7.8 ms of CPU a frame a draw each and 0.19 ms batched.

- **One Dispatch of a Filter's entities** (`entities`): its groups once for each, along z, each element its
  `[entity("PER_INSTANCE")]` buffer's, the images each holds read (`TEXTURE`) or written (`STORAGE`) by their bindless
  indices and told to the frame graph as such (see [kgraph.md](kgraph.md#dispatch-and-draw)). **The gate, for compute:**
  `EntityTest.OneDispatchOfEveryEntityStampsWhatADispatchEachDoes` — the same images, so the same picture — and
  `ScaleTest.OneDispatchOfTenThousandEntitiesCostsLess`: ten thousand entities, an image each, 14 ms of CPU a frame a
  Dispatch each and 0.3 ms as one.

- **The editor says it**: a Draw or a Dispatch of many elements says in the inspector how many it runs a frame, and
  what it would take to be one (`Runtime::BatchingOf`); a Draw whose shader reads a buffer of instances already is
  offered `batched` at a click (see [editor.md](editor.md#a-node-in-the-inspector)).

- **What is in sight, on the GPU, for any indirect draw of entities:** the engine's CullInSight function, the frustum
  cull forward-plus had of its own made general — any buffer of instances, by its stride and where its matrix is —
  with IndirectDraws in eGPU (see [kgraph.md](kgraph.md#many-meshes-in-one-indirect-draw)).

Still to do: more elements than a dispatch counts along z (65,535), which needs `vkCmdDispatchBase` in the framework;
forward-plus moved onto CullInSight in place of its own frustum cull; and a batched Draw given `cull` culled on the GPU by
itself, which needs its shader to find its instance through `visible`.

### E8: A graph made both to try ideas in and to make them fast

Planned 2026-10-21. The graph is to be friendlier without losing anything Vulkan can do, and to be where a frame is
made faster as well as where it is first put together. What is decided is in [ADR 0001](adr/0001-graph-projects-the-framework.md):
the graph shows what `kor::` has, and friendliness comes from Functions that can always be opened. The words are in
[GLOSSARY.md](../GLOSSARY.md): **Page**, **Pass**, **Function**, **Bundle**, **Frame view**. Each step is usable by itself,
and they are taken in this order:

1. **A wire let go on a node offers its ports.** From an output, the node's inputs that can take it; from an input, its
   outputs that can give it. The list includes optional ports not shown, inputs placed in the inspector, and what a
   node gives (`size`, `extent`, `bytes`). A port that can't take the wire says why on a greyed line. One already wired
   says what it is wired from, and choosing it replaces that. Typing filters the list. It always opens, with the first
   port that fits already chosen, and is never wired without asking. `kui::NodeEditor` gains
   `onWireDroppedOnNode(from, fromOutput, node, at)` (GFX, beside `onWireDropped`), and keditor builds the menu.
   **Gate:** an image wire let go on a Draw lists its unshown `param.` image ports and wires the one chosen; a wire
   from a Blit's `source` let go on an Image lists `out` and `size`; covered by keditor tests. **Built** (2026-10-21).
2. **Cost on the canvas.** A Pass shows its GPU time on a badge, and is tinted by its share of the frame. A page in
   the list shows its total, and a Function's call shows the sum of the Passes in it (`Runtime::Timings`).
   **Gate:** forward-plus shows each Pass's cost without opening an inspector. **Built** (2026-10-21).
3. **The add-node list by intent.** It is ranked by what the wire carries, by the page's rate and by what is near.
   Functions come first, grouped by what they are for (Shadows, Post, Culling, Physics, Sort and scan), and every
   builder and command node is under **Vulkan**. Search covers descriptions and tags. **Gate:** "blur" and "shadow"
   find the Functions that do them. **Built** (2026-10-21): a function's file says its `purpose`, `description` and `tags`; the
   engine's nine say theirs. No function blurs or shadows yet: that is step 7.
4. **Async compute as an input.** A Dispatch (and whatever else `kor` lets run there) gets `async`, as it has
   `enabled`. On a device without a second queue, the node's note says it runs on graphics. **Gate:** a Dispatch set
   async is scheduled async (`FrameGraph::Schedule`), and saved and loaded so. **Built** (2026-10-21). The `aliasing`
   switch is not: every resource of a graph is made by its node and imported into the frame graph, and the framework
   aliases only what it makes itself (`PassBuilder::Create`). Aliasing in the graph waits for **transient** Image and
   Buffer nodes, made by the frame graph for the frame, which is a later step.
5. **Bundles.** ForEach, Filter and Entity give `element`, a Bundle of the component's fields and the transform. A
   **Bundle** node builds one and an **Unbundle** takes one apart, and a Function's output may be one. A Bundle fills the fields it reaches
   by exact name, and a wire to a single field takes precedence. What doesn't match stays an open port, and the node's
   note names the unmatched fields on both sides. A Bundle let go on a node offers **By name: n of m fields** first,
   and on a BufferBuilder or ShaderBuffer, the whole buffer. This lifts the "No node gives a struct" limit in
   [kgraph.md](kgraph.md#limits). **Gate:** a ForEach of PointLight's `element` wired to a ShaderBuffer fills
   its fields; a field wired by itself takes precedence; a misspelled field is named on the node. **Built** (2026-10-21).
6. **The Frame view.** It shows only the Passes, in the order `FrameGraph::Schedule` runs them, grouped by dependency
   level, a lane for each queue, and a bar as wide as GPU time. Culled and skipped Passes are greyed, with the reason,
   and each resource is drawn from its first write to its last read, with `Memory()` used and saved by aliasing. It is
   read-only: clicking goes to the node on its page. It is not a wall-clock GPU timeline: that waits for `kor` to
   record when each pass starts. **Gate:** forward-plus's Frame view lists every Pass that ran, and only those, in
   order. **Built** (2026-10-21).
7. **The engine's first Functions:** Camera (a Bundle out), SunShadows, Bloom and Tonemap, made from
   forward-plus's graph, their shaders the engine's (`koral_shadow`, `koral_bloom`, `koral_tonemap`). **Gate:**
   forward-plus rebuilt on them (backed up first, its graph alone migrated, checked with `koral-play`) has under 60
   nodes at the top, down from 182, and draws the same picture. **Built** (2026-10-21), but for the count: it draws
   the same picture to the pixel, and has 144 nodes. The gate's 60 was a guess made before the graph was counted: 54
   of what is left are References, and the pages these four don't touch (occlusion, reflections, sky, light culling,
   the lamps' shadows, the forward pass) are forward-plus's own. Under 60 takes those as the project's own functions,
   or a page that brings in what it uses without a node for each.

Alongside them, **Page** replaces "pass" in the editor and the docs (the **Pages** panel, THIS PAGE). `Graph::passes`
and `PassRate` become `pages` and `PageRate` in one change later, writing `"pages"` and reading `"passes"` as well.

## First steps

The framework milestones and **E1** to **E6** are done, but for the Hub's part of E5. **E7**, batching, has passed its gate for graphics and for compute:
what is left of it is in its section.
