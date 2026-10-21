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
| `Prepare` | `prepare` | `GraphicsScript` only. After the updates and before the passes — in the editor, played or not. |
| `OnDisabled` | `onDisabled` | It was switched off, or is about to go while on. |
| `OnDestroyed` | `onDestroyed` | It is about to go: removed, or its entity destroyed. |

Before each call the fields hold what the engine has, and after it the engine has what they hold. A method
may change the world: create and destroy entities, add and remove components, itself included.

A script that throws is logged once for each kind of call, and the other scripts carry on.

Only what a class does is called: a hook it leaves as `Script` has it — no `LateUpdate` of its own, no `FixedUpdate` —
is never called on its entities, so thousands of a script that only updates cost an update each and nothing more.

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

A new component starts as its class initialises its fields: the engine reads the scripts' sources for it
(`kscript::Declare`), so it does whether or not anything runs them. A number, true or false, and a vector,
quaternion or matrix made of numbers (`new(1f, 0.5f, 0)`, `Vec3(1f, 0.5f, 0f)`) are understood; anything else
starts at zero, and the editor's notes say so. The same reading is how an editor started without a project's host
knows its scripts' components: to give an entity one, and to offer a node of it.

An `Entity` field is another entity of the scene for the script to use: it starts as none (`Entity.Null`, whatever
it is said to start as), and is set in the inspector — chosen from the scene's entities, or an entity dragged there
from the scene tree. A prefab keeps what one of its entities holds of another: each instance's holds its own.

A number field can say what it is kept within: `[Range(1, 179)]` or `[Min(0)]` in C#, `@Range(1f, 179f)` or
`@Min(0f)` in Kotlin (`koral.engine.Range`, `koral.engine.Min`), on the field's line or the line before; and
`[Logarithmic]` (`@Logarithmic`), that it is dragged by so much of what it is — a distance, an intensity, a size. It is read
from the source with the rest, and the inspector's drag boxes — and a Script node's `field.<name>` inputs — keep to it,
dragged or typed; for a vector, each of its numbers. An unsigned field is never less than 0. Nothing is checked as
the script runs: a script can set a field to anything.

```csharp
[Range(1, 179)] public float FieldOfView = 60f;
[Min(0), Logarithmic] public float Intensity = 1f;
```

## The engine's package

Every project has the engine's own scripts as well: `Camera`, `DirectionalLight`, `PointLight`, `SpotLight` and
`Sky`, in `packages/koral/scripts`, in C# and in Kotlin. They are data and nothing else — what a light does is the
graph's, through a ForEach of it — and are compiled with a project's own (Kotlin's build has them as a source
folder), so a script reads one as it reads its own: `World.Read<PointLight>(entity)!.range`. Their fields are
lowerCamel in both languages, as the graph and the scene files name them. `kengine_package_directory()` says
where the package is; `KORAL_ENGINE_PACKAGE` in the environment moves it.

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
World.SetOrientation(wheel, turned);                     // or a Quat, kept as given: for a turn integrated each step
Mat4 placed = World.WorldMatrix(wheel);                  // the car's transform, and its own
foreach (var child in World.ChildrenOf(car)) { ... }     // in order; World.Roots is the top
var found = World.Find("Wheel");
```

### Many entities, one script

A script on each of thousands of entities is a call into the script's language each, every frame: fine for a few
hundred, most of the frame for tens of thousands. One script that moves them all is two calls a frame however many
there are — the transforms of many at once, three floats an entity, in the order the entities are given:

```kotlin
class Spinner : Script() {
    var speed = 90f
    private var cubes: Entities? = null              // where the engine reads them: made once, given each frame
    private var rotations = FloatArray(0)            // kept, so that a frame makes nothing new

    override fun update(delta: Float) {
        val cubes = cubes ?: Entities.of(world.childrenOf(entity)).also { cubes = it }
        rotations = world.rotationsOf(cubes, rotations)
        for (i in 1 until rotations.size step 3) rotations[i] += speed * delta     // about Y
        world.setRotations(cubes, rotations)
    }
}
```

```csharp
var turned = new Vec3[cubes.Length];                 // kept from frame to frame
World.RotationsOf(cubes, turned);
for (var i = 0; i < turned.Length; ++i) turned[i].Y += Speed * delta;
World.SetRotations(cubes, turned);
```

`positionsOf`, `rotationsOf`, `scalesOf` and `setPositions`, `setRotations`, `setScales` (C#: `PositionsOf`, …, spans of
`Entity` and `Vec3`); from C, `kecs_rotations` and the rest. `orientationsOf` and `setOrientations` are the rotations as
quaternions, four floats an entity, x, y, z, w (C#: spans of `Quat`; from C, `kecs_orientations`): what a simulation that
integrates rotations writes back, with nothing lost to Euler angles. One not alive reads as the identity. An entity no longer alive reads as zeros (a scale as ones)
and is not set; the setters say how many were.

What each has of its own stays a component, read and written the same way: a field of many at once, its numbers one
entity's after another — `world.floatsOf(cubes, "SpinSpeed", "degreesPerSecond", speeds)`, `setFloats`, and `intsOf`
and `setInts` for whole numbers and true or false (1 and 0); `world.fieldWidth` says how many numbers the field is an
entity (1 for a Float, 3 for a Vec3, 16 for a Mat4). C#: `FieldFloats`, `SetFieldFloats`, `FieldInts`, `SetFieldInts`
and `FieldWidth`; from C, `kecs_field_floats` and the rest. One without the component reads as zeros and is not set,
and what is set is marked changed — a Filter grouped by it sees it.

A script that is only data — fields, and no function of its own — is called for nothing and looked at by nothing each
frame: such a component on thousands of entities costs nothing until something reads it.

```kotlin
class SpinSpeed : Script() { var degreesPerSecond = 90f }      // a speed each, in the inspector

// in the script that turns them, each frame:
speeds = world.floatsOf(cubes, "SpinSpeed", "degreesPerSecond", speeds)
rotations = world.rotationsOf(cubes, rotations)
for (i in 0 until cubes.size) rotations[i * 3 + 1] += speeds[i] * delta
world.setRotations(cubes, rotations)
```

A script sees what it does by drawing it: `world.debug` (C#: `World.Debug`; from C, `kengine_debug` and Koral's
`koral_debug_*`) is the scene's debug lines — `line`, `arrow`, `point`, `box`, `sphere`, `capsule` and the rest, outlined
or filled, for the frame or for a `duration`. They are drawn over the editor's Scene panel, played or not, and by a
DebugDraw node of the scene's own graph (`koral-play`); the Game panel shows none, as it shows no gizmo. Drawn in
`update`, a shape lasts the frame. `fixedUpdate` runs 0 to 8 times a frame, so a shape drawn there flickers: draw
in `update` what is to be seen every frame, or give it a duration.

```kotlin
override fun update(delta: Float) {
    for (contact in contacts) {
        world.debug.point(contact.position, 0.05f, DebugStyle(color = Vec4(1f, 0f, 0f, 1f), onTop = true))
        world.debug.arrow(contact.position, contact.position + contact.normal * 0.3f)
    }
}
```

A script that is steered reads the window's keys and mouse: `input` in Kotlin — `input.isKeyHeld(Key.eW)`,
`input.isMouseButtonHeld(MouseButton.eRight)`, `input.mousePositionDelta`, `input.mouseScrollDelta` — and `Input`
in C#, `Input.IsKeyHeld(Key.eW)`. In the editor it is the Game panel's: while the pointer is over it, or it was the
last thing clicked.

Kotlin has the same members in lowerCamel: `world.create`, `world.with("Enemy")`, `world.read<Enemy>(entity)`.
In either language `Set` takes whatever the field is — a number, true or false, a vector, a `Quat` or a `Mat4`, an
`Entity`, a node's id for a resource, or null: `World.Set(cube, "Color", "Color", new Vec3(1, 0.5f, 0))`,
`world.set(cube, "Color", "color", Vec3(1f, 0.5f, 0f))`. The transform is `world.setPosition`, `world.worldMatrix` and so on.

### Meshes of a script's own

`World.Meshes` makes meshes of any attributes — the usual ones, or a script's own, which a shader reads by the
same name (`float wind : WIND`) — in the vertex buffers the script says:

```csharp
var blade = World.Meshes.Create("blade", vertexCount: 3, [
    MeshAttribute.Floats("POSITION", 3, positions),
    MeshAttribute.Floats("WIND", 1, sway),
], indices, buffers: "POSITION; WIND");                 // a buffer to each ';' — null: all in one
World.Meshes.Give(grass, blade);                         // "$script/blade": what its Mesh holds

var cube = World.Meshes.Read("$cube");                   // any mesh's numbers, read back from the device
var windy = World.Meshes.WithAttributes("$cube", "windy cube", [MeshAttribute.Floats("WIND", 1, new float[cube.VertexCount])]);
var bare = World.Meshes.WithoutAttributes("@tree.gltf#0", "bare tree", "TANGENT", "BITANGENT");
```

A mesh is called `$script/<name>`; made again under the same name, every entity that held it holds the new one.
Script meshes are not saved with the scene: a script makes them when it runs. Kotlin: `world.meshes.create(...)`,
`MeshAttribute.floats(...)`. From C: `kengine_mesh_create`, `kengine_mesh_with_attributes`,
`kengine_mesh_without_attributes`, `kengine_mesh_attribute` (see `koral_engine.h`).

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

## Scripts that make resources

A `ResourceScript` makes resources and fills them on the CPU as they are made: a noise, a table a shader looks up, the
numbers a simulation starts from, and the views and samplers that go with them. No entity has one. A **Script** node of
the graph names it, and gives each resource it declares as an output, by its field's name (see [kgraph](kgraph.md)).
The editor offers a Script node of each among the nodes to add, by the script's name.

```csharp
public sealed class Terrain : ResourceScript
{
    public int Seed = 7;            // the Script node's inputs: given there, or wired
    public float Scale = 4;

    // What it makes: the Script node's outputs, Height, HeightView, Smooth and Particles.
    public Image Height = new(format: "eR32_SFLOAT", width: 256, height: 256, usage: "eSampled | eStorage | eTransferDst");
    public ImageView HeightView = new(image: nameof(Height));
    public Sampler Smooth = new(minFilter: "eLinear", magFilter: "eLinear");
    public Buffer Particles = new(instanceCount: 4096, usage: "eStorage");

    protected override void Init()
    {
        var random = new Random(Seed);
        foreach (ref var h in Height.As<float>()) h = random.NextSingle() * Scale;
        foreach (ref var p in Particles.As<Vec4>()) p = new(random.NextSingle(), random.NextSingle(), random.NextSingle(), 1);
    }
}
```

```kotlin
class Ramp : ResourceScript() {
    var step = 1f
    val table = Buffer(instanceCount = 64, usage = "eStorage")
    val rows = Image(format = "eR32_SFLOAT", width = 4, height = 8)
    val rowsView = ImageView(image = ::rows.name)
    override fun init() {
        for (i in 0 until table.bytes.capacity() / 4) table.bytes.putFloat(i * 4, i * step)
        for (y in 0 until rows.height) for (x in 0 until rows.width) rows.bytes.putFloat((y * rows.width + x) * 4, y * step)
    }
}
```

What it makes are its public fields of `Image`, `Buffer`, `ImageView`, `BufferView` and `Sampler` — `ResourceScript`'s
own, not Koral's — each given, by name, what it is made with: the inputs a node of its kind has. `width`, `height` and
`depth` are an image's extent; flags are one string, `"eSampled | eStorage"`; an enumeration's value is its name,
`"eR32_SFLOAT"`; a view names what it is of, `nameof(Height)` in C#, `::height.name` in Kotlin. What is not given is as a
node of that kind starts: an Image of 512 by 512, sampled and copyable. The Script node can say otherwise of any of
it: `Height.format`, `Height.extent.x`, `Particles.instanceCount`, or a resource wired into a view's `HeightView.image`.
The editor reads them from the source, so they are written as they are — a constant, not worked out — and one not
understood is said, and left as the node starts it.

Its other public fields (vars) are its settings: they are the Script node's inputs, `field.<name>`, and start as the
source says. `Init` is called once each time what it makes is made, with every image and buffer it makes of nothing else
of its own as it was made, zeros to begin with: an image's `Width`, `Height`, `Depth`, `Layers`, `Format` and
`TexelBytes`, and its first level's texels, a layer after another, a row after another from the top (the smaller
levels are made from it); a buffer's every byte. C# has them as a `Span<byte>`, `Bytes`, and `As<T>()`; Kotlin as a
little-endian `ByteBuffer`, `bytes`. They are there while `Init` runs: after it, asking for them throws. A new object is
made for each fill, so nothing is left over from the last.

It runs when what it makes is made, and again whenever it is made again: a size changed, a field of the node changed,
the script was edited and loaded. A script that throws leaves the node failed, saying what was thrown. From C:
`kengine_resource_script_register`, with what it makes as `KengineScriptResource`s, and
`kengine_resource_script_unregister`.

## Running a project with scripts

A runner runs its project's scripts in that project's scenes only: another project opened in the editor it hosts
has its scripts run by its own runner, or none.

A project's scripts are all in one language: the first script made says which, and `project.json` keeps it
(`"scripts": "kotlin"` or `"csharp"`). **Assets, New script** offers only that one from then on; a project from
before has the language of the scripts it has. A script in the other language, put in the folder by hand, runs
nowhere, and the Problems panel says so.

`koral-editor` on a project with scripts opens no window of its own: it knows the project's language before it
opens any, and starts the editor through that language's runner (below) in a process forked for it, in the same
terminal — it ends when that does. A project opened later from the editor's File menu is handed over the same way.

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
- **A ResourceScript runs only with its project's scripts**, as every script does: in plain `koral-editor` what it
  makes is made with nothing in it, and its node says so.
- **Scripts run only while the scene plays** in the editor (Play, in the Scene panel): nothing is created, enabled
  or started before then — a `GraphicsScript`'s `Prepare` alone runs as the scene is edited, for what is drawn — and
  what they made is gone on Stop. Start the editor with
  `koral-engine-dotnet --edit`: plain `koral-editor` runs no scripts.
- **The inspector follows a script ten times a second**, while the scene plays and the entity is picked: a
  field changed every frame is seen at that rate, not every frame.
- **The Kotlin build's paths are a build's own.** `gradle.properties` points at the engine's build folder;
  an installed engine would need them from elsewhere. A path with a space in it breaks the compile command.
- **C# edits are a new build each time**, not applied in place as the framework's scene scripts are.
- **A script named like an existing plain component** takes that component over.
