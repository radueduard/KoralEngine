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
```

A scene file holds the graph as [kgraph](kgraph.md) saves it and the world as [kecs](kecs.md) saves it.
A resource an entity holds is saved as the id of the node that makes it.

## The panels

| Panel | |
|---|---|
| **Graph** | The scene's pass graph. Right-click where nothing is, or press Control+Space, to add a node. Drag from a port to wire it, drag a node to move it, Delete removes what is picked. A node that failed is outlined in red and says why. |
| **Scene** | The scene as it runs, sized to the panel. |
| **Outliner** | The entities, and a button that makes one. |
| **Inspector** | Whatever was picked last: a node's inputs, or an entity's components. |
| **Problems** | Every node that failed and every pass that passed an entity by, with the reason. Click one to pick its node. |

The menus: **File** makes a project, opens one and saves the scene (Control+S). **Project, Run** saves and
starts `koral-play` on the project (Control+R). **Graph** and **Entity** add a node and an entity.

### Adding a node

The list of nodes opens on every kind there is, under its category: Resources, Pipelines, Values, Math,
Vectors, Logic, Passes. Type what you are after and it becomes the kinds that fit, best first, each with
what it does. Words other than the name find a node too: `texture` finds Image, `mix` finds Lerp, `run
compute` finds Dispatch, `if` finds Select, `<` finds Less. Enter adds the first. The nodes this graph
already has are listed under the kinds, by name, to go to one.

A node is added working: what it needs is set to something sensible, so an Image is a 512 by 512 image
from the start and not a row of blanks.

### What a node gives

**Add output** in the inspector lists what a node gives besides what it makes: an image's `size`, `width`
and `height`, a buffer's `bytes`, and every one of its settings. One picked from it becomes an output on
the node, to wire from. An output that something is wired to stays on the node.

Each output a node shows this way is listed in the inspector with an **x**, which takes it off the node
again, and whatever was wired from it with it.

An input that is x, y and z on one line (`extent`, a Dispatch's `groups`) can be wired as one vector: pick
its name from **Add input** and it becomes a port on the node, in place of the three numbers. Its own **x**
brings the numbers back.

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
- **The hierarchy** isn't shown: the outliner is a flat list.
- `Project, Run` finds `koral-play` beside the editor on Linux only. Elsewhere it has to be on the path.
