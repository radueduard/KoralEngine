# Koral Engine

A scene's rendering and simulation is a graph of nodes that the editor draws and the runtime turns into GPU work.

## The graph

**Page**:
A named group of nodes in the graph, laid out together, with a rate (setup, fixed step, every frame) and a place in the order. It records nothing itself.
_Avoid_: Pass (for the group), tab, layer

**Pass**:
A node that records GPU work (Clear, Blit, Dispatch, Draw, TraceRays, …). It is what is ordered, synchronised, timed and put on a queue.
_Avoid_: Command node, page (for this)

**Function**:
A graph of its own that is called as one node and can always be opened down to the nodes it is made of. It is how the graph gets friendlier without hiding anything.
_Avoid_: Macro, subgraph, black-box node

**Bundle**:
A set of named values carried on one wire: an entity's component element, a camera. A field it reaches is filled by name, and a wire to a single field takes precedence over the bundle.
_Avoid_: Struct (for the wire), record, packet

## Materials

**Material**:
What a drawn entity is made of: a set of named values of one Material kind, shown on the entity as a script's fields are, but doing nothing.
_Avoid_: Pipeline's component (the old meaning), shader parameters

**Material kind**:
The declared shape of a Material — which values it has — with a matching form a shader can include. The engine brings two: PBR and Blinn-Phong; a project may declare more.
_Avoid_: Material type, shading model (for the declaration)

**PBR material**:
The Material kind for files that describe a surface physically: base colour, metallic, roughness, emission.
_Avoid_: Metal-rough, standard material

**Blinn-Phong material**:
The Material kind for files that describe a surface the classic way: diffuse colour, specular colour, shininess.
_Avoid_: Specular-Diffuse, specular-glossiness (a different, PBR model)

**Reported material**:
A material as a model file's importer gives it: which model it is (PBR or classic), its values and its pictures. What a Material kind is chosen and filled from on import.
_Avoid_: Imported material, file material, source material

**Material asset**:
A Material kept in the project — one of a model file's, a project's own, or one of the defaults — that entities share. An entity follows it until a value is changed on the entity itself.

**Override**:
A value of an entity's Material made the entity's own, so it no longer follows its Material asset; the rest still does. Applied to the asset, reverted, or the whole Material unlinked from it.

**Pipeline parameters**:
The values a pipeline's shaders read for each entity that no Material kind provides. Made by the pipeline, and needed, with every Material kind its shaders name, for an entity to be drawn by it.
_Avoid_: Material (for this), pipeline's component

**Frame view**:
The read-only picture of the graph as it runs: only its Passes, in the order the frame graph schedules them, with the resources between them and what each costs.
_Avoid_: Timeline, profiler, overview (the overview is every node at once, laid out)

## Assets

**Asset**:
Anything the project keeps that something else can name: a Resource or a Document. Known by an ID that survives renaming and moving, not by its path.
_Avoid_: File (for this), item

**Resource**:
An Asset a pipeline uses as data on the GPU: a mesh, a material, a texture, a shader. Always made by a Producer.
_Avoid_: Asset (for these alone), GPU object (that is what the runtime makes of one)

**Procedural mesh**:
A mesh the engine makes from a shape and its parameters, named by them (`$sphere?radius=0.8`): each entity tunes its own, and those with the same numbers share one mesh. Native, so it is there without a script host; a script's mesh comes from a Generator.
_Avoid_: Default mesh (for the fixed ones there used to be), primitive

**Shading**:
What a material is at a point, its pictures read, and how that answers light (`IKoralShading`). A kind `K`'s comes from its module `KShading`.
_Avoid_: Shader (the program), lighting model

**Document**:
An Asset the editor opens and edits as a whole: a scene, a prefab, a function, a script.
_Avoid_: Resource (for these)

**Producer**:
What makes a Resource from parameters: an Importer or a Generator. An asset's settings are its Producer's parameters.
_Avoid_: Loader, processor

**Importer**:
A Producer whose input is a file: a model, a picture, a shader module.
_Avoid_: Loader, reader

**Generator**:
A Producer that is a script (or a compute shader, for a texture) whose fields are its parameters: a sphere of so many subdivisions, a noise. Given only what it makes, never the world.
_Avoid_: Procedural script, builder

**Artifact**:
What a Producer made, cached under a hash of what it was made from: the content of the source, the parameters, and the producer's version. What the runtime reads, never the source.
_Avoid_: Cooked file, cache entry, import

**Part**:
A Resource inside another Asset: a model's mesh, material or texture, known by its own sub-ID.
_Avoid_: Sub-asset, child, mesh index

**Setup**:
What every script is: `Initialize` and what follows its life. What it does each frame, at a fixed step, or as a Generator, it adds.
_Avoid_: Script (for the base alone), behaviour

**PerFrame**:
What a script adds to be called every frame while playing (`Update`, `LateUpdate`), and the tag its entities carry for it.
_Avoid_: Update script, ticking
