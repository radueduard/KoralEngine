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
```

A scene file holds the graph as [kgraph](kgraph.md) saves it and the world as [kecs](kecs.md) saves it.
A resource an entity holds is saved as the id of the node that makes it.

`koral-editor` and `koral-play` take `--no-vsync`, to draw frames as fast as they are made rather than at
the display's rate, and `--frames N` to stop after so many.

## The panels

| Panel | |
|---|---|
| **Graph** | The scene's pass graph, a page a pass: the passes are listed down its side. Right-click where nothing is, or press Control+Space, to add a node. Drag from a port to wire it, drag a node to move it, Delete removes what is picked. A node that failed is outlined in red and says why. |
| **Scene** | The scene as it runs, sized to the panel. |
| **Outliner** | The entities, as a tree, and a button that makes one. |
| **Inspector** | Whatever was picked last: a node's inputs, or an entity's components. |
| **Problems** | Every node that failed and every pass that passed an entity by, with the reason. Click one to pick its node. |

The menus:

- **File** makes a project, opens one, starts the standard scene over (**New scene**), reads the scene
  again from its file (**Revert to saved**) and saves it (Control+S).
- **Edit** duplicates, moves to the top, saves as an object, or deletes what is picked.
- **Create** makes an entity from one of the project's objects, at the top or under what is picked.
- **View** frames what is picked, looks through the scene's camera, and chooses the gizmo.
- **Assets** makes a new shader or a new script: a name, a kind, and a file in the project to start from.
- **Scene** plays, pauses and stops.
- **Project, Run** saves and runs the project (Control+R): with its scripts' own runner when it has scripts,
  with `koral-play` when it has none. **Open with its scripts** starts the editor again through that runner.
- **Graph** and **Entity** add a node and an entity.

### Passes

The graph is split into passes, listed down the side of the Graph panel in the order they are drawn. Press
one to see its page: the nodes it makes and runs, and nothing of any other pass. **Every node at once**
shows the whole graph.

- **A new pass** is made by typing its name under the list. Right-click a pass to move it earlier or later,
  or to delete it (with its nodes, or keeping them in no pass). Its name is edited under the list.
- **Create...** makes a node in the pass shown: an image, a framebuffer, a pipeline, a pass node. So does
  right-clicking its page, or Control+Space, for every kind of node.
- **Import...** lists what the passes before this one made. Picking one puts a node on this page that
  stands for it, greyed and labelled with where it is from, to wire from as if it were the thing itself.
- A node's inspector has a **Pass** line to move it to another page. Clicking a problem, or finding a node
  by name, turns to the page it is on.

A node wired straight to one on another page, with nothing imported, shows that node greyed beside it.

### New shaders and scripts

**Assets, New shader** asks for a name and a kind, and writes `shaders/<name>.slang`:

| Kind | What it starts as |
|---|---|
| Compute | Fills an image wired to `param.target`, with a constant the node sets. For a ComputePipeline and a Dispatch. |
| Mesh | Draws a mesh for each entity, with the frame's camera and one field of its own, `color`. For a GraphicsPipeline and a Draw. It imports `engineCommon`, which is written beside it when not there. |
| Fullscreen | One triangle over the picture, reading another picture at `param.source`. Needs no vertex layout and no mesh. |

The name is the module a pipeline node then picks. **Assets, New script** writes `scripts/<Name>.cs` (or
`.kt`): a class of that name with one field and an `Update`, which is a component of that name once the
project runs with its scripts (see [scripts](scripts.md)). A name has to be one code can use, and not one a
shader, a script or a component already has.

### The standard scene

**File, New scene** replaces the scene with one that draws meshes lit under a sky. It writes four shaders
into the project's `shaders` (`engineCommon`, `lit`, `sky`, `outline`; one already there is left alone) and
builds the graph that uses them out of ordinary nodes, to be changed like any other:

| Pass | What is on its page |
|---|---|
| `frame` | The Screen; Camera, Sky, Sun and Lights nodes, reading the scene; `frame data` and `light data`, the two buffers their values are packed into, in the order `engineCommon.slang` declares them. |
| `scene` | `color`, `ids` and `depth` images the size of the window, and `lit`, the pipeline that draws meshes. What makes an entity drawn is a `Mesh` and this pipeline's component, which the shader makes: `albedo`, `metallic`, `roughness`, `emission`. It also draws each entity's number into `ids`. |
| `sky` | The sky, drawn where nothing nearer was. |
| `outline` | A line round what is picked, read from `ids`. |
| `editor` | `helpers` (DebugDraw) for the gizmo and the light and camera shapes, and `pick`. |
| `show` | The Blit of `color` to the Screen. |

Each pass imports what it uses of the ones before it. To send something else to the shaders — a script's
value, another entity's place — wire it into `frame data` as another field and declare it in the block.

It also makes a sky, a camera, a sun and a sphere. `keditor::AddStandardGraph` and `AddStandardScene`
(`<keditor/starter.h>`) do the same from code.

### The scene view

The Scene panel shows the scene from the editor's own view: right-drag turns it about what it looks at,
middle-drag moves it, the wheel zooms, and **Frame** points it at what is picked. **Camera** looks through
the scene's camera instead. Each Camera entity is drawn as the pyramid it sees through, and a picked light
as its reach.

A click on the picture picks the entity drawn there, through the graph's Pick node; what is picked is
outlined and gets a gizmo (**Move**, **Rotate**, **Scale**, or **Select** for none) that changes its
transform. All of this needs the graph to have the nodes for it, as the standard one does: a scene whose
graph has no Camera node is drawn however its own nodes say, and the view and gizmo do nothing to it.

### Playing

A scene in the editor stands still until **Play**: then its scripts run, and it is seen through the
scene's camera. **Pause** holds it. **Stop** puts every entity back as it was when Play was
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
moon, a camera, the three lights and a sky. The mesh objects name the standard graph's `lit` component.

A component is never described by a file. One comes from a pipeline's shader, from a script, or is one of
the scene's own (a camera, a light, the sky).

### Entities

The outliner is the hierarchy. An arrow folds what is under an entity. Drag an entity onto another to put it
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
Screen. No mesh, no vertex layout, no entity. Everything else a draw can say is under **Add input**: a mesh
to draw instead of the entities', how many vertices and instances, an indirect buffer, the viewport and
scissor, what the framebuffer is cleared to, and the state the pipeline leaves open (culling, depth,
stencil, line width).

### A node in the inspector

A node shows the inputs it cannot do without and the ones it has been given. An Image shows its format,
size and usage. **Add input** lists everything else it can take; one picked from it joins the inputs above,
set to what it starts as. `x` takes an input back to nothing given, and off the node again when
it was one it can do without.

Each input has the control for what it is:

| Input | Control |
|---|---|
| A number, or the parts of one thing (`extent`, `groups`) | A number to drag, or one line of them under the thing's name. |
| True or false | A box to tick. |
| An enumeration, or one of a few names (`stages`, `command`, `shape`) | A list of the names. |
| Flags | A tick for each. |
| A colour (a Clear's `color`, an attachment's `clear`) | A swatch that opens a colour picker. |
| A shader module | The project's `.slang` files, by name. |
| An entry point | The functions the module beside it declares with `[shader(...)]`. |
| Components (`with`, `without`) | A tick for each component and tag the scene has. |
| An image parameter (`cover`) | The image parameters of the pipeline the pass runs. |
| A node (`Reference`) | The nodes that make a resource. |
| A vertex layout's attributes | A line an attribute: what it is, its type, how many. |
| A struct | Its fields, each with its own control. |

A pipeline has no shader nodes. It names its shaders: `compute.module` and `compute.entry`, or, for a
graphics pipeline, a module and an entry point for each stage. **`stages`** chooses how a graphics pipeline
is drawn: `eVertex` for a vertex shader with optional tessellation and geometry shaders, or `eMesh` for a
mesh shader with an optional task shader. The node shows the inputs of the one chosen. What the shaders
declare appears as `param.<name>` and `constant.<name>`.

An input that takes a resource is a port on the node, and so is one added from **Add input**: wire it on
the graph. A value a node can work out (a number, a vector, a colour) can be wired too: the small `o`
beside it makes it a port.

When you add a node, the dialog also takes a name. A pipeline's name is what its entities' component is
called, so give it one.

### An entity in the inspector

Its name, then each component with its fields. A field that holds a resource is a list of the nodes whose
output fits. A `vec3` or `vec4` whose name says it is a colour (`color`, `tint`, `albedo`) gets a colour
picker. The tick beside a component's name switches it off without removing it. **Add** lists the
components the entity doesn't have yet: one for each pipeline that runs per entity, `Mesh`, and the
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
- **No undo**, and no copy and paste of nodes.
- **What was asked for with Add input and not set is forgotten** when the editor closes: only inputs that
  were given are in the file.
- **A node can't be renamed** after it is made. Its title can't be edited from the interface either.
- **One scene a project** in the interface. `project.json` can list more, and `koral-play` takes a scene
  file, but the editor opens the starting one only.
- **An entity's own image or buffer** has to come from a node. Nothing in the editor makes a resource per
  entity.
- **Entities aren't reordered among their siblings** by dragging: a drop puts one last under what it is
  dropped on. An entity is renamed in the inspector, not in the outliner.
- **A component the shader makes starts at zero**: a new `lit` is black and polished until its fields are
  set. The objects set them. Objects have no icons.
- **The scene view's toolbar is words**, and a menu line that is on is marked with a `*`.
- `Project, Run` finds `koral-play` beside the editor on Linux only. Elsewhere it has to be on the path.
