# Materials are data of their own; shaders claim them by annotation

A material used to be a pipeline's component: what a pipeline's draws left unwired, made for each pipeline, so an
entity drawn by a forward pass and a ray tracer held two copies of its colour, and the importer filled every component
that happened to have an `albedo`. We decided that a Material is data of a Material kind, declared like a script with
fields and no hooks, independent of any pipeline. The engine ships PBR and Blinn-Phong, and a project can declare more.
A shader takes what it needs by annotating its own struct members (`[PbrMaterial("baseColor")] float4 albedo;`). Each
kind a shader names becomes a requirement for an entity to be drawn by it, and the members nobody claims go into that
pipeline's Pipeline parameters. Materials live as Material assets that entities share. An entity follows its asset
field by field until it overrides a value, as a prefab's instance does. A model's materials become those assets through
a mapping from what the importer reports (PBR or classic) to a kind. That mapping is a default a project can change.

## Considered Options

- Keep the pipeline's component as the material, filled by field name: nothing new, but the importer has to guess which
  components are materials, values are duplicated for each renderer, and nothing can be shared or edited as an asset.
- A shader-side material struct as the source, with the CPU side reflected from it: a project's kind would need Slang
  written first, and a kind could not exist before a shader uses it. The CPU declaration is the source, and the Slang
  form of each kind is generated from it into an includable module.
- A material per entity, copied on import: simpler, but a model with hundreds of parts couldn't be retuned in one place.

## Consequences

- Scenes saved before this load their pipeline components as Pipeline parameters. Their material values move into an
  unlinked PBR material per entity on first load.
- The `Masked` and `Blended` tags are kept in step with a material's `alphaMode`, because a filter can't see inside a
  field.
- How a kind is lit is a Slang module of its own, `<Kind>Shading`, over the interfaces of `koral_shading`: a material read at
  a point gives a shading that answers light. The editor finds the module by name and generates a few lines per kind to
  draw it, so its preview and small pictures need no project graph, and a project's kind and function are drawn like the
  engine's. A generated struct is extended rather than generated with the interface, so the kind's module needs no
  shading and a shader that only takes fields compiles without any.
