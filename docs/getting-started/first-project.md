# A first project

From an empty folder to a lit scene with a turning sphere, then a pass of your own that draws into an image the scene
shows. Kotlin here; C# is the same, a choice when the project is made.

## Make the project

Run `koral-editor`. **File, New project** asks for:

- its **name**,
- the **language** of its scripts — C# or Kotlin — which is the project's from then on,
- whether it is a **project** or a **module** (a module is imported by projects: its scripts, shaders and objects become
  theirs),
- the **folder** it is made in, and the **modules** it imports.

The project is a folder:

```
project.json                 its name, its scenes, and the scene a run starts with
scenes/main.scene.json       a scene: its pass graph and its entities
shaders/                     found before any other shaders, by module name
objects/*.json               things to make entities from: what the Create menu lists
```

## A scene to start from

A new project's scene is empty. **File, New scene** replaces it with the standard one: a camera, a sun, a sphere and a
cylinder, each mesh drawn in a colour of its own and lit by the sun. Its graph is ordinary nodes, written into the
project to be changed like any other: a page `Draw` that draws every entity with a Mesh into a colour image, and a page
`Blit` that copies that image to the Screen. (**File, New lit scene** is the larger one: every light, and a sky.)

Look round the **Scene** panel: right-drag turns the view, and an entity clicked is picked — its components in the
**Inspector**, its node in the **Scene tree**. The **Graph** panel shows the frame's pages as cards; double-click
one to go into it.

## A script

**Assets, New script**, named `Spin`, of the kind **Kotlin**, writes `scripts/Spin.kt`:

```kotlin
import koral.Vec3
import koral.engine.PerFrame
import koral.engine.Setup

class Spin : Setup(), PerFrame {
    var speed = 0f              // a var starts at zero: set it in the inspector, or in initialize

    override fun initialize() {
        if (speed == 0f) speed = 45f
    }

    override fun update(delta: Float) {
        // Turns its entity about the up axis, so many degrees a second.
        world.setRotation(entity, world.rotationOf(entity) + Vec3(0f, speed * delta, 0f))
    }
}
```

Its public `var`s are a component: shown in the inspector, saved with the scene. The editor compiles it at once, and
again whenever it is saved. Pick the sphere, press **Add** in the Inspector, and choose `Spin`.

Press **Play** (in the middle of the title bar, or Control+P): the scripts run, and the **Game** panel shows the scene
through its camera, the sphere turning. **Stop** puts every entity back as it was. Change `speed` while it plays, or
the code — saved, it is compiled and loaded again, keeping each entity's values.

See [Scripts](../scripts.md) for the hooks (`PerFrame`, `FixedStep`, `BeforeDraw`), reading and writing other
entities, and materials.

## A pass of your own

The graph can be changed node by node — right-click a page, or Control+Space, to add one — but a pass can also be a
script, written against Koral's own interface. Put this shader in `shaders/triangle.slang`:

```slang
struct VertexOut {
    float4 position : SV_Position;
    float3 color;
};

[shader("vertex")]
VertexOut vertexMain(uint id : SV_VulkanVertexID)
{
    const float2 corners[3] = { float2(0.0, -0.6), float2(0.6, 0.6), float2(-0.6, 0.6) };
    const float3 colors[3] = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1) };
    VertexOut output;
    output.position = float4(corners[id], 0.0, 1.0);
    output.color = colors[id];
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target
{
    return float4(input.color, 1.0);
}
```

and **Assets, New script** of the kind **Kotlin PassScript**, named `Painter`, with this in it:

```kotlin
import koral.*
import koral.engine.NodeOutput
import koral.engine.PassScript

class Painter : PassScript() {
    @NodeOutput lateinit var canvas: Image          // an output of its node: what it draws into

    private lateinit var target: Framebuffer
    private lateinit var triangles: GraphicsPipeline

    override fun initialize() {
        canvas = Image.Builder().setFormat(ImageFormat.eRGBA8_UNORM).setExtent(UVec2(512u, 512u))
            .setUsage(ImageUsage.eColorAttachment, ImageUsage.eSampled, ImageUsage.eTransferSrc).build()
        target = Framebuffer.Builder()
            .addColor(Framebuffer.ColorAttachment(view = canvas.view(), clear = ClearColor(0f, 0f, 0.2f, 1f))).build()
        triangles = GraphicsPipeline.Builder()
            .setVertexShader(Shader.Builder().setEntryPoint("triangle", "vertexMain").getOrBuild())
            .setFragmentShader(Shader.Builder().setEntryPoint("triangle", "fragmentMain").getOrBuild())
            .setFramebuffer(target).build()
    }

    override fun record(commands: CommandBuffer) {
        commands.beginRendering(target).bindGraphicsPipeline(triangles).draw(3).endRendering()
    }
}
```

It is written as anything is in [Koral](https://radueduard.github.io/Koral/): the image, the framebuffer and the
pipeline made with builders, and the frame's commands recorded into a command buffer. A **ScriptPass** node of it —
`Painter`, among the nodes to add — makes them as it is built, and each frame draws a triangle into the image. What is
marked `@NodeOutput` is an output of the node, by its name. On the `Blit` page, wire the node's `canvas` output to the
Blit's `source` in place of `color`: the scene now shows the triangle.

`@NodeInput` marks what the node is given — a resource wired to it, or a value set on it. A PassScript can record
anything Koral's command buffer can; see [Scripts that are passes](../scripts.md#scripts-that-are-passes).

## Running it

**Project, Run** (Control+R) saves the project and runs it with `koral-play`, as a player would see it: no panels, the
scripts running. From a terminal:

```sh
koral-play <project folder>
```

Next: [Concepts](concepts.md).
