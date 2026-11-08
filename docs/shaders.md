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
| `KoralRayPlane`, `KoralRayBox`, `KoralRayTriangle` → `KoralTriangleHit` | Where a ray meets them. |

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

## Tests

`LibraryTest` (kgraph) runs `modules/kgraph/tests/shaders/library.slang` on the GPU: each check of the library — round
trips of every curve and packing, the spread of every way of sampling, boxes and rays against a lens — writes what it
got and what it should be, and the test names each by its line.
