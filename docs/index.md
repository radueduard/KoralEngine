# Koral Engine

A graphics-programming engine on the [Koral](https://radueduard.github.io/Koral/) framework. A scene is a world of
entities — an ECS — and a **pass graph**: the GPU work that draws and simulates it, drawn as nodes in an editor and
run as the frame graph Koral schedules. Entities are given behaviour by **scripts** in C# or Kotlin, compiled and
reloaded while the editor runs.

<div class="grid cards" markdown>

-   **Getting started**

    Build the engine, make a project in the editor, give an entity a script, and draw something of your own.

    [Start here](getting-started/index.md)

-   **The editor**

    The panels, the graph, the scene view, playing, objects, prefabs, assets and importing models.

    [The editor](editor.md)

-   **The pass graph**

    Every node, how pages run, how resources and entities flow into passes, functions and loops.

    [The pass graph](kgraph.md)

-   **Scripts**

    Components and hooks, materials, scripts that make resources, scripts that record a pass, conditions.

    [Scripts](scripts.md)

</div>

## What is in it

| | |
|---|---|
| **koral-editor** | A window of panels over one scene: the graph, the scene as it runs, the entities, an inspector, the frame as the GPU ran it. See [The editor](editor.md). |
| **koral-play** | Runs a project by itself, with its scripts — in a window, or with none for a server's scene. |
| **kgraph** | The pass graph: nodes kept as data, and the runtime that turns them into GPU objects and passes. See [The pass graph](kgraph.md). |
| **kecs** | The ECS, a library by itself: no GPU, no framework. See [The ECS](kecs.md). |
| **Scripts** | C# and Kotlin over the engine's C interface: components, materials, resource and pass scripts, conditions. See [Scripts](scripts.md). |
| **The shader library** | Slang modules every shader imports, and the engine's own passes. See [The shader library](shaders.md). |

## Where it comes from

The engine is built on Koral, a C++ framework for real-time graphics on Vulkan. What a graph node does is, almost
always, one thing Koral does: an Image node is a `kor::Image`, a Dispatch is a dispatch. Koral's own wiki is at
[radueduard.github.io/Koral](https://radueduard.github.io/Koral/).
