# Building the engine

The engine builds against a **Koral SDK**: a Koral checkout's `stage/debug` or `stage/release`, made by that
checkout's `stage` target in the matching configuration. Koral's wiki says how:
[Building the SDK](https://radueduard.github.io/Koral/getting-started/building/).

## Where the SDK is found

The engine finds the SDK that goes with its own build type — `stage/debug` for a Debug build, `stage/release` for any
other — given the checkout:

- `-DKORAL_FRAMEWORK_DIR=<checkout>`,
- the `KORAL_FRAMEWORK_DIR` environment variable,
- or a checkout beside this one.

`-DCMAKE_PREFIX_PATH=<sdk>` works too, and wins over what would be found.

## Build and test

```sh
git clone https://github.com/radueduard/KoralEngine.git
cd KoralEngine
cmake --preset release && cmake --build --preset release && ctest --preset release -j 8
```

The `debug` preset is the same in Debug. The presets build into `cmake-build-debug` and `cmake-build-release`. Run the
tests eight at a time: each is a process of its own, and the whole run takes a few minutes. The Forward+ sample's
tests are slow and left out unless the build is configured with `-DKORAL_ENGINE_FORWARD_PLUS_TESTS=ON`.

Configured without a Koral SDK, the engine builds only what needs neither the GPU nor the framework — the ECS, the
asset database and the script declarations — which is what its own CI builds and tests.

## The programs

```
cmake-build-release/koral-editor [project folder]
cmake-build-release/koral-play <project folder | scene file>
```

`koral-editor` and `koral-play` run a project's C# or Kotlin scripts themselves: the build puts the engine's C# layer in
`dotnet/` and its Kotlin layer, with Kotlin's compiler, in `kotlin/` beside them. C# needs the .NET 10 runtime on the
machine; Kotlin needs Java 25 or later (`JAVA_HOME`, or the JDK the engine was built with).

Configured with `-DKORAL_ENGINE_SHIP_RUNTIMES=ON`, the build puts a .NET 10 runtime in `dotnet/` and a JRE in `jre/`
beside the programs too (about 170 MB), so that they run a project's scripts on a machine that has neither installed.

Next: [A first project](first-project.md).
