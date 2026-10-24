# The editor

`koral-editor` is a window of panels over one scene. `koral-play` runs a project by itself. Both are built
when the Koral SDK is found, with `modules/keditor` under them.

```
koral-editor [project folder]
koral-play <project folder | scene file>
```

## A project

A project is a folder:

```
project.json                 its name, its scenes, and the scene a run starts with
scenes/main.scene.json       a scene: its pass graph and its entities
shaders/                     found before any other shaders, by module name
objects/*.json               things to make entities from: what the Create menu lists
prefabs/*.prefab             entities that instances are made of, and follow
```

A scene file holds the graph as [kgraph](kgraph.md) saves it and the world as [kecs](kecs.md) saves it.
A resource an entity holds is saved as the id of the node that makes it.

`koral-editor` and `koral-play` take `--no-vsync`, to draw frames as fast as they are made rather than at
the display's rate, and `--frames N` to stop after so many.

## The panels

| Panel | |
|---|---|
| **Graph** | The scene's pass graph, a page a pass: the one picked in **Passes**. Right-click where nothing is, or press Control+Space, to add a node. Drag from a port to wire it, drag a node to move it, Delete removes what is picked. A node that failed is outlined in red and says why. |
| **Passes** | The graph's passes, under the Scene tree: pressed, a pass's page is shown in the Graph panel. See [Passes](#passes). |
| **Here to use** | Beside the Assets panel: what the page shown can use of the passes before it, a picture each, to drag onto the page. See [Passes](#passes). |
| **Scene** | The scene as it runs, sized to the panel. |
| **Scene tree** | The entities, as a tree, and a button that makes one. Each is shown by what it is — a mesh, a light, a camera, a script's — and one with nothing of its own but what is under it by a group's mark. Right-clicked, an entity has a short menu: Rename, Duplicate, Delete; **Create under it ▸**, the objects there are, a submenu each for meshes, lights and cameras; how it is seen (framed, shown or hidden, moved to the top); and **Prefab ▸**, made one, or of an instance its prefab edited, applied, reverted or let go. Right-clicked where no entity is, it offers what can be made at the top. A submenu opens beside its menu as the pointer comes onto its line. |
| **Inspector** | Whatever was picked last: a node's inputs, or an entity's components. |
| **Problems** | Every node that failed and every pass that passed an entity by, with the reason. Click one to pick its node. |
| **Frame** | The frame as it runs: the graph's passes in the order the frame graph schedules them — each with its dependency level, `async` when it is on the async compute queue, its GPU time as a bar against the longest's, and what it reads and writes, by the node that made each — then the passes left out, and why (`Runtime::Frame`). Read only: press one to go to its node. It is not a timeline of when each pass ran: the framework does not yet record when a pass starts. |

The menus:

- **File** makes a project, opens one, starts the standard scene over (**New scene**, or **New lit scene** for
  the larger lit one), reads the scene again from its file (**Revert to saved**) and saves it (Control+S).
- **Edit** duplicates, moves to the top, saves as an object, or deletes what is picked.
- **Create** makes an entity from one of the project's objects, at the top or under what is picked.
- **View** frames what is picked, looks through the scene's camera, and chooses the gizmo.
- **Assets** makes a new shader or a new script: a name, a kind, and a file in the project to start from.
- **Scene** plays, pauses and stops.
- **Project, Run** saves and runs the project (Control+R): with its scripts' own runner when it has scripts,
  with `koral-play` when it has none. **Open at startup** makes the project the one the editor opens when it is
  given none; ticked, it puts the engine's own back.
- A project with scripts is handed over as it is opened: `koral-editor` runs no Kotlin and no C#, so it starts
  the editor again through the scripts' runner (`./gradlew edit`, `koral-engine-dotnet --edit`), which runs them,
  and closes. In `koral-editor` the runner is started once the window has closed, in its terminal, as when it starts
  on such a project. A runner that stops with an error before it gets going (no JDK, no runner built) is said so
  there, and the project opens without its scripts. **Project, Open with its scripts** does the same by hand.
- **Graph** and **Entity** add a node and an entity.
- **Settings, Theme** draws the editor in Koral's design, Material's, Cupertino's or Windows', dark or light
  (`kui::Theme::Of`). The choice is this user's, kept in `~/.config/koral-engine/theme` for the next run.

### Passes

The graph is split into passes, listed in the **Passes** panel in the order they are drawn, each
with how many nodes it has (and how many are wrong). Its pills are all one grey: a pass that takes more of the frame says its time brighter, never in a colour of its own. Press one to see its page: the nodes it makes
and runs, and nothing of any other pass. **Find a node**, at the top, goes to the first node whose name or
kind has what is typed; Enter again goes on to the next.

The passes are under a tab for each rate they run at (see [kgraph: How often a pass runs](kgraph.md#how-often-a-pass-runs)):
**Every frame**; **Fixed step**, the passes run on the fixed step while the scene plays; and **Setup**, the passes made
once. Setup's run first, then the fixed step's, then every frame's, each in their own order. The tab follows the page
shown: going to a node of a setup page turns to Setup.

Only the pages of what the graph's choices choose are listed (`Session::PagesNotChosen`): a page each of whose passes
idles — or is switched off — by a choice's value, a Switch's driver compared with an option (`NotEqual`, `Equal`,
through `Max` or `Or`), is left out, and so is a page of no passes that only such pages use. With Forward+ drawing
vertices, the meshlets' and the rays' pages are not listed; tracing rays, the vertices' and the meshlets' are not, nor
the shadow maps rays need not. A pass idle for a reason of its own (a shadow drawn only when it changed) keeps its page.
Each page keeps its number among them all; the line at the end says how many are left out, and **Show them** lists
them too, dimmed. The page shown stays listed, dimmed, when another choice leaves it out.

A pass node's inspector shows what it **takes**, followed as it runs: its GPU time and the CPU's recording it.

**Every node at once** shows the whole graph, laid out by itself: at first each pass's page beside the one
before, in the order they are drawn, so that none is over another. Moving or arranging nodes there does not
move them on their pages, and moving them on a page does not move them there. A node made there is where it
was put. The layout is saved with the graph (`overview`).

- **+** beside the tab's heading makes a new pass of that tab: type its name, then Enter (Escape calls it off). Drag
  a pass up or down to make it earlier or later among its tab's. Right-click it to move it, to make it run every frame, on
  the fixed step or once as setup, or to delete it (with its nodes, or keeping them in no pass).
- Under THIS PASS: the pass's name, to type over; **Create a node** makes a node in it (an image, a
  framebuffer, a pipeline, a pass node), as right-clicking its page or Control+Space does for every kind.
- The **Here to use** panel, beside Assets, is what the page shown can use: what the passes before it made, setup's
  first, and what is in no pass, under the page each is on — a picture each, as the Assets panel shows files: an image
  as it was last drawn, a mesh by itself, and what kind of thing anything else is. Its heading's button shows them as
  a list instead. The pointer resting on one says what it holds (an image's size and format, a buffer's size) and what
  wrote it before this page. Dragged onto the page, or pressed, it is brought in: a node on this page that stands for
  it, a **Reference**, to wire from as if it were the thing itself. One brought in already is lit, and pressed is
  picked. Values are under **VALUES**, opened by the arrow beside it. Its pictures are made while the panel is in
  sight.
- A node's inspector has a **Pass** line to move it to another page. Clicking a problem, or finding a node
  by name, turns to the page it is on.

A Reference says, on its node, what it stands for and where that was made, what it holds, whether this page reads it,
writes it or both, and what wrote it before this page. Its picture is of the image as this page begins. One of what setup
made is marked in setup's colour. Picked, its inspector is of the thing itself: what is set there is set where it is
made, and for every page that brings it in. **Go to where it is made** turns to that page. A page brings a thing in
once, and a Reference that loses the last wire from it goes, since the drawer still has what it stood for.

A node wired straight to one on another page, with nothing imported, shows that node greyed beside it.

### New shaders and scripts

**Assets, New shader** asks for a name, a kind and what it has, and writes `shaders/<name>.slang`. Each entry point is
called as a pipeline looks for it when none is named — `vertexMain`, `fragmentMain`, `meshMain`, `closestHitMain` — so a
pipeline node only has to name the module:

| Kind | What it is | What it can have |
|---|---|---|
| Vertex + fragment | Draws each entity's mesh, with the frame's camera, its `model` matrix and one value of its own, `color`: push constants a Draw is wired from a ForEach. For a GraphicsPipeline and a Draw. It imports `engineCommon`, which is written beside it when not there. | **Over the whole picture**: one triangle and no mesh, reading another picture at `param.source` — no vertex layout, no mesh. **Tessellation**: control and evaluation shaders, each triangle cut as fine as `constant.level`. **Geometry shader**: each triangle passed on. No **fragment shader**: only depth is drawn, for a shadow map or a depth prepass. |
| Mesh + fragment | Mesh shaders, a triangle a group, and no vertex buffer. For a GraphicsPipeline drawn with eMesh. | **Task shader**: how many mesh shader groups there are, and what each is handed. No **fragment shader**: only depth. |
| Compute | Fills an image wired to `param.target`, with a constant the node sets. For a ComputePipeline and a Dispatch. | |
| Ray tracing | A ray generation shader that traces a ray for each pixel of `param.target` into the AccelerationStructure at `param.scene`, a miss shader, and a hit group of triangles. For a RayTracingPipeline (`raygen`, `miss[0]`, `hitGroup[0]`) and a TraceRays. | **Any hit shader**: every hit, and a ray let pass. **Intersection shader**: what is no triangles, a sphere in each box. |

What this device cannot run is not offered: mesh shading and ray tracing where the GPU has neither, and tessellation and
geometry shaders where it has not them (the graph asks for both, so a GPU that has them has them enabled). The dialog
says why, in place of the choice.

The name is the module a pipeline node then picks. **Assets, New script** writes `scripts/<Name>.cs` (or
`.kt`): a class of that name with one field and an `Update`, which is a component of that name once the
project runs with its scripts (see [scripts](scripts.md)). Its kind can also be a **ResourceScript** of the project's
language: a class that makes an image and a sampler, and fills the image's texels as it is made, run by a Script node
(see [scripts: Scripts that make resources](scripts.md#scripts-that-make-resources)). A name has to be one code can
use, and not one a shader, a script or a component already has.

Each ResourceScript is among the nodes to add, by its name: a Script node of it, whose outputs are what it makes. Its
inspector says what that is, picks another script, and has the script's fields and what each resource is made with as
its inputs. Until something runs the script, what it makes is made with nothing in it, and its node says so.

### The standard scene

**File, New scene** replaces the scene with one that draws each mesh in a colour of its own, lit by the sun. It
writes one shader into the project's `shaders` (`default`; one already there is left alone) and builds the graph
that uses it out of ordinary nodes, to be changed like any other:

| Pass | What is on its page |
|---|---|
| `Draw` | The Screen; `color` and `depth` images the size of it, and `target`, their Framebuffer; the first Camera's main View and main Perspective, with its place, as `camera data` (the shader's `camera`); a ForEach of the DirectionalLight as `light data` (its `light`); and `default`, the pipeline that draws meshes. `drawn`, a ForEach of every entity with a Mesh and the pipeline's component, is wired to the Draw: each entity's mesh, its matrix, and the component's `color`. |
| `Blit` | The Blit of `color` to the Screen. |

**File, New lit scene** is the larger one to start from instead: meshes lit by every light, under a sky. It writes
three shaders (`engineCommon`, `lit`, `sky`) and builds:

| Pass | What is on its page |
|---|---|
| `frame` | The Screen; a ForEach of the first Camera, Sky and DirectionalLight, the main View of the camera, the main Perspective of its lens (its aspect the Screen's) and `seen`, the two multiplied, and what is worked out of them (colours in linear terms, the sun disc's cosine); `frame data`, packed in the order `engineCommon.slang` declares it; and the lights — a ForEach of each kind into a ShaderBuffer of the shader's `Light`, each after the last, `light data` the last of them. |
| `scene` | `color` and `depth` images the size of the window, and `lit`, the pipeline that draws meshes. `all`, every entity, and `drawn`, a Filter of those with a Mesh and the pipeline's component, are wired to the Draw: each entity's mesh, its matrix, and the component's `albedo`, `metallic`, `roughness` and `emission`. An entity with both is drawn. |
| `sky` | The sky, drawn where nothing nearer was. |
| `show` | The Blit of `color` to the Screen. |

The project's objects name the standard graph's `default` component: in a lit scene, a mesh made from one is
given `lit` by hand (or a material dropped on it).

Nothing of it is the editor's. Which entity is under the pointer, the line round the one that is picked, and the
gizmo and the light and camera shapes are made by passes of the editor's own (`keditor::Overlay`), run beside the
scene's whatever its graph is: from every entity with a Mesh, as the editor's view sees it, into a picture laid
over the Scene panel. So any scene is picked from and outlined, and a project that is run has none of it.

Each pass imports what it uses of the ones before it. To send something else to the shaders — a script's
value, another entity's place — declare it in the shader's block and wire it into its buffer (`camera data`,
`light data`; the lit graph's `frame data`) as another field.

The standard scene also makes a camera, a sun, a sphere and a cylinder under it; the lit one a sky, a camera, a
sun and a sphere. `keditor::AddStandardGraph` and `AddStandardScene`, and `AddLitGraph` and `AddLitScene`
(`<keditor/starter.h>`), do the same from code.

### The scene view

The Scene panel shows the scene from the editor's own view: right-drag turns it about what it looks at,
middle-drag moves it, the wheel zooms, and **Frame** points it at what is picked. **Camera** looks through
the scene's camera instead. Each entity with a field of view (a Camera) is drawn as the pyramid it sees through,
and a picked light as its reach — a spot by its range and cone, a point light by its range, a light from far away
by the way it shines: by what its component holds, as a View node goes, not by its name.

**How the scene is shown** is the toolbar's list (the layers button, named for what it shows), the Scene panel's own —
the Game panel, and the project when it is run, show the scene lit:

- **Lit**: as its graph draws it.
- **Material** — Unlit (albedo), Normals, Roughness, Metallic, Emissive, Occlusion: what each shader that imports
  `koral_debug` shows of its material in place of its lit colour. The scene's runtime builds those pipelines with the
  specialization constant `koralDebugView` set to the channel; a pass after them (a tone mapping, a bloom) that
  imports it too leaves the picture as it is. A pipeline whose shaders do not import it is drawn lit, and the list
  says which. The standard and lit graphs' shaders, the engine's Tonemap and Bloom, and Forward+'s import it.
- **Editor** — Wireframe, Clay, Normals (geometry), UV checker, Overdraw, Entities: every entity with a Mesh, drawn by
  the editor's own graph (`packages/koral/editor/scene-view.kgraph`, its shaders `koral_editor_views` and
  `koral_editor_grid`) whatever the scene's graph and materials are. Each is a DebugView of that graph; a project's
  `editor/*.kgraph` are run beside it, and their DebugViews listed after its own. A file written while the editor
  is open is read again.
- **Scene graph**: each DebugView node of the scene's graph — the Forward+ clusters' heat — its picture shown in
  the scene's place. Its passes, wired from its `shown`, run only then, and never in the game.

**The grid** (the grid button; its settings beside it) is a plane through the origin — the floor, XY or YZ — its
lines a cell apart and stronger every so many, its axes in their colours, fading out so far off and hidden behind
what is in front of it. It is drawn by the editor's graph over whatever is shown, and kept for this user in
`~/.config/koral-engine/scene-grid`.

A click on the picture picks the entity drawn there, through the graph's Pick node; what is picked is
outlined and gets a gizmo (**Move**, **Rotate**, **Scale**, or **Select** for none) that changes its
transform. All of this needs the graph to have the nodes for it, as the standard one does: a scene whose
graph has no main View is drawn however its own nodes say, and the view and gizmo do nothing to it.

### Playing

A scene in the editor stands still until **Play**: then its scripts run, and the **Game** panel comes
forward to show it through the scene's camera, while the Scene panel stays the editor's own view. The Game
panel is another view of the same graph and the same world, drawn a second time from the scene's camera —
with nothing picked, no outline and no gizmo, and a click in it picks nothing. A script that is steered (a
FlyCamera, `input` in a Kotlin script) reads the Game panel's keys and mouse: while the pointer is over it, or it
was the last thing clicked. It is a tab beside the Scene panel to begin with; dragged to a part of the middle of
its own, the two are seen side by side. While it is not seen, nothing of it is drawn. A buffer a graphics script
writes into is the same buffer in both views: what it writes is seen in each. Play, Pause, Step and Stop are in the
middle of the window's title bar, on the theme's colour while it plays and a dimmer one while it is paused. **Pause** holds it. **Step** (beside Pause) takes one fixed step and pauses: the scripts' `fixedUpdate` and the graph's fixed
pages, once — starting play first when it was not playing. **Stop** puts every entity back as it was when Play was
pressed, with what was picked still picked. A scene can't be saved while it plays. `koral-play` always
plays.

Scripts are run by a host the editor is started through (`./gradlew edit` for Kotlin, `koral-engine-dotnet
--edit` for C#), not by the editor itself. An editor started any other way has the scripts' components as
data and runs none of them: the Problems panel says so, Play says so, and **Project, Open with its
scripts** saves and reopens the editor through the host.

### Objects

An object is an entity as a file: `{ "template": "Lamp", "name": "Lamp", "position": [0, 2, 0],
"components": { "PointLight": { "range": 5 } }, "children": [...] }`. **Edit, Save as object** writes what
is picked as one, and a new project starts with a cube, a sphere, the other default meshes, a planet with a
moon, a camera, a fly camera (a Camera with the engine's FlyCamera: right button, the mouse and W A S D), the
three lights and a sky. The mesh objects name the standard graph's `default` component. The engine's own objects are
offered in any project, one made before an object was among them included; a file of the project's of the same
name is what it says instead.

### Default meshes and materials

Every project's Assets panel has the editor's own, of no file: **Meshes/Primitives** holds the meshes there are
without making any (`$cube`, `$sphere`, `$roundedBox`, `$wedge`, `$stairs`, `$teapot`... every one of
[the default meshes](kgraph.md#default-meshes)), each pictured, dragged
onto an entity's Mesh as any mesh is. Their inspector chooses their **tangents**, every default mesh alike: a `TANGENT` of
four numbers (w the bitangent's side), a `TANGENT` and a `BITANGENT` of three, as a model's meshes have them, or
neither — saved with the scene's graph, and undone as any change (see [Default meshes](kgraph.md#default-meshes)). **Materials/Defaults** holds materials (`material:Gold`, `material:Red plastic`,
plastics, metals, wood, rubber, two that glow): dropped on an entity in the scene tree or the inspector, what draws it
(a component with an `albedo`, or a pipeline's with a `color`, as the standard graph's `default` has) is given its
albedo, metallic, roughness and emission — as many of them as it has — and its pictures are set white; an entity
drawn by nothing is given one first. The table is `Session::DefaultMaterials()`.

### Prefabs

A prefab is entities that instances are made of — and that the instances go on following. An entity **dragged from
the scene tree into the Assets panel** is written, with what is under it, to `prefabs/<name>.prefab`, and is the prefab's
first instance; so is one picked, with the inspector's **Save as prefab** (offered while it is part of no prefab), the
Assets panel's **Create, Prefab of the picked entity**, **Edit, Save as prefab** or the scene tree's right button. The
Assets panel then shows the prefab, picked.

**A prefab is edited by itself**, as Unity's prefab mode does. The Assets panel's right button (or **+ Create**) has
**Create prefab...**: named, a prefab of one empty entity is written to `prefabs/` and opened. A prefab pressed twice in
the Assets panel, or **Edit prefab** on its menu or in its inspector, opens it. Open, the scene's entities are put aside
— with what can be taken back of them — and the scene tree and the scene view have the prefab's alone, as entities like
any; a bar over the scene tree names it, with **Save** (and Ctrl+S, while it is open) and **Back to the scene**. What is
taken back there is the prefab's. Back to the scene, it is as it was — saved first when the prefab changed — and every
instance of the prefab in it follows the prefab. A prefab is not played; opening another project or scene saves the
prefab and goes back first. Its first entity at the top is the prefab: another beside it is no part of it. A prefab in the Assets panel is placed by dragging it into the scene
panel or the scene tree, at the top, or onto an entity, under it; pressed twice, it is placed at the top.

An instance is made again from its prefab whenever its scene is opened and whenever the prefab is applied, in all
but what the instance made its own: a field set on it, its entities moved, turned or renamed, a component given it
or taken off, one of its entities taken away. Those are kept with the instance; the inspector says how many, and
the scene tree marks an instance. Where the instance itself is, what it is called and what it is under are always its
own, and so is what is put under its entities. In the inspector of an instance, or of any entity of one:

- **Apply to the prefab** writes the instance as it is now to its prefab. Every instance of it in the scene follows
  at once; those in other scenes do when those are opened.
- **Revert** drops what the instance made its own, and what was put under it: it is as its prefab is.
- **Unlink** makes it entities like any other.

A copy of an instance (Duplicate) is another instance, with the same overrides. A prefab's instance placed under
another's is part of the outer prefab once that is applied: the outer prefab names the inner one and keeps the
values of what the inner instance made its own, and each outer instance's inner one follows both prefabs — unless
something of it was changed in that outer instance, which keeps it. A prefab with an instance of itself in it is
made eight deep and no more. What one of a prefab's entities holds of another (a script's `Entity`
field) is that entity in every instance; what it holds of an entity outside the prefab is left out.

**+** over the scene tree, and **Create** in the menu bar, offer them grouped by what each holds: an empty entity,
then **Mesh**, **Light** and **Camera**, each a submenu that opens in place when pressed, then the rest (the sky,
the project's own).

A component is never described by a file. One comes from a pipeline's shader, from a script, or is one of
the scene's own (a camera, a light, the sky).

### Entities

The scene tree is the hierarchy. An arrow folds what is under an entity. Drag an entity onto another to put it
under it, or onto the empty part of the panel to bring it to the top; it stays where it is in the world.
Right-click one for a new entity under it, to move it to the top, or to delete it with everything under it.

An entity's inspector starts with its name and its transform: position, rotation (degrees about X, Y and
Z) and scale, relative to what it is under. These are not components and can't be removed. A pipeline whose
shader has a `float4x4 model` push constant draws the entity where its transform puts it.

A `Mesh` component's field lists the graph's mesh nodes and the default meshes (`$cube`, `$sphere`,
`$plane`, `$cylinder`, `$cone`, `$capsule`, `$torus`), which need no node.

### Drawing

A new Draw needs only its pipeline. With a vertex shader that makes its own vertices (`SV_VertexID`), that
is a whole picture: Image → ImageView → Framebuffer → GraphicsPipeline → Draw, and a Blit of the image to a
Screen. No mesh, no vertex layout, no entity. Everything else a draw can say is in its inspector, as it starts: a mesh
to draw instead of the entities', how many vertices and instances, an indirect buffer, the viewport and
scissor, what the framebuffer is cleared to, and the state the pipeline leaves open (culling, depth,
stencil, line width).

### A node in the inspector

The inspector shows every input a node has, in the order its type lists them: the ones it has been given with their
names lit, the rest dimmed and showing what they start as — set one, and it is given. An input given in parts
(`extent`, `groups`) is one line of its parts, and its `o` wires it whole instead. `x` takes an input back to nothing
given. Under **What it gives** is every output it works out besides its inputs — an Image's `width`, `size` — each
with what it is now and what it is wired to. On the node there are only the ports it needs and those asked for there:
the pin beside an input moves it to the node or back, the port icon beside one that takes a resource puts a port for it
on the node, and the output icon beside an input — or beside an output under What it gives — gives it as an output
on the node, to wire what it is set to elsewhere. Taken off, an output's wires come off with it.

What a node has many of is in parts (`Port::group`): those it cannot do without and a few more at the top, and the
rest each under a heading that opens and closes — a Draw's *What is drawn*, *Indirect*, *Attachments*, *Viewport*,
*Rasterization*, *Depth*, *Stencil*, *Blending*, *Culling*; a Sampler's *Filtering*, *Addressing*, *Level of detail*,
*Comparison*; a pipeline's states, each of its own; a Script node's resources, each of its own. A part with something
given in it, or something the node cannot do without, is open; the rest are closed, saying how many there are
(*6, as they start*, *2 of 6 given*), until opened. What does nothing as the node is set is not there until it does
(`Port::when`, `kgraph::Applies`): a draw of vertices has no indirect buffer's inputs, a draw with its stencil test off no
stencil reference, a sampler no anisotropy while it is off. Its part says what it leaves out, and why — *4 more for
indirect draws* — and an input given before stays, to be taken back.

Each input has the control for what it is:

| Input | Control |
|---|---|
| A number, or the parts of one thing (`extent`, `groups`) | A number to drag, or one line of them under the thing's name, kept within what the input takes (`Port::min`, `Port::max`): dragged or typed, an extent is never 0, a field of view never past 179. A view's first level and layer, and how many of each, are within what the image it is of has — a Script node's views' as well. A whole number is dragged a unit at a time; a size, a distance or a count that may be thousands, by so much of what it is (`Port::logarithmic`). A field of a struct — a stencil's mask, a depth range — is kept as `kgraph::LimitsOf` says. |
| True or false | A box to tick. |
| An enumeration, or one of a few names (`stages`, `command`, `shape`) | A list of the names. |
| A view's swizzle (an ImageView's `componentMapping`) | Under its name: a list for each of red, green, blue and alpha — Identity, Zero, One, or a channel of the image. |
| Flags | A tick for each. |
| A colour (a Clear's `color`, an attachment's `clear`) | A swatch that opens a colour picker. |
| A shader module | The project's `.slang` files, by name. |
| An entry point | The functions the module beside it declares with `[shader(...)]`. |
| Components (a Filter's or a ForEach's `with`, `without`) | A tick for each component and tag the scene has. |
| An image parameter (`cover`) | The image parameters of the pipeline the pass runs. |
| A node (`Reference`) | The nodes that make a resource. |
| A vertex layout's attributes | A picture of its buffers, then each buffer's attributes under it: what each is, its type, how many, which buffer, earlier and later; whether the buffer goes per instance. |
| A struct | Its fields, each with its own control. |

A pipeline has no shader nodes. It names its shaders: `compute.module` and `compute.entry`, or, for a
graphics pipeline, a module and an entry point for each stage. **`stages`** chooses how a graphics pipeline
is drawn: `eVertex` for a vertex shader with optional tessellation and geometry shaders, or `eMesh` for a
mesh shader with an optional task shader. The node shows the inputs of the one chosen. What the shaders
declare appears as `param.<name>` and `constant.<name>`.

An input is set in one of three places, as its node type says (see [kgraph](kgraph.md#pipelines)):

- **On the node.** A control in its row — a number to drag, a box to tick, a colour swatch, a list, a few numbers
  side by side — and a port to wire all the same; wired, the control gives way to the wire. What Math, Vectors,
  Logic and Values nodes work on is set here, so a sum shows what it adds. A pass's `enabled` is the inspector's, with the pin to put it on the node.
- **In the inspector,** as above: what takes room to set.
- **By a wire only:** what only a node makes. The inspector says to wire one in, and — for a pipeline's vertex
  layout, a Mesh's — **Make a VertexLayout** makes the node that sets it up beside this one, wired in.

The pin beside an input in the inspector moves it between the node and the inspector, for this node; it is saved
with the graph. An input that takes a resource and that the node cannot do without is a port on the node, and so is
one put there with its port icon: wire it on the graph, or choose what to wire from in the inspector. A value set only in the inspector (a number, a vector, a colour) can be wired too: the small `o`
beside it makes it a port.

A **VertexLayout** node draws what it makes: a strip for each buffer the vertices are in, its attributes along it
in order, each as wide as its bytes and coloured by what it is (POSITION, NORMAL, UV...), with how many bytes a
vertex — or, for a buffer that goes per instance, an instance — takes of it.

**Add a node** (Control+Space, right-click the graph, or a wire let go over nothing) is in three columns: the kinds of
node, each with its colour and how many it has, in three sections — **Functions** first, each under what it is for
(Physics, Sort and scan, Culling; on a page of the fixed step, Physics first), then **Scene and values** (Components,
Scene, Values, Math, Vectors, Cameras, Logic, Flow), then **Vulkan**: Resources, Pipelines and Passes, what is made on
the device and what runs; the nodes of the kind picked, each with what it does in a line, or, as something is typed, what
it finds among every kind, best first, and the graph's own nodes it names; and the node pointed at, drawn as it will
be on the graph — its controls as they start — with what it does, takes and gives. Enter adds the first found; a
click adds the one clicked.

When you add a node, the dialog also takes a name. A pipeline's name is what its entities' component is
called, so give it one.

### An entity in the inspector

Its name, then each component with its fields. A field that holds a resource is a list of the nodes whose
output fits. A `vec3` or `vec4` whose name says it is a colour (`color`, `tint`, `albedo`) gets a colour
picker. A number is kept within what its field says (a script's `[Range]`, `[Min]`; an unsigned one at 0 or more),
dragged or typed, and dragged by so much of what it is where its script says `[Logarithmic]`: a light's intensity and
range, a camera's near and far. The transform's rotation goes round from −180 to 180 degrees; its scale is dragged by
so much of its size. The tick beside a component's name switches it off without removing it.

A pipeline's component is a **material**. What two or more of an entity's materials have alike — a field of the same
name and type, the forward pass's `albedo` and the ray tracer's — is a card of its own, **Shared material**, framed
apart from the components: the pipelines it is of, each a chip to go to it (● a pass running now uses it, ○ none
does), then each field once. Read from a material in use, it is set in every one of them whichever renderer is
chosen; one set apart since — by a script, an undo — gets a **differs** pill at once, and pressed, every one is as the
one shown. A texture given to the entity (dropped on it from the Assets panel) goes into each material's field of
that name too. Under the card, each material has what is its own: *pipeline's own · 1 field, the rest shared above*.
A material no pass the choices leave running reads (`Session::MaterialInUse`: none runs its pipeline, nor reads its
Draw's buffer of instances — the meshlets read the forward Draw's) is another renderer's: dimmed, shut unless
opened, and what is its own is not set while so. **Add** lists the
components the entity doesn't have yet: each pipeline's (what its draws take from each entity), `Mesh`, and the
scene's tags. **New tag** makes a tag and gives it to the entity.

## Editing is live

Every change goes through `keditor::Document`, which applies it to the running scene at once, so the Scene
panel shows it on the next frame. A half-made node fails quietly: the reason is on the node and under
Problems, and nothing is written to the log. A shader saved in another program is picked up the same way.
If the edit took a parameter away, the input wired to it is removed and the Problems panel says so.

## From code

The editor's panels only call `Document`, so a tool or a test can do everything the editor does:

```cpp
keditor::Document& doc = play.Doc();              // keditor::PlayScene runs one
doc.AddNode("ComputePipeline", 200, 120, "blur");
doc.Connect("blur shader", "out", "blur", "computeShader");
doc.SetInput("run", "groups.x", 8);

const kecs::Entity entity = doc.CreateEntity("Lamp");
doc.AddComponent(entity, "blur");
doc.SetField(entity, "blur", "target", "lamp image");     // a resource: the node that makes it
doc.SetField(entity, "blur", "radius", 4.0);
doc.Save(project.ScenePath("main"), error);
```

`keditor::PlayScene` is the scene a project's scene runs in. The editor opens it offscreen and shows it in
the Scene panel, and `koral-play` opens it in a window, so both run the same file with the same code.

## Not yet

- **The Hub** doesn't know engine projects. They open and run from the editor and from `koral-play`.
- **A port or an output put on a node and not wired is forgotten** when the editor closes: only inputs that
  were given, and wires, are in the file.
- **A node can't be renamed** after it is made: its name is what wires, files and components know it by. What it
  is called on the graph — its title — is set in the inspector.
- **One scene a project** in the interface. `project.json` can list more, and `koral-play` takes a scene
  file, but the editor opens the starting one only.
- **An entity's own image or buffer** has to come from a node. Nothing in the editor makes a resource per
  entity.
- **Entities aren't reordered among their siblings** by dragging: a drop puts one last under what it is
  dropped on. An entity is renamed in the inspector, not in the scene tree.
- **A component the shader makes starts at zero**: a new `default` is black until its colour is set. The objects set them. Objects have no icons.
- **The scene view's toolbar is words**, and a menu line that is on is marked with a `*`.
- `Project, Run` finds `koral-play` beside the editor on Linux only. Elsewhere it has to be on the path.

## Looking at what a node holds

**An image node shows its picture**, always: each image on the page shown is copied out once in thirty frames, each
on frames of its own (`Runtime::WatchEvery`), so that a page of them costs about a copy a frame, and its picture
on its node is a second or two old at most. An image of a format that does not read back, or that nothing draws
into, says so in its place.

A node that makes an image or a buffer has **Look at what it holds** at the foot of its inspector. While it is
looked at, the runtime copies it out (`Runtime::Watch`) and it is shown a few times a second — an image on the
node itself, in the graph, and the rest in the inspector:

- **An image, as a picture**, whatever its format. *As suits it* shows a colour as it is and stretches a depth, a
  count or anything of one channel to its range; the other ways are *As it is*, *Stretched to its range*, and one
  channel alone. Under it are its format and size and what its values run from and to. An image with smaller levels
  has its **level** to choose, an array its **layer**, and one with depth its slice; and its picture is drawn
  **Linear** — blended between texels — or **Nearest**, each texel a square, as a height map's cells are told apart. A
  node that makes several — a Script node — has which of them to look at, **Of**; the first image, to begin with.
- **A buffer, as numbers**: an element a line with each field by the name the shader gives it, for a ShaderBuffer;
  otherwise its words, eight a line.

An image needs `eTransferSrc` among its usage to be looked at, and so does a Buffer node's buffer: one that has
not says so. `keditor/look.h` is the conversion (`ToPicture`, `ToLines`), for code that wants it.

## The viewer

**The Viewer**, a tab beside the Scene, looks at one texture, buffer or mesh closely, as a graphics debugger shows
a resource. It opens with **Open in the viewer** — in a node's inspector and its menu, for a node that makes an
image, a buffer or a mesh; in the inspector of a texture or a mesh of the Assets panel — and a texture or a mesh
pressed twice in the Assets panel opens there too. Closed, what it copied out and drew stops. `Session::View(id)` is
the same, for code: a node's id, or a file's (`@wall.png`, `@model.gltf#3`, `$sphere`).

- **An image** at any **level** and **layer** (a cube's six named), and any slice of one with depth, each copied out
  as it is chosen. Shown as suits it, as it is, stretched, or one channel; its red, green, blue and alpha each on and
  off (alpha with colours lays them over a checkerboard); drawn **Linear** or **Nearest** as it is zoomed. The wheel zooms about the pointer, a drag moves it, **Fit**
  and **1:1** put it back; near enough, the lines between texels are drawn. What the texel under the pointer holds
  is said under it — each channel's value and, for a normalised format, what is stored (`R 0.5 (128)`).
  Beside it: its format, type, size, levels, layers, samples, usage and memory; its **histogram**, a curve a channel,
  linear or log; each channel's least, greatest and mean, and how many values are not numbers or are endless; and its
  **range** — what is black and what is white — as the mode has it, fitted to its values, 0 to 1, or two values dragged.
- **A buffer** as a **table**: a row an element, a column a number. A ShaderBuffer is read by its shader's struct;
  any buffer by a **layout** typed as a shader declares one (`float3 position; uint id; float4x4 model; float
  weights[4]`, packed, four bytes a number), or as eight words a row. Sixty-four rows a page, gone through or
  gone to; whole numbers in hexadecimal when asked.
- **A mesh** drawn by itself (`keditor::MeshViewer`), as big as the tab: a drag turns it about, the wheel brings it
  near, **Seen whole** puts it back. Lit, plain, or coloured by **any of its attributes** — a normal or a
  tangent from −1 to 1, a UV as a checker over it, anything else from 0 to 1, each mapping to choose; a value that is
  not a number is magenta. Its pipeline is made for the mesh's own vertex layout, and a shader for what is shown.
  **Its edges** are drawn by its triangles (a geometry stage tells each corner where in its triangle it is), smooth
  and as wide as asked in pixels, and only where the mesh is seen. Beside it: its vertices,
  triangles and indices, what a vertex is made of (each attribute's semantic, type, buffer and byte), and the box it
  reaches over.

`keditor/look.h` has what the viewer is made of, for code: `ToPicture` with `LookOptions`, `HistogramOf`, `TexelAt`,
`ParseBufferLayout` and `ToTable`.

## Importing a model

**Import a model or a picture...** (the Assets menu, or the Assets panel's Create) brings a file into the project and
nothing into the scene (`Session::ImportAsset`): a picture is copied to `assets/textures`, a model to a folder of its own
in `assets/models`, with the files it names beside it — a glTF's buffers and pictures, an OBJ's materials and the pictures
they name — so that the copy is read as the file was. One of the project's already is only picked. Either is then in the
Assets panel, picked, in its folder.

A model is put in the scene by dragging it onto the scene or the scene tree, or with its **Import into the scene...**: that
makes entities of it (`Document::ImportModel`): an entity for each
thing in the file under one named for the file, placed as the file places them, with the file's lights. Each
drawn part has its Mesh (`@model.gltf#3`), and every component a pipeline of the graph makes that has an
`albedo` field, filled from the part's material a field by name: `albedo`, `metallic`, `roughness`, `emission`,
`alphaCutoff`, and the pictures `albedoMap`, `normalMap`, `roughnessMap` and `emissiveMap` (`$white` for one the
material has not). A part with a cut-out has the tag `Masked`, and one drawn over what is behind it — glass, a
decal — `Blended`. The meshes and pictures stay in their files, and are found by them again when the scene is
opened. All of it is read off the frame: the entity named for the file is there at once, its things are made
under it when the file is read, and their pictures come in after.

**Before it is put in the scene, it is asked about.** Whichever way it is put there — *Import into the
scene...* in the model's inspector or its menu; dropped on the scene or the scene tree — a dialog lists what its
meshes have (the file is read for it off the frame, and nothing of it made), each attribute read or left out and
in which buffer, with the same presets as below; **Import** reads it so, the first time. What is left out is never
read at all. At first it shows how the scene reads the file already, or everything in one buffer.

Its meshes are read with **every attribute they have** — normals, tangents and bitangents, each set of UVs and of
colours, bones — unless the dialog says otherwise. The model's inspector (pick it in the Assets panel) lists them too,
to change later: each can be **left out**, and is
then not held at all — bones a scene does not animate, tangents nothing reads — and each read one is in a **vertex
buffer** of its choosing: *Buffer 1* to *4*, or *All in one buffer*, *Positions and UVs · surface · bones*,
*Positions apart*. It says how many bytes each vertex holds, in how many buffers. A change is an edit of the scene
(`Document::SetMeshImport(file, import)`): taken back with Undo, saved with the scene, and the file is read again at
once, with every entity holding its meshes holding the new ones.

A Draw wired from a Filter `with` Mesh draws every one whose mesh has what its vertex shader reads, however the mesh is
laid out; the Filter's `with` and `without` narrow it by tags. An entity whose mesh lacks something — a normal the shader reads, left
out — is passed by, and its inspector says what it lacks.

## Flying through a scene

`FlyCamera` is a script the engine has of its own, so that a scene can be flown through without a project of
scripts: on an entity — the camera — it turns it with the mouse and moves it with W A S D (E and Q up and down,
Shift faster) while the right button is held. Its fields are `speed` and `sensitivity`. Like any script it runs
while the scene plays: in `koral-play`, and in the editor after Run.

## The project that opens

`koral-editor` and `koral-play` given no project open the one this user chose with **Project, Open at startup**
(kept in `~/.config/koral-engine/startup-project`, or under `$XDG_CONFIG_HOME`), and otherwise the engine's own:
Forward+, made with nodes (`forward-plus` in the projects folder, `KORAL_ENGINE_PROJECTS` — `/mnt/w/EngineProjects` unless the build says otherwise; see `samples/forward-plus/README.md`). Both take `--validate` to run with Vulkan's
validation layer, which they otherwise leave out: it costs a fifth of a frame and more in a scene of many draws.
`koral-play --frames N --timings` says what the frames took and what each pass of them did.

## How abstract a node is

A node's card shows its level (kgraph.md, *Node types*) as five steps at its title bar's end, as many lit as its level
and one — a number lights one, a Function's call all five — and says it in words under its title (`Draw · command`).
A card made of other nodes (a Function's call) is a stack of cards. Its colour stays its category's: what it is for.

## The Pass flow panel

A tab beside the Graph: the pages as cards, left to right in the order they run, each wired from the pages whose work
it uses — the last page before it to write what it reads, not the one that made it (what a page reads of the frame
before, a history, is wired too, and has no say in where either goes). Each page is in the column, between those it
takes from and those that take from it, that keeps its wires shortest — a wire from a page that feeds many counting
the less — and at the height of what it is wired to, above or below, none over another. The branches of a Switch are
bands, framed and named for the choice (`renderer = 1`): a page is of a branch when its passes idle unless the choice
is that option (or are on only then), or when only that option is reached from it; what the branches share, in their
columns, is above them. A Switch that
chooses between branches is a card of its own, with a list to choose by; a page whose passes did not run, or idle
(a branch not chosen), is dimmed. Its cards show their cost on the GPU, and — a page being made of nodes — a stack,
and the steps of its most abstract node. Pressed twice, a page is shown in the Graph. Nothing else is edited there.

## The Scene panel

The scene fills its panel. Its title bar has, at its end, whose eyes it is seen through (the editor's own view,
or the scene's camera); play, pause, a step and stop are the window's, in its title bar. Over the picture, in a corner, are the gizmo's tools — select
only, move, rotate, scale — framing what is picked, how the scene is shown, the grid, and how many frames a second.

**File, New project** makes a project with nothing in it. Only a scene that was in no project yet — made in an
editor opened on none — goes into the one that is made: that is where it is saved.

## Assets

The **Assets** panel, along the bottom with Problems behind it, shows what the project is made of as its folders
have it — the way a file browser does.

- **Down its side, the folders**: the project's own (`assets`, `scenes`, `shaders`, `scripts`, and whatever is
  made in them), then **Outside the project** — the files the scene has that are somewhere else, each in a folder
  named for the last two it is in. Under them, **Everything, by kind**: all models, meshes, textures, shaders or
  scripts, wherever they are.
- **Beside them, what is in the folder that is shown**: its folders first, then its files by name, each a picture
  and a name. A texture that is loaded is its own picture, and a mesh has a small one of it once it is drawn; the rest are shown by their kind. The slider at the
  foot says how big; at its least, each is a line of a list. Over it, the folders to where it is, each to go back to.
- **Pressed twice**, a folder is gone into, a shader or a script opens in Visual Studio Code, and a model shows
  its meshes after it (the arrow on a model does the same).
- **The search** looks in every folder, for part of a name; the choice beside it shows one kind only.
- **+ Create** makes a folder in the one shown, a shader, a script, or brings a model's file in.
- **The right button**, on a thing: open it, import a model into the scene, show it in the file manager, copy its
  path, move it to the trash (asked first; the system's trash, by `gio trash`; only what is the project's). Where
  nothing is: the same things to make, there.
- The line at the foot says where what is picked is.

- **Picked**, an asset is what the inspector shows: its file; a texture's size and levels and the picture; a
  mesh's triangles and the box it is in; a shader's module and entry points; for a model, a button that imports
  it into the scene.
- **A mesh or a texture is dragged** from the panel onto the field of an entity that is to hold it — an entity's
  Mesh, a material's `albedoMap` — which then holds it by the name it is found by (`@textures/wall.png`,
  `@sponza.gltf#3`). A field says what it holds of a file as it does of a node; one dropped on a field of the
  other kind is refused, with why.
- **A shader or a script opens in Visual Studio Code** pressed twice, from its menu, or from the inspector: the project's folder,
  at that file. **Project, Open in Visual Studio Code** opens the folder. `KORAL_ENGINE_CODE` names another
  editor's command.

A project is given what that editor needs to know it (`keditor::WriteEditorSetup`), when it is made, opened, or
given a script: `.vscode/settings.json` (where a shader's imports are looked for, what is left out of the file
list) and `.vscode/extensions.json` (the Slang, C# and Kotlin extensions), and `<Name>.Scripts.csproj`, which
names the project's C# scripts and the engine's own assemblies — so a script is written with the engine's names
offered and checked. Nothing builds that file, and it names this machine's paths: it is written again each
time. Kotlin scripts have the same of the Gradle build the project is given.

## Where projects are made

**File, New project** and **Open project** start in `/mnt/w/EngineProjects` — the build's `KORAL_ENGINE_PROJECTS`,
or the environment variable of that name — which is made if it is not there.

## Taking back, and not losing what was done

- **Edit, Undo** (Ctrl+Z) puts the scene back as it was before the last thing done to it — its graph and its
  entities — and **Redo** (Ctrl+Y, or Ctrl+Shift+Z) does it again. What is done in one go, a drag or a number
  dragged through many values, is one thing to take back. What a scene does while it plays is no edit. The last
  64 states are kept, for as long as the scene is open.
- The window's title says **(not saved)** while there is something not saved, and what would throw the scene
  away — another project, a scene made anew, the saved one put back — asks first: save, go on without, or cancel.
- Where the panels are is kept from one run to the next (`~/.config/koral-engine/editor.layout`).

## Keys

| Key | What it does |
|---|---|
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Ctrl+S, Ctrl+R, Ctrl+Space | Save, run, add a node |
| Ctrl+D, Delete | Duplicate what is picked; remove it (an entity, or nodes) |
| Ctrl+C, Ctrl+V | In the graph: copy the nodes that are picked; paste them where the pointer is, on the page that is shown |
| Ctrl+A, Escape | In the graph: pick every node; pick none |
| Q, W, E, R | Select only; move, rotate, scale |
| F | Frame what is picked — in the graph, the nodes that are picked, or the whole page |
| Right button held | Look round from where the view is; with W A S D, E and Q, fly — Shift faster. As a scene's `FlyCamera` does when it plays. |

## The graph, read at a glance

- A node of a loop says how many turns it is there for (`cast   x4`), and a Repeat how many it has.
- **What it costs is on it.** A pass node says its GPU time and its share of the frame's, followed as it runs: faint
  for what hardly counts, amber from 5% of the frame, red from 15%. A Function's call says what every pass inside it
  adds up to, and a node of a loop what its turns do (`keditor::CostOf`). A device that can't time the GPU says the
  CPU's recording instead. Each page in the list of passes says what its passes take of the GPU together, coloured the
  same way.
- A port is called what the shader or the component calls it — `position`, not `field.position`; what kind of
  input it is is in what the pointer over it says.
- A node that makes an image or a buffer has an **eye** on it: on, what it holds is shown on the node. Any number
  are looked at at once.
- **Find a node**, in the list of passes, goes to the node whose name or kind has what is typed; Enter again goes
  on to the next.
- A **Note** node is words on the graph. It makes nothing.
- A Switch whose index a Number or a Bool node gives is a choice in the Scene panel's tools, by what each option
  is called: the picture, or a debug view of it.

## What is wrong, where it is mended

An entity a pass could not run for — a picture field with nothing in it, a mesh that does not fit — is marked in
the scene tree, and its inspector says which pass and why, whether or not it is in sight. The status bar counts the
files still being read.

A problem that has something plain to be done about it says what, under what is wrong: a field with nothing in
it, a shader that does not compile, a node waiting for one that failed. A mesh has a small picture of it, drawn a
few frames after it is first listed and shown in the inspector when the mesh is picked; in the list a mesh is
shown by its kind. The panel shows the first 160 textures of what it lists by themselves, and a filter brings the
rest forward. A menu's line that is on has a tick before it.

## A buffer, from the pipeline that expects it

A pipeline's parameter that is a buffer nothing is wired to has **Make its buffer** beside it in the inspector.
It makes a ShaderBuffer node laid out as the pipeline's shader declares that buffer — `frame`, for a shader with
`ConstantBuffer<FrameData> frame`, is a node with an input for each field of `FrameData` — beside the pipeline, in
its pass, wired to the parameter and picked, for the data to be wired to its fields. Each of the pipeline's
shaders is tried, and the first that declares the buffer lays it out. `Document::MakeBufferFor(pipeline,
parameter)` is the same, for code.

## Working on the graph

The graph has the middle of the window, and most of it; **View, Put the panels back where they start** gives it
that again after the panels have been moved.

- **A page is looked at where it was left.** One first shown is framed whole. A node gone to — found by name,
  pressed among the problems — is picked and brought into view.
- **The right button**, let go where it was pressed, is a menu. On a node: look at what it holds, open its shader
  in Visual Studio Code, go to where it is made (for one brought in from another pass), copy, duplicate, take its
  wires off, delete. On the graph: add a node there, add a note, paste, put the page in order, frame everything.
  Held and dragged it moves the view, as the middle button does.
- **A node pressed twice** goes to where it is made, when it is of another page; opens its shader, when it has one;
  and otherwise looks at what it holds.
- **A wire let go over nothing** asks what it should go to: the list of nodes, with only the kinds that have
  somewhere for it. The one chosen is made there and wired — to the port of the wire's own kind, when it has one.
- **A wire let go on a node** asks which of the node's ports it goes to: from an output, every input of it that
  takes what the wire carries — shown on the node or not, set there or in the inspector — and from an input, every
  output it has, what it gives of itself (`size`, `extent`, `bytes`) among them. The first that fits is ready for
  Enter; typing narrows the list. One wired already says what from, and chosen, the new wire takes its place. Under
  them, greyed, are the ports that can't take it, each with why. `keditor::PortChoices` is the same, for code.
- **Copy, paste and duplicate** keep the wires among what is copied, and the wires from the rest of the graph.
  A copy has a name of its own (`draw forward 2`); pasted, it goes on the page that is shown.
- **Arrange this page** (the graph's menu, or Graph in the menu bar) puts the page's nodes in columns, each after
  what it is wired from and as near what it is wired to as that leaves it, and down each column in the order that
  crosses the fewest wires it can find: each column ordered by where what it is wired to is on either side, at the
  ports it is wired at — a wire across several columns counted in each it passes — sweep after sweep, then nodes side
  by side swapped while that crosses fewer, and all of it begun again from other orders, the best kept. Then each node
  is put level with what it is wired to, its port with the other's, as near as the rest of its column leaves room for
  (the nearest in order there can be, by least squares), so that wires run straight where they can; a wire that runs
  past a column has a lane kept clear through it, and is held straighter than the rest, so that it goes round what it
  passes. Columns are as far apart as the wires between them are steep. Each card is taken at the size it came out on
  the page, and one that shows a picture is that big before its picture comes. Kept level, the page stays so unless
  that is far from the panel's shape — about twice as wide or as tall as it could be — or crosses more wires than
  stacking would: then a column taller than the panel leaves is wrapped into more beside it, at the height, of those
  that shape it so, whose wires cross least. Nodes wired to nothing go in rows underneath, as wide as the rest. Each loop's frame is a block of its own: its
  nodes are put in order by themselves, and the block among the rest as one node, so that a frame holds its loop's nodes
  and nothing else, and no two loops' frames are over each other. A node is of one loop: one that runs once a turn of a
  Repeat and once an element of a ForEach (a shadow's draw) is in the Repeat's. Ctrl+Z puts them back.
  `keditor::ArrangeNodes` is the same, for code.
- **A wire has the colour of what it carries**: an image's is teal, a buffer's amber, a sampler's violet, a
  pipeline's orange, a mesh's green, a number's blue. The wires of what is picked stand out from the rest.
- **A node's title bar has an icon of its kind** (`NodeType::icon`, a Material icon): a picture for an image, a chip
  for a compute pipeline, a funnel for a Filter. Its colour is its category's.
- **A node says what it is set to** in a line under its kind — an image's format, a pipeline's shader, a number —
  and a field given a value where a wire could go says the value beside its port.
- **A loop is a node that holds its nodes**: a Repeat and everything that is there once a turn because of it, with
  how many turns it has; a ForEach or a Filter (with the Entities node it keeps some of, when that is its alone) and
  everything worked out once an element of it — the buffers it fills, the Draws and Dispatches that draw each — with
  how many elements it has. It has a title bar in the loop's colour; one loop inside another is drawn inside it.
  Dragged by its title, the loop moves as one, and arranging the graph keeps it together.
- **A ForEach has a frame** round it, everything worked out once an element of it, and the buffers they fill, with
  the component it goes through and how many elements it has.
- **Each component is a node.** The list of nodes has, under Components, two for every component the world has —
  the engine's package's Camera and lights, the project's scripts', a pipeline's: `PointLight`, a ForEach of it, and
  `PointLight Buffer`, a BufferBuilder laid out as its element is. A script made with **New script** is one at once.
- **A Bundle** is many wires as one, by name, drawn in pink. A Bundle node's inspector edits its names a line each,
  each an input on the node; an Unbundle has an output for each member of what it takes apart. A Bundle let go on a
  buffer, or on a Draw or a Dispatch, offers first **By name: n of its m members**, which fills every input of the
  same name.
- **A BufferBuilder's layout** is edited a line a field: its name, its type, and how many of it (more than one is a
  fixed array). Its fields are ports from the start, as a ShaderBuffer's are.
- A new node goes where the pointer asked for it, or in the middle of what the graph shows.

Not yet, in the Assets panel: a file is not renamed or moved from it (the scene knows a file by where it is, and
would lose it); a scene is listed, and not opened from there. The grid shows the first 160 pictures of what
it lists — textures, and meshes once theirs are drawn — and the rest by their kind.

## The scene tree, the inspector, and what is carried between panels

**Functions** are nodes as one, kept in the project (`functions/<name>.kfunc`) and called wherever they are
wanted. Pick nodes and **Make a function of them...**: they become the function, and one node calls it where they
were — each wire into them an input of it, each wire out an output, the call wired as they were. A call is one node,
so it is in one pass, and everything in it runs there: what a draw needs made in an earlier pass is one call to
put there. Pressed twice (or **Open the function**), a call's function opens in place: its nodes on a page of their
own, a pass after the call's, its inputs given what the call was given. What is done there is the function's: **Done**
writes it to its file, and every call follows — in other scenes as they are opened; **Discard** puts it back. A scene
saved while one is open is saved with it closed, kept. **Write it out here** turns a call back into its nodes. A
call's `function` is chosen among the project's; an input of it left unwired takes what is typed on the call, or else
its default.

**The panels** each have a picture on their button in the stripes. The graph and the scene share the middle, side
by side, with a line between them to drag, and the Game panel is a tab beside the scene. The middle is in as many
parts as are made: a panel held over the left or right edge of any part becomes a part of its own on that side
of it (`kui::DockArea::eCenter` with the part's number, from the left), and dropped on a part's title or its
centre it is a tab there. A part that loses its last panel is gone, and the others share its room.

**The scene tree** shows each entity by what it is — a camera, a sun, a light, a mesh, data, a group, or nothing
yet — with its name, how many are under it while it is shut, a warning when a pass could not run for it (why is
what the pointer over it says), and an eye that shows and hides it with what is under it.

- **Pressed twice**, or Rename in its menu, its name is typed over where it is.
- **Dragged**, an entity goes before another (dropped on its top edge), after it (its foot) or under it (its
  middle): a line or a light says which. Dropped past the tree, it goes to the top.
- **The line over the tree** finds entities by name, or by a component they have; **+** makes an empty one or
  one of the objects.
- **From the Assets panel**: a mesh dropped on an entity is its Mesh (which it is given, when it has none); a
  texture is its first picture field with nothing in it, its albedo's before any other; a script is the
  component of its name. A model dropped on the tree's own ground, or on the scene, is imported.

**The inspector**, for an entity: what it is and its name, its eye, and a button that frames it; **Under**, what
it is under — chosen among the entities it can go under, or an entity dropped there from the scene tree; **Run
by**, the passes' nodes that run for it, each to go to; its transform, with Reset; then each component, with its
kind said beside it (run by a pipeline, what is drawn, a light, data, a tag), an arrow that shuts it, and for a
pipeline's a button that goes to the pipeline.

**What a choice offers is what can be chosen:**

- A field that holds a mesh lists the meshes: those built in, those the scene has of files, the graph's. One that
  holds a picture lists plain white, black and a flat normal, the textures the scene and the project have, and of
  the graph only the images made in a pass before the one its pipeline is run in — not what that pass draws into,
  nor what a later one makes. What it holds is shown beside it, small.
- An asset dropped on a field is taken when it is the kind of thing the field holds, and refused, with why, when not.
  Dropped anywhere else on an entity's inspector, a script is the component it makes, a mesh or a texture is given
  to it as **Add** does, and a prefab is an instance under it.
- A field that holds an entity (a script's `Entity`) is a list of the scene's entities, each under what it is under,
  and takes an entity dragged onto it from the scene tree.
- **Add** lists the components the entity has not, a pipeline's first, then data, then tags, each said for what it is.
- A node's input that takes a resource has a list of the nodes that give one of its kind — of its page, in no
  pass, or made before it, and never one wired from the node itself. Choosing one of another pass brings it in.
- A loop's `of` is a node of a loop (a Reference's is what the drawer brought in, and its inspector the thing itself); a ForEach's
  entity is one that has its component.
- An image's format is one its use allows: a depth format for what is drawn into as depth; none that is depth or
  packed for what is drawn into or written by a shader.
- An input given whole or in parts is one line of its parts in the inspector, and wired whole from there.

**A texture or a shader dropped on the graph** from the Assets panel is the node it makes, there: an ImageFile, or
a pipeline of the shader's entry points (a compute one when it has a compute entry point).

