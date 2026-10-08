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
| **Graph** | The scene's pass graph. Right-click where nothing is to add a node there. Drag from a port to wire it, drag a node to move it, Delete removes what is picked. A node that failed is outlined in red and says why. |
| **Scene** | The scene as it runs, sized to the panel. |
| **Outliner** | The entities, and a button that makes one. |
| **Inspector** | Whatever was picked last: a node's inputs, or an entity's components. |
| **Problems** | Every node that failed and every pass that passed an entity by, with the reason. Click one to pick its node. |

The menus: **File** makes a project, opens one and saves the scene (Control+S). **Project, Run** saves and
starts `koral-play` on the project (Control+R). **Graph** and **Entity** add a node and an entity.

### A node in the inspector

Every input the node has is listed, with an editor for its type: a number to drag, a box to tick, a list
of an enumeration's names, a tick for each flag, a row of numbers for a vector, the fields of a struct one
under another. A pipeline also lists what its shaders declare: `param.<name>` and `constant.<name>`.

- An input that takes a resource is a port on the node. Wire it on the graph.
- An input that takes a value is edited here. The small `o` beside it makes it a port too, so a value node
  can be wired to it.
- `x` takes an input back to nothing given.
- Anything with no editor of its own is typed as JSON.

When you add a node, the dialog also takes a name. A pipeline's name is what its entities' component is
called, so give it one.

### An entity in the inspector

Its name, then each component with its fields. A field that holds a resource is a list of the nodes whose
output fits. The tick beside a component's name switches it off without removing it. **Add** lists the
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
- **A node can't be renamed** after it is made. Its title can't be edited from the interface either.
- **One scene a project** in the interface. `project.json` can list more, and `koral-play` takes a scene
  file, but the editor opens the starting one only.
- **An entity's own image or buffer** has to come from a node. Nothing in the editor makes a resource per
  entity.
- **The hierarchy** isn't shown: the outliner is a flat list.
- `Project, Run` finds `koral-play` beside the editor on Linux only. Elsewhere it has to be on the path.
