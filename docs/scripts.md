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

Every entity has a name, a place in the hierarchy and a transform, which are the world's own and not
components:

```csharp
World.SetParent(wheel, car);                             // it stays where it is in the world
World.SetPosition(wheel, new Vec3(1, 0.5f, 0));          // relative to the car
World.SetRotation(wheel, new Vec3(0, 0, 90));            // degrees, about X, Y and Z
Mat4 placed = World.WorldMatrix(wheel);                  // the car's transform, and its own
foreach (var child in World.ChildrenOf(car)) { ... }     // in order; World.Roots is the top
var found = World.Find("Wheel");
```

A script that is steered reads the window's keys and mouse. In Kotlin that is `input`:
`input.isKeyHeld(Key.eW)`, `input.isMouseButtonHeld(MouseButton.eRight)`, `input.mousePositionDelta`,
`input.mouseScrollDelta`. In the editor it is the editor window's input, whichever panel the pointer is over.

Kotlin has the same members in lowerCamel: `world.create`, `world.with("Enemy")`, `world.read<Enemy>(entity)`. The transform is `world.setPosition`, `world.worldMatrix` and so on.

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

**C#.** Scripts are the `.cs` files in the project's `scripts` folder; the editor's **Assets, New script**
makes one to start from. Nothing else is needed: they are
compiled when the project starts.

```
koral-engine-dotnet <project>            run it
koral-engine-dotnet <project> --edit     open it in the editor, scripts running
```

Both need `KORAL_SDK` set to the Koral SDK. `koral-engine-dotnet` is built into `<build>/dotnet` when the
machine has the .NET 10 SDK. `System`, `System.Linq`, `System.Collections.Generic`, `Koral` and
`Koral.Engine` are already in scope.

**Kotlin.** Kotlin is compiled by a build, and the editor gives a project one: **Assets, New script** with
Kotlin chosen writes a Gradle build at the project's root, whose sources are the `scripts` folder
(`keditor::WriteKotlinBuild` does the same from code).

```
./gradlew edit      the project in the editor, its scripts built again and loaded whenever one is saved
./gradlew play      the project on its own
```

`gradlew` needs a JDK to start with: set `JAVA_HOME`. `gradle.properties` is this machine's — where the Koral
SDK, the engine's library and its Kotlin artifact are — and is written by the editor, not kept with the
project. The build depends on `koral:koral-engine:0.1.0` and runs `koral.engine.MainKt`, which is
`koral.engine.run`: the project's folder, `--edit`, `--frames N`, `--classes <dir>` for the compiled
classes, and `--sources <dir>` with `--compile <command>` to have a saved source built.

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
- **Scripts run only while the scene plays** in the editor (Play, in the Scene panel). Start the editor with
  `koral-engine-dotnet --edit`: plain `koral-editor` runs no scripts.
- **The inspector doesn't follow a script.** A field a script changes each frame shows its new value only
  when something else makes the panel rebuild.
- **C# scripts have no `input`** yet: only Kotlin's do.
- **The Kotlin build's paths are a build's own.** `gradle.properties` points at the engine's build folder;
  an installed engine would need them from elsewhere. A path with a space in it breaks the compile command.
- **C# edits are a new build each time**, not applied in place as the framework's scene scripts are.
- **A script named like an existing plain component** takes that component over.
