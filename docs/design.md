# Koral Engine — design

Status: early design, started 2026-10-08. This file replaces the earlier "Koral Engine Plan" document.
Everything under **Decided** has been agreed. Everything under **Open questions** is still to be decided.

## Intent

Koral Engine is about graphics programming. Unity and Unreal are built around game logic. Koral is built
around GPU resources and render passes, described visually and interactively.

It starts with three features:

1. **An ECS** that is fast and extensible.
2. **Game scripts** that extend the ECS with game logic.
3. **Graphics scripts** that extend the ECS with graphics logic.

All three are tied together by **a visual, interactive description of render passes and GPU operations**:
a node editor.

The engine is written in C++ on top of the Koral framework (`/mnt/w/CLionProjects/GFX`, branch
`v2-development`). Scripts are written in Kotlin or C#. The engine lives in this repository, separate from
the framework.

## Decided

### Pipeline nodes and the per-entity split

Take the simplest case, a compute shader. In the node editor, the user sets the resources the shader uses
in general: an image, a buffer, some constants. Whatever the shader needs that they did not set is per
entity.

So every pipeline's resources split into two descriptor set layouts:

- **The pipeline's set.** Its resources are chosen in the editor and bound once. The pipeline owns the
  descriptor set.
- **The per-entity set.** The engine generates **an ECS component type holding exactly these resources**.
  An entity takes part in the pipeline by having that component, and gets a descriptor set of its own.
  Which entities run a pipeline is therefore an ECS query.

If every resource is set on the pipeline, there is no per-entity set. The pipeline runs once and no
component is created.

### Bindings come from the editor, not the shader

Shaders carry no `[[vk::binding]]`, `layout(set, binding)` or similar annotations. Where a shader has
them, they are ignored.

The engine reflects the shader's parameters, the user puts each on one side of the split in the editor,
and the engine then assigns set and binding numbers by writing them into the compiled SPIR-V. Changing
the split needs no recompile.

### Constants

- **Constants set on the pipeline are specialization constants** (`constant_id`), baked in when the
  pipeline is built. Pipeline-wide push constants are not needed.
- **Per-entity constants are the shader's push constants.** With one dispatch or draw per entity, each
  entity pushes its own values. When batched (below), they become one element per entity in a buffer.

Because of this, the shader's top-level uniform values never need splitting between the two sides.
Uniform and storage buffers are ordinary resources and go on either side.

### Dispatch size

The node decides it, and the user chooses in the node editor: a fixed value, or an expression over
resources (for example `image.width / 8`).

### Graphics pipelines

These are vertex, tessellation and geometry pipelines. They work like compute pipelines, with two
differences:

- **Input: a compatible mesh per entity.** "Compatible" means the mesh provides every vertex attribute
  the vertex shader asks for, matched by semantic as the framework already does (`mesh(SEMANTIC)`). An
  entity whose mesh lacks one is reported, not drawn.
- **Output: a framebuffer, global to the node.**

### Batching (opt in)

The shader is always written for a single entity. Batching is the engine's job, done through reflection
and SPIR-V rewriting, and it is opt in per node. When a node can't be batched, it runs per entity.

To batch, per-entity resources become arrays (buffers packed into storage buffers, images into a bindless
table), indexed by an entity number the GPU supplies:

- **Compute:** one dispatch over all entities, with an extra dispatch dimension as the entity number. A
  shader that uses that dimension itself can't be batched. Entities that need different dispatch sizes
  are padded to the largest.
- **Graphics:** one multi-draw-indirect call, with the draw index as the entity number. Meshes have to
  share vertex and index buffers. The framework's mesh allocator is the starting point.

Per-entity dispatches and draws are built first. Batching comes later and changes nothing in the editor.

### Resource ownership

The engine owns every GPU resource. Entities and pipelines hold references, so a texture or a buffer can
be shared. Resources are refcounted and freed when nothing references them, which is what the
framework's `Resource`/`ResourceRef` and repository already do.

### Scripts

- **Languages.** The engine is C++. Scripts are Kotlin or C#, over the framework's existing bindings plus
  a C API for the ECS.
- **Game scripts** extend the ECS with game logic.
- **Graphics scripts** produce input resources that have no standard way of being made. For example,
  shadow mapping turns lights into cameras. A graphics script runs before the passes that need what it
  makes, reads the entities it cares about (the lights), and writes resources (light cameras, matrix
  buffers) that nodes take as inputs.

### Node types

Everything is a node. Nodes reflect the framework's low-level API, and each object node is made through
its builder: the node's inputs are the builder's settings, and its output is what the builder makes. The
node descriptions are **generated** from the framework, so they stay in step with it.

- **GPU objects:** images, image views, samplers, buffers, buffer views, framebuffers, and pipelines of
  all three kinds (compute, graphics, ray tracing).
- **Shaders,** named by module and entry point (as `Shader::Builder::SetEntryPoint(module, entry)` already
  does with Slang).
- **Meshes,** as generic descriptions: a mesh node describes a vertex layout, not one particular mesh. A
  graphics pipeline takes a mesh node, and an entity runs that pipeline with any mesh that matches its
  layout.
- **Commands:** dispatch, trace rays, and draws. Which draw is the user's choice: `Draw`, `DrawIndexed`,
  the indirect and indirect-count draws, or mesh tasks. Running a pipeline means wiring it into a command
  node.
- **Simple passes:** passes that are only a clear or a blit, with no pipeline.
- **Values:** scalars, vectors and matrices, and the operations on them (arithmetic and the kmath
  functions). An expression such as a dispatch size is a small graph of these.
- **Dependency nodes:** a resource made by another pass appears in this pass as a dependency node. That
  makes the dependency visible in the graph.

Descriptor set layouts and descriptor sets are not nodes. They follow from each pipeline's split.

### Pass order

The order comes from resource usage: a pass that reads what another writes runs after it. The wiring
does not decide it. This is how the framework's frame graph already orders passes.

- **Per-entity resources count too.** If a pass samples an image an entity holds, and another pass
  writes that image, the order follows.
- **A cycle** (A reads what B writes, and B reads what A writes) is an error unless one side reads the
  resource as it was at the end of the previous frame. The framework supports that already
  (`PassBuilder::ReadPrevious`).

### When a shader changes

The pipeline's parameters, and the component generated from its per-entity set, change with it:

- A parameter that keeps its **name and type** stays wired, and on entities keeps its value.
- Any other parameter is reset: unwired on the node, back to its default (or empty) on entities.

Entities whose component changed are moved to the new component type. They are not dropped.

### Saving

Everything is saved as JSON: graphs, nodes and their settings, scenes, entities and their components.
Resources are saved as their builder settings, with files (images, meshes) referenced by path rather
than embedded. The framework's reflection already writes and reads JSON (`ToJson`/`FromJson`).

### Script-defined components

Game scripts can define component types of their own, in the style of Unity's `MonoBehaviour`: a Kotlin
or C# class whose fields are the component's data, and whose methods the engine calls on each one. The
base class is `Script` ("add a script to an entity"), and graphics scripts derive from `GraphicsScript`.

- **Data:** the engine registers the class as a component type defined at run time, with its fields taken
  from the class, and stores the data in the C++ ECS like any other component. It shows in the editor's
  inspector, is saved as JSON, and follows the reload rule below when the class changes. Because the data
  is the ECS's, it survives a script reload while the methods are swapped.
- **Behaviour:** lifecycle methods called on each component, as Unity does: created, started, updated
  (also late and fixed update), enabled, disabled, destroyed. Calling into a script once per entity per
  frame is fine at gameplay scale. Per-entity work at GPU scale belongs in graphics nodes.

### Choosing entities: tags and disabled components

A command node runs on the entities that have its pipeline's generated component. Tags narrow that down:

- **Tags** are components with no data. A node can require tags and exclude tags, for example "has
  `Visible`, not `Hidden`".
- **A component can be disabled on one entity** without being removed. A disabled component matches no
  query, so a node doesn't run on it and a script component's update isn't called. It gets disabled and
  enabled notifications instead. Enabling and disabling is cheap: it flips a bit and doesn't move the
  entity to another archetype.

### One graph per scene

Each scene has one pass graph. It describes everything the scene draws and computes, cameras and views
included.

### Hot reload

Scripts hot reload, Kotlin and C# both, and so do shaders. The ECS's data lives in the C++ engine, not
in the scripts, so it survives a reload. Component types that change follow the same rule as a changed
shader: same name and type keep their values, anything else resets.

### The ECS

The earlier plan's ECS (kecs) is kept and adapted. That plan had an archetype ECS, component types
defined at run time and named by reflection, cached queries, change ticks, deferred commands, observers,
a parent/child hierarchy, snapshots across reload, and a C API under C# and Kotlin.

What the engine adds to it:

- **Components generated from shaders.** A pipeline's per-entity set becomes a component type at run
  time, holding references to resources and per-entity constants.
- **Component types that change shape.** When a shader or a script changes a component type, the
  entities that have it are migrated by the rule above.
- **Components that hold GPU resources.** Copying, moving and destroying a component keeps the
  resources' reference counts right.
- **Tags and per-entity enable bits** on every component, which queries respect (see **Choosing
  entities**).
- **Managed components** (script classes) whose data the ECS stores and whose methods it calls.
- **A query per pipeline node.** Change ticks say which entities' descriptor sets must be rebuilt, so an
  unchanged entity costs nothing.
- **JSON through reflection**, for saving.

### The editor

The node editor is the main interface, built on koral-ui. koral-ui has no node-graph widget yet: nodes
with ports, wires, panning and zooming, selecting and moving several nodes, and an inspector per node.
That has to be built.

## What the framework needed

Five things, all built on GFX `v2-development` on 2026-10-08. [plan.md](plan.md) has the detail.

- **Shaders reflect their specialization constants** and every parameter by name.
- **Pipelines number their shaders' descriptors**: `SetBinding(name, set, binding)` on each pipeline
  builder, written into the SPIR-V without recompiling.
- **Builders are described** (`kor::builders`), and objects are built from a description and values.
- **Resource sets** (`kor::ResourceSet`) give a frame graph per-entity resources under one name.
- **A node editor** in koral-ui (`kui::NodeEditor`).

## Open questions

None at the moment. The plan to build this is [plan.md](plan.md).
