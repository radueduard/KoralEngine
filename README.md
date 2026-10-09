# Koral Engine

A graphics-programming engine on the Koral framework: an ECS, Kotlin/C# scripts, and render passes described in a node editor.

See [docs/design.md](docs/design.md) and the plan, [docs/plan.md](docs/plan.md).

- `modules/kecs`: the ECS, a library by itself. See [docs/kecs.md](docs/kecs.md).
- `modules/kgraph`: the pass graph, as data and running. Needs the Koral SDK. See [docs/kgraph.md](docs/kgraph.md).
- `modules/keditor`, `koral-editor`, `koral-play`: the editor, and a project run by itself. See [docs/editor.md](docs/editor.md).
- `modules/kscript`, `modules/kengine`, `bindings/csharp`, `bindings/kotlin`: scripts in C# and Kotlin, over the engine's C interface. See [docs/scripts.md](docs/scripts.md).

## Building

The engine builds against a Koral SDK: a Koral checkout's `stage/debug` or `stage/release`, made by that
checkout's `stage` target in the matching configuration. The engine finds the one that goes with its own
build type, given the checkout: `-DKORAL_FRAMEWORK_DIR=<checkout>`, the `KORAL_FRAMEWORK_DIR` environment
variable, or a checkout beside this one. A Debug build uses `stage/debug`; any other uses `stage/release`.

```
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug
cmake --preset release && cmake --build --preset release && ctest --preset release
```

The presets build into `cmake-build-debug` and `cmake-build-release`. `-DCMAKE_PREFIX_PATH=<sdk>` still
works, and wins over what would be found.

A project with Kotlin scripts records which build of the engine runs it, in its `gradle.properties`. The
editor writes that for its own build whenever it runs the project or reopens it with its scripts, so the
build that is open is the build that runs.
