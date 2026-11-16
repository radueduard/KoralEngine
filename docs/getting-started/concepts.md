# Concepts

The words the engine and its guides use, in the order a project meets them. The [Glossary](../glossary.md) is the
short form; each section here links the guide that says more.

## A project and its scenes

A **project** is a folder: `project.json` (its name, its scenes, the one a run starts with), its scenes, its shaders,
its scripts, its objects and prefabs, and its assets. A **module** is a project others import: its scripts, shaders,
functions, objects and assets become theirs. Physics and networking are modules.

A **scene** is one file, `scenes/<name>.scene.json`: a world of entities and the pass graph that draws and simulates
it.

[The editor: A project](../editor.md#a-project)

## The world

The world is an ECS. An **entity** is a number; a **component** is data an entity has — a transform, a Mesh, a light, a
script's fields. Components are found together by queries, and a pass of the graph runs once for each entity a
**ForEach** or a **Filter** finds, or once for all of them.

[The ECS](../kecs.md)

## The graph

What the GPU does for a scene is its **pass graph**: nodes the editor draws and the runtime turns into GPU objects and
work. A node is a **resource** (an Image, a Buffer, a pipeline, a framebuffer), a **value** (a number, a camera's
matrices), a **pass** (a Clear, a Draw, a Dispatch, a Blit, a TraceRays, a ScriptPass) or a **function** called like a
node. Wires carry what one node makes into another.

Nodes are grouped in **pages**, each run at a **rate**: **Setup** once, **Fixed step** on the game's steady step,
**Every frame** each frame. Within a rate, what a page reads and writes orders it after what writes it; the frame
graph Koral schedules does the rest — barriers, memory, the async compute queue.

[The pass graph](../kgraph.md) · [The editor: The frame](../editor.md#the-frame)

## Shaders

Shaders are Slang, found by module name: the project's `shaders/` first, then its modules', then the engine's
library. A pipeline node names a module and an entry point, and its shader's parameters become the node's inputs,
`param.<name>`, to wire. Materials are data kinds of their own, and a shader claims one by annotation.

[The shader library](../shaders.md) · [Scripts: Materials](../scripts.md#materials)

## Scripts

A **script** is a C# or Kotlin class. One added to entities is a **Setup**: its public fields are a component, and it
takes the **pieces** of what it does — **PerFrame**, **FixedStep**, **BeforeDraw**. Others are run by the graph:
a **ResourceScript** makes and fills resources once, a **PassScript** says what a pass does each frame and can make
what it uses, and a **Conditions** class says which entities a ForEach or a Filter keeps. Scripts are compiled when
the project opens and again when one is saved, and run in the editor from Play to Stop.

[Scripts](../scripts.md)

## Playing and running

In the editor a scene stands still until **Play**: then its scripts run, and the **Game** panel shows it through its
camera. **Stop** puts it back. `koral-play` runs a project by itself, always playing; a scene with no Screen runs
with no window at all — a server's.

[The editor: Playing](../editor.md#playing) · [A scene over a network](../networking.md)
