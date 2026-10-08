# Scripts

A script is a class added to entities. Its public fields are a component: data the engine keeps, shows in
the inspector, saves with the scene and carries across a reload. Its methods are called on each entity that
has it. Scripts are written in C# or Kotlin.

```csharp
public sealed class Spin : Script
{
    public float Speed;             // in the inspector, in the file, kept across a reload
    public float Angle;
    private int _frames;            // this run's only

    protected override void Update(float delta) => Angle += Speed * delta;
}
```

```kotlin
class Spin : Script() {
    var speed = 0f
    var angle = 0f
    private var frames = 0

    override fun update(delta: Float) { angle += speed * delta }
}
```

## What is called, and when

| C# | Kotlin | When |
|---|---|---|
| `OnCreated` | `onCreated` | The script was added to the entity. Once, switched on or off. |
| `OnEnabled` | `onEnabled` | It was switched on, or added switched on. |
| `Start` | `start` | Before its first update. Once. |
| `FixedUpdate(step)` | `fixedUpdate(step)` | A steady step of the game: 0 to 8 times a frame. |
| `Update(delta)` | `update(delta)` | Once a frame, while it is switched on. |
| `LateUpdate(delta)` | `lateUpdate(delta)` | After every script's update. |
| `Prepare` | `prepare` | `GraphicsScript` only. After the updates and before the passes. |
| `OnDisabled` | `onDisabled` | It was switched off, or is about to go while on. |
| `OnDestroyed` | `onDestroyed` | It is about to go: removed, or its entity destroyed. |

Before each call the fields hold what the engine has, and after it the engine has what they hold. A method
may change the world: create and destroy entities, add and remove components, itself included.

A script that throws is logged once for each kind of call, and the other scripts carry on.

## Fields

| | C# | Kotlin |
|---|---|---|
| Numbers | `float`, `double`, `int`, `long`, `short`, `byte` and the unsigned ones | `Float`, `Double`, `Int`, `Long` |
| True or false | `bool` | `Boolean` |
| Vectors | `Vec2`, `Vec3`, `Vec4`, and the `IVec` and `UVec` ones | `Vec2`, `Vec3`, `Vec4` |
| Others | `Mat4`, `Quat`, `Entity` | `Mat4`, `Quat`, `Entity` |

A public field (a public `var` in Kotlin) of one of these is the component's. A private one is the
object's own: it lasts while the script runs and is gone on a reload. A public field of any other type is
an error when the scripts load, which names the field.

A new component starts with every field at zero. An initialiser in the class does not apply: the engine
makes the component, not the class.

## The scene from a script

`World` is the scene the script is in:

```csharp
var bullet = World.Create("Bullet");
World.Add(bullet, "Mover");                              // a script, by its class name
World.Set(bullet, "Mover", "Speed", 12);                 // a field, by name
World.SetResource(bullet, "blur", "target", "canvas");   // a pipeline's component: a node's output

foreach (var entity in World.With("Enemy"))              // every entity with a component, or a tag
{
    var enemy = World.Read<Enemy>(entity)!;              // another entity's script, as an object
    enemy.Health -= 1;
    World.Write(entity, enemy);
}
World.Buffer("light camera")?.Write(matrix);             // what a node made: a Koral Buffer, an Image
```

Kotlin has the same members in lowerCamel: `world.create`, `world.with("Enemy")`, `world.read<Enemy>(entity)`.

## Graphics scripts

A `GraphicsScript` makes what the graph's passes take and nothing else makes. Its `Prepare` runs after
every update and before anything is drawn, so what it writes is what this frame's passes read. The usual
case is a buffer node the script fills:

```csharp
public sealed class LightCamera : GraphicsScript
{
    public float Reach;
    protected override void Prepare()
    {
        var light = World.Read<Light>(Entity);
        if (light is null) return;
        World.Buffer("light camera")?.Write(CameraOf(light, Reach));     // sixteen floats
    }
}
```

The graph has a Buffer node called `light camera`, wired to the `param.light` of the shadow and the lit
pipelines. Nothing in the graph says how the matrix is made: the script does. Ask for the buffer each time,
because a node that is built again makes another.

Koral's matrices are stored a column after another and Slang reads a row after another, so a shader given
one multiplies with the vector first: `mul(float4(position, 1.0), light.viewProjection)`.

## Running a project with scripts

**C#.** Scripts are the `.cs` files in the project's `scripts` folder. Nothing else is needed: they are
compiled when the project starts.

```
koral-engine-dotnet <project>            run it
koral-engine-dotnet <project> --edit     open it in the editor, scripts running
```

Both need `KORAL_SDK` set to the Koral SDK. `koral-engine-dotnet` is built into `<build>/dotnet` when the
machine has the .NET 10 SDK. `System`, `System.Linq`, `System.Collections.Generic`, `Koral` and
`Koral.Engine` are already in scope.

**Kotlin.** Kotlin is compiled by a build, so a project's scripts are a Gradle module that depends on
`koral:koral-engine:0.1.0` (published to `<build>/maven`) and whose `main` is:

```kotlin
fun main(args: Array<String>) = koral.engine.run(args)
```

`run` takes the project's folder, `--edit`, `--classes <dir>` for where the build puts the compiled
classes, and `--sources <dir>` with `--compile <command>` to have a saved source built. It needs
`KORAL_SDK`, `KORAL_ENGINE_LIBRARY` (the path to `libkoral-engine.so`) and
`--enable-native-access=ALL-UNNAMED`.

## Editing while it runs

Save a script and it is loaded again without the scene stopping. The data is the engine's, so it stays:

- A field that keeps its **name and type** keeps its value on every entity.
- A new field, or one whose type changed, starts at zero. A removed one is gone.
- A script that had started is not started again.
- A script whose class is gone stops being called. Entities keep its data, for when it is back.
- Scripts that **do not compile change nothing**. The ones running carry on and the errors are logged.

C# is compiled in the process, into a load context of its own that is unloaded once it is replaced. Kotlin
classes are loaded through a class loader of their own. With `--sources` and `--compile`, the host runs the
build when a source is saved and loads what it made. Without them it watches the classes folder, for an IDE
or `gradlew -t classes` to write into.

## From C, or another language

`koral_engine.h` is what both layers are written on: entities and components by handle, a node's resource
as a `KoralResource*`, and a script type registered as a size, a list of fields and a struct of function
pointers. `modules/kengine/tests/test_capi.cpp` uses nothing else.

## Not yet

- **Text, lists and resources as fields.** A script reaches a resource through `World`, by node id.
- **No play and stop.** Scripts run whenever the scene does, in the editor too. Start the editor with
  `koral-engine-dotnet --edit`: plain `koral-editor` runs no scripts.
- **The inspector doesn't follow a script.** A field a script changes each frame shows its new value only
  when something else makes the panel rebuild.
- **Kotlin has no runner or project template.** A project brings its own Gradle build and `main`. The
  Kotlin layer is tested with no display, and has not been run in a window.
- **C# edits are a new build each time**, not applied in place as the framework's scene scripts are.
- **A script named like an existing plain component** takes that component over.
