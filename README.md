# Koral Engine

A graphics-programming engine on the Koral framework: an ECS, Kotlin/C# scripts, and render passes described in a node editor.

See [docs/design.md](docs/design.md) and the plan, [docs/plan.md](docs/plan.md).

- `modules/kecs`: the ECS, a library by itself. See [docs/kecs.md](docs/kecs.md).
- `modules/kgraph`: the pass graph, as data and running. Needs the Koral SDK. See [docs/kgraph.md](docs/kgraph.md).
- `modules/keditor`, `koral-editor`, `koral-play`: the editor, and a project run by itself. See [docs/editor.md](docs/editor.md).
- `modules/kscript`, `modules/kengine`, `bindings/csharp`, `bindings/kotlin`: scripts in C# and Kotlin, over the engine's C interface. See [docs/scripts.md](docs/scripts.md).

Build with the Koral SDK on the prefix path: `cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<Koral>/stage/debug`.
