# Koral Engine

A graphics-programming engine on the Koral framework: an ECS, Kotlin/C# scripts, and render passes described in a node editor.

See [docs/design.md](docs/design.md) and the plan, [docs/plan.md](docs/plan.md).

- `modules/kecs`: the ECS, a library by itself. See [docs/kecs.md](docs/kecs.md).
- `modules/kgraph`: the pass graph, as data and running. Needs the Koral SDK. See [docs/kgraph.md](docs/kgraph.md).
- `packages/koral/shaders`: the shader library — the base modules every shader imports, and the engine's passes. See [docs/shaders.md](docs/shaders.md).
- `modules/keditor`, `koral-editor`, `koral-play`: the editor, and a project run by itself. See [docs/editor.md](docs/editor.md).
- `modules/kscript`, `modules/kengine`, `bindings/csharp`, `bindings/kotlin`: scripts in C# and Kotlin, over the engine's C interface. See [docs/scripts.md](docs/scripts.md).

## Building

The engine builds against a Koral SDK: a Koral checkout's `stage/debug` or `stage/release`, made by that
checkout's `stage` target in the matching configuration. The engine finds the one that goes with its own
build type, given the checkout: `-DKORAL_FRAMEWORK_DIR=<checkout>`, the `KORAL_FRAMEWORK_DIR` environment
variable, or a checkout beside this one. A Debug build uses `stage/debug`; any other uses `stage/release`.

```
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug -j 8
cmake --preset release && cmake --build --preset release && ctest --preset release -j 8
```

Run the tests eight at a time (`-j 8`): each test is a process of its own, the ones that time themselves run alone, and
the whole run takes under five minutes. The Forward+ sample's tests are slow and left out unless the build is configured
with `-DKORAL_ENGINE_FORWARD_PLUS_TESTS=ON`.

The presets build into `cmake-build-debug` and `cmake-build-release`. `-DCMAKE_PREFIX_PATH=<sdk>` still
works, and wins over what would be found.

`koral-editor` and `koral-play` run a project's C# or Kotlin scripts themselves: the build puts the engine's C#
layer in `dotnet/` and its Kotlin layer, with Kotlin's compiler, in `kotlin/` beside them. C# needs the .NET 10
runtime on the machine; Kotlin needs Java 25 or later (`JAVA_HOME`, or the JDK the engine was built with).

Configured with `-DKORAL_ENGINE_SHIP_RUNTIMES=ON`, the build puts a .NET 10 runtime in `dotnet/` and a JRE in `jre/` beside
the programs too (about 170 MB), so that they run a project's scripts on a machine that has neither installed.
