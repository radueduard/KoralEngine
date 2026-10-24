# Forward+, with nodes

The framework's ForwardPlus project (`~/Koral/ForwardPlus`: C++ render passes and GLSL shaders) made as a Koral
Engine scene: every pass a page of the graph, every buffer a node. Nothing in the engine knows of it.

- `forward-plus` in the projects folder (`KORAL_ENGINE_PROJECTS`: `%USERPROFILE%\KoralProjects` on Windows,
  `/mnt/w/EngineProjects` elsewhere, unless the build says otherwise) is the project itself, and what `koral-editor` and `koral-play` open when they are given none: its
  `shaders` folder — the original's shaders in Slang — and a scene made of the original's own models, the three
  Sponza packages under `/mnt/w/Assets`, which it names by where they are. It is not in the repository.
- `shaders` here are the sample's shaders, as the project started with them: what the tests copy into the projects
  they make.
- `forward_plus.h` is the graph and a scene for it to draw, added through the document as the editor's panels add
  them: `forwardplus::BuildGraph(doc, error)` and `forwardplus::BuildScene(doc)`. The camera has the engine's
  `FlyCamera`: hold the right button, and the mouse turns it and W A S D E Q move it.

`modules/keditor/tests/test_forward_plus.cpp` builds it, runs it, and leaves it as a project in the build folder:

```sh
cmake --build cmake-build-debug --target keditor_tests koral-play koral-editor
cmake-build-debug/modules/keditor/keditor_tests --gtest_filter='ForwardPlus.*'
cmake-build-debug/koral-play cmake-build-debug/forward-plus-project
cmake-build-debug/koral-editor cmake-build-debug/forward-plus-project
```

`FORWARD_PLUS_SHEET=<stem>` makes the test write the picture to `<stem>.ppm`.

## The pages

| Page | What it does | The original's |
|---|---|---|
| `frame` | The camera, the sun with its cascades and the sky — each a ForEach of the engine's package's component, the first of it, and a View of the camera — and the point lights, a ForEach of `PointLight`: as the three buffers the shaders declare, each a ShaderBuffer, the lights an element an entity. Their places, collected, are what the lamps' shadow cubes are made round. | The camera module's set, `LightCulling::SetPointLights`, `DirectionalLight::Update` |
| `lights` | Sorts the lights into 16 x 9 x 24 clusters: a ClearBuffer of the counter, and a Dispatch of one work group a cluster. | `Light.h`, `lightCulling.comp.glsl` |
| `shadows` | Four cascades into a quarter each of one depth image: a pipeline and a Draw, in a loop of four turns. | `DirectionalLight.h` |
| `prepass` | Depth, before anything is shaded. | `DepthPrePass.h` |
| `forward` | Every entity with a Mesh and the `forward` component (albedo, metallic, roughness, emission, and its four pictures), lit by its cluster's lights and the sun; beside the picture, where each surface is, which way it faces and how it mirrors. | `ForwardPass.h`, `albedo.frag.glsl` |
| `occlusion` | Horizon-based ambient occlusion, blurred, over the picture. | `HBAO.h` |
| `sky` | The sky where nothing was drawn. | `Skybox.h` |
| `reflections` | Screen-space reflections: traced, then added. | `SSR.h` |
| `bloom` | Six levels down and five back up, each way a loop, then over the picture. | `Bloom.h` |
| `show` | Tone mapped, and in the window; or the clusters' heat. | `ToneMapping.h`, `Present.h` |

## Looking into it

- `debug view`, a DebugView on the `show` page: **Cluster heat** in the editor's Scene panel, how many lights each
  pixel's cluster holds (`clusterHeat.slang`) — grey for none, blue through green and yellow to red, with a line
  where a cluster ends. The pass that makes it runs only while it is shown there, and never in the game.
- In the editor, any image or buffer of the graph can be looked at from its node's inspector: the depth, the shadow
  atlas, a level of the bloom chain, the lights' buffer by field.

## A model in place of the primitives

`forwardplus::ImportScene(doc, model, error)` makes the scene of a model file instead — each of its parts with its
mesh and its material's pictures — and `BuildGraph(doc, error, environment)` takes an `.hdr` for the sky to be. The
test's third case does both with the glTF samples' Sponza, where this machine has it, and leaves that project in
`cmake-build-debug/forward-plus-project-sponza`; `FORWARD_PLUS_MODEL` and `FORWARD_PLUS_SKY` name other files, and
`FORWARD_PLUS_MORE` more models for the same scene, a colon between them. The original's own scene is

```sh
FORWARD_PLUS_MODEL=/mnt/w/Assets/main_sponza/NewSponza_Main_glTF_003.gltf \
FORWARD_PLUS_MORE=/mnt/w/Assets/pkg_a_curtains/NewSponza_Curtains_glTF.gltf:/mnt/w/Assets/pkg_b_ivy/NewSponza_IvyGrowth_glTF.gltf \
cmake-build-debug/modules/keditor/keditor_tests --gtest_filter='ForwardPlus.AModel*'
```

and its `scenes` folder and `project.json` are what the `forward-plus` project has. The files are read off the frame:
the scene is there at once and fills in over ten seconds or so.
In the editor, **File > Import model...** does the same for any scene.

The loops: the shadow pass is one pipeline and one Draw in a Repeat of four turns, and each way of the bloom
chain is a view, a pipeline and a Dispatch in a Repeat, with a Previous for the level the turn before wrote.

## What is not here

- **Light through cloth and through glass** (the original's translucency and transmission). Glass and decals are
  drawn over what is behind them by how opaque they are — the `forward blended` pipeline, for what has the tag
  `Blended` — and a cut-out (`alphaCutoff`) by the forward pass alone. Nothing is culled by which way it faces; what is out of the camera's sight, or a cascade's, is not drawn (`cull`).
- **A model's tangents.** Its meshes are laid out as the engine's own are, so a normal map's frame is worked out
  in the shader from how the surface and its texture coordinates change across a pixel.
- **TAA.** It needs the camera moved a part of a pixel each frame and last frame's picture to blend with; a graph
  can ask for neither yet. The reflections' noise, which TAA averaged away, is therefore still.
- **Drawing from one buffer of draws, and the occlusion cull** (`VisibilityCull.h`, off in the original unless
  asked for): entities are drawn one at a time, as the engine draws them.
- **SSAO** (the original's other ambient occlusion, off unless asked for).

## Glass

The forward shader's component has a `transparency`: how much of what is behind a thing shows through it, over
what its albedoMap lets — 0 none, 1 all of it. It counts where the thing is drawn by the blended pipeline: an
entity with the `forward blended` component and the `Blended` tag. What it mirrors of the sky stays on it however
clear it is, more so seen along itself.

A model's file says which of its materials are blended, and the importer goes by that. The new Sponza's glass
(`glass`, `lamp_glass_01`) is opaque in its glTF, so its 53 parts in the `forward-plus` project were made clear by
hand: moved from `forward` to `forward blended`, tagged `Blended`, and given a transparency — 0.85 for the windows'
panes, 0.5 for the lamps'.

## The lamps' shadows

Each point light has a cube of shadow maps: six square views from where it is, a quarter turn wide, as far as it
reaches. A `PointShadows` node gives the faces' matrices — six a light, for the first `most` lights with a
PointLight, in the order an Entities node lists them, so a light's place in the lights' buffer is its cube's. The
faces are the layers of one depth image (`lamp shadow maps`, six times `most` layers of 512), and the pass `lamp
shadows` draws them in a loop, a face a turn: a view of that layer, a framebuffer of it, the `lamp shadow` pipeline
told which face it is, and a Draw that leaves out what is wholly out of that face's sight (`cull`).

The forward shader looks a point up in the face that looks its way — chosen by which way from the light it lies
most — with the same matrix the face was drawn by, and softens the edge over nine taps. It is an image of layers
sampled as a cube by hand, not a cube-map sampler: the framework makes a cube of an image of exactly six layers
only, and has no cube arrays.

Drawing every face every frame is most of a frame's work, so `each` on the node says how many lights' cubes are
drawn again a frame, one light after another, and the Draw's `idle` leaves the rest as they were drawn last. A
light that moved, or reaches another distance, is drawn again at once; something that moves near a lamp has its
shadow from that lamp catch up within as many frames as it takes to go round them all.

The `forward-plus` project has the lights its model came with and no others: the file's sun (given the brightness
the file leaves at nothing) and its 22 lamps. The 64 lamps and the sun the sample put there are gone, and the
model's stand-in for the sky all round is no longer a point light.

