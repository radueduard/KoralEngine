# The shader library

The engine's shaders are in `packages/koral/shaders`, on every project's search path after the project's own and its
modules'. They are of two sorts:

- **The base library**: Slang modules a shader imports for what every renderer needs — numbers, colour, what is
  random, packing, rays and boxes, depth — and the contracts others build on. Each is `koral_<topic>`, its public names
  `Koral…`, and nothing in it is a pass of its own.
- **The engine's passes**: the shaders of the engine's functions (Bloom, Tonemap, TAA, HiZ, the culls, the sorts) and of
  the editor's own views, each built on the base library and on nothing private of its own.

What a project can do without is in modules it imports (see [editor.md](editor.md), Projects and modules), never in the
base library.

Matrices are as the engine's shaders use them: a row vector times a matrix, `mul(v, m)`. Clip space has x and y from
-w to w and z from 0 to w; a picture's uv runs from 0 to 1 as clip x and y run from -1 to 1 (`uv * 2 - 1`).

## The base library

### `koral_math`: numbers and space

| | |
|---|---|
| `KoralPi`, `KoralTau`, `KoralHalfPi`, `KoralInvPi` | |
| `KoralSquare`, `KoralPow5`, `KoralMax3`, `KoralMin3` | |
| `KoralRemap`, `KoralRemapClamped` | A number from one range to another; kept within it. |
| `KoralSafeNormalize(v, otherwise)` | Unit length, or `otherwise` where `v` is too short to have a direction. |
| `KoralTransformPoint`, `KoralTransformDirection` | Through a matrix: with its place, and without. |
| `KoralTransformNormal(normal, model)` | Through the model's cofactors: square to the surface however it is scaled, turned back where a scale mirrors it. |
| `KoralRotate(q, v)`, `KoralMatrixOf(q, scale, position)` | A quaternion (x, y, z, w) turning a vector; as a matrix with a scale and a place. |
| `KoralInverse(m)` | The matrix that undoes `m`. |
| `KoralFullscreenCorner(index)` | Vertex 0, 1 or 2 of one triangle over the whole picture, in clip space: a draw of three with no mesh. |
| `KoralPixelUv(pixel, size)` | A pixel's middle, 0 to 1. |
| `KoralGroupsFor(groups)`, `KoralThreadIndex(id, groupSize)`, `KoralGroupIndex(group)` | More groups than a dispatch counts along x, as rows along y, and which thread or group of them this is. |

### `koral_color`: colour

Light adds up in linear terms. The engine's curve for a colour as it is picked, and for the display, is the power 2.2
(the Lights function's ColorConvert of `eGamma22`); sRGB's exact one is for what is stored as sRGB.

| | |
|---|---|
| `KoralLinearFromGamma22`, `KoralGamma22FromLinear` | The engine's curve: a material's colours and pictures are read with it, the display is given it. |
| `KoralLinearFromSrgb`, `KoralSrgbFromLinear` | sRGB's piecewise curve. |
| `KoralLuminance` | How bright a linear colour looks (Rec. 709). |
| `KoralToYCoCg`, `KoralFromYCoCg` | Brightness and colour apart: where a box round a pixel's neighbours is tightest (TAA). |
| `KoralOklabFromLinear`, `KoralLinearFromOklab` | A space where a step looks the same size everywhere: for blends and gradients. |
| `KoralRgbFromHsv` | |
| `KoralIsBad`, `KoralSane` | Whether a colour is not a number or infinite; black where it is — one bad pixel spreads through a blur or a history. |
| `KoralColorOfIndex(index)` | A colour of a number's own, to tell entities, clusters or meshlets apart. |

### `koral_display`: light as the display shows it

| | |
|---|---|
| `KoralDisplay(light)` | The engine's: ACES's fit, then the display's curve. What the Tonemap function, the material previews and the lit starter scene show. |
| `KoralAcesFitted`, `KoralPbrNeutral` | Two curves: contrasty with hues pulled to white; and colours kept as they are up to white. |
| `KoralExposureFromEv100(ev)` | A camera's exposure value at ISO 100 as a scale. |

Other curves — AgX, Reinhard, a 3D table — are the color-grading module's.

### `koral_random`: numbers that look random, and even sequences

| | |
|---|---|
| `KoralPcgHash`, `KoralPcg3d`, `KoralXxHash32` | A uint, or three, well mixed. |
| `KoralUnitFloat(bits)`, `KoralRandom(index, seed, stream)`, `KoralRandom2(pixel, frame)` | From 0 up to 1, the same every time from the same numbers. |
| `KoralHalton(index, base)`, `KoralR2(index)` | Sequences that fill a range evenly: jitter, samples of a disc. |
| `KoralInterleavedGradientNoise(pixel)` | A different number at each pixel, none the same nearby: for dithering. |

### `koral_encoding`: numbers packed small

`KoralOctEncode`/`KoralOctDecode` (a direction in two numbers), `KoralPackQuaternion`/`KoralUnpackQuaternion` (a
rotation in 32 bits), `KoralPackHalf2`, `KoralPackUnorm4x8`, `KoralPackSnorm2x16`, `KoralPackRgbe` (any brightness in
32 bits), each with its unpacking.

### `koral_sampling`: directions from numbers

| | |
|---|---|
| `KoralBasis`, `KoralBasisOf(normal)` | Three directions about a normal, with no seam: `ToWorld`, `ToLocal`. |
| `KoralCosineHemisphere`, `KoralUniformHemisphere`, `KoralUniformSphere`, `KoralUniformCone`, `KoralConcentricDisc` | From two numbers 0 to 1, spread as a matte surface, the sky, a light or a lens spreads them. |
| `KoralGgxVisibleNormal(toEye, alpha, u)` | A rough surface's facet as the eye sees it (Heitz): reflected about it, a direction as its highlight spreads them. |
| `KoralFibonacciSphere(index, count)` | Directions spread evenly over the sphere. |

### `koral_bounds`: boxes and rays

| | |
|---|---|
| `KoralBoxOutOfSight(low, high, model, viewProjection)` | Wholly past a plane of the picture: nothing of it seen. |
| `KoralPlaceBox(…, reversed)` → `KoralBoxInPicture` | The rectangle it covers and its nearest depth: what an occlusion test reads a depth pyramid with. |
| `KoralSeeBox(centre, half, toClip, halfHeight)` → `KoralBoxSeen` | Out of sight, how far its nearest part is, how many pixels it spans: for many boxes cheaply. |
| `KoralSphereOutOfSight` | |
| `KoralRayPlane`, `KoralRaySphere`, `KoralRayBox`, `KoralRayTriangle` → `KoralTriangleHit` | Where a ray meets them. |

### `koral_depth`: depth and the view

`KoralWorldFromDepth(uv, depth, inverseViewProjection)`, `KoralLinearDepth(depth, near, far, reversed)`,
`KoralRayThrough(clip, inverseViewProjection)` → `KoralRay` (the way the eye looks through a pixel, from the near plane),
`KoralReproject(uv, depth, currentToPrevious)` (where it was the frame before), `KoralClipFromUv`, `KoralUvFromClip`,
`KoralInPicture`.

### `koral_wave`: what a wave does together

`KoralWaveAppend(keep, counters, counter)`: where this thread's goes in a list, with one atomic for the wave.

### `koral_indirect`: indirect draws on the GPU

`KoralDrawCommand`, `KoralMeshRange` (a MeshPack's), `KoralHasBounds`, `KoralCommandOf(commands, count, instance)`,
`KoralLoadMatrix(buffer, at)`, `KoralDrawInstance(…)`: what CullInSight and InstanceCull work out of.

### `koral_lights`: lights, and their shadows

`KoralLight` is a light as the engine's Lights function writes it (64 bytes in a buffer): where it is and how far it
reaches, the way it shines and its kind (`KoralLightDirectional`, `KoralLightPoint`, `KoralLightSpot`), its colour in
linear terms times its intensity, a spot's cone as cosines, and `kindIndex` — which of its kind it is, what its shadow
is found by. Every light's colour is converted with the engine's curve, the power 2.2.

| | |
|---|---|
| `KoralArrivalAt(light, position)` → `KoralArrival` | What reaches a point: from where, how far, and how much — `KoralDistanceFalloff` (less by the square of the way, none at its range) and `KoralSpotFalloff`. |
| `IKoralLights` | Where lights are kept: `KoralLightBuffer(lights, count)` (the Lights function's), `KoralSingleLight(light)`, `KoralStudioLights` (the editor's key and fill, for a thing by itself). `KoralDirectionalLight(direction, color)` makes one. |
| `IKoralShadows` | How much of a light reaches a point past what hides it: `Visibility(light, arrival, position, normal)`. `KoralNoShadows` lets all of it through; `koral_shadows` has the engine's. |

### `koral_brdf`: how a surface answers light, a lobe at a time

A shading is made of a specular lobe (light mirrored off its facets) and a diffuse lobe (light taken in and scattered
out), each a struct of a static `Reflected` — the BRDF, before the cosine:

| | |
|---|---|
| `IKoralSpecularLobe` | `KoralGgxLobe` (GGX facets, Smith's height-correlated shadowing, Schlick's Fresnel), `KoralBlinnPhongLobe`. |
| `IKoralDiffuseLobe` | `KoralLambertLobe`, `KoralBurleyLobe` (Disney's: brighter at a grazing angle when rough). |
| `KoralAnglesOf(normal, toEye, toLight)` → `KoralAngles` | The cosines a lobe is worked out of. |
| `KoralFresnelSchlick`, `KoralReflectanceAtNormal(baseColor, metallic)`, `KoralGgxDistribution`, `KoralSmithGgxCorrelated`, `KoralAlphaOf(roughness)` | The parts, for a lobe of a project's own. |
| `KoralEnvironmentBrdf(roughness, nv)`, `KoralEnvironmentReflectance` | How much of what is around a surface mirrors: the split sum's second half by Karis's fit, no table. |
| `KoralEnergyCompensation(f0, roughness, nv)` | What a rough metal loses to light bouncing between its facets, given back. |
| `KoralShininessOf`, `KoralRoughnessOfShininess` | Blinn and Phong's exponent and a roughness of the same width. |

Roughness is as a material gives it, 0 to 1; a lobe squares it where it needs alpha.

### `koral_shading`: materials and shadings

The contracts a material kind and its shading are written to (see [scripts.md](scripts.md), Shading):

| | |
|---|---|
| `KoralSurface`, `KoralSurfaceAt(position, normal, uv, sampler)`, `KoralSurfaceOf(…, tangent, …)` | A point of a surface: its directions along it worked out from the fragment, or given (a ray's hit). |
| `KoralMappedNormal(surface, texel)`, `KoralNormalTexelOf(rg)`, `KoralBlendNormalTexels(base, detail)` | A normal map's texel; one of two channels; one map's turn over another's. |
| `IKoralEnvironment` | The light from all round: `KoralNoEnvironment`, `KoralUniformEnvironment`, `KoralGradientEnvironment` (zenith, horizon, ground), `KoralCubeEnvironment`. |
| `IKoralShading`, `IKoralMaterial` | What a shading answers (`Reflected`, `Ambient`, `Emitted`, `Opacity`); what a material reads into one. |
| `KoralLit(shading or material, surface — or position and normal —, camera, lights[, environment[, shadows]])` | The light sent to the eye, not tone mapped. |

### `koral_shadings`: the engine's shadings

| | |
|---|---|
| `KoralPbrOf<S, D>` | Metal-rough, of any specular and diffuse lobe; energy compensated. |
| `KoralPbr` | `KoralPbrOf<KoralGgxLobe, KoralLambertLobe>`: what PbrMaterial is read into. `KoralMatte(normal, color)` is one, matte. |
| `KoralBlinnPhong` | A diffuse and a specular colour and a shininess: what BlinnPhongMaterial is read into. |
| `KoralUnlit(color, opacity)` | Light of its own, nothing reflected. |

Every lit thing the engine draws goes through these: the material previews and small pictures (`KoralStudioLights`
and a cube map), the editor's mesh thumbnails and mesh viewer, its clay view, and the starter scenes' shaders.

### `koral_ibl`: light from all round, baked

| | |
|---|---|
| `KoralSh9`, `KoralShOf(buffer)`, `KoralShBasis`, `KoralShDiffuse(sh, normal)` | Nine spherical harmonics of the light all round: what a white matte surface facing a way sends back. |
| `KoralShEnvironment(sh, prefiltered, sampler, mips)` | An `IKoralEnvironment` of them and a cube prefiltered a mip a roughness. |
| `KoralCubeDirection(face, uv)`, `KoralCubeTexelSolidAngle` | A cube's texels as directions, and how much of the sphere each covers. |

The engine's **PrefilterEnvironment** function bakes both of a cube (`koral_ibl_bake`): GGX importance sampled, each
sample from the mip as wide as it covers, and the harmonics a weighed sum over the cube.

### `koral_shadows`: how much of a light reaches past what hides it

Read from the maps the engine's functions draw (see [kgraph.md](kgraph.md), Lights' shadows): a point is moved off its
surface and toward its light by as much of the world as a texel of the map covers there before it is compared, and
filtered — the sun's 5 x 5 taps, as wide in the world in every cascade and faded into the next over each one's last
part; a point light's or a spot's over a disc of `taps` comparisons (`KoralVogelDisk`, turned by `noise` for TAA to
average).

| | |
|---|---|
| `KoralSunShadows(atlas, cascades, viewDepth, filter)` | SunShadows' atlas, `KoralSunCascades` as ShadowCascades fits them (matrices, splits, world texels). |
| `KoralPointShadows(maps, faces, compare, filter)` | PointLightShadows' cubes, six layers a light. |
| `KoralSpotShadows(maps, matrices, compare, filter)` | SpotLightShadows' maps. |
| `KoralCombineShadows(a, b)` | Two of them as one, for KoralLit: what both let through. |
| `KoralShadowFilter` | `bias`, `softness`, `taps`, `cascadeBlend`, `noise`. |
| `KoralSunVisibility`, `KoralSunVisibilityTap`, `KoralPointVisibility`, `KoralSpotVisibility`, `KoralMapPlaceOf`, `KoralCubeFaceOf` | The parts: a tap for fog, a map of a shader's own. |

Each lets through every light but its own kind, and a light past its function's `most`.

## The engine's passes

| Function | Shader | |
|---|---|---|
| Lights | — | Every light in one buffer of `KoralLight`. |
| SunShadows, PointLightShadows, SpotLightShadows | `koral_shadow` | The shadow maps koral_shadows reads. |
| PrefilterEnvironment | `koral_ibl_bake` | A cube prefiltered a mip a roughness, and its harmonics. |
| GaussianBlur | `koral_blur` | A picture blurred across then down: `radius` taps each side, of spread `sigma`. |
| MipChain | `koral_mips` | A picture's levels, each the average of the one before. |
| Bloom | `koral_bloom` | What is brighter than a threshold bleeds round it: a 13-tap chain down and a tent back up. |
| Tonemap | `koral_tonemap` | `KoralDisplay` of each pixel, exposed. |
| TAA | `koral_taa` | Each pixel blended with where it was, held to what is round it now. |
| HiZ | `koral_hiz` | A depth pyramid, each level the farthest of four. |
| CullInSight, InstanceCull | `koral_cull`, `koral_occlusion` | What of an indirect draw is in sight; and not behind what was drawn first. |
| Instances, CullInstances, SplatPoints | `koral_instancer`, `koral_instance_cull`, `koral_points` | Millions of one mesh, made, culled and drawn far off as points, on the GPU. |
| Scan, RadixSort, BitonicSort, OddEvenSort, Compact, IndirectArgs | `koral_scan`, `koral_sort`, `koral_compare_sort`, `koral_compact`, `koral_args` | Prefix sums, sorts, what is kept, how many groups. |

## The engine's modules

In `packages/modules`, each imported by its name (`@modules/filters`; see [editor.md](editor.md), Projects and modules):
what a project can do without. Each has a library its shaders import by the module's name (`import filters;`), passes
(`<module>_passes`), and functions the graph calls. `ModuleTest` (kgraph) runs every function and checks every library.

| Module | Its library | Its functions |
|---|---|---|
| `noise` | Value (in three dimensions or over a plane), Perlin (`KoralPerlin`, tiled), simplex, Worley (`KoralWorley`), `KoralFbm`, `KoralDomainWarp`, `KoralCurl` | — |
| `sdf` | Signed distances of 3D and 2D shapes, hard and smooth booleans, `KoralMarch` of any `IKoralField`, its normal and soft shadow | — |
| `surface_mapping` | Triplanar and biplanar pictures and normals, `KoralParallax` (occlusion mapping), height-blended layers, detail normals | — |
| `color_grading` | AgX, Reinhard's, Hable's and Lottes's curves (`KoralCurve`), lift/gamma/gain, contrast, saturation, white balance, a 3D table | Grade, GradeExposed, AutoExposure |
| `filters` | Catmull-Rom and bicubic sampling, bilateral and joint weights, à-trous weights, `KoralSharpen` | KawaseBlur, DualBlur, Bilateral, Denoise, Sharpen |
| `antialiasing` | `KoralFxaa`, `KoralLanczosUpscale` | FXAA, Upscale |
| `camera_effects` | Circle of confusion, chromatic aberration, vignetting, grain | DepthOfField, MotionBlur, LensEffects, LensFlare |
| `shading_extended` | `KoralOrenNayarLobe`; `KoralClearCoatPbr`, `KoralAnisotropicPbr`, `KoralClothPbr` (sheen), `KoralTranslucentPbr`; `KoralThinFilmFresnel` | — |
| `shading_stylized` | `KoralToon`, `KoralGooch`, `KoralRim`, `KoralBands`, `KoralHatching` | Outline |
| `lights_area` | Sphere, tube and rectangle lights by their representative point (`KoralSphereLightArrival`...), IES profiles | — |
| `ibl_precise` | The split sum from a table (`KoralBrdfFromTable`, `KoralEnergyFromTable`), box-projected probes and their weights | BrdfLut |
| `shadows_soft` | `KoralPcss`, a Poisson disc, EVSM (`KoralEvsmVisibility`), `KoralContactShadow` | ShadowMoments |
| `screen_space` | A pixel's place and normal of the depth, depth-kept weights, a subsurface profile | GroundTruthOcclusion, ScreenSpaceReflections, SubsurfaceBlur |
| `atmosphere` | Hillaire's air and its tables, the sky's light (`KoralSkyLight`), the sun's disc; its clouds (`atmosphere_clouds`, of the noise module, which it imports) | Atmosphere, Sky, Fog, FogSunShadowed, FogTraced |
| `animation` | Linear and dual quaternion skinning, morph targets, wind sway, billboards, baked vertex animation | — |
| `raytracing_extras` | Inline rays (`KoralTraceAny`, through what casts no shadow: `KoralShadowMask`), a RayScene's `scene` read (`KoralSceneTraceable`, `KoralSceneStructure`), traced shadows and occlusion, ray cones for a picture's level | TracedOcclusion |

Not among them, for want of what they are made of: SMAA (its area and search tables), and area lights by linearly
transformed cosines (their fitted tables) — the representative point stands for those.

## Tests

`LibraryTest` (kgraph) runs `modules/kgraph/tests/shaders/library.slang` on the GPU: each check of the library — round
trips of every curve and packing, the spread of every way of sampling, boxes and rays against a lens — writes what it
got and what it should be, and the test names each by its line. `shadowCheck.slang` does the same with what
PointLightShadows and SpotLightShadows drew of a cube under two lights; another test blurs one white pixel and takes a
mip chain of it.
