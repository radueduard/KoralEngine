# Getting started

Three pages, in order:

1. [Building the engine](building.md): the Koral SDK it builds against, and `koral-editor` and `koral-play`.
2. [A first project](first-project.md): a project made in the editor, a lit scene, a script that turns an entity, and
   a pass of your own.
3. [Concepts](concepts.md): projects, scenes, the world, the graph and its pages, scripts and modules — and which guide
   says more about each.

What it needs:

- A GPU with Vulkan 1.3 and its driver.
- A C++23 compiler, CMake, Ninja and [vcpkg](https://vcpkg.io).
- A checkout of [Koral](https://github.com/radueduard/Koral), staged (see [Building the engine](building.md)).
- For C# scripts, the .NET 10 runtime; for Kotlin scripts, Java 25 or later.
