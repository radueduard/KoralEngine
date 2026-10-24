# Node survey: what kgraph has, what kor can do, what the industry offers, and what to add

Research report, 2026-10-23. Sources: `modules/kgraph/src/catalog.cpp` (every node type), `modules/kgraph/src/runtime.cpp`,
`docs/kgraph.md`, `docs/editor.md`, `packages/koral/functions/*.kfunc`, `samples/forward-plus/forward_plus.h`, the kor
headers in `/mnt/w/CLionProjects/GFX/include` and `/mnt/w/CLionProjects/GFX/modules/*/include`, and the web sources cited
in section 3.

Abstraction levels used throughout:

| Level | Name | What it means here |
|---|---|---|
| **L0** | Value | Pure CPU values: constants, math, vectors, matrices, logic, camera/lens math. No GPU object. |
| **L1** | Resource | A GPU object: image, buffer, sampler, view, pipeline, mesh, acceleration structure, framebuffer. |
| **L2** | Command | One GPU operation, i.e. one pass in kor's frame graph: Draw, Dispatch, TraceRays, Blit, Copy, Clear. |
| **L3** | Scene/data binding | Reads the ECS world or turns per-entity data into GPU data: Entities, Filter, ForEach, MeshPack, RayScene, ShaderBuffer. |
| **L4** | Technique | A whole rendering/simulation technique as one node or Function: Bloom, SunShadows, Tonemap, XpbdStep. |

"Composite" means the node is made of other nodes (a `.kfunc` Function, a Repeat loop, a Bundle) or expands into
several passes/objects at runtime. "Atomic" means it maps to one kor object, one command, or one CPU computation.

---

## 1. Inventory of existing kgraph node types

`kgraph::Catalog()` builds **171 node types**: 12 from kor's builder descriptions plus 159 engine types. Add the
**13 engine Functions** in `packages/koral/functions` and the total classified here is **184**. Two types are kept
only for old graphs and are hidden from the node list (`Number`, `Vector`). `Shader` is built but not offered
(`listed = false`), because pipelines name their shaders themselves. The editor also offers per-component *presets*
(`PointLight` and `PointLight Buffer`), but these are saved as ForEach and BufferBuilder nodes, so they are not
separate types.

Category is the `NodeType::category` the editor colours by (`editor.cpp:3767`): Resources, Pipelines, Passes, Values,
Math, Vectors, Logic, Cameras, Scene, Flow, Functions.

### 1.1 Builder-derived types (kor `builders::All()`, `src/core/builders/builders.g.cpp`)

Each builder setting becomes an input. A setting with several arguments becomes `name.arg` inputs, and a repeatable
one becomes `name[].arg` inputs. A pipeline's `binding` and `specializationConstant*` settings are hidden; wired
`param.<name>` and `constant.<name>` inputs stand in for them.

| Node | Category | Purpose | Level | Composite? |
|---|---|---|---|---|
| Buffer | Resources | `kor::Buffer`: instanceCount, usage, memory type, isPerFrame, sharedAcrossQueues (no `data`: bytes are not held in a graph) | L1 | Atomic |
| Image | Resources | `kor::Image`: type, format, extent, mips, layers, samples, usage, isPerFrame | L1 | Atomic |
| ImageView | Resources | A view of part of an image (mip range, layer range, view type, swizzle) | L1 | Atomic |
| Sampler | Resources | Filtering, addressing, LOD, anisotropy, compare | L1 | Atomic |
| BufferView | Resources | A texel-buffer view (format, offset, range) | L1 | Atomic |
| Shader | Resources (unlisted) | One shader stage by module and entry point. Pipelines make their own, so it is not offered | L1 | Atomic |
| GraphicsPipeline | Pipelines | Vertex (+tess/geometry) or mesh (+task) pipeline with its fixed-function state and framebuffer | L1 | Atomic. Borderline: it also reflects the shader into `param.*`/`constant.*` ports, but it is still one kor object |
| ComputePipeline | Pipelines | A compute shader. It offers `numThreads` | L1 | Atomic |
| RayTracingPipeline | Pipelines | Raygen, miss[], hitGroup[], callable[], maxRecursionDepth | L1 | Atomic |
| Framebuffer | Resources | Colour attachments, depth/stencil and resolve mode | L1 | Atomic |
| Mesh | Resources | Vertex buffer, index buffer and vertex layout | L1 | Atomic |
| AccelerationStructure | Resources | BLAS from meshes/geometries, or TLAS from instances | L1 | Atomic |

### 1.2 Values (category "Values")

| Node | Purpose | Level | Composite? |
|---|---|---|---|
| Number *(hidden, legacy)* | A number of no particular kind | L0 | Atomic |
| Float | A 32-bit float | L0 | Atomic |
| Double | A 64-bit float | L0 | Atomic |
| Int | A 32-bit signed integer | L0 | Atomic |
| UInt | A 32-bit unsigned integer | L0 | Atomic |
| Bool | True or false | L0 | Atomic |
| String | Text | L0 | Atomic |
| Vector *(hidden, legacy)* | 2–4 numbers | L0 | Atomic |
| Vec2 / Vec3 / Vec4 | float2–float4 | L0 | Atomic |
| IVec2 / IVec3 / IVec4 | int2–int4 | L0 | Atomic |
| UVec2 / UVec3 / UVec4 | uint2–uint4 | L0 | Atomic |
| Color | A picked RGBA colour | L0 | Atomic |
| Constant | π, τ, e, √2, φ | L0 | Atomic |
| Component | One component of a vector, by index | L0 | Atomic |
| Split | A vector's x/y/z/w as separate outputs | L0 | Atomic |
| Append | Concatenate two vectors or numbers | L0 | Atomic |
| ImageExtent | An image's width/height/depth as a vector | L0 | Atomic. Borderline: it reads an L1 resource, but its output is a CPU value |
| VertexLayout | A list of vertex attributes as `KoralVertexLayout` | L0 | Atomic. Borderline: it describes GPU data, but it is a plain struct value and creates no GPU object |

### 1.3 Math (category "Math"). Each is L0 and atomic, and works on scalars or component-wise on vectors

| Node | Purpose |
|---|---|
| Add, Subtract, Multiply, Divide | a ± × ÷ b |
| Min, Max | Lesser or greater of a and b |
| Pow | a to the power b |
| Mod | Wrapping remainder |
| MultiplyAdd | a·b + c |
| Negate, Abs, Sign | −a, \|a\|, sign of a |
| Floor, Ceil, Round, Trunc, Fract | Rounding, and the fractional part |
| Saturate, OneMinus, Reciprocal | clamp(a, 0, 1), 1 − a, 1 / a |
| Sqrt, InverseSqrt | √a, 1/√a |
| Exp, Exp2, Log, Log2, Log10 | Exponentials and logarithms |
| Sin, Cos, Tan, Asin, Acos, Atan, Atan2 | Trigonometry, in radians |
| Sinh, Cosh, Tanh | Hyperbolic functions |
| Radians, Degrees | Angle conversion |
| Clamp | Clamp x between min and max |
| Lerp, InverseLerp | Interpolate, and its inverse |
| Remap | Map a value from one range to another, without clamping |
| Smoothstep, Step | Hermite step and hard step |

(45 types in total.)

### 1.4 Vectors (category "Vectors"). Each is L0 and atomic

| Node | Purpose |
|---|---|
| Length, Distance | Magnitude, and distance between points |
| Normalize | Unit vector |
| Dot, Cross | Dot and cross products |
| Reflect, Refract, Project | Reflection, refraction (with eta) and projection onto a vector |
| Linear | sRGB-ish colour to linear (pow 2.2), with alpha kept |
| MatrixMultiply, MatrixInverse, MatrixTranspose, MatrixDeterminant | 4×4 matrix algebra |
| MatrixTranslate, MatrixRotate, MatrixAxisAngle, MatrixScale, MatrixCompose | Building transforms (TRS composed as the world does it) |
| TransformPoint, TransformDirection, TransformPlane | Apply a matrix to a point (with w divide), a direction, or a plane |

(21 types.)

### 1.5 Logic (category "Logic"). Each is L0 and atomic

Less, LessOrEqual, Greater, GreaterOrEqual, Equal, NotEqual (comparisons that give a bool); Not, And, Or, Xor
(boolean); Select (a when the condition is true, else b). That is 11 types.

### 1.6 Cameras (category "Cameras")

| Node | Purpose | Level | Composite? |
|---|---|---|---|
| View | lookAt view matrix plus its inverse and basis vectors. `main` replaces it with the editor camera | L0 | Atomic |
| Perspective | Perspective projection with reversed-Z and infinite-far options | L0 | Atomic |
| Orthographic | Orthographic box projection | L0 | Atomic |
| Frustum | Off-centre perspective (stereo, tiles, portals) | L0 | Atomic |
| Oblique | Moves a projection's near plane onto a world plane (mirrors, water) | L0 | Atomic |
| Paraboloid | Dual-paraboloid parameters and a turn matrix | L0 | Atomic |
| CubeFace | Direction and up vector of cube face *n* | L0 | Atomic |
| ShadowCascades | Four fitted CSM matrices, splits and texel sizes | L0 | Atomic. Borderline: it is technique logic (practical split scheme, texel snapping) but produces only values, so it stays L0. It is the value half of the L4 SunShadows |
| PointShadows | Six matrices per point light, with round-robin `idle` scheduling | L0 | Atomic. Same reasoning as ShadowCascades; it also keeps per-frame state for the round-robin |

### 1.7 Engine resources (category "Resources", not builder-derived)

| Node | Purpose | Level | Composite? |
|---|---|---|---|
| Screen | The window's swap-chain image and its size and aspect | L1 | Atomic |
| Reference | Stands for another node's output by id, so pages can share resources without wires | L1 | Atomic. It is graph plumbing, an alias, and typed as a resource |
| ImageFile | An image loaded from disk with its mips generated | L1 | Atomic |
| Script | A project `ResourceScript` that makes images, buffers, views and samplers, filled on the CPU | L1 | Composite: it expands to several resources, and to whatever the script declares |
| DataBuffer | A uniform or storage buffer packed from wired values in std140/430 order | L1 | Atomic. Borderline: L3 when its values come from the scene, but the node itself only packs values |
| ShaderBuffer | A buffer laid out from a shader's reflected struct, one input per field. Fed by a ForEach, it holds one element per entity | **L3** | Atomic (one buffer). L3 because it is the bridge from ECS elements to GPU structs |
| BufferBuilder | A buffer whose struct is laid out on the node. Fed by a ForEach, it holds one element per entity | **L3** | Atomic. Same reasoning as ShaderBuffer |
| Primitive | One of the built-in default meshes | L1 | Atomic |
| MeshBuffers | A mesh's vertex, index, normal and UV buffers as storage buffers, for compute | L1 | Atomic |
| MeshPack | The distinct meshes of a Filter packed into one vertex/index pair, with ranges and bounds | **L3** | Composite at runtime: it copies N meshes, can read back to the CPU, and gives several outputs |
| Meshlets | A MeshPack cut into meshlets with bounding spheres and normal cones (CPU, meshoptimizer-style) | **L3** | Composite at runtime: CPU processing plus 4 buffers |
| RayScene | One BLAS per packed mesh and a TLAS of every Filter entity, rebuilt on motion | **L3** | Composite: N BLASes, 1 TLAS and 2 buffers |
| IndirectDraws | `VkDrawIndexedIndirectCommand`s from pack ranges × Filter groups, in CPU or GPU-count mode | **L3** | Atomic (one buffer, plus `visible`). L3 because it is driven by ECS groups |

### 1.8 Scene (category "Scene")

| Node | Purpose | Level | Composite? |
|---|---|---|---|
| Entities | Every entity, iterated one element at a time | L3 | Atomic. It is a loop source: what is wired from it is evaluated once per element |
| Filter | Keeps entities with/without given components, `where` values, groupBy and a single id; gives groups | L3 | Atomic (loop source) |
| ForEach | Every entity with a component, with its fields and transform, plus `repeat` per entity | L3 | Atomic node, but it turns what is downstream into a loop. Treated as a container in the editor |
| Entity | One entity: the first with a component, or one by name | L3 | Atomic |
| ComponentValue | One field of one entity's component | L3 | Atomic |
| EntityTransform | One entity's world and local transform | L3 | Atomic |
| Selected | The entity picked in the editor, and whether a pick is pending | L3 | Atomic |
| FixedStep | dt, steps, count, lost and alpha of the fixed-step clock | L0. Borderline: it reads scene time, not entities, so it is placed with values (the only clock node in kgraph) | Atomic |
| WriteBack | Reads a GPU buffer back and writes positions, rotations and fields into entities | L3 | Atomic: one readback plus ECS writes |

### 1.9 Flow and Functions

| Node | Category | Purpose | Level | Composite? |
|---|---|---|---|---|
| Repeat | Flow | A loop: everything wired downstream exists once per turn (≤256) | — (structural). Classified as L0 because its outputs are numbers | **Composite-maker**: it unrolls its downstream subgraph |
| Previous | Flow | The previous turn's output of a loop node | — (structural) | Part of loop composition |
| Element | Flow | One turn's output of a loop, or one output of a Function call | — (structural) | Part of loop/function composition |
| Collect | Flow | Gathers every ForEach element's value into one list | L0, gathering values from L3 iteration | Atomic |
| Bundle | Flow | Many named values on one wire | L0 | **Composite** (a struct of wires, written out before evaluation) |
| Unbundle | Flow | Splits a Bundle into named outputs | L0 | Atomic |
| Switch | Flow | Picks one of N inputs (resource or value) by index | Level of what passes through (L0/L1) | Atomic |
| DebugView | Flow | Registers an image as an editor scene-view mode, and gives `shown` | L2-adjacent (debug). Classified as L1 because it passes an image through | Atomic |
| Note | Flow | A comment on the canvas | — | Atomic |
| Function | Functions | Calls a `.kfunc`; its nodes are written out as `<call>/<node>` | Level of the called function (usually L4) | **Composite** |
| FunctionInput | Functions | An input edge of a function | — | Part of composition |
| FunctionOutput | Functions | An output edge of a function | — | Part of composition |

### 1.10 Passes (category "Passes"). Every one takes `enabled` and `views`; the async-capable ones also take `async`

| Node | Purpose | Level | Composite? |
|---|---|---|---|
| Clear | Fills an image with a colour | L2 | Atomic |
| Blit | Scaled image copy (region, mip, layer, filter). This is how a picture reaches the screen | L2 | Atomic |
| Resolve | MSAA resolve | L2 | Atomic |
| Copy | Image-to-image copy of the same size | L2 | Atomic |
| CopyBuffer | Buffer-to-buffer copy (with `idle`) | L2 | Atomic |
| ClearBuffer | Zeroes a buffer range | L2 | Atomic |
| CopyBufferToImage | Copies buffer bytes into image texels | L2 | Atomic |
| CopyImageToBuffer | Copies image texels into buffer bytes | L2 | Atomic |
| Keep | Copies a buffer or image as it is at this point among the passes (the previous state) | L2 | Atomic. It makes a resource and a copy pass |
| GenerateMipmaps | Blit-chains an image's mips | L2 | Atomic. kor does it as one command, though internally it is N blits |
| Pick | Reads back one pixel of an ID image on demand | L2 | Atomic |
| DebugDraw | Draws the scene's kor `DebugDraw` shapes over an image | L2 | Atomic. It wraps `kor::DebugDrawPass` |
| Dispatch | Runs a compute pipeline: groups, cover-an-image, or indirect. Once, or per ForEach element | L2 | Atomic (per element) |
| TraceRays | Runs a ray tracing pipeline | L2 | Atomic |
| Draw | Every draw kind (direct, indexed, mesh tasks, indirect, indirect-count), the render-pass load/store/clear, and per-draw dynamic state, culling, `idle` and `whenChanged` | L2 | Atomic per element. Borderline: wired from a Filter it issues one draw per entity, with CPU frustum culling, but it is still one pass |

### 1.11 Engine Functions (`packages/koral/functions/*.kfunc`). All are composite

| Function | Purpose (`purpose` tag) | Level | Built from |
|---|---|---|---|
| Scan | Exclusive prefix sum of uints, up to 1M (Sort and scan) | L4. It is a *compute primitive*, and is classified as a technique only because it is a whole algorithm | 2 Buffers, 3 ComputePipelines, 3 Dispatches (`koral_scan`) |
| RadixSort | Stable 4-bit radix sort of key/value uints, up to 16M | L4 (primitive) | Repeat×8, Switch×4 ping-pong, 2 pipelines/dispatches, and a call to **Scan** |
| Compact | Stream compaction by flags | L4 (primitive) | Buffers, CopyBuffer, Dispatch, and a call to **Scan** |
| IndirectArgs | Turns a GPU counter into dispatch-indirect args | L4 (primitive). Borderline L2: it is one dispatch | 1 Buffer, 1 pipeline, 1 Dispatch |
| Camera | The scene camera as view/projection/viewProjection, plus a camera Bundle | **L3** (reads the ECS; makes only values) | Screen, Entity, View, Perspective, MatrixMultiply, Bundle |
| SunShadows | Four-cascade sun shadow atlas, redrawn only on change | L4 | Image, Framebuffer, Repeat×4, Filter, GraphicsPipeline, Draw (`koral_shadow`) |
| Bloom | Six-level down/up bloom, in place | L4 | 2 Images, 3 ImageViews, 2 Repeats, Previous, Elements, 3 pipelines, 3 Dispatches |
| Tonemap | Exposure, ACES fit and display encode into an 8-bit image | L4 | Screen, Image, 1 pipeline, 1 Dispatch |
| SpheresStep | GPU sphere physics (sequential impulses, hashed grid, colouring) | L4 (simulation) | 13 Buffers, 11 Dispatches, and calls to **RadixSort**, **IndirectArgs** and **Scan** |
| XpbdStep | GPU XPBD rigid bodies: contacts, joints, sleep, meshes | L4 (simulation) | 10 Buffers, 11 Dispatches, Repeat, and a call to **RadixSort** |
| XpbdPrepare | Mass, inertia and reach from shape | L4 (simulation helper) | 1 Buffer, 1 Dispatch |
| XpbdFromComponents | Bodies, materials and shared state from RigidBody/Collider/Joint data | L4. Borderline L3: it converts component data, but on the GPU | 4 Buffers, 6 Dispatches |
| XpbdRigidBodies | Every rigid body, joint and mesh collider, made ready for XpbdStep | **L3 + L4** (it reads the ECS, then calls a technique) | 2 ForEach, Entities, Filter, 3 BufferBuilders, MeshPack, MeshBuffers, and a call to **XpbdFromComponents** |

### 1.12 Observations from the inventory

* **Distribution of the 171 catalog types:** L0: 115 (67%, counting Repeat, Collect, Bundle, Unbundle and
  FixedStep); L1: 21 (counting Switch and DebugView); L2: 15; L3: 14; structural: 6 (Previous, Element, Note,
  Function, FunctionInput, FunctionOutput). Of the 13 Functions, 11 are L4 and 2 are L3 (Camera, XpbdRigidBodies). The
  graph is rich at the extremes (scalar math, raw Vulkan-shaped commands) and thin at L3/L4. The only rendering
  techniques that ship as a single node are Bloom, Tonemap and SunShadows.
* **Large techniques live as hand-written graphs, not Functions.** `samples/forward-plus/forward_plus.h` builds
  clustered light culling, a depth prepass, GPU frustum and occlusion culling, a visibility mark/gather pass, HBAO with
  a bilateral blur, a sky, screen-space traced reflections, TAA (jitter is advanced by a *compute pass*
  `taa/advance`, because there is no Time or FrameIndex node), bloom, point-light cube shadows, a meshlet branch and
  a ray-traced branch. All of it is raw nodes. Each of these is a ready candidate for a Function (section 4).
* **No clock.** FixedStep is the only time source. There is no frame time, elapsed time, frame index or
  jitter-sequence node.
* **Every pass is atomic.** Composition happens only through Function and Repeat. There is no "subpass group" node
  and no "render feature" grouping above a page.

### 1.13 The engine's own classification, and where it is questionable

Since this survey began, the engine classifies node types itself. `kgraph::NodeType::level` (`kgraph::Level`:
`eValue`, `eResource`, `eCommand`, `eScene`, `eTechnique`) is assigned by `LevelOf()` in `catalog.cpp`, and
`NodeType::composite` is true only for `Function`. The five levels are the same five used here (L0–L4). The rule is:

```
MeshPack, IndirectDraws, Meshlets, RayScene, ShaderBuffer, BufferBuilder, DataBuffer, MeshBuffers, category Scene → eScene
category Functions → Function: eTechnique; FunctionInput/FunctionOutput: eValue
category Passes → eCommand
category Resources/Pipelines → eResource (Reference: eValue)
everything else (Values, Math, Vectors, Logic, Cameras, Flow) → eValue
```

For most types, the engine's level and the one in the tables above agree. The cases where they differ, or where this
report thinks the engine is wrong, are:

| Node | Engine says | This report says | Why |
|---|---|---|---|
| **DataBuffer** | eScene | **eResource** | It packs wired values into a buffer and never touches the ECS. It is the same kind of node as a Buffer with data. If it stays eScene, so should every node a ForEach-derived value can reach, which would be meaningless |
| **MeshBuffers** | eScene | **eResource** | It exposes one mesh's existing buffers as storage buffers. It is a typed view of an L1 resource, and is not per-entity |
| **FixedStep** | eScene (category Scene) | eValue (or eScene, acceptable) | It reads the scene *clock*, not entities. It belongs with the Time/FrameIndex nodes proposed in section 4, which should all share one level. Keeping it eScene is defensible ("state of the running scene"), but then a future `Time` node must be eScene too |
| **Reference** | eValue | **level of its target** (usually eResource) | A Reference stands for another node's output. It is shown on cards as "value" while carrying an Image or a Buffer, so the level badge says the wrong thing. Best: inherit the referenced node's level at display time |
| **Switch** | eValue | level of its options | The same issue: a Switch between two images is a resource router. The `option[]` port is typed `Resource` |
| **DebugView** | eValue | eCommand-adjacent / eResource | It passes an image through and gates passes. It is a debug resource, not a CPU value |
| **Function** (always eTechnique) | eTechnique | **per function** | `Camera` is an L3 scene binding that makes only values. `Scan`, `RadixSort`, `Compact` and `IndirectArgs` are compute *primitives*, not techniques. `XpbdRigidBodies` is L3+L4. Suggest a `"level"` field in `.kfunc` metadata (next to `purpose`) that the call node takes, defaulting to eTechnique |
| **FunctionInput / FunctionOutput** | eValue | structural (or the level of their `kind`) | An `eImage` FunctionInput carries a resource |
| **composite = Function only** | — | also **Script**, **MeshPack**, **Meshlets**, **RayScene**; and *containers*: **Repeat**, **ForEach**/**Filter** | Script makes N resources. MeshPack copies N meshes and reads back. RayScene builds N BLASes and a TLAS. Repeat unrolls its downstream subgraph, and the editor already draws loops as frames. Suggest a three-valued property: `atomic` / `expands` (one node, several GPU objects or passes at runtime) / `composite` (made of user-visible nodes: a Function call). Loop sources could carry a separate `container` flag |
| Collect, Bundle, Unbundle | eValue | eValue ✓ | Agreed. Bundle is a struct of wires, not a composite of nodes |
| ShadowCascades, PointShadows | eValue | eValue ✓ | Agreed. They are the value half of shadow techniques. PointShadows keeps per-frame round-robin state, which no other eValue node does. Worth a note on the card |
| Selected | eScene | eScene ✓ | Editor selection is scene state |
| Keep | eCommand | eCommand ✓ | It records a copy pass, even though it also creates its copy resource |

---

## 2. kor capabilities not exposed as nodes

Every builder *setting* is exposed automatically: a builder that gains a setting gains an input. So tessellation,
geometry and task stages, callable shaders, TLAS instances, stencil op state, depth bounds, depth clamp, per-attachment
blend state and specialization constants (through `constant.<name>`) are all reachable. The gaps are what lives
*outside* the builders: command-buffer commands, frame-graph features, buffer/image methods, the bindless table,
`Context` queries, and the modules. Each row was checked against `modules/kgraph/src` (grep of `runtime.cpp`,
`entities.cpp`, `scene.cpp`, `meshes.cpp`).

### 2.1 Commands (`include/commandBuffer.h`, `class kor::CommandBuffer`)

| Capability | Symbol | State in kgraph |
|---|---|---|
| Write bytes into a buffer from the CPU in the command stream | `CommandBuffer::FillBuffer(buffer, data, offset, size)` (l.1109) | Used internally (runtime.cpp:5605, DataBuffer/BufferBuilder upload), but there is no node that writes literal or Script-made bytes at a point among passes. docs: "A buffer holds no data of its own in a graph" |
| Debug labels and markers | `BeginDebugLabel` / `EndDebugLabel` / `InsertDebugLabel` / `DebugLabel(label, body, color)` (l.886–906) | **Not used at all.** RenderDoc and Nsight captures show no pass names. docs/kgraph.md Limits: "A debug label or a GPU timer round a pass can't be asked for yet" |
| GPU timer scopes inside a pass | `BeginTimer(label)` / `EndTimer` / `Timer(label, body)` / `CollectTimer` / `Timings` / `OnTimings` (l.159–239) | Per-pass timings exist through `FrameGraph::PassTimings`. There are no sub-pass scopes (per Draw element, per loop turn), and timings are not available *as values* in the graph |
| Timer support query | `SupportsTimers()` (l.255) | Editor-only |
| Per-face stencil ops, set dynamically | `SetStencilOp(face, fail, pass, depthFail, compare)` (l.543); `StencilFace` for compare, write and reference | Draw has a single `stencilReference/CompareMask/WriteMask` for both faces and no dynamic op (Limits: "stencil operations are the pipeline's only") |
| Depth bias enable, dynamically | `SetDepthBiasEnable` (l.546) | Draw sets `depthBias` values only |
| Drawing a sub-range of a mesh | `DrawSubMesh(mesh, baseIndex, indexCount)` (l.992); `DrawMesh(mesh, instances, baseInstance)` (l.982) | Reachable through `firstVertex`/`vertexCount` on Draw. No node turns a MeshPack range into a draw |
| Explicit barriers | `Barrier`, `BufferBarrier`, `ImageBarrier` (l.866–873) | Intentionally hidden, because the frame graph derives them. Not a gap for users, but there is no override for UAV-to-UAV ordering inside one pass (a Repeat of dispatches relies on separate passes) |
| Binding a hand-made descriptor set | `BindDescriptorSet(index, set)` (l.587); `DescriptorSet::Builder::Write(binding, Buffer::Slice …, index)` (descriptorSet.h l.194–224) | Used internally. There is no **buffer slice** binding (offset/range of a buffer as a parameter) and no **array element** binding (`index`) for `Texture2D textures[]` |
| Copying descriptors / heap barrier | `CopyDescriptors`, `DescriptorHeapBarrier` (l.837–845) | Not exposed; this is the descriptor-heap model |
| Pushing a bindless handle as a push constant | `PushConstant(name, image/view/buffer/sampler/AS)` (l.785–821) | Partly exposed: `push.<name>` takes values. A resource wired to a `push.*` handle is used for some RayScene/entity textures, but there is no general "bindless index of X" value |
| Present helpers | `BlitToScreen`, `ResolveToScreen` (l.1165, 1198) | Equivalent to Blit/Resolve onto Screen ✓ |
| Native escape hatch | `Run(callback)` (l.1262) | No node, by design. A C++ "native pass" plug-in node would use it |
| CPU-side branching while recording | `If`, `Condition`, `ForEach` (l.1271–1309) | Covered by `enabled`/`idle`/Switch/ForEach ✓ |
| Screen-touch query | `HasTouched(image)`, `ScreenImage()` (l.294–300) | Internal |

### 2.2 Frame graph (`include/frameGraph.h`)

| Capability | Symbol | State in kgraph |
|---|---|---|
| **Transient (aliased) resources** created by a pass, sized relative to the screen or another image | `PassBuilder::Create(name, ImageDesc{format, usage, scale, sizeOf, extent, mipLevels})`, `Create(name, BufferDesc)` (l.45–107); `FrameGraph::SetAliasing(bool)` (l.499); `Memory()` (l.509) | **Not used.** Every kgraph Image and Buffer is a persistent builder object, so a 4K renderer's G-buffer-sized intermediates never share memory. The `scale` and `sizeOf` relative sizing is also absent: kgraph does it by wiring Screen→Divide→extent |
| **Previous-frame resource** (history) | `PassBuilder::ReadPrevious(name[, usage])` (l.136–138); `PassResources::PreviousImageNamed/PreviousBufferNamed` (l.222–223); `RenderPass::HasPrevious` (l.310) | **Not used.** TAA in forward-plus copies into a `history` image by hand (a `keep` dispatch), and `Keep` copies within a frame. A `History` node backed by ReadPrevious would give TAA, reprojection, HiZ-from-last-frame, auto-exposure and SVGF their inputs, with the "is it valid?" flag (`HasPrevious`) as an output |
| Consume (rename-on-write) | `PassBuilder::Consume(name, as[, usage])` (l.115–118) | Not used. The SSA-style "write a new version" model that Frostbite and RDG use |
| Side-effect pass | `PassBuilder::SideEffect()` (l.120) | Used once internally |
| CPU pass in the graph | `class CpuPass` with `Run()` (l.355–364) | **No node.** A CPU-side compute step ordered with the GPU passes (fill a buffer from script logic each frame, a readback consumer) |
| Imported resource sets | `FrameGraph::ImportSet` (l.410) | Used internally for per-entity sets ✓ |
| Schedule, skipped, culled, history introspection | `Schedule()`, `SkippedPasses()`, `CulledPasses()`, `KeptForNextFrame()`, `Times()`, `Timing()` (l.430–481) | Shown by the editor (`Runtime::Frame()`), but not available as graph values, for example for an on-screen profiler overlay drawn by the graph |
| Pass settings reflection | `RenderPass::Settings()`/`SettingsChanged()` (`KORAL_REFLECT`) (l.284–290) | n/a: kgraph passes are nodes |

### 2.3 Resources: images, buffers, samplers, acceleration structures

| Capability | Symbol | State in kgraph |
|---|---|---|
| Initial image or buffer data | `Image::Builder::SetData`, `Buffer::Builder::SetData` (image.h l.366–381; buffer.h l.363–403) | Skipped deliberately (`Kind::eBytes`). Only Script or a setup pass fills resources. A `Bytes`/`Table` value (a small LUT, a Halton sequence, a blue-noise tile) cannot be typed in |
| Resize in place | `Image::Resize(extent)` (image.h l.408) | kgraph rebuilds instead ✓ (works, but invalidates dependants) |
| Format capability query | `Image::IsFormatSupported(format, usage)` (l.522); `IsBlockCompressed` (l.531); `IsDepthStencilFormat`/`IsStencilFormat` (l.655–658) | No node. A graph cannot fall back from `eRGBA16_SFLOAT` storage to something else, or choose a depth format |
| Default views by shape | `Image::View(shape, coverage)` (l.485), `ViewCoverage::eTopLevel/eWholeImage` | Implicit ✓ |
| CPU readback of buffers (sync/async) | `Buffer::Read`, `ReadAt`, `ReadAsync` (Task), `Map()` (buffer.h l.500–1206), `Buffer::Type::eReadback` | Used internally by Watch, WriteBack and Pick. **There is no general "Readback → value" node**, for example an auto-exposure average or a GPU counter read back as a number the graph or a script can use |
| CPU write of buffers | `Buffer::Write`, `WriteAt`, `UploadPerFrame` | Used internally by DataBuffer ✓ |
| Buffer slices | `Buffer::SliceOf(buffer, offset, size)` (l.1120) | Not exposed (see 2.1) |
| Memory types | `eDeviceDynamic` (l.164) | Exposed through Buffer `type` ✓ |
| Sampler border colour / reduction (min/max) | `AddressMode::eClampToBorder` exists, but **no border-colour setter**; **no reduction mode** (sampler.h l.46–181) | Needs **new kor support** for min/max reduction samplers (single-tap HiZ build and depth-pyramid sampling) and border colours (shadow maps outside the atlas = lit) |
| AS: refit/update, compaction, build flags, AABB/procedural geometry, GPU-written instance buffers | `AccelerationStructure::Builder` has only `AddMesh`, `AddGeometry{mesh, ranges}` and `AddInstance{blas, transform, customIndex, hitGroupIndex, opaque, mask}` (accelerationStructure.h l.63–135) | Partly exposed: RayScene rebuilds the TLAS on any motion. **Needs new kor support** for refit, `allowUpdate`, compaction, AABBs (particles, SDF primitives) and GPU-driven TLAS instance input |
| Bindless table | `kor::bindless::Sampled/Storage/StorageBuffer/UniformBuffer/Sampler/AccelerationStructure(...)` → `DescriptorIndex`, `Release`, `Count`, `CollectBindless` (bindless.h l.120–175) | Used for entity textures and RayScene's `scene` buffer. There is **no node that gives a resource's bindless index as a value** to pack into a BufferBuilder (for example a material table) |
| Descriptor heaps | `DescriptorHeap::Builder{role, slotCount}`, `Write`, `Index`, `Encode` (descriptorHeap.h) | Not exposed (backend detail) |
| MeshHeap (TLSF sub-allocation of many meshes in one buffer pair) | `kmesh::MeshHeap::AllocateMesh/Create/Free` (modules/mesh/include/koralMeshHeap.h l.122–279); `AccelerationStructure::Builder::AddMesh(heap, allocation)` (l.113) | MeshPack re-copies meshes on change. MeshHeap would make streaming packs (add/remove without a full repack) possible |

### 2.4 Device and context queries (`include/context.h`, `include/deviceFeatures.h`)

| Capability | Symbol | State in kgraph |
|---|---|---|
| Feature queries | `Context::Supports(Feature)`, `GpuHas(Feature)`, `SupportsRayTracing()`, `SupportsAsyncCompute()`, `AsyncComputeIsSeparateFamily()`, `ActiveDescriptorModel()` (context.h l.196–227); `Feature::eMeshShader, eRayTracing, eAtomicFloat32, eInt64Atomics, eCooperativeMatrix, eShaderFloat16, eMultiDrawIndirect, eDrawIndirectCount, eFillModeNonSolid, eWideLines …` (deviceFeatures.h l.26–56) | **No node.** forward-plus's `renderer` switch (vertex / meshlets / rays) is chosen by hand. A `DeviceSupports(feature) → bool` value wired into Switch gives automatic fallbacks |
| Feature registration | `detail::RegisterFeatures(by, required, optional)` (deviceFeatures.h l.81) | A graph cannot say "this graph needs ray tracing" |

### 2.5 Debug draw (`include/debugDraw.h`, `class kor::DebugDraw`)

| Capability | Symbol | State in kgraph |
|---|---|---|
| Immediate-mode shapes | `Line, Arrow, Point, Axes, Grid, Frustum(viewProj), Triangle, Quad, Plane, Circle, Box(min,max)/Box(mat), Sphere, Cylinder, Cone, Capsule, Camera(view,proj), PointLight, SpotLight, DirectionalLight` (l.124–173) with `DebugStyle` (colour, depth test, duration, fill) | The DebugDraw *pass* draws what C++ and scripts put in. **No node can emit a shape from graph values**, for example a frustum per ShadowCascades matrix, the bounding box of a MeshPack range, or a sphere per meshlet |
| Gizmos | `Gizmo(mode, transform, viewProj, pointer, options)` (l.190) | Editor-only ✓ |
| Counters | `LineCount`, `TriangleCount` (l.207–208) | — |

### 2.6 Shaders, pipelines, reflection

| Capability | Symbol | State in kgraph |
|---|---|---|
| Shader language choice | `Shader::Lang::eGLSL / eSlang / eSPIRV` (shader.h l.78) | Slang only (modules are `.slang`). GLSL/SPIR-V modules are not offered |
| Explicit binding assignment | `Pipeline::SetBinding/SetBindings` (pipeline.h l.93–99) | Hidden by design ✓ |
| Embedded (immutable) samplers | `SetSampler(name, sampler)` (pipeline.h l.79) | Exposed as a builder setting ✓ (`sampler[]`) |
| Semantic auto-filled structs | `[Kor(KOR_VIEW_PROJECTION_MATRIX)]` etc. + `SemanticBuffers`/`SemanticSerializer` (semantics.h) | **Not used by kgraph.** The Camera Function and ShaderBuffer do this by name instead. A ShaderBuffer could fill `[Kor(...)]`-annotated fields automatically from the main camera, the time and the screen |
| Push-constant reflection | `Pipeline::PushConstants()` (l.230) | Used (`push.<name>` ports) ✓ |

### 2.7 App, window, time, input

| Capability | Symbol | State in kgraph |
|---|---|---|
| Frame time, unscaled time, time scale, fixed fraction | `kor::Time::FrameTime()`, unscaled, `FixedDeltaTime`, `FixedFraction` (gtime.h l.29–78) | Only the fixed-step part (FixedStep). **No Time node** (frame dt, elapsed, frame index) |
| Camera jitter (TAA) | `kcam::Camera::SetJitter/Jittering` (modules/camera koralCamera.h l.537) | **Not used**: forward-plus jitters in a compute pass (`taa/advance`) |
| Offscreen targets / several windows | `App::OpenOffscreen(name, OffscreenSettings)` (app.h l.166); `Window::Format` (eBGRA8/eRGBA8, UNORM/SRGB) (window.h l.71); `IsVSync` | One Screen per graph. No HDR swap-chain format exists in kor either (**needs new kor support**: 10-bit and FP16 scRGB / HDR10 presentation) |
| Input | `kor::Input` keys and mouse (input.h) | No node. Mouse position is useful for debug probes ("value of the pixel under the cursor") |

### 2.8 Modules

| Module | Capability | Header + symbol | State in kgraph |
|---|---|---|---|
| image-import | Cubemaps from six faces or from an equirect | `kimg::LoadCubemap`, `LoadCubemapFromEquirectangular`, `EquirectangularToCubemap` (koralImageImport.h l.176–212) | ImageFile loads 2D only. **An environment-map node is missing** |
| image-import | Decode to the CPU | `DecodeImageFile` → `CpuImage` (l.115) | Internal |
| image-export | Saving images to disk | `kimg::SaveImage`, `SaveImageAsync`, `SaveImageSet`, `ExtensionFor` (koralImageExport.h l.92–147) | **No Screenshot/Capture node** (golden-image tests, baking a LUT, offline rendering) |
| image-compress | BC/ASTC/UASTC KTX2 baking | `kimg::CompressToKtx` (koralImageCompress.h l.156–176) | Build-time only; not a graph concern |
| model-import | Meshes, materials and scenes from files | `kmodel::Importer::GetMesh/GetMaterial/LoadScene` (koralModelImport.h l.214–229) | Reached through entities, not nodes ✓ |
| mesh | Mesh heap, TLSF | see 2.3 | — |
| camera | Perspective camera with controller, aspect source, jitter, semantics | `kcam::PerspectiveCamera::Builder` (koralCamera.h l.694–764) | kgraph has its own View/Perspective ✓ (jitter missing) |
| kmath | Noise, random, colour (sRGB↔linear exact, HSV/HSL, **Oklab**, colour temperature, luminance), octahedral packing, half packing, Bezier/Catmull-Rom/Hermite, AABB/geometry | `kmath/color.h` (`SrgbToLinear`, `LinearToOklab`, `ColorTemperature`, `PackOctahedral`), `kmath/random.h` (`Random`, `Hash`, `HashToFloat`), `kmath/interp.h`, `kmath/geometry.h`, `kmath/packing.h` | Only `Linear` (pow 2.2, which approximates rather than gives the exact sRGB curve). No Random/Hash, HSV, Oklab, Kelvin, Luminance, spline or AABB value nodes |
| net, ui, gui-extras | Networking, UI, editor panels | knet, kui | Out of scope for render nodes (a kui overlay pass could be a node later) |

### 2.9 Things kor does *not* have, which the missing-node list needs ("needs new kor support")

Variable-rate shading (fragment shading rate), conservative rasterization, multiview / view masks (VR, and
single-pass cube shadows), sparse/tiled resources (virtual shadow maps and virtual texturing page tables need either
sparse binding or a software page table), sampler min/max reduction, sampler border colour, AS refit and compaction,
procedural AABB geometry, HDR swap chain formats, GPU timestamp queries usable *inside* a pass by node, pipeline
statistics queries, occlusion queries, `R11G11B10_UFLOAT` / `A2B10G10R10` formats (absent from `Image::Format`,
image.h l.82–211: there is no packed HDR colour format), DLSS/FSR/XeSS SDK integration, and cooperative-matrix
inference (the `eCooperativeMatrix` feature exists, but there is no API around it).

## 3. Industry survey

Method: several dozen web searches (2026-10-23). Pages could not be fetched in full, so descriptions rely on search
snippets of the primary sources, plus well-documented public knowledge of these systems. URLs are given so each claim
can be checked.

### 3.1 Systems: what granularity they expose

| System | Authoring form | Granularity offered | Levels present (L0–L4) | Notes for KoralEngine |
|---|---|---|---|---|
| **Frostbite FrameGraph** (O'Donnell, GDC 2017) — [slides](https://www.slideshare.net/DICEStudio/framegraph-extensible-rendering-architecture-in-frostbite), [GDC Vault](https://www.gdcvault.com/play/1024612) | C++ code: `addPass(setup, execute)` with virtual resources | One node = one pass; resources are *transient* (created/aliased by the graph) or imported. Graph is rebuilt each frame, culls unused passes, aliases memory ("saved tons of GPU memory") | L1 (virtual), L2 | The template kor's FrameGraph follows. KoralEngine uses kor's ordering, but **not** its transient/aliased resources (2.2) |
| **Unreal RDG** — [FRDGBuilder](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRDGBuilder), [AddPass](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGBuilder/AddPass), [AddPassDependency](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGBuilder/AddPassDependency) | C++: `GraphBuilder.AddPass(name, params, flags, lambda)`; parameter structs declare resources; `Execute()` compiles, culls, runs | L2 pass per lambda; resources L1 transient; *blackboard* for cross-feature data; **async compute** flag; explicit dependencies; RDG Insights / `r.RDG.Debug` for visualisation | L1, L2 | RDG exposes nothing to artists; the artist-facing levels are the Material editor, Niagara and Post Process Volumes |
| **Unreal Material editor** — [Substrate](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine), [post-process materials](https://dev.epicgames.com/documentation/unreal-engine/post-process-materials-in-unreal-engine), [named reroute](https://docs.unrealengine.com/4.27/API/Runtime/Engine/Materials/UMaterialExpressionNamedRerouteD-/index.html) | Node graph compiled to HLSL | L0 expression nodes (math, texture sample, SceneTexture), **Material Functions** (composite, with inputs/outputs), **Material Layers**, **Substrate slabs** (L4-ish BSDF blocks), Custom (raw HLSL), **Named Reroute** declaration/usage (like kgraph's Reference). Post-process materials pick a **Blendable Location** (before tonemapping, after, replacing tonemapper) | L0, L4 (slab/layer) | Blendable location = choosing *where in the frame* a node runs, which KoralEngine expresses by pages |
| **Unreal Niagara** — [scratch pad](https://dev.epicgames.com/documentation/en-us/unreal-engine/niagara-scratch-pad-modules-in-unreal-engine), [emitter spawn](https://dev.epicgames.com/documentation/en-us/unreal-engine/emitter-spawn-group-reference-for-niagara-effects-in-unreal-engine) | Stack of **modules** inside System → Emitter → Spawn/Update groups; modules are node graphs; **simulation stages** iterate over particles or grid cells (Grid2D/3D, Neighbor Grid 3D) | Module = composite of L0 nodes; stack = fixed L3/L4 skeleton; data interfaces read meshes, textures, the scene | L0, L3, L4 | Simulation stages ≈ KoralEngine's fixed pages + Dispatch over a buffer; a "particle system" Function family is the KoralEngine equivalent |
| **Unity SRP RenderGraph (URP/HDRP)** — [write a pass](https://docs.unity.com/en-us/engine/6000.6/manual/render-pipelines/universal-render-pipeline/customizing-urp/render-graph/write-render-pass), [viewer](https://docs.unity3d.com/Manual/urp/render-graph-viewer-reference.html) | C#: `AddRasterRenderPass<PassData>` / `AddComputePass` / `AddUnsafePass`; `UseTexture`, `SetRenderAttachment`; `UniversalResourceData` gives camera colour/depth/normals/motion | L2 passes, L1 transient textures (allocated before first writer, released after last reader); **Render Graph Viewer** shows passes × resources with read (green) / write (red) blocks, culled passes, async-compute flag | L1, L2 | The Viewer is a ready model for kgraph's `Runtime::Frame()` display. URP's `ScriptableRendererFeature` (an injectable bundle of passes at a named point) ≈ a Function call on a page |
| **Unity Shader Graph** — [node library](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.0/manual/Node-Library.html), [Sub Graph](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.1/manual/Sub-graph), [Custom Function](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.0/manual/Custom-Function-Node.html) | Node graph → HLSL | Categories Artistic, Channel, Input, Math, **Procedural** (noise, shapes, checkerboard), Utility, UV; **Sub Graphs** (composite, convertible from a selection); Custom Function (string or file); **Block nodes** in master-stack contexts | L0 (+ L4 via sub-graphs) | "Convert selection to Sub Graph" ≈ what KoralEngine should offer for Functions |
| **Unity VFX Graph** — [contexts](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@9.0/manual/Contexts.html) | Vertical **contexts** (Spawn → Initialize → Update → Output) holding **blocks**, fed by horizontal **operators**; GPU events | Context = L3/L4 container; block = L2-ish step on every particle; operator = L0 | L0, L2, L3, L4 | Two directions of flow (vertical = execution/stack, horizontal = values) visually separate *what runs* from *what is computed* |
| **Godot** — [Compositor](https://docs.godotengine.org/en/4.7/tutorials/rendering/compositor.html), [RenderingDevice](https://docs.godotengine.org/en/4.6/classes/class_renderingdevice.html), [VisualShaderNodeCustom](https://docs.godotengine.org/en/latest/tutorials/plugins/editor/visual_shader_plugins.html), [fog volumes](https://godotengine.org/article/fog-volumes-arrive-in-godot-4) | Visual shaders (node graph → shader), CompositorEffect scripts calling RenderingDevice (`compute_list_dispatch`, `uniform_set_create`) at fixed callback points | L0 nodes, custom nodes in GDScript; the frame is fixed (Forward+/Mobile) with **injection points** for compute effects | L0, L2 (by code) | Shows the "fixed renderer + injection points" model KoralEngine already exceeds |
| **Blender Geometry/Shader/Compositor nodes** — [node parts & socket shapes](https://docs.blender.org/manual/en/5.0/interface/controls/nodes/parts.html), [4.0 node panels](https://developer.blender.org/docs/release_notes/4.0/node_editor/), [GPU defocus](https://projects.blender.org/blender/blender/pulls/116391) | Node graphs with **node groups** (composite, green header in themes), **frames**, **panels** (collapsible socket groups), **fields** (diamond sockets: per-element values) vs single values (circle) | L0 (math), L3 (geometry fields, "Sample Index", "Capture Attribute"), L4 (node groups, Principled BSDF), compositor L2/L4 (Glare, Defocus, Denoise, Kuwahara) | L0, L3, L4 | **Field vs single-value socket shapes** map exactly onto kgraph's "worked out once per ForEach element" vs plain values |
| **Houdini** — [Copernicus](https://www.sidefx.com/docs/houdini/copernicus/index.html), [OpenCL COP](https://www.sidefx.com/docs/houdini/nodes/cop/opencl.html), [assets/HDA](https://www.sidefx.com/docs/houdini/assets/index.html), [badges](https://www.sidefx.com/docs/houdini/network/badges.html), [flags](https://www.sidefx.com/docs/houdini/network/flags.html), [themes](https://sidefx.com/docs/houdini/network/options.html) | Contexts (SOP, VOP, COP, LOP) each a network type; VOPs compile to VEX; Copernicus GPU image ops incl. **OpenCL** node; Solaris/Karma via LOPs; **HDAs** (digital assets) package subnets | Per-context levels: VOP L0, COP L2/L4, SOP L3, LOP scene; HDA = composite at any level, **locked/unlocked badges**, per-type shapes/colours themes, **flags** (bypass, display, render) | All | The richest precedent for "composite node = asset with lock state", node **shapes** per type, **badges** and **flags** |
| **NVIDIA Falcor** — [docs index (fork mirror)](https://gitee.com/colorcolor/Falcor), [ReSTIR pass example](https://github.com/WeakKnight/ReSTIRPass) | C++ `RenderPass` classes with `reflect()` declaring named inputs/outputs; graphs written in **Python** scripts, edited in RenderGraphEditor, run in Mogwai | One node = one *technique-sized* pass (GBufferRaster, PathTracer, AccumulatePass, SVGF, ToneMapper, DLSS, RTXDI, ErrorMeasure, SideBySide…) | **L4 almost only**, resources are implicit | Falcor proves an L4-only graph is productive for research; KoralEngine should offer both L2 and L4 (via Functions) |
| **AMD FidelityFX SDK** — [SDK](https://gpuopen.com/amd-fidelityfx-sdk-1/), [SPD](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/single-pass-downsampler/), [FSR3 upscaler](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/) | Effect libraries with a fixed dispatch API (`ffxFsr3UpscalerDispatch`, `ffxSpdSetup`) | Each effect is an **L4 black box**: FSR (upscale/frame-gen), SPD (single-pass mip/depth pyramid), CACAO (AO), SSSR (reflections), Blur, Brixelizer/GI, Parallel Sort, LPM/tonemap, Denoiser, VRS, DoF, Lens | L4 | Precise input contracts (motion vectors, depth, reactive mask, jitter) define what kgraph nodes must produce |
| **AMD Render Pipeline Shaders (RPS)** — [intro](https://gpuopen.com/learn/rps_1_0/), [tutorial](https://gpuopen.com/learn/rps-tutorial) | HLSL-extension DSL: `graphics node`/`compute node` declarations with access attributes; an `export` entry calls nodes, recorded into a graph | Node = L2; access attributes compiled to signature metadata | L1, L2 | A text form close to kgraph's JSON; its access attributes are what kgraph infers from reflection |
| **Filament** | C++ FrameGraph (Frostbite-style) with a **blackboard** of named handles | L2 passes, L1 transient | L1, L2 | (search did not reach Filament docs; known from source) |
| **bgfx** — [internals](https://git.redump.net/mame/tree/3rdparty/bgfx/docs/internals.rst), [ViewMode](https://docs.unity3d.com/Packages/com.unity.tiny@0.32/api/Bgfx.bgfx.ViewMode.html) | Numbered **views** (≈ passes) with sort modes; sort-key bucketing | View = L2; no graph, order by view id | L2 | Minimal model; no dependency tracking |
| **Granite** (Themaister) — [deep dive](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/) | C++ render graph with `add_color_output`, `add_texture_input`, history inputs, size-relative attachments | L1 (size classes relative to swapchain), L2 | L1, L2 | "Size relative to backbuffer" attachment info = kor `ImageDesc::scale` |
| **The Forge** — [VB 2.0 release](https://github.com/ConfettiFX/The-Forge/releases/tag/v1.57) | C/C++ framework; Visibility Buffer 1.0/2.0 samples | L2 | — | Reference for visibility buffer as a whole-renderer technique |
| **EA SEED Halcyon** — [architecture](https://media.contentapi.ea.com/content/dam/ea/seed/presentations/wihlidal-halcyonarchitecture.pdf), [SyysGraph 2018](https://www.ea.com/seed/news/syysgraph-2018-modern-graphics-abstractions-real-time-ray-tracing) | Render backend / device / handles / commands / **render graph** / proxy; graph as a "convenience API" over explicit commands | L1 handles, L2 commands, graph above | L1, L2 | Same layering as kor + kgraph |
| **Bevy** — [post-processing example](https://bevyengine.org/examples-webgpu/Shaders/post-processing) | Rust render graph per view (`Core3d` sub-graph, `Node3d::{MainOpaquePass, Taa, Bloom, Tonemapping…}`), custom `ViewNode` inserted by edges | Node = L2/L4 feature; fixed sub-graph with insertion | L2, L4 | Named, ordered *feature slots* (like Blendable Location) |
| **Cocos render graph** — [GDC 2023](https://gdcvault.com/play/1029268/Open-Source-Game-Development-Summit) | Data-driven (not code-driven) graph with visualisation | L2 | L1, L2 | Data-driven like kgraph |

### 3.2 Techniques: what a production node/Function for each looks like

| Technique | Key sources | Shape as nodes (inputs → outputs) | kor fit |
|---|---|---|---|
| Clustered / tiled lighting | Olsson et al. 2012 [Chalmers](https://research.chalmers.se/en/publication/161725), tiled+clustered forward [Chalmers](https://research.chalmers.se/en/publication/161726), Doom 2016 [study](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/), [80.lv](https://80.lv/articles/doom-the-secret-of-60-fps) | camera, lights buffer, (decals, probes) → cluster grid + index list | ✓ (exists by hand in forward-plus) |
| Virtual shadow maps | [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine), [Fortnite blog](https://www.unrealengine.com/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4) | depth → page marking → page allocation → cached page rendering → physical atlas + page table; 16k virtual, 128² pages, clipmaps for directional | software page table works with kor; needs atomics, indirect, per-page draws |
| GPU-driven culling (HiZ, two-phase) | Hi-Z & two-phase overview [IfcOpenShell commit](https://dev.glassvision.dedyn.io/Glassvision/IfcOpenShell/commit/a34c36d22e8357c126ad5a20a02f5afce5314ced), [gamedev.net](https://gamedev.net/forums/topic/714059-hi-z-usefulness-in-deferred-pipelines), SPD [GPUOpen](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/single-pass-downsampler/) | instances, bounds, view, HiZ(prev) → phase-1 draw list → depth → HiZ → phase-2 list | ✓; HiZ benefits from min/max sampler reduction (new kor) |
| Visibility buffer | Burns & Hunt 2013 [JCGT](https://jcgt.org/published/0002/02/04/paper.pdf), Engel [The Forge](https://github.com/ConfettiFX/The-Forge/releases/tag/v1.57), Schied & Dachsbacher [DAIS](https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf) | geometry → 32-bit (instance, triangle) image → material classify/tile → shade compute | ✓ |
| Nanite-style virtualized geometry | Karis et al. 2021 [Advances](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) | cluster DAG (LOD), persistent-thread culling, HW+SW raster into 64-bit visbuffer | needs 64-bit atomics (`eInt64Atomics` exists), cluster DAG builder (CPU, Meshlets extension) |
| Temporal AA / upscaling | Karis 2014 (Advances), Salvi GDC 2016 [pdf](https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf), FSR3 [manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/) | colour, depth, **motion vectors**, jitter, history, reactive mask → output (display res) | history via `ReadPrevious`; jitter via Time/sequence node; FSR as shader port ✓; DLSS/XeSS need vendor SDK (new kor) |
| GTAO / SSAO | Jimenez 2016, XeGTAO [GitHub](https://github.com/GameTechDev/XeGTAO), CACAO | depth (+normals) → AO (+bent normal) → spatial denoise (→ TAA) | ✓ |
| SSR / SSGI | FidelityFX SSSR, forward-plus `reflections.slang` | depth, normals, roughness, colour, HiZ → reflection + confidence | ✓ |
| ReSTIR DI/GI | Bitterli 2020 [NVIDIA](https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal), RTXDI [ReSTIR GI](https://github.com/NVIDIAGameWorks/RTXDI/blob/main/doc/RestirGI.md) | gbuffer (curr+prev), light buffer, TLAS → reservoirs (temporal, spatial) → shading | needs history buffers (ReadPrevious) + RT ✓ |
| DDGI | Majercik 2019 [NVIDIA](https://research.nvidia.com/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields), 2021 [scaling](https://research.nvidia.com/publication/2021-05_scaling-probe-based-real-time-dynamic-global-illumination-production) | probe volume, TLAS → trace → blend irradiance + depth moments (octahedral atlases) → sample in shading | ✓ |
| Lumen-like | Wright et al. 2022 [Advances](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf), [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine) | surface cache + screen probes + SDF/HW tracing | very large; long-term |
| Path tracing + denoising | SVGF [KIT](https://cg.ivd.kit.edu/english/svgf.php), NRD [NVIDIA](https://developer.nvidia.com/nrd) | radiance (diffuse/spec split), normals+roughness, viewZ, motion → ReBLUR/ReLAX/SIGMA outputs | ✓ (SVGF as Function); NRD as external lib (new kor/plugin) |
| Volumetric fog | Wronski 2014 [GDC Vault](https://gdcvault.com/play/1020397/Assassin-s-Creed-IV-Black), Godot [fog volumes](https://godotengine.org/article/fog-volumes-arrive-in-godot-4), TLOU2 [SIGGRAPH](https://history.siggraph.org/learning/volumetric-effects-of-the-last-of-us-part-two-by-kovalovs) | froxel 3D image: inject media + lights (+shadow) → integrate along Z → apply | ✓ (3D storage images) |
| Volumetric clouds | Schneider 2015 [Advances](https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf), Nubis Cubed 2023 [pdf](https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf) | 3D noise volumes, weather map → raymarch at ¼ res → temporal reprojection | ✓ (needs 3D noise generator, history) |
| Atmosphere | Hillaire 2020 [EG](https://diglib.eg.org/handle/10.1111/cgf14050), [slides](https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf) | transmittance LUT → multi-scatter LUT → sky-view LUT (+aerial perspective 3D LUT) → sky/apply | ✓ |
| Decals (deferred/clustered) | Doom 2016 clustered decals (above) | decal entities → cluster list (shared with lights) → applied in forward | ✓ |
| OIT | McGuire & Bavoil 2013 [JCGT](https://www.jcgt.org/published/0002/02/09/paper.pdf), MBOIT [KIT](https://cg.ivd.kit.edu/mboit.php) | transparents → accum + revealage (WBOIT) or moments (2 passes) → composite | ✓ (needs per-attachment blend, exposed) |
| Post: DoF, motion blur | Jimenez 2014 [iryoku](https://iryoku.com/?p=1707), Wronski post pipeline [blog](https://bartwronski.com/2014/12/09/designing-a-next-generation-post-effects-pipeline/) | colour, depth, velocity → tile max/neighbour max → gather | ✓ |
| Auto-exposure | Unity [Auto Exposure](https://docs.unity3d.com/Packages/com.unity.postprocessing@3.4/manual/Auto-Exposure.html), Bevy [AutoExposure](https://dev-docs.bevyengine.org/bevy/core_pipeline/auto_exposure/struct.AutoExposureSettings.html) | HDR colour → 64/256-bin histogram (atomics) → percentile average → adapted EV (history) | ✓ |
| Colour grading LUT | HDRP LUT [blog](https://unity.com/blog/engine-platform/get-acquainted-with-hdrp-settings-for-enhanced-performance) | grading params → 32³ 3D LUT (bake) → apply in tonemap | ✓ |
| GPU sort/scan | Merrill & Garland [decoupled look-back](https://research.nvidia.com/sites/default/files/publications/nvr-2016-002.pdf), Onesweep [arXiv](https://arxiv.org/abs/2206.01784v1), [GPUOpen](https://gpuopen.com/learn/boosting_gpu_radix_sort/) | exists (Scan, RadixSort, Compact); single-pass variants need forward-progress guarantees | ✓ |
| Debug/profiling | Unity Render Graph Viewer (above), RDG Insights | pass × resource timeline, GPU timers, labels | labels/timers in kor, not wired (2.1) |

### 3.3 Synthesis

1. **Every production system separates two audiences.** Engine programmers get an L1/L2 graph in code (RDG, Unity
   RenderGraph, Frostbite, Filament, Granite, RPS). Artists and technical artists get L0 expression graphs (Material
   editor, Shader Graph, visual shaders, VOPs) and L4 blocks (Falcor passes, VFX contexts, Niagara modules,
   FidelityFX effects, Post Process Volume features). **kgraph puts both audiences in one canvas**, which no surveyed
   system does except Houdini (through contexts) and Falcor (L4 only). That is a strength, but it makes clear level
   signalling (section 5) essential.
2. **L3 is where engines are proprietary and thin in graphs.** Blender's fields and Niagara's data interfaces are the
   only artist-facing L3. kgraph's ForEach/Filter/MeshPack/RayScene is unusually rich here, and it is worth keeping
   and extending (instance culling, LOD, light lists, decal lists).
3. **Composite mechanisms**: Material Functions, Sub Graphs, Niagara modules, node groups, HDAs and Blueprint
   collapsed graphs/macros all share the same affordances: *convert selection to composite*, *double-click to open*,
   *edit in place vs. edit the asset*, *lock/unlock*, *version*, and *expose parameters*. kgraph Functions already
   have open-in-place and engine-vs-project override. Missing: convert-selection, a lock badge, a visible version
   mismatch, and a collapsed in-place preview.
4. **Temporal history and transient memory** are first-class in every modern render graph (RDG
   `QueueTextureExtraction`/history, Granite history inputs, Frostbite transients, Unity's lifetime-managed textures).
   kor has both (`ReadPrevious`, `Create`); kgraph uses neither. This is the largest *infrastructure* gap.

## 4. Missing nodes (main deliverable)

**Priorities.** **P0**: unblocks correctness or a technique KoralEngine already ships by hand, or is a cheap,
high-leverage gap. **P1**: a standard modern-renderer feature with clear precedent, buildable now. **P2**:
specialised, large, or blocked on new kor support.

**Columns.** *Form*: **atomic** (a new C++ node type in `catalog.cpp`/`runtime.cpp`) or **Function** (a `.kfunc`
in `packages/koral/functions`, with the existing nodes it would be built from). *kor*: the capability it rests on;
"new kor" means kor needs new support first. *Precedent*: the systems from section 3.

Evidence from the codebase is cited where it explains a priority: `forward_plus.h` and `taa.slang` show what is done
by hand today.

### 4.1 L0 Value

| # | Proposed node | Purpose | Form | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|---|
| 1 | **Time** | clock | atomic | — → `dt`, `unscaledDt`, `elapsed`, `frame` (index), `timeScale`, `paused` | `kor::Time::FrameTime`, unscaled time, time scale (gtime.h) | UE *Time*, Unity `_Time`, Shader Graph *Time*, Godot `TIME` | **P0** | There is no clock besides FixedStep. forward-plus advances TAA's frame counter with a *compute pass* (`taa/advance`) only for lack of this |
| 2 | **Jitter** | upscaling/AA | atomic | `index` (Time.frame), `phases` (or auto from render/display size), `renderSize` → `pixels` (vec2), `clip` (vec2), `jittered` (projection with offset applied, from an input `projection`) | `kcam::Camera::SetJitter` logic; pure math | FSR `ffxFsr3UpscalerGetJitterPhaseCount`, UE TAA Halton(2,3), Unity TAA | **P0** | TAA, FSR, DLSS and XeSS all need it. Today it is GPU-side code in `taa.slang` |
| 3 | **Previous** *(value form)*, e.g. `LastFrame` | temporal | atomic | `value` → `previous` (last frame's), `valid` | CPU-side state (as PointShadows keeps) | RDG history, Unity `previousViewProjection`, UE `PrevViewToClip` | **P0** | Motion vectors and reprojection need the previous view-projection. `taa.slang` stores it in a GPU buffer (`s.previousViewProjection`). (The name `Previous` is taken by loops, so call it `LastFrame`) |
| 4 | **DeviceSupports** | platform | atomic | `feature` (choice of `kor::Feature`: eMeshShader, eRayTracing, eInt64Atomics, eAtomicFloat32, …) → `bool` | `Context::Supports(Feature)`, `SupportsRayTracing`, `SupportsAsyncCompute` | UE RHI feature level / `GRHISupports…` switches in materials (*Feature Level Switch*), Shader Graph keywords | **P0** | Lets the `renderer` Switch in forward-plus (vertex / meshlets / rays) fall back automatically. One line of C++ |
| 5 | **Setting** *(quality/scalability)* | config | atomic | `name`, `default`, `choices` → `value` (from a project/user settings table, editable at runtime) | — | UE scalability cvars, Unity quality levels, HDRP frame settings | **P1** | Shadow size, bloom on/off, SSR quality as user options without editing the graph |
| 6 | **RenderResolution** | upscaling | atomic (or Function of Screen + Multiply + Ceil) | `display` (Screen.extent), `scale` or `mode` (Quality/Balanced/Performance) → `render` extent, `ratio`, `mipBias` | — | FSR quality modes, DLSS modes, UE `r.ScreenPercentage` | **P1** | Needed for every upscaler; mip bias feeds the Sampler's `mipLodBias` |
| 7 | **Random** / **Hash** | procedural | atomic | `seed`, `index`, `kind` (float, vec2/3, in sphere, in disk) → value | `kmath::Random`, `Hash`, `HashToFloat` (random.h) | Blender *White Noise*, Shader Graph *Random Range*, Houdini `rand` | **P1** | Scattering, per-light colours, test scenes. With a ForEach, this is per element |
| 8 | **ColorConvert** | colour | atomic | `color`, `from`/`to` (sRGB exact, linear, HSV, HSL, Oklab, Kelvin→RGB), → `color`; plus a `Luminance` output | `kmath/color.h` (`SrgbToLinear`, `RgbToHsv`, `LinearToOklab`, `ColorTemperature`, `Luminance`) | Shader Graph *Colorspace Conversion*, Blender *Separate/Combine Color*, *Blackbody* | **P1** | `Linear` is pow 2.2, not the sRGB curve. Light colour by temperature is common |
| 9 | **Curve** / **Gradient** | authoring | atomic | `x`, curve or gradient (edited in the inspector) → value / colour | `kmath/interp.h` (Bezier, CatmullRom, Hermite) | Blender *Float Curve*, *Color Ramp*; Unity AnimationCurve/Gradient; UE Curve assets | **P1** | Exposure curves, fog density by height, artist ramps. As a 1D LUT image too (see #24) |
| 10 | **Quaternion** set (FromEuler, FromAxisAngle, Slerp, Rotate, ToMatrix) | math | atomic | as named | `kmath/quaternion.h`, `euler.h` | Blueprint rotator nodes, Houdini VOPs | **P2** | `orientation` outputs exist with nothing to compute on them |
| 11 | **Bounds** set (Union, Transform, Contains, FrustumTest) | math | atomic | AABBs as (min, max) → AABB / bool | `kmath/geometry.h` (`Union`, `Contains`, `Overlaps`, `ClosestPoint`) | Houdini *Bounding Box*, Blender *Bounding Box* | **P2** | Feeds ShadowCascades `boundsMin/Max`, debug drawing, CPU culling tests |
| 12 | **Reprojection** | temporal | **Function** of LastFrame + MatrixMultiply + MatrixInverse | `view`, `projection` → `viewProjection`, `previousViewProjection`, `currentToPrevious` (clip→clip), `jitterDelta` | — | UE `ClipToPrevClip`, Unity `_PrevViewProjMatrix` | **P1** | Packages what TAA, SSR, motion blur and ReSTIR all compute |
| 13 | **Input** | interaction/debug | atomic | `key` or mouse → `pressed`, `held`, `position` (pixels), `wheel` | `kor::Input` (input.h) | Blueprint input nodes | **P2** | Debug probes ("texel under the cursor"), toggling debug views in play |
| 14 | **PackValues** helpers (PackUnorm4x8, PackHalf2x16, PackOctahedral) | data | atomic | value → uint | `kmath/packing.h`, `PackOctahedral` (color.h) | Shader Graph *Pack*, HLSL intrinsics | **P2** | Building compact GPU structs from values through a BufferBuilder |

### 4.2 L1 Resource

| # | Proposed node | Purpose | Form | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|---|
| 15 | **RenderTarget** *(transient image)* | memory | atomic | `format`, `usage`, `scale` (of Screen or of `sizeOf` image), or `extent`, `mips` → Image (graph-owned, aliased) | `PassBuilder::Create(name, ImageDesc{scale, sizeOf, extent})`, `FrameGraph::SetAliasing`, `Memory()` | Frostbite transient resources, RDG `CreateTexture`, Unity RenderGraph textures, Granite size classes | **P0** | Every kgraph image is persistent and unaliased. forward-plus has about 20 screen-sized images. It also removes the Screen→Divide→extent wiring |
| 16 | **TransientBuffer** | memory | atomic | `bytes`, `usage` → Buffer (aliased) | `PassBuilder::Create(name, BufferDesc)` | RDG `CreateBuffer` | **P1** | Scratch for sort, scan and compaction (RadixSort, Scan and Compact each hold 2–3 persistent buffers) |
| 17 | **History** | temporal | atomic | `of` (an Image or Buffer the graph makes) → `previous` (last frame's contents), `valid` (bool: false on the first frame and after a rebuild) | `PassBuilder::ReadPrevious`, `PassResources::PreviousImageNamed/PreviousBufferNamed`, `RenderPass::HasPrevious` | RDG history extraction, Granite history inputs, Unity `RTHandle` history system | **P0** | TAA keeps a `history` image by hand with a `keep` dispatch. Auto-exposure, HiZ-from-last-frame, SVGF and ReSTIR all need this, and the `valid` flag removes first-frame garbage |
| 18 | **EnvironmentMap** | IBL/sky | atomic | `file` (equirect .hdr, or six faces), `size` → cube Image (with mips) | `kimg::LoadCubemapFromEquirectangular`, `LoadCubemap`, `EquirectangularToCubemap` (koralImageImport.h) | UE Cubemap asset + Sky Light, Unity Cubemap import, Godot Sky | **P1** | ImageFile is 2D only. This is needed for IBL (#61) and skyboxes |
| 19 | **BindlessIndex** | data | atomic | resource (image, view, buffer, sampler, AS) → `index` (uint) | `kor::bindless::Sampled/Storage/StorageBuffer/Sampler/AccelerationStructure` (bindless.h) | UE `GetBindlessResourceIndex`, Unity bindless (DOTS), The Forge descriptor indexing | **P1** | Lets a BufferBuilder hold a material table of texture indices. Needed for the visibility buffer (#70) |
| 20 | **DataTable** *(literal bytes)* | data | atomic | typed list (floats/uints/vec4s) or a CSV/JSON asset → Buffer (or 1D/2D Image) filled once | `Buffer::Builder::SetData`, `Image::Builder::SetData` (skipped today: `Kind::eBytes`) | UE DataTable, Unity ScriptableObject arrays | **P1** | Kernel weights, Halton tables and LUTs without writing a ResourceScript |
| 21 | **NoiseTexture** | procedural | **Function**: Image + ComputePipeline (`koral_noise`) + Dispatch on a **setup** page | `kind` (value/Perlin/simplex/Worley/curl/fBm), `dimensions` (2D/3D), `size`, `octaves`, `frequency`, `seed`, `tiling` → Image | 3D storage images, setup pages | Blender *Noise/Voronoi Texture*, Shader Graph *Procedural*, Houdini Copernicus noise, Nubis cloud noise | **P1** | Clouds, fog, dissolve and wind. The setup-page pattern already exists (docs/kgraph.md "Setup") |
| 22 | **BlueNoise** | sampling | atomic (ImageFile array) or Function | `frame` (Time.frame) → `texture` (this frame's slice of a spatiotemporal blue-noise set), `index` | ImageFile, 2D-array image | NVIDIA STBN, UE `BlueNoise` global shader params | **P1** | Every stochastic effect (AO, SSR, RT shadows, fog) converges faster under TAA with it |
| 23 | **SamplerPreset** | convenience | atomic | `preset` (LinearClamp, LinearRepeatAniso, Point, ShadowCompare, MinReduce, MaxReduce) → Sampler | Sampler builder (+ new kor for reduction) | UE `TStaticSamplerState`, Unity inline sampler names | **P2** | Cuts four-field Sampler set-ups to one choice |
| 24 | **CurveTexture** | LUT | Function: Curve (#9) sampled into a 1D image | curve → Image (256×1) | Image + CopyBufferToImage, or Script | Blender ColorRamp → image, UE Curve Atlas | **P2** | GPU access to artist curves |
| 25 | **ModelFile** | assets | atomic | `file` → `meshes` (list), `layout`, `materials` | `kmodel::Importer::GetMesh/GetMaterial` | UE Static Mesh asset node, Houdini File SOP | **P2** | Meshes reach graphs only through entities today. This is useful for procedural/test graphs |
| 26 | **Sampler: `reduction` (min/max) and `borderColor` inputs** | HiZ/shadows | builder setting | — | **new kor** (`VkSamplerReductionModeCreateInfo`, border colour) | D3D12 min/max filtering, UE HZB sampler | **P1** | One-tap HiZ tests. Shadow maps that read as lit outside the atlas |
| 27 | **Image formats R11G11B10_UFLOAT, A2B10G10R10, BGRA8** | memory | kor enum | — | **new kor** (`Image::Format` has none) | Standard HDR scene colour in UE and Unity | **P1** | Halves HDR colour bandwidth compared with RGBA16F |

### 4.3 L2 Command

| # | Proposed node | Purpose | Form | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|---|
| 28 | **ComputePass** | convenience | **composite** (expands to ComputePipeline + Dispatch `cover`) | `module`, `entry`, `param.*`, `push.*`, `cover` → (pass) | as today | RDG `FComputeShaderUtils::AddPass`, Unity `AddComputePass`, Godot CompositorEffect | **P0** | forward-plus's `compute()` helper builds exactly this pair about 25 times. Halves node count on post pages |
| 29 | **FullscreenPass** | convenience | **composite** (GraphicsPipeline with a built-in fullscreen-triangle vertex shader + Draw `vertexCount: 3`) | `module`, `entry` (fragment), `target`, `param.*`, `blend` → (pass) | as today | UE `FPixelShaderUtils::AddFullscreenPass`, Unity `Blitter.BlitTexture`, post-process materials | **P1** | Every raster post effect (sky, composite, blending into the target) needs a hand-written vertex shader today |
| 30 | **Readback** | data to CPU | atomic | `buffer` or `image` (+ offset, texel), `format` → `value` (number/vector, a few frames late), `frameAge` | `Buffer::ReadAsync`, `Buffer::Type::eReadback`, `CopyImageToBuffer` | Unity `AsyncGPUReadback`, UE `FRHIGPUBufferReadback` | **P0** | Turns GPU counters (instances visible, contacts, average luminance) into values the graph, a UI or a script can use. Pick, WriteBack and Watch already do this internally |
| 31 | **DebugLabel** *(input on every pass + page-level region)* | debugging | atomic (an input `label`, plus the page name as a region) | `label`, `color` | `BeginDebugLabel`/`EndDebugLabel`/`InsertDebugLabel` (commandBuffer.h l.886–906) | RDG event names (`RDG_EVENT_NAME`), Unity profiling samplers | **P0** | RenderDoc and Nsight captures of kgraph frames are unlabelled today (kgraph.md Limits). Near-zero cost |
| 32 | **GpuTimer** | profiling | atomic (input on passes) or a scope node | `label` → `ms` (value) | `BeginTimer`/`EndTimer`/`Timings`/`OnTimings` | UE `RDG_GPU_STAT_SCOPE`, Unity ProfilingSampler | **P1** | Sub-pass timing (per Draw element, per loop turn) and timings as values for an in-graph HUD (#86) |
| 33 | **WriteBuffer** | data | atomic | `buffer`, `offset`, values or DataTable → (pass) | `CommandBuffer::FillBuffer` (l.1109) | RDG `QueueBufferUpload`, Unity `CommandBuffer.SetBufferData` | **P1** | Reset counters to non-zero values, patch indirect args, upload per-frame tables in pass order |
| 34 | **ClearImage** *(generalised Clear)* | basics | atomic | `image`, `value` (float/uint/int, depth/stencil), `mip`, `layer`, region | `ClearColorImage` exists; **new kor** for depth/stencil clears outside a render pass and for integer clear values | RDG `AddClearUAVPass`, `AddClearDepthStencilPass` | **P1** | Clear is RGBA float only. Clearing uint visibility or ID images and depth pyramids needs a dispatch today |
| 35 | **Screenshot** | output/testing | atomic | `image`, `file`, `when` (every n frames, on key, once) → (pass) | `kimg::SaveImage(Async)` (koralImageExport.h) + CopyImageToBuffer | UE `HighResShot`, Falcor frame capture, Movie Render Queue | **P1** | Golden-image tests for graphs (and for the editor-test crash memory), baking LUTs and noise |
| 36 | **DebugShapes** | debugging | atomic | `kind` (frustum, box, sphere, line, axes), values (e.g. ShadowCascades.matrices, a list of AABBs), `color`, `depthTest` → (adds to `kor::DebugDraw`) | `DebugDraw::Frustum/Box/Sphere/Line/Axes…` (debugDraw.h l.124–173) | UE `DrawDebugBox` in Blueprints, Houdini guide geometry | **P1** | Visualise cascades, light ranges, meshlet spheres and pack bounds from the graph itself. Today only C++ and scripts can draw |
| 37 | **Draw: `stencilOp` per face, `depthBiasEnable`, `depthBounds`** | raster | inputs on Draw | `stencilFront/Back` {fail, pass, depthFail, compare} | `SetStencilOp`, `SetDepthBiasEnable` (commandBuffer.h l.543–546) | every RHI | **P2** | Stencil-masked decals, portal and outline techniques (kgraph.md Limits) |
| 38 | **NativePass** | extensibility | atomic, with a C++ plug-in registry | `name` (registered C++ pass), declared reads/writes, settings → outputs | `CommandBuffer::Run`, `RenderPass` subclass, `CpuPass` | Godot CompositorEffect, URP ScriptableRenderPass, Falcor RenderPass plug-ins | **P2** | The escape hatch for vendor SDKs (DLSS, NRD, XeSS) and engine features before they become nodes |
| 39 | **CpuStep** | CPU work in order | atomic | `script` (a project script with `Run()`), resources → outputs | `kor::CpuPass` (frameGraph.h l.355) | Unity `CommandBuffer.IssuePluginEvent`, RDG CPU passes | **P2** | CPU-side work (fill a buffer from game logic) ordered with GPU passes |

### 4.4 L3 Scene / data binding

| # | Proposed node | Purpose | Form | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|---|
| 40 | **ForEach/Filter output `previousMatrix`** *(and `moved`)* | motion vectors | atomic (new outputs on ForEach, Filter and Entity) | — → last frame's world matrix, whether it moved | ECS transform history in `entities.cpp` | UE `PreviousLocalToWorld`, Unity `unity_MatrixPreviousM` | **P0** | `taa.slang` reprojects by **camera only**, so moving objects ghost. Per-object motion vectors need the previous transform per instance |
| 41 | **Lights** | lighting | **Function** of ForEach(PointLight/SpotLight/DirectionalLight/AreaLight) + BufferBuilder + Collect + PointShadows | `entities` → `lights` buffer (unified struct), `count` per type, `sun` bundle, `shadowed` indices | as today | UE/Unity light lists (intrinsic), Falcor `LightCollection` | **P0** | forward-plus's `frame` page builds this by hand. Every lit technique starts from it |
| 42 | **InstanceCull** *(GPU frustum + two-phase HiZ)* | GPU-driven | **Function** of Dispatch (`frustumCull`, `occlusionCull` from forward-plus), IndirectDraws (`mode: eGPU`), ClearBuffer, HiZ (#55), History (#17) | `instances` (Filter), `ranges`, `bounds`, `viewProjection`, `hiz` (previous) → `commands`, `count`, `visible`, `phase2` list | as today | Wihlidal GDC 2016, UE Nanite/instance culling, The Forge | **P0** | It exists in forward-plus as about 15 hand-wired nodes. Packaging it makes GPU-driven drawing reusable in any project |
| 43 | **MaterialTable** | materials | **Function** of ForEach(Material component) + BindlessIndex (#19) + BufferBuilder | `entities` → `materials` buffer (factors + bindless texture indices), `materialOf` (per instance) | bindless | UE GPUScene / Nanite material tables, Unity BatchRendererGroup | **P1** | A prerequisite for the visibility buffer and for ray-traced material lookup. RayScene already gives `meshOf` |
| 44 | **SceneBounds** | utility | atomic | `entities` (Filter) → `min`, `max`, `center`, `radius` | ECS + mesh bounds (MeshPack ranges have `boundsMin/Max`) | Houdini *Bounding Box*, UE `GetActorBounds` | **P1** | ShadowCascades wants `boundsMin`/`boundsMax`; today they are typed in or omitted |
| 45 | **Skinning** | animation | **Function**: ForEach(Skeleton) → BufferBuilder (bone matrices) + Dispatch (skin into a vertex buffer) + MeshPack update | `entities`, `mesh` (with BONE_IDS/WEIGHTS) → skinned `vertices`, `mesh` (and `previousVertices` for motion vectors) | compute ✓ | UE GPU skin cache, Unity compute skinning | **P1** | `eSkinnedCylinder` exists as a default mesh, but nothing skins it. RayScene needs pre-skinned positions for a BLAS rebuild |
| 46 | **LodSelect** | geometry | atomic (CPU) or Function (GPU) | `entities`, `lods` (MeshPack ranges per LOD), `viewProjection`, `screenSizeThresholds` → `groups`/`ranges` for IndirectDraws | meshoptimizer simplify (as Meshlets already does) | UE LOD groups, Unity LODGroup, Nanite DAG | **P1** | MeshPack has one LOD |
| 47 | **Decals** | surface detail | **Function** of Filter(Decal) + BufferBuilder + cluster binning (shared with #56) | `entities` → `decals` buffer, cluster lists | ✓ | Doom 2016 clustered decals, UE DBuffer, HDRP decal projector | **P1** | forward-plus has decals as a tag drawn with blending. Clustered decals are cheaper and work on any surface |
| 48 | **Particles** | VFX | **Function family**: Emit (Dispatch append), Simulate (Dispatch on a fixed page), Sort (RadixSort on depth keys), Draw (IndirectArgs + Draw instanced quads), plus a ParticleEmitter component ForEach | emitter fields → `particles` buffer, `count`, draw args | ✓ (atomics, indirect) | Niagara modules and simulation stages, VFX Graph contexts | **P1** | GPU physics already proves the compute side (SpheresStep). Particles are the most-requested visual feature after lighting |
| 49 | **RayScene: `update` (refit) mode, `aabbs` (procedural)** | ray tracing | inputs on RayScene | `refit: bool`, `procedural` (Filter of AABB entities) | **new kor** (AS update/refit, AABB geometry, compaction) | DXR/Vulkan RT `ALLOW_UPDATE`, UE ray tracing instance updates | **P1** | RayScene rebuilds the TLAS on any motion ("Made again when one moves"). Refit is the standard fix |
| 50 | **Probes** (reflection/irradiance capture) | GI | **Function**: Filter(ReflectionProbe) → Repeat×6 CubeFace + View + Perspective + Draw → cube array → prefilter (#61) | `entities` → `probes` cube array, `probeData` buffer | ✓ (multiview would help: **new kor**) | UE Reflection Capture, Unity Reflection Probe, HDRP probe system | **P2** | Baked/static local reflections without RT. CubeFace and PointShadows already provide the views |
| 51 | **ClusterHierarchy** (Nanite-style DAG) | virtual geometry | atomic (CPU build, extends Meshlets) | MeshPack → cluster DAG buffers (groups, error metrics, parent bounds) | meshoptimizer; 64-bit atomics (`eInt64Atomics`) for the visibility buffer | Nanite (Karis 2021) | **P2** | The long-term path from Meshlets. Large |
| 52 | **Views** *(multi-camera)* | cameras | atomic | `component` (Camera) → per-camera `view`/`projection`/`target`, a loop source like ForEach | ✓ | Unity multiple cameras / camera stacking, Bevy per-camera graphs, UE SceneCapture | **P2** | Render-to-texture cameras, split screen, mirrors and security monitors |

### 4.5 L4 Technique (Functions), grouped by purpose

All are **Functions** (composite) unless noted. "From forward-plus" means the shader and the wiring already exist in
`samples/forward-plus` and only need packaging as a `.kfunc` with FunctionInput/FunctionOutput edges.

#### Anti-aliasing, upscaling, temporal

| # | Function | Built from | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|
| 53 | **TAA** | Jitter (#2), History (#17), ComputePass (`taa.slang` resolve), Reprojection (#12) | `color`, `depth`, `motion` (optional), `jitter` → `color` | ✓ | Karis 2014, Salvi 2016, UE TAA, Unity TAA | **P0** | It exists by hand in forward-plus. Packaging it fixes the frame counter (#1) and history (#17) workarounds |
| 54 | **MotionVectors** | Draw (velocity of moving entities, using `previousMatrix` #40) + ComputePass (camera reprojection for static pixels) | `entities`, `depth`, `viewProjection`, `previousViewProjection` → `motion` (RG16F) | ✓ | UE velocity pass, Unity motion vector pass, FSR/DLSS inputs | **P0** | Needed for correct TAA on moving objects, for upscalers and for motion blur |
| 55 | **HiZ** *(depth pyramid)* | Repeat + ImageView + ComputePass (min or max reduce), as Bloom's down-chain; later SPD single dispatch | `depth`, `reduce` (min/max) → `pyramid` (mipped R32F), `mips` | ✓ (sampler reduction would help: #26) | FidelityFX SPD, UE HZB | **P0** | Occlusion culling (#42), SSR and SSAO all use it. forward-plus's occlusion cull reads last frame's IDs instead |
| 56 | **Upscale** (spatial: FSR1 EASU+RCAS; temporal: FSR2/3 port) | ComputePass ×2 (spatial) or many (temporal); RenderResolution (#6), Jitter (#2), MotionVectors (#54) | `color` (render res), `depth`, `motion`, `reactive` → `color` (display res) | ✓ for FSR (open source); DLSS/XeSS via NativePass (#38) + vendor SDK (**new kor**) | FidelityFX FSR, DLSS, XeSS | **P1** | Render-resolution scaling is standard. 4K at native resolution costs too much for RT branches |
| 57 | **Sharpen** (CAS/RCAS) | ComputePass | `color`, `amount` → `color` | ✓ | FidelityFX CAS, UE Tonemapper sharpen | **P2** | Companion to TAA and upscaling |
| 58 | **FXAA / SMAA** | ComputePass / FullscreenPass ×3 (SMAA) | `color` → `color` | ✓ | UE FXAA, Unity SMAA | **P2** | A cheap alternative when TAA ghosting is unacceptable |

#### Lighting, shadows, GI

| # | Function | Built from | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|
| 59 | **ClusteredLights** *(light culling)* | Lights (#41), ClearBuffer, ComputePass (`lightCulling.slang`), Buffers | `lights`, `camera`, `clusters` (x, y, z), `maxPerCluster` → `grid`, `indices`, `counter` | ✓ | Olsson 2012, Doom 2016, UE clustered deferred | **P0** | It exists by hand on forward-plus's `lights` page |
| 60 | **PointShadows** *(cube shadow atlas)* | PointShadows (values), Image (cube array), Repeat (faces), Filter, GraphicsPipeline, Draw (`idle`, `whenChanged`) | `entities`, `lights`, `size`, `most`, `each` → `cubes`, `matrices` | ✓ | UE/Unity point-light shadows | **P0** | The companion to SunShadows. It exists by hand in forward-plus (`point shadows` pages) |
| 61 | **ImageBasedLighting** *(IBL prefilter)* | EnvironmentMap (#18), ComputePass ×3 (irradiance SH or cube, GGX prefiltered mips via Repeat, BRDF LUT on a setup page) | `environment` → `irradiance`, `specular` (mipped cube), `brdfLut` | ✓ | UE Sky Light, Unity reflection probes, Filament IBL | **P1** | The standard PBR ambient term. Today only the sky colour and AO are used |
| 62 | **AmbientOcclusion** (GTAO) | DepthPrep (linearise + HiZ #55), ComputePass (GTAO main, `ambientOcclusion.slang` upgraded), ComputePass (spatial denoise) | `depth`, `normals`, `camera`, `radius`, `quality` → `ao` (+`bentNormal`) | ✓ | XeGTAO, FidelityFX CACAO, UE GTAO | **P1** | forward-plus has HBAO plus a blur by hand. GTAO is the modern baseline. Package what exists first |
| 63 | **ScreenSpaceReflections** | HiZ (#55), ComputePass (`reflections.slang`), resolve/temporal (History #17) | `color`, `depth`, `normals`, `roughness`, `hiz` → `reflections`, `confidence` | ✓ | FidelityFX SSSR, UE SSR | **P1** | It exists by hand in forward-plus |
| 64 | **Atmosphere** | ComputePass ×3–4 (transmittance, multi-scatter, sky-view LUTs; aerial-perspective 3D LUT) on setup and frame pages, FullscreenPass (sky) | `sun` (direction, illuminance), planet and atmosphere params → `skyView`, `transmittance`, `aerial` (3D), `sky` (pass) | ✓ | Hillaire 2020, UE SkyAtmosphere | **P1** | Physically based sky and sun colour; `sky.slang` is a simple gradient |
| 65 | **VolumetricFog** | Image (3D froxel, RGBA16F), ComputePass (inject media + lights + sun shadow), ComputePass (integrate along Z), History (#17, temporal reprojection) | `lights`, `clusters`, `sunShadow`, `camera`, `density`, `anisotropy` → `froxels` (scattering/transmittance), apply in shading | ✓ (3D storage images) | Wronski 2014, UE Volumetric Fog, Godot fog volumes, TLOU2 | **P1** | A standard atmosphere effect that reuses the cluster grid |
| 66 | **ContactShadows** *(screen-space)* | ComputePass (raymarch depth towards the light) | `depth`, `light dir`, `length` → `shadow` mask | ✓ | UE contact shadows, Days Gone SSS | **P2** | Fills the near-field gap of cascades |
| 67 | **VirtualShadowMaps** | ComputePass (mark pages from depth), Compact (#existing) / Scan (allocate), ComputePass (page table), Draw (per page, indirect, `whenChanged`-like cache), physical atlas | `depth`, `entities`, `sun`/`lights` → `physical` atlas, `pageTable` | software page table ✓; sparse binding would be **new kor** | UE VSM | **P2** | High quality, but large. SunShadows' `whenChanged` caching is a first step towards page caching |
| 68 | **DDGI** | RayScene, ComputePass (probe ray trace via TraceRays), ComputePass (blend irradiance + depth moments into octahedral atlases), ComputePass (border copy) | `scene` (RayScene), `volume` (origin, spacing, counts), `lights` → `irradiance`, `distance` atlases, `probeData` | ✓ (RT) | Majercik 2019/2021, RTXGI | **P1** | Dynamic diffuse GI that reuses the ray-traced branch's RayScene |
| 69 | **ReSTIR DI** | History (reservoirs), ComputePass/TraceRays (initial candidates, temporal, spatial reuse, shade) | `gbuffer` (+ previous), `lights`, `scene` → `direct` lighting | ✓ (RT) | Bitterli 2020, RTXDI | **P2** | Many-light direct lighting for the rays branch |
| 70 | **VisibilityBuffer renderer** | Draw (IDs into R32_UINT: instance + triangle), MaterialTable (#43), ComputePass (classify tiles by material), ComputePass (shade) | `entities`, `pack`, `materials` → `color`, `gbuffer`-like outputs | ✓ (64-bit atomics for SW raster) | Burns & Hunt 2013, The Forge VB, Nanite | **P2** | forward-plus's prepass already writes instance IDs. This is the natural next renderer |

#### Ray tracing and reference rendering

| # | Function | Built from | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|
| 71 | **PathTracer** (reference) + **Accumulate** | RayScene, RayTracingPipeline + TraceRays, History-style accumulation buffer reset on camera or scene change | `scene`, `camera`, `materials`, `bounces` → `radiance` (progressive) | ✓ | Falcor PathTracer + AccumulatePass, UE Path Tracer | **P1** | Ground truth for checking every raster technique, and a showcase for the rays branch |
| 72 | **SVGF** *(denoiser)* | History (#17) ×2, ComputePass (temporal accumulate + variance), Repeat (5 à-trous iterations, like Bloom's chain) | `noisy` (diffuse/specular), `normals`, `depth`, `motion` → `denoised` | ✓ | Schied 2017, NRD ReLAX | **P1** | Makes 1-spp RT shadows, AO and GI usable. The Repeat + Previous pattern fits à-trous exactly |
| 73 | **RayTracedShadows** / **RayTracedAO** | TraceRays (shadow rays with mask 1, RayScene's `noShadow`), BlueNoise (#22), SVGF or a simple spatial denoise | `depth`, `normals`, `lights`, `scene` → `shadow`/`ao` | ✓ | UE RT shadows, NRD SIGMA | **P1** | RayScene already supports shadow masks (`noShadow`) |

#### Post-processing

| # | Function | Built from | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|
| 74 | **AutoExposure** | ComputePass (64/256-bin luminance histogram, atomics), ComputePass (percentile average + adaptation), History (#17) or a 1-texel persistent buffer, Time.dt (#1) | `color`, `minEV`, `maxEV`, `low%`, `high%`, `speedUp`, `speedDown` → `exposure` (buffer, read by Tonemap), `ev` (Readback #30 for UI) | ✓ | Unity Auto Exposure, Bevy AutoExposure, UE Eye Adaptation | **P0** | Tonemap takes a fixed `exposure`. HDR scenes need adaptation. Small, standard, uses existing patterns |
| 75 | **ColorGrading** *(LUT)* | ComputePass (bake 32³ LUT from lift/gamma/gain, white balance (Kelvin, #8), saturation, curves) on change; Tonemap reads it | params → `lut` (3D image) | ✓ | UE/Unity LUT-based grading, HDRP | **P1** | Look development without editing shaders. Tonemap gains a `lut` input |
| 76 | **Tonemap: operator choice** *(extend existing)* | Tonemap + Switch | add `operator` (ACES fit, AgX, Khronos PBR Neutral, Reinhard, Uchimura), `hdrOutput` (needs **new kor** HDR swap chain) | ✓ / new kor | UE filmic, Blender AgX, Filament PBR Neutral | **P1** | Only Narkowicz ACES exists |
| 77 | **DepthOfField** | HiZ-less: ComputePass (CoC), downsample, gather (scatter-as-you-gather), composite | `color`, `depth`, `focus`, `aperture` → `color` | ✓ | Jimenez 2014, UE Cinematic DOF | **P2** | Cinematic feature |
| 78 | **MotionBlur** | ComputePass (tile max velocity, neighbour max), gather | `color`, `depth`, `motion` → `color` | ✓ | McGuire 2012 / Jimenez 2014, UE motion blur | **P2** | Needs MotionVectors (#54) |
| 79 | **LensEffects** (vignette, chromatic aberration, film grain, lens dirt, flares) | ComputePass ×1–3 | `color`, params → `color` | ✓ | UE/Unity post stacks, FidelityFX Lens | **P2** | Cheap polish, and easy first Functions for users |
| 80 | **Blur** (Gaussian separable / Kawase / dual filter) | ComputePass ×2, or Repeat (Kawase) | `image`, `radius` → `image` | ✓ | FidelityFX Blur, Unity Kawase | **P1** | A building block used by AO, SSR, bloom variants and UI backgrounds |
| 81 | **Outline** *(selection)* | Selected, Draw (IDs), ComputePass (edge detect), composite | `ids`, `entityId`, `color` → `color` | ✓ | UE selection outline | **P2** | The editor already draws IDs for Pick, so this is reusable for in-game highlights |

#### Compute primitives

| # | Function | Built from | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|
| 82 | **DownsampleChain** *(SPD-style)* | one ComputePass with a global atomic counter, or Repeat + ImageView (Bloom's pattern) | `image`, `kernel` (average/min/max), `mips` → mipped image | ✓ (`globallycoherent` atomics) | FidelityFX SPD | **P0** | A general form of HiZ and Bloom-down. Replaces GenerateMipmaps where blit filtering is wrong (depth, integer) |
| 83 | **Reduce** | ComputePass ×log n (or subgroup-reduce single pass) | `buffer`/`image`, `op` (sum/min/max), `count` → 1-element buffer | ✓ | CUB `DeviceReduce`, Houdini COP stats | **P1** | Bounds, totals and luminance averages. Pairs with Readback |
| 84 | **Histogram** | ComputePass (shared-memory bins + atomics), ClearBuffer | `values` or `image`, `bins`, `min`, `max` → `histogram` buffer | ✓ | CUB `DeviceHistogram` | **P1** | Auto-exposure (#74), debug views |
| 85 | **Sequence / Iota / Fill** | ComputePass | `count`, `start`, `step` (or a constant value) → buffer | ✓ | CUB, `std::iota` | **P1** | RadixSort payloads (indices), initial states. Today each needs a custom shader |
| 86 | **SortFloatKeys** / **SortByKey** wrapper | ComputePass (float → orderable uint flip) + RadixSort + optional un-flip | `keys` (float), `values`, `count`, `descending` → sorted | ✓ | Onesweep, FidelityFX Parallel Sort | **P1** | Particles and transparency need depth sorting. RadixSort takes uints only |
| 87 | **Segmented / Indexed gather-scatter** (Gather, Scatter, Permute) | ComputePass | `values`, `indices` → `values` | ✓ | Thrust/CUB gather/scatter | **P2** | General data plumbing for compaction results |
| 88 | **ImageMath** (Add, Multiply, Lerp, Max of two images; channel shuffle) | one generic ComputePass with a specialization constant per op | `a`, `b`, `op`, `t` → image | ✓ | Blender compositor *Mix*, Houdini Copernicus *Blend*, UE Composure | **P2** | Composition and debugging without writing a shader |

#### Debugging and profiling

| # | Function/node | Built from | Inputs → Outputs | kor | Precedent | Pri | Rationale |
|---|---|---|---|---|---|---|---|
| 89 | **Visualize** | ComputePass (`koral_visualize`: linearise depth, normals ×0.5+0.5, motion vector arrows/colour wheel, uint ID hash colour, heat ramp with min/max, buffer-as-image) + DebugView | `image` or `buffer`, `mode`, `range` → `picture` (→ DebugView) | ✓ | Unity Rendering Debugger, UE buffer visualisation, RenderDoc overlays | **P0** | DebugView takes a ready picture, and each project writes its own heat shader (`clusterHeat.slang`). One node covers the common cases |
| 90 | **ProfilerOverlay** | GpuTimer (#32) outputs, Readback (#30) counters, a text/graph draw pass (kui or DebugDraw) | timings → overlay on Screen | kui | UE `stat gpu`, Unity Frame Debugger | **P2** | In-game profiling in the player (the editor already shows costs on node cards) |
| 91 | **ImageCompare** | ComputePass (difference, MSE/PSNR via Reduce #83), side-by-side split | `a`, `b` → `difference` image, `mse` (Readback) | ✓ | Falcor ErrorMeasure/SideBySide | **P2** | Validates techniques against PathTracer (#71) and golden images (#35) |
| 92 | **PipelineStatistics / OcclusionQuery** | atomic | pass → `primitives`, `invocations`, `samplesPassed` | **new kor** (query pools) | RenderDoc pipeline stats, UE `stat RHI` | **P2** | Overdraw and culling verification |

### 4.6 Infrastructure the list depends on (not nodes, but changes to how nodes work)

* **Per-function level metadata** (`"level"` in `.kfunc`, next to `purpose`). Without it, every proposed primitive
  (#82–88) and scene Function (#41, #43) would show as "technique". See 1.13.
* **`expands` vs `composite`**: ComputePass (#28) and FullscreenPass (#29) are *macro nodes* that expand like
  Function calls, but are atomic to the user. The catalog needs a way to say so.
* **Pages as feature slots**: the URP/Bevy/UE Blendable-Location pattern of *named insertion points* ("before
  tonemap", "after opaque") would let a project drop a Function onto a slot instead of rewiring. This is optional, but
  it is how every engine in section 3 makes L4 composable.
* **Field-ness of ports**: a port worked out once per ForEach element is a *field* in Blender's sense. Many proposed
  L3 nodes produce per-element outputs (#40, #46), so marking it matters (see 5.3).

### 4.7 The top 10 P0 proposals, in suggested order

1. **Time** (#1): trivial, and removes a GPU workaround in TAA.
2. **DebugLabel** on every pass and page (#31): trivial, and every capture becomes readable.
3. **History** (#17): kor's `ReadPrevious` is already there, and it unblocks temporal everything.
4. **RenderTarget** (transient, aliased) (#15): memory, and less wiring.
5. **Jitter** + **LastFrame** value (#2, #3).
6. **ForEach `previousMatrix`** (#40) → **MotionVectors** (#54) → **TAA** Function (#53).
7. **HiZ** / **DownsampleChain** (#55, #82) → **InstanceCull** Function (#42).
8. **Lights** (#41) + **ClusteredLights** (#59) + **PointShadows** Function (#60): package forward-plus.
9. **AutoExposure** (#74) with **Readback** (#30).
10. **ComputePass** composite (#28) + **Visualize** (#89) + **DeviceSupports** (#4): ergonomics and fallbacks.

(20 entries are P0, 44 are P1 and 28 are P2, for 92 entries in total.)

## 5. Recommendations on presentation

### 5.1 What the surveyed editors do

| Editor | Abstraction / kind | Composite vs atomic | Other affordances |
|---|---|---|---|
| **Unreal Blueprint** ([scripting](https://docs.unrealengine.com/4.26/ProgrammingAndScripting/Blueprints/Scripting), [collapsing](https://dev.epicgames.com/documentation/en-us/unreal-engine/collapsing-graphs-in-unreal-engine), [colours (community)](https://unrealcommunity.wiki/blueprint-fundamentals-r6eucpmu)) | Title-bar **colour by kind**: red events, blue functions, green pure getters, grey macros, collapsed graphs and flow. **Pure** nodes (no exec pins, recomputed) look different from **impure** ones (exec pins); purity is togglable on some nodes | **Collapsed Graph** node (grey, with exec pins) and **Macro**; *Collapse Nodes* / *Expand Node*; *Collapse to Function*; double-click opens a tab, with a **breadcrumb** path at the top of the graph | Comment boxes with colour; reroute nodes |
| **UE Material editor** | Expression nodes are uniform; **Material Function Call** nodes show the function's name and its declared inputs/outputs (header styling differs from plain expressions; not verified for UE5); Named Reroute declaration/usage pairs share a colour | Material Functions open on double-click; Material Layers in a separate stack UI | Live preview thumbnails on nodes; *Blendable Location* sets where a post material runs |
| **Unity Shader Graph** ([Sub Graph](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.1/manual/Sub-graph)) | Categories in the Create Node menu (Artistic, Channel, Input, Math, Procedural, Utility, UV); **Master Stack** blocks are visually different from free nodes | **Sub Graph** nodes, created by *Convert To Sub-graph*; double-click opens the asset | Per-node preview, precision (half/float) badges, colour modes (*Category*, *Precision*, *User Defined*) |
| **Unity VFX Graph** ([contexts](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@9.0/manual/Contexts.html)) | Two visual layers: large vertical **contexts** (execution: Spawn → Initialize → Update → Output) holding **blocks**, versus small horizontal **operators** (values) | Subgraph Blocks / Subgraph Operators / Subgraph Contexts | Flow wires (vertical) and value wires (horizontal) have different directions |
| **Unity Render Graph Viewer** ([viewer](https://docs.unity3d.com/Manual/urp/render-graph-viewer-reference.html)) | Not an editor: a pass × resource grid; read access green, write red, culled passes filterable, async-compute flag | — | Resource lifetimes as bars |
| **Blender** ([node parts](https://docs.blender.org/manual/en/5.0/interface/controls/nodes/parts.html), [4.0 panels](https://developer.blender.org/docs/release_notes/4.0/node_editor/)) | Header **colour by node class** (input, shader, texture, colour, vector, converter, group, layout) set by theme. **Socket shapes**: circle = single value, **diamond = field** (per-element), diamond-with-dot = could be either. Socket colours by data type | **Node groups**: green header, a *group* icon, Tab to enter/exit, breadcrumbs in the top-left; **collapsible panels** inside nodes | **Frames** (coloured, labelled backgrounds); hiding unused sockets; node collapse (header only) |
| **Houdini** ([badges](https://www.sidefx.com/docs/houdini/network/badges.html), [flags](https://www.sidefx.com/docs/houdini/network/flags.html), [options/themes](https://sidefx.com/docs/houdini/network/options.html)) | **Node shapes** per type (themes ship three shape sets) as well as colours; network *contexts* (SOP/VOP/COP/LOP) each in their own pane | **HDA badges**: locked (synced) vs unlocked (editable) asset; subnets show an inside/outside path bar; *dive in* with `I`, *up* with `U` | **Flags** on the node edge: bypass (yellow), display, render (purple), template; badges for errors, warnings, time-dependence, cooking, comments; badge size configurable |
| **Falcor RenderGraphEditor** | All nodes are passes (L4); ports named by channel | none (the Python script is the composite) | — |
| **Niagara** | The stack is the main view: groups (System/Emitter/Particle Spawn/Update) are coloured bands; modules are rows | Modules open into graphs; **scratch pad** modules local vs exported assets | Per-module enable checkbox; module "uses" pin badges |

### 5.2 What KoralEngine shows today

From `editor.cpp:3767` and `docs/editor.md` ("How abstract a node is", "The graph, read at a glance"):

* **Colour = category** (Pipelines violet, Passes orange, Resources amber, Values blue, Math teal, Vectors indigo,
  Logic purple, Cameras green, Scene green-teal, Components green, Flow slate, Functions cyan). A Material icon per type.
* **Level**: five steps at the end of the title bar, as many lit as the level plus one, and the level in words under
  the title (`Draw · command`).
* **Composite**: a Function call is drawn as a **stack of cards**.
* **Loops**: a Repeat or ForEach is a framed container node with its count (`x4`), and nodes in a loop say their
  turns.
* **Cost**: GPU time and share of the frame on pass nodes (faint, amber ≥5%, red ≥15%), summed on Function calls.
* **Wires coloured by what they carry** (image teal, buffer amber, sampler violet, pipeline orange, mesh green,
  number blue, Bundle pink).

This is already closer to Houdini and Blender than to most engines. The recommendations below refine it.

### 5.3 Recommendations

1. **Keep colour for *purpose* and add *shape* for level.** The current five-step meter is good for reading, but
   small at low zoom. Houdini's per-type shapes and Blueprint's pure/impure split suggest a second, zoom-robust
   channel:
   * **L0 value**: compact, rounded "pill" card, no title-bar band (like Blueprint pure nodes, Blender converter
     nodes). These nodes are cheap and recomputed, and should look light.
   * **L1 resource**: a card with a solid left edge, the resource-kind colour already used for its wire (teal image,
     amber buffer…), and a thumbnail when watched (the eye).
   * **L2 command**: a card with a thick title band and a **play/flow notch** on the left edge, the only nodes that
     *run*. Optionally show an implicit "execution order" index from `Runtime::Frame()` (the Unity Render Graph
     Viewer's pass column), which tells users why order matters only here.
   * **L3 scene**: a card with a **world/globe stripe** and, for loop sources, the existing container frame.
   * **L4 technique**: the existing stacked-card look, plus a header gradient. It should look *heavier* than
     everything else at a glance.

   Keep the meter for exact reading. At low zoom, render shape only.
2. **Mark per-element ports the way Blender marks fields.** Use a **diamond** socket for any port whose value is
   worked out once per ForEach/Filter element (an `entityId`, a `matrix`, a value derived from it), and a circle for
   single values. This answers the most common kgraph confusion ("why does this Draw run N times?") at the port,
   before the user finds the loop frame. A diamond wire into a single-value port (a broadcast is fine; the reverse is
   a Collect) can warn on hover.
3. **Fix the level semantics shown on cards** (section 1.13). Show **Reference**, **Switch** and **Element** at the
   level of what they carry. Give Functions a per-function level from `.kfunc` metadata, so Scan, RadixSort and
   Compact read as *primitives* (suggest: L4 with a "primitive" subtitle, or a sixth "algorithm" glyph), and Camera
   reads as *scene*. Move DataBuffer and MeshBuffers to *resource*.
4. **Make composite three-valued and show each state differently.**
   * **atomic**: a plain card.
   * **expands** (ComputePass, FullscreenPass, Script, MeshPack, RayScene): a single card with a small **"⋯ n"
     badge** ("expands to 2 passes", "N BLAS + TLAS"). Hovering lists what it becomes (from `Runtime::Frame()`).
   * **composite** (Function call): the stacked card, with a **lock badge** for engine functions (Houdini's
     locked/unlocked HDA). An engine function opened and edited becomes the project's own, so show the badge
     changing from "engine" to "project" and offer *Revert to engine version*.
5. **Collapsed and expanded views of a composite in place.** Blueprint (*Expand Node*), Blender (Tab into a group)
   and Houdini (dive in) all let users see inside without losing context. Recommend:
   * Double-click → open the function on its own page with a **breadcrumb** (`scene ▸ show ▸ Bloom`). editor.md
     says a Function is "opened" on a double-press; add the breadcrumb bar and Esc/`U` to go up.
   * **Peek** (hover with a modifier key, or a chevron on the card) → a translucent inline preview of the internal
     nodes, with per-internal-pass costs. The Function card already sums costs, so this explains them.
   * **Convert selection to Function** (Shader Graph *Convert To Sub-graph*, Blueprint *Collapse to Function*).
     This is the single most useful authoring action for getting users from L2 graphs to L4. FunctionInput and
     FunctionOutput nodes are created from the wires that cross the selection boundary.
6. **Group the node list by level first, purpose second.** The Add Node menu is currently split into "scene"
   categories, "vulkan" categories and Functions by purpose (`editor.cpp:2927`). Offer a toggle between *By purpose*
   (lighting, shadows, post…, the `.kfunc` `purpose` tags, as Shader Graph does) and *By level* (Values → Resources
   → Commands → Scene → Techniques, as Houdini separates contexts). Default new users to *Techniques* at the top,
   because that is how Falcor and Niagara onboard people.
7. **Flags on pass nodes, Houdini-style.** Passes already have `enabled`, `views` (all/game/editor), `async`, and a
   page rate (setup/fixed/frame). Show them as **edge flags** on L2 and L4 cards: a bypass bar when `enabled` is
   false, a small queue glyph for `async`, an eye-with-slash for editor-only, ⟳ / ⏱ / ▶ for setup / fixed / frame
   rate. They are visible state that today lives only in the inspector.
8. **A level filter and a level overview.** A toolbar control to *dim* all nodes below or above a level, for example
   "show me only commands and techniques" to read the frame's structure, or "only values" to debug math. The Pass
   flow panel (memory: *Three renderers*) is already the L4/page-level view; link a card there to the Function or
   page it summarises.
9. **Wire styles by level crossing.** Value wires (L0) thin; resource wires (L1) thicker and coloured by kind (as
   now); a wire that carries a **history** resource (#17) dashed, Unity-Viewer-style "previous frame"; a wire from a
   transient RenderTarget (#15) shown with its lifetime span on hover.
10. **Status badges** (Houdini badges): error, warning, "rebuilt this frame" (`Status(id)->builds` changed),
    "resting" (a setup pass that will not record), "idle" (`idle` ≠ 0), "watched". Several of these are already
    spelled out in node notes; badges make them scannable.

### 5.4 Summary of what to adopt

| Adopt | From | Effort |
|---|---|---|
| Shape per level (pill / edge / notch / stripe / stack) plus the existing meter | Houdini shapes, Blueprint pure vs impure | Medium (kui `GraphNode` already has `accent`; it needs a shape enum) |
| Diamond sockets for per-element (field) ports | Blender fields | Small: the runtime knows which ports are per-element |
| Level fixes: inherit on Reference/Switch, per-function level, DataBuffer/MeshBuffers → resource | — | Small |
| `atomic` / `expands` / `composite` with badges, plus an engine/project lock badge on Functions | Houdini HDA badges | Small–medium |
| Breadcrumbs, peek, *Convert selection to Function* | Blueprint, Blender, Shader Graph | Medium |
| Pass flags on cards (enabled, async, views, rate) | Houdini flags | Small |
| Node list by level or by purpose | Shader Graph categories, Houdini contexts | Small |
| Render-graph viewer grid (pass × resource, read/write colours, lifetimes, culled) | Unity Render Graph Viewer | Medium: the data exists in `Runtime::Frame()` |
