# Scripts

A script is a class added to entities. Its public fields are a component: data the engine keeps, shows in
the inspector, saves with the scene and carries across a reload. Every script is a **Setup** — initialised once,
switched on and off, gone — and takes the **pieces** of what else it does: **PerFrame** to be called every frame,
**FixedStep** on the game's steady step, **BeforeDraw** before the frame is drawn. Only what it takes is ever called on
it. Scripts are written in C# or Kotlin.

```csharp
public sealed class Spin : Setup, PerFrame
{
    public float Speed;             // in the inspector, in the file, kept across a reload
    public float Angle;
    private int _frames;            // this run's only

    public void Update(float delta) => Angle += Speed * delta;
}
```

```kotlin
class Spin : Setup(), PerFrame {
    var speed = 0f
    var angle = 0f
    private var frames = 0

    override fun update(delta: Float) { angle += speed * delta }
}
```

## What is called, and when

| | C# | Kotlin | When |
|---|---|---|---|
| Setup | `Initialize` | `initialize` | Before anything else is called on it while the scene plays. Once. |
| Setup | `OnEnabled` | `onEnabled` | It was switched on, or added switched on. |
| FixedStep | `FixedUpdate(step)` | `fixedUpdate(step)` | A steady step of the game: 0 to 8 times a frame. |
| PerFrame | `Update(delta)` | `update(delta)` | Once a frame, while it is switched on. |
| PerFrame | `LateUpdate(delta)` | `lateUpdate(delta)` | After every script's update. Optional: a PerFrame need not have one. |
| BeforeDraw | `Prepare` | `prepare` | After the updates and before the passes — in the editor, played or not. |
| Setup | `OnDisabled` | `onDisabled` | It was switched off, or is about to go while on. |
| Setup | `OnDestroyed` | `onDestroyed` | It is about to go: removed, or its entity destroyed. |

Before each call the fields hold what the engine has, and after it the engine has what they hold. A method
may change the world: create and destroy entities, add and remove components, itself included.

A script that throws is logged once for each kind of call, and the other scripts carry on.

An entity running a script is given a tag for each piece the script takes — `PerFrame`, `FixedStep`, `BeforeDraw` —
which a graph's Filter can ask for, and which goes when no script of the entity's takes that piece any more. The tags
are worked out again from the scripts, so a scene's file does not hold them.

A script written before Setup derives from `Script` (or `GraphicsScript`, for `Prepare`) and overrides the hooks it
uses — `OnCreated`, `Start`, `Update`... It still runs, with only what it overrides called; a new one is a Setup.

## Generators

A script that takes **MeshGenerator** makes its entity's mesh. Its public fields are what the mesh is made of, and
its `Initialize(MeshWriter)` writes it — positions, normals, UVs, tangents or attributes of its own, by semantic, and
its triangles. It is given only the writer, never the world: what it makes is of its fields alone.

```csharp
public sealed class Pyramid : Setup, MeshGenerator
{
    [Range(0.1f, 10)] public float Height = 1;
    public void Initialize(MeshWriter mesh)
    {
        mesh.Positions([new(-0.5f, 0, -0.5f), new(0.5f, 0, -0.5f), new(0.5f, 0, 0.5f), new(-0.5f, 0, 0.5f), new(0, Height, 0)]);
        mesh.Indices([0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0, 0, 1, 2, 0, 2, 3]);
    }
}
```

```kotlin
class Pyramid : Setup(), MeshGenerator {
    var height = 1f
    override fun initialize(mesh: MeshWriter) {
        mesh.positions(floatArrayOf(-0.5f, 0f, -0.5f, 0.5f, 0f, -0.5f, 0.5f, 0f, 0.5f, -0.5f, 0f, 0.5f, 0f, height, 0f))
        mesh.indices(intArrayOf(0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0, 0, 1, 2, 0, 2, 3))
    }
}
```

Added to an entity, it is that entity's procedural mesh: the entity's `Mesh` holds what it made
(`$script/gen/Pyramid/<what it was made of>`), so a Filter `with Mesh` finds it as it finds any other. It is made
again whenever its fields change — dragged in the inspector, set while the scene plays, or not playing at all — and
every entity with the same fields shares one mesh, made once. What was made is kept in the project's
`.koral/cache/meshes`: a scene opened before its scripts are compiled, or where they cannot be run, still has it.

A script that takes **ImageGenerator** makes a picture of its entity's material the same way: its
`Initialize(ImageWriter)` sizes the picture and writes its texels, and the material field its `Into` names
(`baseColorMap` unless it says) holds it — made again when its fields change, shared by every entity of the same
fields, kept in `.koral/cache/images`. A picture a compute shader makes is a graph's Image, which a material's picture
field can hold as it is.

A field that `initialize` reads takes **`@Reinitialize`** (C#: `[Reinitialize]`). While the scene plays, when something
other than the script changes it — the inspector, another script — the script starts over: it is switched off and
destroyed, and a new object is made with the entity's fields as they are and started as if it had just been added.
So what it holds in private goes with the old object, and what it allocates in `initialize` it lets go of in
`onDestroyed`. A change the script makes itself does nothing, and neither does a change while the scene is not playing:
it starts from the new value when it next plays. Only what is marked counts, so a `g` that `update` reads stays out of it.

In the editor, an entity's Mesh offers the project's mesh generators beside its meshes, as *Pyramid (procedural)*:
picked, the entity is given that generator, and it makes the mesh. **Save the generator**, in the inspector, writes the
generator with its fields as they are to `assets/generators/<name>.kgen`; dropped on another entity from the Assets
panel, that one is given the generator and **follows the file**, as an entity follows a Material asset: the fields set
in the `.kgen`'s inspector reach every entity that follows it, the one it was saved from among them, but for what an
entity has set itself. A scene opened has what the file says now.

## Fields

| | C# | Kotlin |
|---|---|---|
| Numbers | `float`, `double`, `int`, `long`, `short`, `byte` and the unsigned ones | `Float`, `Double`, `Int`, `Long` |
| True or false | `bool` | `Boolean` |
| Vectors | `Vec2`, `Vec3`, `Vec4`, and the `IVec` and `UVec` ones | `Vec2`, `Vec3`, `Vec4` |
| Others | `Mat4`, `Quat`, `Entity` | `Mat4`, `Quat`, `Entity` |
| Text | `string` | `String` |

A public field (a public `var` in Kotlin) of one of these is the component's. A private one is the
object's own: it lasts while the script runs and is gone on a reload (an edit made in place keeps it). A public field of any other type is
an error when the scripts load, which names the field.

A new component starts as its class initialises its fields: the engine reads the scripts' sources for it
(`kscript::Declare`), so it does whether or not anything runs them. A number, true or false, and a vector,
quaternion or matrix made of numbers (`new(1f, 0.5f, 0)`, `Vec3(1f, 0.5f, 0f)`), and text in quotes (`"hello"`), are understood; anything else
starts at zero, and the editor's notes say so. The same reading is how an editor whose scripts are not running yet
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

Every project has the engine's own components as well, in `packages/koral/scripts`, in C# and in Kotlin. They are
[Data](#data) — what a light does is the graph's, through a ForEach of it — and are compiled with a project's own
(Kotlin's build has them as a source folder), so a script reads one as it reads its own:
`World.Read<PointLight>(entity)!.range`. Their fields are lowerCamel in both languages, as the graph and the scene
files name them. `kengine_package_directory()` says where the package is; `KORAL_ENGINE_PACKAGE` in the environment
moves it.

```
Camera (kind)              near, far
├─ PerspectiveCamera       fieldOfView
└─ OrthographicCamera      height
Light (kind)               color, intensity
├─ DirectionalLight
└─ LocalLight (kind)       range
   ├─ PointLight
   └─ SpotLight            innerAngle, outerAngle
Sky, Instancer
Material (see Materials)   PbrMaterial, BlinnPhongMaterial
```

`Camera`, `Light` and `LocalLight` are kinds: no entity has one itself, and a query for one finds every entity that has
what derives from it — `World.With("Camera")`, a graph's Entity node of `Camera` — its fields read from whichever that is.
A scene, object or prefab saved when a `Camera` was one component (with `orthographic` saying which) loads as the
`PerspectiveCamera` or `OrthographicCamera` it was.

## Data

A class that derives from `Data` is structured data and nothing else: its public fields are a component, shown on an
entity, saved with the scene and set from a script by name, as a script's are — but nothing is called on it, no host
runs it, and it takes no pieces (one that says `PerFrame` is said in the Problems, and is no component). The engine
reads it from its source, so it needs no runtime: a project of Data alone needs neither .NET nor a JRE. What it is for
is the graph's, which reads it.

```csharp
public abstract class Reading : Data         // a kind
{
    [Min(0f)] public float value = 1f;
}
public sealed class Temperature : Reading
{
    public Vec3 where = new(0f, 0f, 0f);
}
```

A class derives from another of a project's, or of the package's: the base's fields come first, in the same places
in each of what derives from it — as the hosts lay them out. An **abstract** Data is a **kind**: nobody has a
`Reading` itself (adding one is refused), but every entity with a `Temperature` is one, `World.With("Reading")` finds
it, and `World.FieldJson(entity, "Reading", "value")` reads the value from its Temperature. A script's own abstract bases
(`abstract class Mover : Setup`) give their fields the same way, but are no kind. `World.Read<T>` and `World.Write`
take a Data as they take a script.

## Materials

A material kind is a class that derives from `Material` — a [Data](#data) of its own: what a drawn thing is made of. Its
public fields are a component, shown on an entity as a script's are, saved with the scene and set from a script by
name (`World.Set(entity, "PbrMaterial", "roughness", 0.2f)`). Nothing is called on it. Besides a script's field types,
it has pictures: a `Texture` field, which starts as the picture it is given — `"@textures/ramp.png"`, or a texel,
`Texture.White`, `Texture.Black`, `Texture.Normal`.

```csharp
public sealed class Toon : Material
{
    public Texture ramp = "@textures/ramp.png";
    public Vec4 tint = new(1f, 1f, 1f, 1f);
    [Range(1, 8)] public int bands = 3;
    [Choices("Hard", "Soft")] public int edge = 0;      // a whole number, shown by name
}
```

The engine's package has two: `PbrMaterial` (base colour, metallic, roughness, emission: what a glTF describes) and
`BlinnPhongMaterial` (diffuse, specular, shininess: what an OBJ, or an FBX's Phong, describes). A project's are found
with its scripts.

Each kind is also a Slang module of its name, written into the project's `cache/shaders` (or, with no project, the
user's temporary folder) whenever the kind is declared: a struct of it — a picture as its bindless index, true or false
as a `uint`, a field of no GPU form (`Entity`) left out and said — and the attribute a shader takes one of its fields by,
`[Toon("tint")]`. `import koral_materials;` imports every kind there is.

### Shading

What a kind looks like is a Slang module of the same name with `Shading` after it, built on the two interfaces of
`koral_shading`: `IKoralMaterial` says how a kind's fields are read at a point of a surface, and what that gives is an
`IKoralShading`, which holds what was read and answers light (`Reflected`, `Ambient`, `Emitted`, `Opacity`).
`PbrMaterialShading` (GGX) and `BlinnPhongMaterialShading` are the engine's. A shader gets the lit colour with one call:

```slang
import PbrMaterialShading;

const KoralSurface surface = KoralSurfaceAt(world, normal, uv, smooth);     // normals, and how to read a picture
const float3 lit = KoralLit(material, surface, KoralCamera(eye), KoralLightBuffer(lights, count), environment);
```

`environment` is optional: anything `IKoralEnvironment`, such as `KoralCubeEnvironment`. Lights are `KoralLight`s,
as the Lights function lays them out, or any `IKoralLights`. The result is light, not yet tone mapped. Shadows are the
shader's: call `Reflected` itself with each `KoralArrivalAt` scaled by what reaches the point.

A project's kind `Toon` is shaded by `shaders/ToonShading.slang`. It extends the generated struct:

```slang
module ToonShading;
__exported import koral_shading;
__exported import Toon;

public struct ToonLit : IKoralShading { /* its values, and the four answers */ };
public extension Toon : IKoralMaterial {
    public typealias Shading = ToonLit;
    public ToonLit Read(KoralSurface at) { /* the fields, pictures read */ }
};
```

`Shading` can be `KoralPbr` or `KoralBlinnPhong` instead, to read a kind of your own into the engine's function. The
editor finds the module by its name, and draws the kind's small pictures and its material viewer through it; a kind
without one says so in the viewer, whose **Write its shading** starts one.

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
class Spinner : Setup(), PerFrame {
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
class SpinSpeed : Setup() { var degreesPerSecond = 90f }      // a speed each, in the inspector

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

`image` draws a picture among them, on four corners — or, in a `logicSpace`, between two — behind the fills and lines
of its space. It takes an Image, or a 2D ImageView of one to say which level and layer (a view starts with all of them:
`setBaseArrayLayer(1).setArrayLayerCount(1)` is one layer), and a Sampler when linear and clamped is not what is wanted.
The image needs `eSampled` among its usage: a graph's Image says so in its `usage`, for nothing is wired to it from
here. A script's numbers reach it through a Buffer and a CopyBufferToImage, and a buffer is reached by index with
`asArray` (C#: `As<T>()`), which holds nothing open and can be kept for as long as the node's buffer is:

```kotlin
import koral.asArray

data class Texel(val red: Byte, val green: Byte, val blue: Byte, val alpha: Byte)

override fun update(delta: Float) {
    val texels = world.buffer("fluid bytes") ?: return
    texels.asArray<Texel>()[index] = Texel(-1, 0, 0, -1)            // each index a copy of its own: a whole picture is one write
    texels.close()

    val picture = world.image("fluid debug") ?: return
    world.debug.logicSpace(16f, 9f) {
        image(Vec2(1f, 1f), Vec2(15f, 8f), picture, nearest)
        for (p in particles) circle(p.position, 0.05f)
    }
    picture.close()                                                 // the handle: the graph keeps the image
}
```

The pass that draws the shapes is not told of the image, so nothing orders the two: a copy that is scheduled after it
shows a frame later.

A script that is steered reads the window's keys and mouse: `input` in Kotlin — `input.isKeyHeld(Key.eW)`,
`input.isMouseButtonHeld(MouseButton.eRight)`, `input.mousePositionDelta`, `input.mouseScrollDelta` — and `Input`
in C#, `Input.IsKeyHeld(Key.eW)`. `world.viewportSize` (C#: `World.ViewportSize`) is how big, in pixels, the window is
that `input.mousePosition` is from the top-left of: with the camera's matrices, a ray of the cursor into the scene.
In the editor it is the Game panel's: while the pointer is over it, or it was the
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

## Before the frame is drawn

A script that takes `BeforeDraw` makes what the graph's passes take and nothing else makes. Its `Prepare` runs after
every update and before anything is drawn, so what it writes is what this frame's passes read. The usual
case is a buffer node the script fills:

```csharp
public sealed class LightCamera : Setup, BeforeDraw
{
    public float Reach;
    public void Prepare()
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

`koral-editor` and `koral-play` run a project's scripts themselves, inside their own process: open a project, and
its scripts are compiled and run on its scenes. No other program is started, and there is nothing to set up.

A project's scripts are all in one language, chosen when the project is made — **File, New project** asks, C# or
Kotlin — and kept in `project.json` (`"scripts": "csharp"` or `"kotlin"`). **Assets, New script** offers only that
one. A project from before says nothing until its first script is made, which says it; one with scripts already
has the language of the scripts it has. A script in the other language, put in the folder by hand, runs nowhere,
and the Problems panel says so.

What runs them is started the first time a project needs it, and stays until the program ends: .NET for C#, a JVM
for Kotlin (`khost::Scripts`). Another project opened from the File menu has its own scripts run, in the same
program; a program can have both, one project after another. Each calls the engine's C interface — the program's
own copy of the engine, which it exports (`kengine-capi`), not a library loaded beside it.

**C#.** Scripts are the `.cs` files in the project's `scripts` folder, with the engine's package's. They are
compiled in the process by Roslyn when the project opens, and again when one is saved. .NET is started by its
`hostfxr` as `Koral.Engine.Host` — the build puts it, Koral.Engine and Roslyn in `dotnet/` beside the programs —
and `Koral.Engine.Embedded` is what the program calls. It is looked for in `dotnet/` beside the program (a runtime
shipped with it), then `DOTNET_ROOT`, then where .NET is installed: the .NET 10 runtime is all a machine needs.
`System`, `System.Linq`, `System.Collections.Generic`, `Koral` and `Koral.Engine` are already in scope.

**Kotlin.** Scripts are the `.kt` files in the project's `scripts` folder, with the engine's package's. They are
compiled in the process too, by Kotlin's own compiler (`kotlin-compiler-embeddable`), on a thread of its own so that
the editor carries on as they compile: into the project's `.koral/cache/kotlin`, where a build of the sources as
they are is loaded at once the next time the project is opened. The build puts the engine's Kotlin layer and what it
runs on in `kotlin/lib` beside the programs, and the compiler in `kotlin/compiler`; `koral.engine.Embedded` is what
the program calls. The JVM is looked for in `jre/` beside the program (one shipped with it), then
`KORAL_ENGINE_JAVA_HOME`, `JAVA_HOME`, and the JDK the engine was built with: Java 25 or later. There is no Gradle
build: a project is its folder.

When the scripts cannot be run — no .NET, no Java, the engine's layer not built — the project opens all the same,
its scripts' components as data, and the Problems panel says why.

## Editing while it runs

Save a script and it is loaded again without the scene stopping. The data is the engine's, so it stays:

- A field that keeps its **name and type** keeps its value on every entity.
- A new field, or one whose type changed, starts at zero. A removed one is gone.
- A script that had started is not started again.
- A script whose class is gone stops being called. Entities keep its data, for when it is back.
- Scripts that **do not compile change nothing**. The ones running carry on and the errors are logged.

A C# edit made inside functions' bodies only — a method's, an accessor's, an operator's; no field, signature,
constructor or file changed — is **made to the code that runs, in place**, rather than loaded: the objects are the
same, so what they hold privately is kept too, and nothing is registered again (`ScriptHost.Edits` counts them).
The editor and the player let .NET do so (`DOTNET_MODIFIABLE_ASSEMBLIES=debug`, set before it starts); an edit
that would add to the code — a lambda, a query — is loaded as any other.

C# is compiled in the process, into a load context of its own that is unloaded once it is replaced. Kotlin is
compiled in the process as well, each build into a folder of its own, its classes loaded through a class loader of
their own; what was running carries on until the build is done. A `koral.engine.ScriptHost` made by a program of
its own can instead be given `--sources` with a `compile` command to run, or watch a classes folder an IDE writes.

## From C, or another language

`koral_engine.h` is what both layers are written on: entities and components by handle, a node's resource
as a `KoralResource*`, and a script type registered as a size, a list of fields and a struct of function
pointers. `modules/kengine/tests/test_capi.cpp` uses nothing else.

## Not yet

- **Lists and resources as fields.** A script reaches a resource through `World`, by node id. Text is a field (`string`,
  `String`): the engine's own text where the field is, read into the script before each call and written back after.
- **A ResourceScript runs only with its project's scripts**, as every script does: until they are compiled, or
  where they cannot be run, what it makes is made with nothing in it, and its node says so.
- **Scripts run only while the scene plays** in the editor (Play, in the Scene panel): nothing is created, enabled
  or started before then — a `BeforeDraw`'s `Prepare` alone runs as the scene is edited, for what is drawn — and
  what they made is gone on Stop.
- **The inspector follows a script ten times a second**, while the scene plays and the entity is picked: a
  field changed every frame is seen at that rate, not every frame.
- **A Kotlin project's first compile takes a couple of seconds.** Opened by `koral-editor` or `koral-play`, a Kotlin
  project has the JVM started at once and Kotlin's compiler run once on nothing, on its own thread, while the window and
  the scene are made (`khost::Scripts::WarmUp`): FluidSimulation's first compile, from no build, went from 3.5 s to 2.1 s.
  Its scripts run once it is done, and a project opened again with its scripts as they were loads the last build at once.
- **The runtimes are found, unless shipped.** `dotnet/` and `kotlin/` are beside the programs in a build. Configured
  with `-DKORAL_ENGINE_SHIP_RUNTIMES=ON`, the build also puts .NET 10 (its `host/fxr` and `shared/Microsoft.NETCore.App`,
  from where `dotnet` is) in `dotnet/`, and a JRE of its JDK (`jlink`) in `jre/`: about 170 MB, which khost looks in
  first — a machine with neither runs a project's scripts. Without it, they are found where they are installed.
- **Scripts are looked at between frames**: an editor that is not in front draws few, so a script saved from
  another program is compiled a little later than it would be.
- **Only a C# edit to bodies is applied in place**, and only one that adds nothing to the code: a lambda, a query, a
  local function or an `async` one makes it a load like any other. The functions below an edited one keep the lines
  they had, in a stack trace, until the next load. Kotlin's edits are always a load.
- **A script named like an existing plain component** takes that component over — a kind too: a project's own `Light` script is the `Light` from then on, and the package's lights are no longer found by that name. In a host's build, a class named as one of the package's is two of one name, and does not compile.
- **A light's kind starts each of what derives from it the same**: a field starts as its class says, so `PointLight` and `SpotLight` start white, of intensity 1 and range 5, as `Light` and `LocalLight` say. An object (the Point Light's, the Spot Light's) gives its own.
