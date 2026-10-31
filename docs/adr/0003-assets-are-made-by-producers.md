# Assets are made by producers, known by an ID, and owned by kasset

Asset handling had no owner. `kgraph::Runtime` read files (`ResourceOf`, `ModelOf`, `AssetId`), `cook.h` cooked textures,
and the Assets panel and the importer lived in the editor's main file. An asset was known by its path (`@textures/wall.png`,
`@sponza.gltf#3`, `@sponza.gltf#material:2`, `$cube`, `material:Gold`, `shader:module#entry`), so a file couldn't be
renamed or moved, and re-ordering a model's meshes in Blender pointed every reference at the wrong mesh. Import settings
were in four places: a mesh's attribute layout in the scene, a model's material kind in `project.json`, its material edits
in `<model>.materials.json`, and a texture's format in a cook recipe. Only textures were cooked, so models were parsed
from the source file every time a scene opened.

We decided:

- **Two tiers, one identity.** *Resources* are GPU data a pipeline uses: mesh, material, texture, shader. *Documents* are
  scene, prefab, function and script. Both are assets: they get an ID, appear in the Assets panel, can be referenced,
  and have a thumbnail.
- **Every resource is made by a producer from parameters.** A producer is an *importer* (its input is a file) or a
  *generator* (a script, or a compute shader for a texture). Import settings are just the importer's parameters. What
  a producer makes is an *artifact*, cached under a hash of the source's content, the parameters and the producer's
  version.
- **A stable ID in a `.meta` file beside each source.** The `.meta` holds the ID, kind, producer, parameters, the
  *parts* (a model's meshes, materials, textures, lights and cameras by name → sub-ID) and the edits made to its
  materials. A `.kgen` is a `.meta` file with no source next to it: a generator with its parameters. A reference is
  `{id}` or `{id}/{sub}`. Built-in assets have fixed IDs, and old references are rewritten once, when a scene is opened.
- **A model file is a container.** Its meshes, materials and textures are parts. Its hierarchy, lights and cameras are
  a *generated prefab*, so placing a model is instantiating that prefab, and a re-import reaches every placed copy as a
  prefab change does. The file's lights and cameras are entities running the engine's Light and Camera scripts.
- **A script is Setup plus what it adds.** Setup has `Initialize`, `OnEnabled`, `OnDisabled` and `OnDestroyed`. It
  adds *PerFrame* (`Update`, `LateUpdate`), *FixedUpdate*, *Prepare* (every frame, in the editor too) or a generator
  output (*MeshGenerator*, *ImageGenerator*). These are interfaces in the script's language, and the engine gives an
  entity running one a tag component of the same name, which the systems query.
- **A generator is a pure function of its fields.** Its `Initialize` is given only its output, never the world, so it
  can run off the frame, be shared by every entity with the same fields, and be baked at build time. It runs again
  whenever a field changes, both in the editor and while playing. It emits attributes by semantic and may fix a
  layout of its own. Otherwise the same layout setting an imported mesh has decides the layout.
- **Generated meshes are meshes.** An entity's `Mesh` points to a shared mesh resource (buffers, layout, indices,
  parts, bounds), whatever made it. `ProceduralMesh` is a component beside it that holds a generator and its fields
  and fills the `Mesh`, so a filter `with Mesh` matches generated meshes too. A material's texture field can name a
  generator in the same way.
- **kasset never touches the GPU.** It owns identity, the asset database, producers, the cache, which asset is used
  by which, and `kasset::Ref<T>` (pending, ready, or failed with a reason). `kgraph::Runtime` keeps what happens on
  the GPU: uploading, stand-ins, and swapping the real resource in.

## Considered Options

- Keep paths as identity and have the editor rewrite references when it renames a file. That breaks for a file moved
  outside the editor (an export, `git mv`), and does nothing for a model whose meshes are re-ordered.
- Keep IDs in one database file for the whole project. That avoids clutter beside each file, but the file conflicts on
  every merge and loses track of a file moved outside the editor.
- An ID that is the content's hash. It changes every time the file is edited.
- Component inheritance in kecs (`ProceduralMesh : Mesh`, matched by a query for `Mesh`). It's a new ECS feature that
  makes every query look up subtypes, when a companion component that fills `Mesh` gives the same result.
- A generator with access to the world in `Initialize`. That would make it a game script, which can't be cached, shared
  or baked.
- Cooking only textures, as before. Every scene opened would still run the model importer, and a shipped game would
  need it too.

## Consequences

- Two scenes can no longer read the same model with different layouts. A mesh's layout is the asset's, and a change to
  it is undone through the editor's commands rather than as a scene edit.
- `<model>.materials.json` is replaced by the model's `.meta`. A model's material can be *extracted* into a `.kmat` of
  the project's own, which a re-import then leaves alone.
- Every script is rewritten once from `Script` to Setup with the pieces it uses, and `GraphicsScript.Prepare` becomes
  the Prepare piece.
- The `$` procedural meshes stay native: a generator needs a script host, and they must be there without one. An entity
  tunes its own, and the numbers are part of the ID (`$sphere?radius=0.8`), so the same numbers are the same mesh with
  no component or file for them, and they stay pure functions of their fields as a generator is.
- A material's small picture is drawn from its values by the editor's own shader: a project's shader needs what the
  project's graph makes (its lights, camera and shadows), which a small picture has none of.
- A texture's colour space and compression are inferred from the first material field that uses it, and can be
  changed.
- A model's materials go into the kind chosen for them by field name. A kind of the project's own gets a mapping table,
  and what nothing matches is listed rather than dropped.
- A shipped game reads a manifest of what its scenes reach. Meshes generated from the initial fields are baked, and a
  generator runs only when a field changes while playing.
- Work is done in eight phases, each leaving the editor working: the kasset skeleton and the migration of references;
  producers and the cache for every kind; which asset is used by which, and the panel moved out of the editor's main
  file; the script split; generators; models as containers; thumbnails; the build manifest.
