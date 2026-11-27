# Technique decisions

Newest last. Each entry: what was chosen, why, what was rejected, and the
sources. Check here before re-researching a topic.

## Current choices (index)

One line per area: the choice in force and the entry that holds it. Entries
marked superseded carry a "Superseded by" line under their title. Entries
listed under "Tuned under older conditions" were tuned before the current
defaults and are candidates to re-check when their area is next touched,
not verdicts.

- Resolution and anti-aliasing: native 1440p, DLSS Super Resolution DLAA
  default, own TAA the fallback, Ray Reconstruction an A/B: "DLSS DLAA is
  the default anti-aliasing (2026-11-22)" (resolution: "Native 1440p with
  our own TAA is the default").
- Sun shadows: raster cached cascades, `--shadows rt` an A/B: "Raster sun
  shadows are the default (2026-11-21)".
- Global illumination: terrain-following probes, ground cache, on by
  default, foliage lit where the ray meets it: "GI probes on by default",
  "GI probes: foliage lit where the ray meets it", "GI reads material
  albedo". The screen-space bitmask half was never built.
- Crown occlusion: "GI: crown visibility only without GI (#30)".
- Trees: Megascans Trees from Unreal, leaves as opaque geometry traced from
  the atlas: "Tree leaves as opaque geometry, traced from the atlas",
  "Leaf transmission per KHR_materials_diffuse_transmission".
- Far vegetation: voxel tiers ("Far-field forests: voxel tiers", "Mid tier:
  rasterized prototype voxels"); the DAG voxel design (branch `g1`)
  replaces them once it merges.
- Placement: "Geo channels and causal placement".
- Clouds: volumetric, outline off: "Cloud outline off by default" (look and
  march entries under "M4 clouds").
- Haze: "Haze as a boundary layer over the valley floor".
- Camera: "Camera field of view: 70 degrees horizontal, configurable".
- Judging: "Benchmark route and ledger", "Reference path tracer".
- Performance gate: "Performance gate restated (2026-11-24)".
- Geometry shading: "Visibility buffer is unconditional (2026-11-24)".
- Memory: "VRAM budget (2026-11-24)".

Tuned under older conditions (re-check when the area is next touched):

- The five TAA entries ("TAA: sharpen in proportion to motion", "TAA: a
  longer history when still", "TAA: sharper history resampling, shorter
  blend in motion", "TAA stability over sharpness", the TAA parts of
  "Motion clarity: DLSS 4.5, Ray Reconstruction, TAA v2, grading") and
  "Cloud temporal stability under TAA": tuned for the own TAA, now the
  fallback.
- "Micro and contact shadows", "Plant micro occlusion", "Visible quality
  pass", "Starting grade: neutral saturation, stronger greens": tuned
  before GI probes and raster shadows were defaults.
- "Frame cost measured, perf lane": the scene-early split predates opaque
  leaves, the Megascans beech and the causal forest.
- "Global illumination: terrain-following probes + screen-space bitmask":
  the bitmask half was never built.

## Foundations (2026-11-18/19)

- **Vulkan 1.3 from day one** (dynamic rendering, synchronization2, buffer
  device address), not OpenGL or DX12. DX12 isn't available on Linux;
  OpenGL has no hardware ray tracing and more driver overhead.
  vk-bootstrap handles init and VMA handles memory.
- **Reverse-Z, infinite far plane, D32** for even depth precision across
  open-world distances.
- **Vertex pulling through buffer device address**: no vertex input state.
  It's bindless-friendly and the ray tracing inputs share the same buffers.
- **2 frames in flight**, precise sync2 barriers (no ALL_COMMANDS).

## HDR and exposure

- **Physical units + pre-exposure**: the sun in lux, luminance in cd/m^2,
  and EV100 exposure (Lagarde & de Rousiers 2014, "Moving Frostbite to PBR",
  K = 12.5, 1.2 saturation factor). Exposure is applied while shading so
  FP16 doesn't overflow (as Unreal/Frostbite do).
- **AgX tonemapper**, ported from Filament `src/ToneMapper.cpp` (Rec.2020
  working space, 7th-order contrast polynomial). Khronos recommends filmic
  curves for strongly HDR scenes; PBR Neutral is for product shots. A
  remembered polynomial turned out outdated, so it was verified against the
  source.
  - https://www.khronos.org/news/press/khronos-pbr-neutral-tone-mapper-released-for-true-to-life-color-rendering-of-3d-products
- **Output dither** (2026-11-20): the sky showed concentric bands. Measured
  along sky lines, every step was exactly 1 LSB of the 8-bit output, in
  plateaus 10-150 px wide, so the FP32 sky-view LUT and FP16 HDR target
  were not the cause. The tonemap pass adds triangular-PDF noise of +-1
  LSB in sRGB-encoded space (the swapchain is `B8G8R8A8_SRGB`, so it
  encodes, dithers and decodes, and the hardware re-encodes), the
  approach of Gjøl's "Banding in Games" (Playdead, GDC 2016); the noise is
  interleaved gradient noise (Jimenez 2014) remapped to a triangular PDF,
  whose error variance doesn't depend on the signal. Static per pixel:
  the dither runs after TAA, so nothing accumulates it, and at +-1 LSB a
  per-frame shift buys nothing visible. Result: averaged sky profiles step
  <= 0.5 LSB, mean image change ~1e-4/255, tonemap 0.02 -> 0.04 ms at
  2560x1440. A 10-bit or HDR swapchain would need the same dither at its
  own LSB.
  - https://loopit.dk/banding_in_games.pdf
  - https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
- **Auto exposure**: log-luminance histogram (256 bins, shared-memory
  atomics, quarter-resolution sampling: +0.1 ms instead of +0.34 ms at full
  resolution), averaging the 10th-90th percentile, exponential adaptation.
  The result stays on the GPU and the next frame uses it as pre-exposure.
  - https://bruop.github.io/exposure
  - https://www.unrealengine.com/en-US/tech-blog/how-epic-games-is-handling-auto-exposure-in-4-25

## Shadows

*Superseded in part: the ray-traced sun shadows chosen below gave way to "Raster sun shadows: cached cascades", then "Raster sun shadows are the default" (`--shadows rt` stays an A/B). Terrain march, cloud shadow and contact shadows stand.*

- **Hardware ray-traced sun shadows** (ray queries in the fragment shader),
  not cascaded shadow maps: no cascade seams or shimmer, per-pixel accuracy,
  and published comparisons show equal or lower cost. RT is required. The
  TLAS is built once for the static world and again only when instances
  change (see "Static TLAS" below).
  - https://github.com/ZihanWG/VulkanEngine/pull/69
  - https://www.gamedeveloper.com/programming/an-optimal-implementation-of-fast-ray-traced-soft-shadows-in-a-game-engine
- **Soft shadows**: one ray per pixel toward a point on the sun disk
  (angular radius 0.00465 rad), chosen with interleaved gradient noise
  (Jimenez 2014) and accumulated by TAA.
  - https://link.springer.com/content/pdf/10.1007/978-1-4842-7185-8_24.pdf
- **Alpha-masked shadows**: non-opaque BLAS geometry, and the ray query
  alpha-tests each candidate through a per-instance info table. Future:
  opacity micromaps for dense foliage (cut 7.9 to 3.6 ms in Indiana Jones).
  - https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass

## Anti-aliasing and upscaling

- **TAA** now, **DLSS later** in the same slot. Halton(2,3) jitter (phase
  count from the render/display ratio, see motion clarity), motion
  vectors as previous - current in UV, 9-tap Catmull-Rom
  history sampling (MJP), variance clipping in YCoCg toward the box center
  (Salvi; Playdead), and luminance-weighted blend (Karis).
  - http://behindthepixels.io/assets/files/TemporalAA.pdf
  - https://github.com/playcanvas/engine/blob/main/src/extras/effects/taa/glsl/sample-catmull-rom-ps.js
- **DLSS on Linux**: Super Resolution and Ray Reconstruction through the
  NGX SDK directly (Streamline is Windows-only). Frame Generation is
  experimental (`libnvidia-ngx-dlssg.so`). The user wants clean inputs for
  DLSS NR / Ray Reconstruction (albedo, normals, roughness guides).
  - https://github.com/unfocused-mods/vspt-issues/issues/30

## Sky and atmosphere

- **Hillaire 2020** ("A Scalable and Production Ready Sky and Atmosphere
  Rendering Technique"), ported from the MIT reference
  `sebh/UnrealEngineSkyAtmosphere` with Y-up frames, RGBA32F LUTs (the
  reference reports artifacts with 16-bit), and Earth defaults from
  Bruneton 2017. The sky has rotation-only motion vectors.
  - https://github.com/sebh/UnrealEngineSkyAtmosphere
- **Summer haze (2026-11-19)**: the game sets a summer Mie aerosol for
  the Swiss Plateau / Napf (`summerHaze(aod, H)` in main.cpp). Presets:
  `--haze summer` (AOD(550) 0.10, H 0.5 km; replaced 2026-11-21 by the boundary layer, last entry "Haze as a boundary layer"), `--haze
  summer-hazy` (AOD 0.2, H 1.5 km), `--haze clean` (Bruneton's very clean
  defaults, ~0.004/km, visibility ~300 km). The engine defaults stay
  Earth-clean and parametric. The derivation below is for summer-hazy;
  summer follows it with its own numbers.
  - Boundary layer: the convective boundary layer over the Plateau
    (Payerne, 491 m) peaks at 1200-1500 m above ground in May-August
    (Collaud Coen 2014), so its top is ~2 km above sea level; Swiss sun
    photometers measure an AOD scale height of 1-2 km and monthly mean
    AOD(500 nm) from 0.05 in winter to 0.3 in summer (Ingold 2001).
    Scale height 1.5 km.
  - Extinction: take a boundary-layer AOD(550) of 0.2 (summer means ~0.26
    after the Angstrom shift to 550 nm include free-troposphere and dust
    events). With an exponential profile, AOD = beta(ground) * H, so
    beta = 0.2 / 1.5 = 0.133/km at the Plateau floor (0.5 km). Koschmieder
    (V = 3.912 / beta_total, 2% contrast threshold; Rayleigh adds
    0.0116/km at 550 nm): ~27 km visibility on the Plateau, and
    0.133 * exp(-0.85 / 1.5) = 0.075/km, ~45 km, on the Napf ridges
    (1.35 km), which is OPAC's "continental average" (0.075/km, 35 km at
    80% RH). Hazy but clear: ridges 5-15 km away at that level keep exp(-0.087 d), 65-27%, of their contrast.
  - Altitude reference: the atmosphere's density is exp(-y / H) with y
    above sea level (atmospherePosition), so the coefficient passed is the
    sea-level value 0.133 * exp(0.5 / 1.5) = 0.186/km. Density is highest
    in the valleys and falls through the camera's altitude; above ~3 km the
    view looks down onto the haze layer.
  - Optics (OPAC continental average, 550 nm): single-scattering albedo
    0.925, asymmetry g 0.703 (g = 0.7), Angstrom exponent 1.42 for
    500-800 nm and 1.11 for 350-500 nm, applied at the 680/550/440 nm the
    RGB coefficients stand for: extinction x {0.740, 1, 1.281}. The haze
    is bluer than gray, as aerosol of mostly sub-micron particles is.
  - Effect: noon sky is paler (top-of-frame blue/red 1.51 vs 2.33 clean)
    and the region around the sun brighter (g 0.7). That read as too
    grey: on a real hazy summer day the zenith stays clearly blue and the
    haze sits low. A first milder try (AOD 0.15, H 1.2 km) still greyed
    the sky (noon blue/red 1.63).
  - summer (default): AOD(550) 0.10, inside Ingold's summer range, with
    H 0.5 km. What greys the zenith is the aerosol above the camera, and
    an exponential profile cannot have the real layer's well-mixed body
    and sharp top, so the scale height is a deliberate, perceptual
    deviation from the ~2 km CBL: it keeps the haze in the valleys the
    view looks across. beta = 0.2/km at 0.5 km, 0.13/km at the Napf's
    valley floors (0.7 km, V ~27 km), 0.037/km on the ridges (V ~80 km);
    sea-level coefficient 0.2 * exp(0.5 / 0.5) = 0.544/km. The aerosol
    column above the summit is 0.017 (summer-hazy 0.11).
  - Measured (2560x1440 captures, summit at 1.41 km): zenith blue/red
    1.98 (clean 2.11, hazy 1.58) with the default sun, 1.59 with a
    74-degree sun 16 degrees off the zenith (clean 1.91, hazy 1.24). Ridges
    5-15 km away (rows 745-810 of the far-horizon view): Laplacian std
    0.063 (clean 0.141, hazy 0.046), so their fine contrast is cut by
    55%. Tried and rejected: H 1.0 (zenith 1.81), H 0.7 (1.89), AOD
    0.08 with H 0.7 (1.93, less ridge fade).
  - Hess, Koepke, Schult 1998, "Optical Properties of Aerosols and Clouds:
    The Software Package OPAC", BAMS 79(5), Table 3:
    https://lweb.cfa.harvard.edu/hitran/HITRAN2012/Aerosols/papers/hess_opac_1998.pdf
  - Collaud Coen et al. 2014, "Determination and climatology of the
    planetary boundary layer height above the Swiss plateau", ACP 14:
    https://acp.copernicus.org/articles/14/13205/2014/
  - Ingold, Matzler, Kampfer, Heimo 2001, "Aerosol optical depth
    measurements by means of a Sun photometer network in Switzerland",
    JGR 106, doi:10.1029/2000JD000088
  - Koschmieder 1924 visibility relation (V = -ln(0.02) / beta):
    https://en.wikipedia.org/wiki/Visibility#Meteorological_visibility

## Materials and assets

- **Filament's BRDF**: GGX, height-correlated Smith, Schlick, Lambert, with
  multiple-scattering energy compensation from a DFG LUT (Filament
  `DFV_Multiscatter`). Specular ambient comes from the sky-view LUT with
  the sky capture's clouds over it until RT reflections exist.
- **Material AO on ambient light (2026-11-20)**, on every surface (glTF
  materials and terrain layers):
  - **Diffuse:** AO goes through the multi-bounce fit of Jimenez et al.
    2016. Material AO is single-bounce visibility; light that bounces
    inside the cavity brightens it, more so for bright albedo.
  - **Specular:** the sky reflection uses Lagarde's specular occlusion,
    derived from AO, NoV and roughness, then the same multi-bounce fit with
    f0. This is the pairing Filament uses. It is chosen over plain AO
    because a narrow lobe seen head-on escapes most cavities, and over a
    cone/bent-normal method because there are no bent normals.
  - **Scanned layers' AO** (Poly Haven, red channel of ARM, mean / p5 /
    p95): forest_ground_04 0.86 / 0.69 / 0.98, rock_face_03 0.92 / 0.78 /
    0.98, forest_leaves_04 0.60 / 0.29 / 0.89. The first two are plausible
    cavity AO. The leaf litter is heavy, and it correlates with its albedo
    (r = 0.55): the gaps are already dark in the base color, so that layer
    counts its occlusion twice.
  - **Measured** (pre-tonemap luminance, beech shadow umbra vs sunlit
    needle ground, `--placement off --scatter 100 --camera 1115.47 1375
    -429.72 1.223 -0.5`, default sun, summer haze): the effect is small at
    these albedos (0.12-0.16; the fit turns AO 0.6 into 0.63), 9.0% ->
    9.2% of the sunlit luminance.
  - **Per-layer strength:** `TerrainLayer::occlusionStrength` scales a
    layer's AO map as glTF's occlusion strength does for materials
    (`mix(1, ao, strength)`). The game sets the needle layer to 0.5 and
    keeps the others at 1. Its AO repeats the darkness already in its base
    color (r = 0.55; soil 0.02), so at full strength each gap is darkened
    twice. Halving it removes roughly the duplicated part and keeps the
    cavities' directional effect on the sky light. Same view: 10.8%
    (strength 0 would give 12.3%); at eye height (`--camera 1115.47 1356.6
    -429.72 1.223 -0.3`) 9.5% -> 11.4% against plain AO at full strength.
    No GPU time change. Real shadows are 10-20%; the rest of the gap is the
    haze's height profile and missing bounce light (backlog).
  - Jimenez, Wu, Pesce, Jarabo, "Practical Real-Time Strategies for
    Accurate Indirect Occlusion", SIGGRAPH 2016 course:
    https://research.activision.com/publications/archives/practical-real-time-strategies-for-accurate-indirect-occlusion
  - Lagarde, de Rousiers, "Moving Frostbite to Physically Based Rendering
    3.0", SIGGRAPH 2014 course (specular occlusion):
    https://seblagarde.wordpress.com/wp-content/uploads/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf
  - Filament, "Ambient occlusion" (multi-bounce and specular occlusion):
    https://google.github.io/filament/Filament.md.html
- **Bindless textures** (descriptor indexing, update-after-bind, partially
  bound, 16384 slots) and a material table read by buffer address.
- **KTX2 + Basis Universal UASTC** (`KHR_texture_basisu`), transcoded to BC7
  at load (UASTC is a BC7 subset, so nearly lossless). Basis Universal is
  used instead of KTX-Software because the latter's repo needs git-lfs and
  Basis alone covers both encoding and transcoding.
  - https://docs.vulkan.org/samples/latest/samples/performance/texture_compression_basisu/README.html
- **glTF via fastgltf** (2-20x faster than alternatives) with MikkTSpace
  tangents when missing. `KHR_texture_transform` (required by Poly Haven
  models that tile bark) is baked into the texture coordinates at load,
  before tangent generation; a material whose textures disagree on the
  transform is rejected rather than given a per-material GPU transform no
  asset needs yet. The rotation sign follows the extension's worked
  example and three.js; the matrix in its sample shader rotates the other
  way (a unit test pins the example).
  - https://fastgltf.readthedocs.io/latest/overview.html
  - https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_transform
- **meridian-cook** converts PNG/JPEG to KTX2 UASTC (slower pack level,
  zstd, mips; sRGB for color, renormalized normal maps) and writes a
  standard glTF. It cooks blended or masked materials opaque when their
  alpha is provably 1 (base color factor alpha 1, no base color image
  pixel below 255, no RGBA vertex colors): Poly Haven marks modeled leaves `BLEND` and plants
  `MASK` with JPEG base colors, which cost a blended, unculled
  full-detail draw or a no-op alpha test and kept leaves out of shadows.
- **Transparency**: separate opaque/masked/blended pipelines (discard would
  disable early-Z for opaque draws), premultiplied blending so glass keeps
  full specular, no motion writes for blended surfaces, and back-to-front
  sorting by object origin.
- **Terrain material**: three scanned layers (forest soil and pine-needle
  litter on flat ground, mixed by 40 m value noise; rock cliff on slopes)
  with snow kept procedural. Flat layers use a top-down world projection;
  rock uses triplanar with the Whiteout normal blend and back-side u flip
  (Golus), checked numerically for our right-handed axes. Layer borders
  use height blending (Mishkinis) so the taller texel wins instead of a
  soft cross-fade. Every sample is `textureGrad` with world-position
  derivatives, so per-pixel layer skipping keeps correct mips and 16x
  anisotropy. Mip bias stays 0: a negative bias shimmers under native TAA
  and only pays off with an upscaler (DLSS: log2(render/display) - 1).
  - https://bgolus.medium.com/normal-mapping-for-a-triplanar-shader-10bf39dca05a
  - https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-upscalers-in-unreal-engine
- **Hex-tiling** (Mikkelsen 2022) on the flat terrain layers against visible
  repetition: three randomly rotated/offset copies on a hex grid,
  luminance-weighted (fall-off contrast 0.6, exponent 7) so contrast
  survives; normals blended as rotated height derivatives. Deviations from
  the reference: 2 hex cells per texture repeat instead of sqrt(12) (the
  reference grid ghosts twigs and leaves in its wide blend zones; 1 cell
  shows seams), one weight set from base color for every map so layers
  stay coherent, and copies under 2% weight skip their other maps (same
  image, less cost). Heitz-Neyret histogram-preserving tiling was the
  alternative; it needs a per-texture LUT for little gain here.
  Scanned normal maps also carry bumps at tile scale that repeat as a grid
  at distance, so terrain normals fade out once a pixel covers 2-20% of a
  tile. Rock stays plain triplanar (hex x triplanar is 3x the taps) and
  still repeats when seen from far above; revisit with the large terrain.
  Cost: +0.45 ms (wide view) to +0.66 ms (ground filling the screen) at
  1269x1388 on an RTX 4080, median of 5.
  - https://jcgt.org/published/0011/03/05/paper.pdf
  - https://github.com/mmikk/hextile-demo
- **Test content from Poly Haven** (CC0, public API, no login), fetched by
  script so `assets/` stays reproducible. Megascans (free on Fab for any
  engine since 2026-11) needs a browser login, so it is a manual option
  for later. Only game-weight assets for now: rocks, litter, small plants
  and saplings (2k-160k triangles each). Poly Haven's full-size trees are
  film assets (0.5-7M triangles per tree, no LODs) and wait for
  virtualized geometry.
  - https://api.polyhaven.com
  - https://cgchannel.com/2026/11/epic-has-made-megascans-free-again-for-good-this-time

## GPU-driven rendering (phase 1, 2026-11-19)

- **Persistent GPU scene and shared geometry pools**, culled by a
  **compute pass that appends draw commands, drawn with
  vkCmdDrawIndexedIndirectCount**, the standard 2025-26 GPU-driven
  pipeline. Instances persist (`addInstance`); the CPU does no per-object
  work besides TLAS instance writes and sorting blended instances.
  - https://deepwiki.com/vblanco20-1/vulkan-guide/9-extra-chapter:-gpu-driven-rendering
  - https://daslang.io/doc/reference/tutorials/vulkan/12_gpu_driven.html
- Draw lists split by pipeline and cull mode (opaque/masked x single/double
  sided), so one indirect call covers each. Blended stays CPU-sorted.
- Next, in order: phase 3, virtualized geometry (meshlets with a cluster
  LOD hierarchy, Nanite-style, rather than discrete LODs, so film-grade
  assets such as multi-million-triangle trees become usable; RT needs
  cluster acceleration structures or proxy meshes); then a 15 km world
  (chunked terrain with LOD and streaming, large-scale scatter, GPU
  grass). Benchmarks for task/mesh vs indirect: williscool.com.
  - https://www.williscool.com/technical/task-mesh-benchmarking/task-mesh-benchmarking.md.html

## GPU-driven rendering (phase 2, 2026-11-19)

- **Two-phase Hi-Z occlusion culling** (niagara, Nanite): phase 0 draws
  last frame's visible instances (frustum only), a min-reduced depth
  pyramid is built from that depth, phase 1 tests every instance against
  it, draws the newly visible and records visibility. Conservative, so no
  popping on disocclusion; the single-pass reprojection alternative pops
  for a frame. Shadows are unaffected (RT through the TLAS).
- Pyramid: R32F, screen rounded down to powers of two; level 0 takes the
  min over every pixel a texel touches (rounding makes a texel span 1-2
  unaligned pixels, touching up to 3), higher levels one fetch through a `VK_SAMPLER_REDUCTION_MODE_MIN`
  sampler (reverse-Z: farthest = min). The sampler needs maxLod set, or
  every lookup silently falls back to level 0 (found as a false cull).
- Test: sphere -> screen bounds (Mara & McGuire 2013, zeux's form), level
  = ceil(log2(span in texels)) so the bounds cover at most one texel and
  the 2x2 fetch at their center covers them (floor, as some references
  use, can miss an edge texel), with a one-texel margin for the TAA jitter
  in the pyramid's depth, nearest depth near/(distance - radius)
  against the farthest occluder. Spheres crossing the near plane pass.
- Measured (`--scatter 3000`, 1269x1388, RTX 4080): start view behind a
  rise 1866 -> 92 drawn instances, 5.97 -> 1.44 ms; open view 1956 ->
  1151, 7.66 -> 4.64 ms (median of 3); turned view 1868 -> 327. Images match with culling off (no pixel
  differs by more than 15%), including while moving.
  - https://github.com/zeux/niagara
  - https://zeux.io/2023/01/12/approximate-projected-bounds/
  - https://vkguide.dev/docs/gpudriven/compute_culling/

## Virtualized geometry (GPU-driven phase 3, 2026-11-19)

Design: `docs/superpowers/specs/2026-11-19-virtualized-geometry-design.md`
(five slices; differentiators over Nanite: RT that matches the raster,
foliage that survives simplification).

- **Slice 1, cluster LOD raster.** meshoptimizer 1.0 `clusterlod.h`
  builds a Nanite-style DAG per primitive (128-triangle clusters,
  permissive simplification weighted by normals, UV seams protected),
  cached as `<name>.clusters` beside the glTF (keyed by a hash of the
  loaded geometry and versioned with the meshoptimizer pin; one code path
  instead of a cooker step). The cache is only an accelerator: a damaged
  one is validated away and rebuilt, an unwritable one only warns. Writing our own
  builder bought nothing for slice 1; slice 3 changes its simplifier for
  foliage.
  - https://github.com/zeux/meshoptimizer/blob/v1.0/demo/clusterlod.h
  - https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/
- **LOD cut** (Karis 2021): a cluster is drawn when its group's coarser
  replacement projects over the threshold (1 pixel) and the group it came
  from projects at or under it; clusterlod's monotonic bounds make the cut
  crack-free. Projected error ignores perspective distortion (clusterlod's
  estimate) and scales with the instance's largest axis scale.
- **Raster: `VK_EXT_mesh_shader`**, one workgroup (32 threads, up to 128
  vertices/triangles) per culled cluster, `vkCmdDrawMeshTasksIndirectEXT`
  per draw list with the cull writing the group count. Cross-vendor
  (NVIDIA Turing+, AMD RDNA2+, Intel Arc); no index expansion, unlike a
  compute-expanded index buffer. Culling ends in a cluster pass (indirect
  dispatch, one workgroup per work item of up to 64 clusters, which the
  cluster-group BVH traversal's leaves emit) applying the LOD cut and
  frustum test. Full work-item or draw lists drop the
  excess, give the count back so indirect arguments stay at capacity
  (device creation checks the dispatch limits cover it), and report the
  drops.
- **RT mismatch, interim:** shadows still trace full-detail BLAS, and the
  coarser raster surface can lie under it (acne blotches over distant
  terrain at 1 px). Shadow rays start the cluster's own simplification
  error above the surface; that removed them (overview mean difference
  vs the pre-cluster renderer 5.9/255 -> 0.29/255, the rest is LOD
  shading). Slice 2 traces the clusters themselves.
- **Cluster occlusion, Nanite's two passes:** the early cluster cull tests
  against last frame's depth pyramid (seen from last frame's view; the
  pyramid is from last frame's early pass, so it has fewer occluders and
  stays conservative) and queues what it hides; the late cull retests
  those against this frame's pyramid. No per-cluster visibility state
  (niagara keeps per-meshlet bits, which grow with instances x clusters).
  Valid while instances don't move; the first frame after a resize skips
  the early test. Measured (`--scatter 3000`, 1920x1080): clusters drawn
  at the start view 11534 -> 3367 with occlusion, open view 9694 -> 6382;
  images match occlusion off (mean 0.04/255 still, 0.09/255 moving at
  5 m/s, below the 0.12/255 run-to-run noise).
- **Measured (slice 1, Release, 2560x1440, `--scatter 3000`, GPU ms,
  median of 5)**, vs the renderer before slice 1 (51faf7f) on the same
  15-model scene: start 3.46 -> 1.82, open 6.12 -> 1.58, overview
  2.43 -> 1.46, scatter close-up (`7.2 83 706 0 0.15`) 7.48 -> 1.87.
  Adding Poly Haven's island_tree_01 (1.6M triangles, ~190 instances):
  1.54-2.12 ms in every view; the close-up draws 1.57M triangles at 1 px
  vs 78.5M (12.70 ms) at `--lod-error 0`.

## Large worlds (2026-11-19)

- **Camera-relative rendering** for the 15 km world (7.5 km from the
  origin). Shaders subtract the camera position from world positions
  before any matrix multiply, and every view and view-projection matrix
  leaves out the camera translation. Two nearby floats subtract exactly
  (Sterbenz lemma), so the only error left is each vertex's own
  world-space rounding (~0.5 mm at 7.5 km), which is static and doesn't
  jitter as the camera moves. Folding the translation into the matrix
  instead rounds the large terms inside the multiply, which does jitter.
  Last frame's clip position uses last frame's camera position with its
  own translation-free matrix. The instance format is unchanged.
  Rebuilding `Camera::view()` as `viewRotation() * translation(-position)`
  also fixed a CPU precision loss: `lookAt(position, position +
  forward())` rounded away much of the direction's precision far out
  (9 of 16 elements off at 7.4 km).
- Verified against the previous build: mean difference 0.50/255 at the
  default view, 0.12 high, 0.10 scatter at 5 m/s. The `view()` fix alone
  moves the default view 0.55/255: any float-math change re-rolls the
  ground texture's sub-texel noise. (The integer hashes under "Terrain
  texture coordinates" later replaced those sin hashes.) Brightness, draw counts, validation
  and motion sharpness are unchanged.
- Rejected: tile + offset coordinates (Unreal's large world coordinates).
  They buy precision far beyond 15 km that this world doesn't need.
  - https://godotengine.org/article/emulating-double-precision-gpu-render-large-worlds

## Terrain data (2026-11-19)

- **A real DEM instead of procedural noise plus erosion.** Real terrain
  has the large-scale structure (ridge networks, drainage, valley
  profiles) that noise needs heavy erosion simulation to approximate, and
  it is free. swissALTI3D at 2 m (swisstopo open government data, free
  use with the attribution "© swisstopo") is the highest-resolution
  national DEM freely downloadable without an account.
- **Region: Napf (Emmental/Entlebuch), E 2631..2645, N 1198..1212 (LV95
  km), 15 x 15 tiles = 15 x 15 km.** Steep wooded ridges and meadows
  without glaciers or bare high alpine rock; relief 625..1572 m, so
  16-bit steps stay ~1.4 cm. The summit tile (E 2638, N 1205, 1406.7 m)
  is the crop center. The 1572 m maximum is the south-east corner tile
  (E 2645, N 1198), real terrain, not no-data.
- **Source:** the swisstopo STAC collection `ch.swisstopo.swissalti3d`,
  tiles at `data.geo.admin.ch/ch.swisstopo.swissalti3d/swissalti3d_<year>_<E>-<N>/
  ..._2_2056_5728.tif` (2 m, LV95, LN02 heights, float32 GeoTIFF with
  overview pages). The newest year holding each tile is taken; page 0 is
  the full resolution.
- **Format:** `.height` = 28-byte header (MHF1, version, width, height,
  spacing, minHeight, heightScale) + uint16 samples. 16 bits over the
  tile's own min..max beats float32 on size (113 MB vs 225 MB) with
  sub-2-cm error. Outside the grid, indices mirror like Vulkan
  MIRRORED_REPEAT on CPU and GPU alike.

## Terrain rendering: CDLOD (2026-11-19)

- **CDLOD with GPU quadtree selection** (Strugar 2009) for the Napf
  heightfield, replacing the 2 km terrain mesh. Every node is a fixed
  32x32-quad grid and node size doubles per level. Vertices morph toward
  the next coarser grid near the end of their level's distance range, so
  neighboring levels meet without cracks or T-junctions, and LOD changes
  don't pop. Selection runs on the GPU, one indirect dispatch per
  quadtree level from the root down (as in Far Cry 5's GPU terrain, GDC
  2018). It frustum-tests each node's box, with heights from the min/max
  pyramid, and splits a node while the camera is within the next finer
  level's range. Draw: `terrain.mesh`, 16 workgroups of 8x8 quads per
  node, first in the early scene pass, so hills occlude objects through
  the depth pyramid.
  - Rejected: geometry clipmaps (no adaptivity, and ring updates don't
    fit a GPU-driven pipeline); terrain through the cluster LOD path (the
    DAG of ~112M triangles would cost minutes and GBs to build).
  - https://github.com/fstrugar/CDLOD
- **Numbers:** level 0 nodes are 16 m (0.5 m quads); the root is level 16
  (1049 km, see Curvature), centered on the origin. Level 0's LOD range is 80 m and
  ranges double per level. Morphing covers the last 30% of each level's
  span. The range is 5x the node size: at 64 m (4x) the no-crack margin
  was marginal (the L+1 neighbor could start morphing where level-L
  nodes still reach). The quadtree is shifted under one sample spacing
  so level 2's 2 m vertices land exactly on data samples; unshifted, the
  grid would sit between samples and blur the data. Levels 0-1 are finer
  than the data and add value-noise detail (wavelengths 4-0.5 m, at most
  0.3 m), fading out by the end of level 1's range, so level >= 2
  vertices are exactly the bilinear heightfield. Motion vectors use last
  frame's camera for the morph and detail weight too, so morphing doesn't
  smear under TAA.
- **Heights and normals:** an R16_UNORM image of the samples, read with
  `texelFetch` and the CPU's mirror rule (exactly `Heightfield::heightAt`).
  The per-pixel normal comes from an RG16_SNORM normal map (x and z; y is
  reconstructed): the full level is built on the GPU at load from central
  differences, the mips are blitted, and it is sampled trilinear and
  anisotropic through a mirrored-repeat sampler, with the slope's sign
  flipped in mirrored copies. Distant normals are filtered, not aliased.
  The detail noise's analytic slope is added near the camera. Reading
  the 16 samples per pixel from a buffer instead cost 0.82 ms. The
  min/max pyramid (levels >= 1, `min | max << 16`) stays a buffer for
  selection and the shadow march.
- **Measured** (Release, 2560x1440, RTX 4080, median of 5; procedural
  shading on both sides to leave out the material): terrain select
  0.07-0.08 ms. Select + scene early: 0.59 ms (open view) and 0.63 ms
  (full-ground view), against 0.69-0.75 ms for the old 2 km mesh's scene
  early. About 377-393 nodes (0.77-0.80M triangles) at eye height; 173
  nodes (0.35M) from 3000 m. GPU memory: height image 113 MB, normal map
  300 MB with mips, pyramid 75 MB.
- **Draw order: near to far.** Selection lists nodes coarse level first,
  which is far first; `terrain.mesh` reads the list backwards, so nearer
  terrain fills depth first and early-Z rejects the hills behind it
  before the (expensive) terrain fragment shader runs. Open view, Release,
  median of 5: scene early 2.37 -> 1.08 ms, frame 2.95 -> 1.76 ms, image
  identical. The ~1.8 ms first measured as the material's cost on the
  Napf was mostly this overdraw.
- **Known cost (content, not this technique):** the terrain material is
  mostly triplanar rock on the steep slopes (12 anisotropic fetches per
  pixel); with near-to-far drawing, the open view's scene early is
  1.08 ms with the material.
- Coarse LOD triangles don't gate the sun on terrain: their facing
  differs from the per-pixel normal on steep slopes and cut the light
  per triangle.

## Terrain shadows: heightfield march (2026-11-19)

- **The sun ray marches the terrain's max pyramid** (Tevs et al. 2008,
  maximum mipmaps; Lumen traces heightfields the same way), next to the
  TLAS query, on every surface. Rejected: terrain BLAS + partitioned TLAS
  (a 15 km heightfield at 2 m is ~112M triangles to build and keep
  resident, and the LOD raster would still disagree with it). The march
  runs in the flat world, like the TLAS query.
  - Tevs, Ihrke, Seidel, "Maximum mipmaps for fast, accurate, and
    scalable dynamic height field rendering", I3D 2008:
    https://doi.org/10.1145/1342250.1342279
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
- **March:** in sample units, starting at level 0; a cell whose max lies
  below the ray over the cell is skipped to its exit and the march goes
  one level coarser, otherwise one level finer. Level 0 is exact: surface
  minus ray height along the segment is a quadratic, checked at both ends
  and its peak, from one `textureGather` of the 2x2 samples. Hard cap of
  256 steps (counts as unoccluded); the ray also ends above the highest
  terrain (the pyramid top) or at 20 km. Mirroring: inside one period tile
  ([-0.5, w - 0.5) samples) the mirrored field is the crop with clamped
  indices, in coordinates reversed on odd tiles; steps stop at tile edges.
- **Pyramid level 0 bounds bilinear cells** (samples x..x+1, z..z+1), so
  a level-k texel bounds the whole bilinear surface over its cells and a
  step needs one read. Culling bounds only get slightly looser.
- **Ray origin on terrain:** the bilinear height at the fragment (or the
  raster, if higher) plus 5 cm. Offsetting by the per-vertex LOD error
  left acne on distant lit hills (coarse triangles sit further below the
  heightfield than the single-sample estimate), so that varying is gone.
  On objects the march starts at the same rule's height (bilinear height
  plus 5 cm, or the offset fragment if higher); the TLAS query keeps the
  normal-offset origin. An object's base can sit below the bilinear
  field (detail displacement, coarse ridges), where the march hit at once
  and left a sunless band (the helmet plinth's lower edge in the ground
  view, high sun: mean luminance there 0.077 -> 0.101; whole-image mean
  difference 0.65/65535, `--scatter 16` from above 2e-8).
- **Terminator lift (2026-11-20):** the terrain is shaded with the smooth
  normal-map normal (central differences over two cells), but the march
  traces the bilinear cells. Near the terminator (slopes whose angle away
  from the sun nears its elevation) a cell can face away from the sun
  while the smooth normal still faces it, so the cell shadowed itself in
  jagged, cell-sized bands along the contours. The march now starts
  above the plane of the shading normal by the fragment's cell's highest
  corner over that plane (the cell minus a plane is bilinear, so its
  corners bound it): a ray the shading normal lets through clears its own
  cell. The shifted-origin idea of Chiang et al. for smooth-normal
  triangles; the TLAS query keeps the unlifted origin, so object contact
  shadows don't leak. Start-ridge view from 150 m up (`1119 1500 -431
  1.57 -1.5 --scatter 300`): terrain-shadowed pixels with NoL > 0 whose
  own cell faces away went from 36162 to 23227 (eye height on the ridge,
  10520 -> 4402); what remains is relief in neighboring cells. The march
  still matches the brute-force reference (0.0034/255 open, 0.032/255
  sunset); scene early cost within noise (open view median 1.23 vs 1.24
  ms, full-ground 1.28 vs 1.28).
  - Chiang, Li, Burley, "Taming the Shadow Terminator", SIGGRAPH 2019
    Talks: https://doi.org/10.1145/3306307.3328172
- **Costs found on the way:** indexing `Terrain.pyramidOffsets` on a
  copied struct spilled it to local memory (one more dependent load per
  step, 0.3 ms); per-step tile math, 4 texel fetches and a start at
  level 3 cost another 0.25 ms. The march is latency-bound: most pixels
  take 5-16 steps. Cap 64 would lose long sunset shadows (3.2/255).
- **Measured** (Release, 2560x1440, RTX 4080, median of 5, scene early,
  march vs `terrainOccludes` returning false, near-to-far terrain): open
  view 1.25 vs 1.10 ms (0.15 ms); full-ground view 1.33 vs 1.21 ms;
  sunset view 2.46 vs 1.31 ms (lit rays climb slowly). A/B against the
  brute-force march (`MERIDIAN_TERRAIN_MARCH_BRUTE_FORCE`: bilinear height
  every 1 m for 2 km, then 8 m): mean difference 0.017/255 (open),
  0.027/255 (ground at sunset), 0.032/255 (sunset), 0.039/255 (across the
  crop edge, grazing sun).

## Curvature: display warp (2026-11-19)

- **Rasterized positions bend; the world stays flat.** Every vertex
  (terrain, clusters, blended) drops by d^2 / 2R below the camera (d =
  horizontal distance, R = Atmosphere bottomRadius), so far terrain sinks
  under the horizon and meets the atmosphere's spherical ground, and
  objects stay on the ground they stand on at any distance. The common
  "fake curvature" vertex shader trick, applied to everything drawn.
  Lighting, the TLAS query and the heightfield march keep running in the
  flat world, where the BLAS/TLAS and the heightfield live: varyings carry
  the flat camera-relative position (so normals from its derivatives and
  shadow ray origins are unwarped); aerial perspective and the
  atmosphere lookups at the surface (sun transmittance, sky luminance)
  use the displayed one, since the flat point lies d^2 / 2R too high in
  the spherical atmosphere (+196 m at 50 km). That fix is subtle: summit
  and sunset views change by under 0.04/255 mean in any band, under 30
  pixels by more than 4/255 (the displayed point's up also tilts by d/R,
  so near terrain shifts by noise-level amounts too). Rejected: a real spherical world (every system, the TLAS
  and gameplay would need it, for a 15 km map where the drop is 4.4 m at
  7.5 km).
  - https://frozenfractal.com/blog/2025/12/12/around-the-world-28-scaling-up/
  - https://leadwerks.com/community/blogs/entry/2595-terrain-deformation
- **Motion vectors:** the previous clip position warps with the previous
  camera, so the warp moving with the camera reprojects correctly (high
  overview at 50 m/s: Laplacian ratio motion/still 0.857, Task 5 0.858).
- **Culling** tests displayed bounds. Spheres drop by the drop at their
  nearest horizontal distance and grow by the difference to the drop at
  their farthest (conservative: every point drops between the two);
  occlusion against last frame's pyramid uses last frame's camera for the
  warp. Terrain boxes: min lowered by the drop at the farthest corner, max
  by the drop at the nearest point. LOD selection (CDLOD ranges, cluster
  error) stays on flat distances, matching the geomorph.
- **Measured** (Debug, 2560x1440): from the summit (-359, 1408.7, -465)
  the skyline drops ~5-6 px (median row 721 -> 726/727 at pitch 0, the
  true horizon is ~23 px below center, hidden by terrain), no step or gap.
  Near-field mean difference vs flat 0.07/255 (ground) and 0.08/255
  (`--scatter 3000`), mostly the exposure shift from the changed horizon.
- **Quadtree root raised from level 14 (262 km) to 16 (1049 km).** The
  262 km root ended ~131 km out, which sank inside the true horizon
  (sqrt(2Rh): 235 km at 4.35 km altitude) and showed as an edge against
  the atmosphere's ground from ~5 km up. Level 16 reaches the horizon up
  to ~20 km altitude. Cost: ~60 more coarse nodes drawn (ground view 393
  -> 455 of 8192), selection +0.01 ms, scene pass unchanged within noise;
  the summit and full-ground views are bit-identical (the extra
  terrain is behind the horizon there). Level-0 indices now need 16 bits,
  so nodes are two uints ((level | x << 5), z); the root's sub-sample
  shift is added after the node offset, since at ~524 km float rounds it
  to 3 cm.

## Terrain texture coordinates: wrapped offsets (2026-11-19)

- **Relative position plus a wrapped camera offset.** World-projected
  terrain UVs built from `relative + cameraPosition` round to ~0.5 mm at
  7 km (about a texel of the 1.5 m needles tile), so textures step and
  jitter as the camera moves. Each layer instead gets the camera position
  minus the nearest multiple of its period, computed on the CPU in double
  each frame (`GpuTerrainLayer::offset`), and the shader uses
  `relative + offset`: UVs stay within about half a period plus the view
  distance anywhere on the map. The common "origin rebasing" of world UVs;
  everything the material derives from the position is periodic with the
  period, so a wrap changes nothing on screen.
- **Periods:** 256 tiles on every axis (soil 806 m, needles 384 m, rock
  691 m; triplanar rock wraps y too), except along z on the hex-tiled
  layers: the grid's rows are sqrt(3)/4 tiles apart, so no whole number of
  tiles is a whole number of rows, and they wrap z every 592 rows (256.3
  tiles). Each cell's rotation, offset and *center* come from its index
  wrapped to 512 columns x 592 rows (centered on the origin), so the
  texture copy a cell shows doesn't depend on the texture repeating with
  the period. The 40 m soil/needles noise wraps every 4000 m (100 cells;
  its 2.7x octave 270 cells) with its lattice wrapped the same way.
- **Integer hashes** (`terrainHash`, the detail noise's) of the wrapped
  integer cell index for the hex offsets and rotations and the mix noise,
  replacing `fract(sin(x) * 43758.5)` hashes and the reference's
  `mod(x * y, 2 pi)` rotation. Those amplify last-bit float differences:
  the same integer cells re-rolled when unrelated code changed how the
  compiler evaluated them, so the look depended on the build, compiler and
  GPU. The look re-rolled once with this change (same character, no
  visible tiling in the ground or overview views).
- **Not wrapped:** the detail displacement and its normals
  (`terrainDetail`), the heightfield normal map's UVs and the snow line
  still use the float world position: their ~0.5 mm rounding at 7.5 km is
  under 0.1% of their shortest wavelength (0.5 m detail octave, 2 m
  heightfield samples).
- **Measured** (Debug, 2560x1440): every offset moved by one whole period
  (temporary build) vs normal, same camera: mean difference 0.015-0.049/255
  over six views (overview, start, ground, origin, corner, a needles
  wrap boundary), unstructured float noise, no cell- or tile-shaped
  differences. Corner view (7000, 1288.2, 7000) at 5 m/s along x:
  Laplacian ratio motion/still 0.914, previously 0.904 (0.918 with the old
  hashes, so the content re-roll moves it too); the start's full-ground
  view 0.957 (0.966 before; different needles/soil layout).

## M1 wrap-up: walk mode and measurements (2026-11-19)

- **Walk mode:** `F` toggles walk/fly. Walking keeps the eye 1.7 m above
  `Heightfield::heightAt` (bilinear, mirrored outside the crop, so it also
  works past the edge), moves horizontally at 1.4 m/s (Shift: 5 m/s), and
  ignores Space/Ctrl. Fly mode is unchanged. There is no CLI flag:
  interactive only.
- **Measured** (Release, 2560x1440 fullscreen, RTX 4080, median of 5
  capture runs, 0 validation errors in all 25; ms per frame):

| View | Total | Top passes (scene early / next) |
|---|---|---|
| ground `1119 1355.9 -431 -1.55 -0.25` | 1.78 | 1.20 / pyramid 0.25, taa 0.14 |
| open `1119 1356 -431 -1.55 -0.02` | 1.72 | 1.13 / pyramid 0.25, taa 0.13 |
| overview `1119 4354 -431 -1.55 -0.35` | 2.99 | 2.64 / taa 0.12, terrain select 0.06 |
| summit `-359 1408.7 -465 1.57 0` | 1.91 | 1.54 / taa 0.11, terrain select 0.08 |
| corner `7000 1288.2 7000 -1.55 -0.25` | 2.61 | 2.02 / pyramid 0.26, taa 0.14 |

  The terrain raster and its shadow march sit in scene early; everything
  else is under 0.3 ms. The 3 degree sunset view (the costliest march,
  ~1.15 ms) was not re-measured here.

## Static TLAS and one instance id space (M2a, 2026-11-19)

- **One TLAS, built when the instance set changes** (the first frame, and
  after `addInstance`), not every frame: the world is static, and M2a's
  millions of placed instances make a per-frame rebuild unaffordable.
  Per-frame TLAS copies and scratch left `FrameData`; the CPU-written
  instance records stay per frame (the CPU may write them while the other
  frame's commands still read its own). Sync for the shared TLAS: before a
  build, a barrier waits for earlier fragment-shader ray queries (WAR) and
  earlier builds (scratch and TLAS WAW); after it, a build -> fragment
  shader AS read barrier, whose second scope covers every later frame in
  queue submission order. `upload + tlas` drops from ~0.14 ms to 0.00 ms
  on later frames with 3000 scattered models (Debug, 2560x1440); captures
  match the per-frame build within 0.03/255.
- **One instance id space**: culling, draw lists, visibility and the TLAS
  custom index share 24-bit ids (`kMaxInstanceId`, statically checked
  against the custom index field). Ids below `kPlacedBase` (=
  `kMaxInstances`) index `InstanceData`; the rest are reserved for placed
  instances. Every shader reads instances through `loadInstance`
  (common.glsl), so placed instances join without touching the readers.
  - https://developer.nvidia.com/blog/rtx-best-practices/
  - https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-pipeline-barriers
  - https://github.com/nvpro-samples/vk_lod_clusters/blob/main/docs/partitions_and_ptlas.md

## Tree import: Megascans Megaplants USD (M2a, 2026-11-19)

*Superseded by "Megascans Trees (card leaves) from Unreal replace the Megaplants", then "Tree leaves as opaque geometry, traced from the atlas" (2026-11-22), which holds.*

- **Two stages.** `tools/usd2gltf/usd2gltf.py` reads the USD with Pixar's
  `usd-core` (pinned 26.11 in `requirements.txt`, installed into a
  git-ignored venv by `setup.sh`) and writes an *unflattened* glTF: one
  mesh per mesh prim (one primitive per bound material, from the
  `materialBind` GeomSubsets), one node per place it is drawn, with
  transforms composed through the nested PointInstancers
  (`ComputeInstanceTransformsAtTime`, prototype-root transform included).
  It only converts: polygons are fanned, leftHanded winding is flipped,
  face-varying `st`/normals split vertices (V flipped for glTF), and
  materials keep the UsdPreviewSurface flat color and roughness. Leaf
  materials are found by Megascans' `TwoSided_Material` name (the meshes
  leave `doubleSided` unset). Skinning primvars are ignored (no wind).
  `meridian-cook --tree` (C++, meshoptimizer) does all geometry work.
- **Why `usd-core`:** Pixar's own reader as an official binary wheel (no
  build), the only reader certain to handle nested PointInstancers and
  every usdc crate version Fab ships; an offline import dependency only,
  outside the engine and the CMake build. Rejected: OpenUSD from source in
  `meridian-cook` (a large C++ build with TBB for an offline step);
  tinyusdz (single C++ library that would fit FetchContent, but its usdc
  and nested PointInstancer support is unverified for these files, and a
  half-imported tree is worse than a Python step); Blender as converter
  (not installed, heavy, its USD instancer import another unknown).
  Megaplants specifics the importer relies on: prototypes live under
  `class` prims (traversed without the abstract filter only inside
  prototypes), a prototype root's `invisible` hides only its source
  definition, Skeleton prims are `guide` purpose.
- **Budget.** One simplification ratio for every prototype (each gets a
  share of the budget weighted by its instance count), lowered until the
  flattened tree fits or meshoptimizer stops at its error limit:
  `meshopt_simplifyWithAttributes` with normals (weight 0.5) and an
  absolute 1 cm error in prototype space, for wood and for each leaf
  piece (position-connected component, simplified on its own after
  dropping its UVs). Still over budget, whole leaf pieces are pruned in a
  hashed random order and the survivors scaled about their centroids so
  the total leaf area stays the same (stochastic pruning with area
  compensation). The six vertices holding the tree's bounds are locked and
  their pieces kept unscaled; each scale is clamped to the bounds, so
  bounds stay exact. A 5 mm leaf error left beech needing 44% of its leaf
  pieces pruned (scale 1.35); 1 cm keeps 95% (scale 1.05) with the same
  silhouette (offline coverage of the source vs. the cook differs by 0.7%
  of pixels, side view).
- **Per-leaf tint.** The leaf material's base color is a 16x16 palette of
  the species color jittered by +-3% hue and +-8% brightness; every vertex
  of a leaf piece gets one hashed texel-center UV (constant UV, no bleed at
  mip 0). No vertex-format or shader change.
- **Bark.** ambientCG Bark012 (CC0, 2K): Color, NormalGL, and
  AmbientOcclusion + Roughness packed as glTF ORM, cooked to KTX2 through
  the existing encoder. The base color factor is the species color divided
  by the texture's linear mean, so the bark averages to the authored color
  at a distance.
- **Measured** (Debug cook, variant A; budget 4M for canopy trees, 1M for
  saplings, a few meters tall):

| Tree | Source tris | Cooked tris | Leaf area m^2 (delta) | Height m | Cook s | Clusters s / MB |
|---|---|---|---|---|---|---|
| european beech 01 | 46.1M | 4.00M (wood 0.75M) | 533.4 (0.00%, 95% of pieces kept, x1.049) | 26.6 | 78 | 15.4 / 59 |
| english oak forest 01 | 14.2M | 4.00M (wood 0.68M) | 67.8 (0.00%, x1.012) | 17.7 | 61 | 14.8 / 57 |
| norway maple 01 | 5.90M | 4.00M (wood 1.84M) | 218.8 (0.00%) | 15.5 | 44 | 11.3 / 52 |
| european beech saplings 01 | 0.86M | 0.86M | 6.3 | 3.0 | 27 | 1.9 / 11 |
| rowan sapling | 3.46M | 1.00M (wood 0.06M) | 1.6 (0.00%, all pieces kept) | 2.8 | 31 | 2.5 / 14 |

  Cook time includes ~20 s of bark encoding per tree; usd2gltf takes 12-14 s
  for beech and oak, 1-2 s for the others. The scatter with all five
  needs 12.9M vertices, 51M indices, 30M + 34M cluster vertex references
  and triangles (the pools grow to fit; see "Geometry pools grow with the
  content").
- **Tangents only where read.** `loadGltf` runs MikkTSpace only for
  materials with a normal map, the only place `mesh.frag` reads tangents;
  the trees' ~10M leaf triangles skip it, and Debug `--scatter 100` startup
  dropped from 3:45 to 2:30. Tangent bytes are part of the cluster cache
  key, so caches rebuild once; captures match exactly (0/255) against the
  old loader with freshly built caches.
  - https://pypi.org/project/usd-core/
  - https://openusd.org/release/api/class_usd_geom_point_instancer.html
  - https://openusd.org/release/api/usd_preview_surface.html
  - https://github.com/lighttransport/tinyusdz
  - https://graphics.pixar.com/library/StochasticSimplification/ (Cook, Halstead, Planck, Ryu 2007)
  - https://meshoptimizer.org/#simplification
  - https://ambientcg.com/view?id=Bark012

## Cluster-group BVH (M2a, 2026-11-19)

- **One BVH per DAG level over that level's cluster groups**, as
  vk_lod_clusters ("a set of hierarchies - one for each LOD level"):
  leaves are clusterlod groups (a contiguous cluster range), inner nodes
  their children's conservative LOD bounds. A node's LOD sphere encloses
  the self and refined spheres of every cluster below and carries the
  largest self error, so a node whose bound projects at or under the
  threshold holds no drawable cluster and is skipped; reached leaves test
  each cluster with the unchanged cut rule. A level is entered only when
  its band can hold the cut (largest self error at the root's nearest
  point over the threshold, smallest refined error at the farthest point
  a refined sphere can be at or under it). Built at load inside
  `buildClusterLod`, stored in the `.clusters` cache (version 3).
- **Branching factor 8, top-down median splits** along the longest axis
  of the leaf centers (three bisections per node), nodes laid out
  breadth-first with each node's children contiguous and after it: fixed
  48-byte nodes with indices, ready for the GPU's one-dispatch-per-depth
  traversal (Task 7), and a 1.6M-triangle tree's levels stay a handful of
  depths deep. Wider nodes (32) would cut depths but waste threads on
  sparse groups; 8 is the middle ground and is easy to revisit.
- **Float safety:** enclosing radii are widened by 1e-4 so rounding in
  the distance tests never makes a node look farther than a cluster below
  it. `selectClustersBvh` (the CPU replica) is tested to select exactly
  the flat `lodVisible` scan on a 1.55M-triangle synthetic tree (16
  levels, terminal groups at several depths) at 1-1000 m and 0.5-4 px.
  - https://github.com/nvpro-samples/vk_lod_clusters/blob/main/docs/lod_generation.md
  - https://github.com/zeux/meshoptimizer/blob/v1.0/demo/clusterlod.h

## Placement rules and dither pattern (M2a, 2026-11-19)

- **Rule-driven, dithered placement** (Horizon Zero Dawn's GPU placement):
  per layer, a density in 0..65535 is the product of smoothstep-shaped
  factors (slope range, altitude range, fractal-noise mask with an edge
  band, clustering noise), and a pattern point is kept iff its dither
  threshold is below that density. Raising density adds points instead of
  moving them, so rule edits and density gradients stay stable.
- **Progressive blue-noise pattern**: Mitchell's best-candidate sampling on
  the torus (8 candidates per existing point, grid-accelerated nearest
  search); a point's insertion rank is its threshold, so every
  lowest-threshold prefix is itself evenly spread. Measured min pair
  distance / nominal spacing (tile / sqrt(n)): 0.77, 0.75, 0.77 at 10, 50,
  100% of 256 points. One tile covers one 64 m cell; a layer's footprint
  is the nominal spacing at full density (sqrt(cell area / points)) and
  sets the point count ((64 / footprint)^2); the measured minimum is
  ~0.75 x footprint inside a cell, not guaranteed across cell seams. Repetition is broken per cell by one of the 8
  square symmetries plus a toroidal offset, both from a hash of (cell,
  layer, seed); spacing across a cell border isn't guaranteed (an
  occasional close pair at a seam, which reads as natural).
- **Bit-exact keep/reject**: everything on the keep path is 32-bit integer
  or 16.16 fixed point, so the GPU (`placement.glsl`, no int64 needed) and
  the C++ reference agree on any GPU and driver: PCG3D hash (Jarzynski and
  Olano 2020), value noise on integer lattices with a fixed-point
  3p^2 - 2p^3 fade, ramps and products in 16-bit fixed point. Rules read
  the nearest 16-bit sample: altitude in sample steps, slope as the
  squared central difference compared with tan^2 thresholds converted
  once on the CPU (no transcendentals on the GPU). Floats are converted
  once (`placementField`, `placementRule`, `prototypeThresholds`).
  Continuous attributes (height via `heightAt`, rotation, scale, the
  mask's edge shrink) are floats. The mask noise depends on the seed only,
  and a negative mask edge gives 1 minus the same band's ramp, so a forest
  layer and a lone-tree layer with the same threshold and opposite edges
  sum to exactly full density. Clustering noise is salted per layer, so
  forest, lone trees and rocks don't clump in the same places. The keep
  path covers crops under 32 km wide (cells -256..255, rejected before
  any cell arithmetic).
  - https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf
  - Mitchell 1991, "Spectrally optimal sampling for distribution ray
    tracing" (best-candidate): https://doi.org/10.1145/127719.122736
  - Jarzynski and Olano 2020, "Hash Functions for GPU Rendering" (PCG3D):
    https://jcgt.org/published/0009/03/02/

## GPU placement at load (M2a, 2026-11-20)

- **Count, scan, write** (Horizon Zero Dawn places on the GPU the same
  way, from rules and dither patterns): `placement.comp` runs one
  workgroup per 64 m cell, layers in order inside it. `PLACEMENT_COUNT`
  counts kept points per (cell, layer), `scan.comp` turns the counts into
  offsets (one workgroup: chunk sums, a shared-memory scan, rewrite) and
  reads back with the total (submit 1, sizes the table), and
  `PLACEMENT_WRITE` writes each kept point at its offset plus its rank in
  its 64-point batch (submit 2). The order is (cell, layer, pattern point)
  and never depends on atomics, so the table is identical run to run.
  The samples go to the GPU as raw 16-bit words in a temporary buffer:
  the keep path reads integers, not the R16_UNORM image through floats.
  Each layer's dither pattern is seeded by its layer id.
- **Identity**: the table index is not a persistent id (it shifts when an
  earlier cell or layer changes); (layer id, cell, pattern point) is,
  recomputable from the rules. Nothing stores it yet.
- **Instance format** (24 B, `PlacedInstance`): position 3 x f32, the
  rotation quaternion's x, y, z in snorm16 with w >= 0 rebuilt, scale as
  a half float, prototype u32. Measured round trip: < 7.5e-5 rad for
  rotations up to ~106 degrees (w >= 0.6), worst 0.011 rad (0.6 degrees)
  near 180 degrees, where w is rebuilt from 1 - |xyz|^2 and the 16-bit
  grid is coarse; invisible for random yaw and rock rotations
  (a smallest-three layout in the same 48 bits would fix it if ever
  needed). Scale within 5e-4 relative.
- **Cells**: the crop's grid (`placementGrid`, 236 x 236 = 55,696 cells on
  the Napf), row-major from the north-west; each holds its run of the
  table and the height range of its instances' bounding spheres
  (prototype sphere rotated by the decoded quaternion, scaled).
- **Debug check**: whole (cell, layer) runs from the GPU table against
  `evaluatePoint`: every layer of the crop's four corner cells and one
  edge cell, then random runs up to at least 4096 points. Keep
  decisions, order, prototypes and x/z bit-exact, height within 1 mm,
  rotation within 2 snorm steps, scale within one half-float step; plus
  each sampled cell's run and height range (recomputed from its decoded
  instances). Napf: 4148 points, 1240 kept, 0 mismatches (giving the GPU
  the wrong seed fails it). The tables' FNV-1a hash (`placement hash
  0x0d994d776496812c`) repeats run to run. The GPU's `heightAt` is
  `precise` (no FMA contraction; the C++ build doesn't contract either)
  and matched the CPU bit for bit on every compared instance; the check
  still allows 1 mm because GLSL division isn't guaranteed correctly
  rounded.
- **Measured on the Napf** (seed 1): 2,916,637 instances: forest
  2,590,549 (6 m footprint, slopes under 42 degrees, mask 1/1500 m at 0.45
  with a 0.08 edge, clustering 0.3), lone trees 33,495 (40 m, under 25
  degrees, the mask's complement), rocks 292,593 (12 m, the ground
  material's rock slopes via `kRockNormalY`, clustered 0.6). Table 70 MB,
  cells 0.9 MB, a 112 MB samples buffer during the pass only. Release:
  placement 65-135 ms; loading the prototypes (three 4M-triangle trees,
  saplings, the 4K-textured cliff, rocks) adds ~7 s to startup (2.1 ->
  9.4 s); `--placement off` skips both.
- **Rocks from Fab**: the massive sandstone cliff (tier 1 glTF) comes in
  centimeters and Z up through its node's rotation and scale;
  `meridian-cook` now bakes a childless node's rotation and scale into a
  mesh only it uses (translations stay), so the mesh stands upright in
  meters as a prototype. The "rough sandstone rock" download is a surface
  scan (textures on a 2 m plane), not a rock mesh, so it isn't used.
- **Ground fit: accepted for impostor-range trees.** Instances stand on
  the bilinear heightfield (`heightAt`), while CDLOD draws level k with
  2^(k-2)-sample quads, so ridges drawn coarse sit below the true surface
  and trunks float. Measured over 2M random Napf points (true minus
  coarse; positive = floating, before the 0.3 m sink):

  | level (drawn at) | p50 | p90 | p99 | floats > 1 m |
  |---|---|---|---|---|
  | 3 (320-640 m) | 0.00 | 0.14 | 0.47 | 0.08% |
  | 4 (640-1280 m) | 0.01 | 0.39 | 1.24 | 0.9% |
  | 5 (1.3-2.6 km) | 0.03 | 0.98 | 2.90 | 6.4% |
  | 6 (2.6-5.1 km) | 0.08 | 2.36 | 6.27 | 20% |

  A pixel at 1440p (fovY 1.2) is ~0.95 m at 1 km and ~1.4 m at 1.5 km, so
  up to level 5 the p90 gap is under a pixel and the p99 about two; what
  floats is a sub-pixel trunk under a 15-27 m canopy. A per-level sink
  would bury every tree by the p99 bound and step at level changes, so it
  isn't used. If the ridge capture at 1-2 km (Task 5, the first drawn
  placement) shows sky under canopies, the fix is to snap each drawn
  instance to the displayed CDLOD height at its origin (same morph as
  terrain.mesh) rather than to sink.
  Checked with the first drawn placement (open view toward the summit,
  trees on the summit ridge at ~1.5 km, 1920x1080, work-item list raised
  so every tree is drawn): trunk bases meet the ridge line, no sky shows under canopies,
  so no snap.
  - https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf
  - Harris, Sengupta and Owens, "Parallel Prefix Sum (Scan) with CUDA",
    GPU Gems 3 ch. 39 (stream compaction by count + scan + write):
    https://developer.nvidia.com/gpugems/gpugems3/part-vi-gpu-computing/chapter-39-parallel-prefix-sum-scan-cuda
  - https://github.com/fstrugar/CDLOD (the level/quad sizes above)

## Far-field forests: voxel tiers (M2a, 2026-11-20)

*To be superseded by the DAG voxel foliage plan (branch `g1`, ray-query bake removed) once it merges; until then this entry holds.*

- **Chosen (user, 2026-11-20): voxels, no impostors, in three tiers**,
  as Chlorophyll Engine describes its own: near trees are clusters of
  their LOD DAG; from where a tree's triangles go sub-pixel it is drawn as
  its prototype's voxels (a sparse brick hierarchy in the prototype's
  space, mips from height / 128 down to 4 m, shared by all instances);
  past a per-cell far switch (a 4 m voxel <= 1 px, ~4.8 km) each 64 m
  cell skips its tree instances and draws one aggregate of world voxel
  bricks (L0 4 m, L1 8 m, L2 16 m). The world bricks are also the far ray
  field: a sun shadow map built from them for far receivers now, sky
  visibility in M2b (replacing M2b's 2D canopy field). Spec:
  `docs/superpowers/archive/specs/2026-11-20-far-field-design.md`.
- **Why three tiers:** the Task 8 prototype (per-cell bricks beyond 4 km,
  clusters nearer) measured the early scene pass at ~13 ms at the overview,
  mostly cluster raster of trees inside 4 km, whose coarse DAG foliage
  also loses leaves. That band is where triangles are sub-pixel, the
  case UE 5.7's Nanite Foliage and Chlorophyll turn into voxels. Per
  instance, prototype voxels cost memory per prototype, not per world.
  Beyond the far switch, per-instance work for 1M+ trees of 2-6 px is
  what the per-cell aggregate avoids.
- **Rejected:**
  - Impostors (mid range or to the horizon): sub-pixel alpha-tested quads
    shimmer, the user dropped them.
  - Per-instance voxels to the horizon: 1M+ tiny boxes at the overview.
  - Per-cell bricks with clusters to 4 km (the prototype): the cost and
    thin foliage above.
  - Rasterized voxel cubes: one-pixel triangles at the far switch (for
    the per-cell far field; the mid tier rasterizes its prototype voxels
    after all, see "Mid tier: rasterized prototype voxels").
  - (B) a 2.5D canopy aggregate: no 3D crowns; the user chose C.
  - Nanite's stochastic per-pixel normals: noisy; a deterministic 0.5
    opacity hit gives a stable image.
- **Task 8 prototype, measured** (1440p Release, Napf seed 1, `--trees`):
  resident 659 MiB (2.15M L0 bricks, 31.7 of 64 voxels occupied on
  average), load peak 3.68 GB; far pass 2.40 ms ridge / 6.16 overview,
  of which the per-pixel shadow march was 60-70% (0.93 / 2.22 with TLAS
  shadow rays); scene early + far field 5.94 vs 4.01 (TLAS shadows) at
  the ridge. Fixes chosen: bricks storing an occupancy mask and only
  their occupied voxels, the L0 splat in spatial slices, a sun shadow map
  of the bricks instead of the per-pixel march, one raster box per
  occupied brick column, two-phase Hi-Z culling of far cells.
- **Rays:** M2a keeps the static TLAS and chooses per ray, by the
  receiver's distance from the camera against `kFarShadowDistance` (the
  far switch distance). Near receivers trace every tree in the TLAS (the
  reference). Far receivers mask placed trees out of the ray query (an
  instance-mask bit) and read the sun shadow map of the bricks, rebuilt
  over frames into a second map when the sun turns (no stall; the per-ray
  brick march covers a fast sun drag until the swap). Every ray uses
  exactly one tree representation.
- **Far shadow map, measured** (Task 9 step 3, 1440p Release, `--trees
  --rr off`, 3.74M placed): 8 m texels, 16-bit depth of the first
  0.5-opacity point of the L1 bricks along the sun (`farfield_shadow.comp`,
  the cell DDA of `farFieldTransmittance` at one level), 2x2 bilinear depth
  compare in `farShadowVisibility` (no sun-disk jitter: a texel is about the
  penumbra at 1 km). The receiver moves one texel along its normal and the
  bias is 2 m + texel x min(tan(angle to the sun), 4) (normal offset plus
  slope-scaled bias: a fixed bias either lit the forest floor or
  self-shadowed voxel surfaces). Far field pass vs the per-ray brick march
  (median of 3): forest overview 7.45 -> 1.97 ms, high overview 10.87 ->
  2.70, sunset 10.28 -> 2.91, ridge 2.95 -> 0.94; the frame drops 26.2 ->
  18.1, 36.9 -> 15.3, 32.1 -> 18.5 ms (scene early falls too: far
  receivers there stop marching). Maps: 15 MiB each, 30 MiB both
  (2655x1827 texels used at the default sun); the first builds in 2-7 ms
  on frame 1. A rebuild runs 131072 texels per frame (~0.3 ms); it restarts only
  when the sun is more than the stale angle (1 degree) from the direction
  being built, because the spec's 0.1 degrees starved under even a slow
  drag. While a map is up to 1 degree stale a km-long shadow edge can sit
  up to ~17 m off and steps at the swap; past ~0.01 degrees per frame the
  finished map is already stale and receivers fall back to the per-ray
  march. Shadowing no longer needs the TLAS for far casters (`--far-shadows
  tlas` is the legacy A/B).
- **Gates** (spec): far field resident <= 500 MB and load peak <= 2.5 GB;
  far pass <= 1.0 ms and mid pass <= 1.5 ms at the forest and overview
  views; far shadows cost no more than the TLAS path; shadow pairs for
  the user; coverage of each prototype's voxels within 5% of its
  ray-traced full-detail BLAS.
- **Compact bricks, sliced splat** (Task 9): a level's bricks are one u32
  pool, a 3-word header per brick (64-bit occupancy mask, pool index of
  the first occupied voxel), then the occupied voxels in brick order;
  voxel i = `pool[firstVoxel + bitCount(mask & ((1 << i) - 1))]`
  (`farBrickVoxel`). Bricks with no occupied voxel keep their slot with
  mask 0 (12 B each) rather than leaving holes in the columns' brick runs:
  11.9% of L0 slots, ~3 MB, against re-trimming columns. The splat runs
  in slices of whole cell rows, each at most 1 GiB of accumulators
  (`farSliceRows`); a slice reads only the instance rows within the
  instances' reach, clips deposits to its rows, counts and scans the
  occupied voxels (mask pass, CPU prefix of the masks, pack pass), and
  keeps its voxels in a chunk copied into the level's pool at the end, so
  the pool is bit-identical for any slicing. Measured (Release, 1440p,
  `--trees --rr off`, 3.74M placed): resident 702 -> 394 MiB, load peak
  3937 -> 1257 MiB (both gates pass), splat 238 -> 299 ms; far field pass
  +0.1-0.4 ms (ridge 2.95 -> 3.32, high overview 10.87 -> 11.18, sunset
  10.28 -> 11.22: the rank lookup's extra loads), which the far shadow map
  and the far-cell raster address.
- **Build on the player's hardware:** prototype templates come from ray
  queries against each prototype's full-detail BLAS (three-axis pass
  fractions calibrate extinction against what RT shadows trace). The
  world bricks are a GPU splat of the placed table at load. Nothing goes
  to disk unless the template bake exceeds ~5 s.
  - https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage
  - https://forums.unrealengine.com/t/bad-raytraced-shadows-with-voxelised-nanite-foliage-in-5-7/2696233
  - https://www.youtube.com/watch?v=VC_NUxbeA0E (Chlorophyll Engine)
  - https://wccftech.com/custom-forest-engine-delivers-stunning-visuals-at-130-fps-on-an-rtx-3080-at-1440p-without-hardware-ray-tracing/
  - https://dl.acm.org/doi/10.1145/3820021 (HPG 2026 massive foliage;
    far plants as aggregates)
  - https://hal.archives-ouvertes.fr/hal-01468817v1 (Loubet and Neyret
    2017, mesh-volume LoDs)
  - http://www.cse.yorku.ca/~amana/research/grid.pdf (voxel DDA)

## Placed rendering: cells, chunks, prototype BLAS (M2a, 2026-11-20)

- **Cell cull -> chunks -> instance cull.** `cell_cull.comp` runs one
  thread per 64 m cell in both culling phases (two-phase occlusion as
  for instances: phase 0 takes cells visible last frame, frustum only;
  phase 1 tests every cell against the frustum and this frame's depth
  pyramid and records each cell's visibility in the visibility buffer,
  one uint per cell after the instance ids' one uint each). The cell box is its square grown by the instances' horizontal
  reach (`cellMargin`, the prototypes' sphere extent x the layers' max
  scale: 28.8 m on the Napf) and its stored height range, tested as
  displayed under the curvature drop. Survivors emit chunks of up to 64
  consecutive ids (`uvec2(first, count)`); the hand-placed instances are
  one more chunk source. `cull.comp` runs one workgroup per chunk
  (indirect dispatch), testing the prototype's sphere; a chunk flag says
  whether phase 0 already processed the cell, so phase 1 neither redraws
  nor skips an instance whose cell was culled last frame.
- **Draw distance per prototype.** Cells mix layers, so the cell test
  uses the largest layer draw distance (none on the Napf: trees have
  none); the instance test uses `PrototypeInfo.maxDrawDistance`, the
  largest of the prototype's layers (rocks: 1500 m). It fills the brief's
  pad slot, keeping `PrototypeInfo` 48 B.
- **One BLAS per prototype**, a geometry per mesh (a ray hit's geometry
  index finds the mesh for the alpha test, `instanceMesh`), full detail,
  built with ALLOW_COMPACTION and compacted: 21 prototypes, 944 -> 339
  MiB. Hand-placed meshes get their BLAS on their first `addInstance`
  (prototype-only meshes never get one).
- **TLAS fill on the GPU.** `tlas_fill.comp` writes one
  `VkAccelerationStructureInstanceKHR` per placed instance at
  `setPlacement` (custom index `kPlacedBase` + table index); the
  hand-placed records are copied in after them before each build. One
  build of 2,916,652 records: 178 MiB of records, TLAS 524 MiB, scratch
  228 MiB, ~7 ms (wall time of the submit). Device memory allocated
  after load 4140 MiB; VRAM in use at the forest views 4.6 GB of 16 GB
  (nvidia-smi), so >= 3 GB stays free.
- **Moss rock sets**: each rock stays its own prototype; `meridian-cook`
  now also bakes a lone node's vertical offset (besides rotation and
  scale) into its mesh, so a rock keeps its authored scale (rock07
  0.679) and its base height, and only the horizontal offset (up to 3 m,
  which would float or sink it on the steep slopes rocks sit on) is
  dropped. Drawn models are unchanged (the node keeps the horizontal
  offset). The rock sets' bounds change the cells' height ranges, so the
  placement hash is now 0x1c6b673748ee8f8f (instances unchanged; the old
  cook still gives 0x0d994d776496812c).
- **Measured** (Release, 1920x1080, forest edge `600 1205.6 -400 -1.55
  0.02`, median of 5): GPU 4.73 ms (cull cells 0.01, cull early 1.49,
  scene early 1.48, cull late 1.51); 23,759 instances, 2,369 cells.
  The flat work items (every cluster of every DAG level per instance and
  mesh, ~1,100 per tree) overflow the 512K list: 24.9M dropped at the
  edge, 457M at the overview, 164M at the start, so most trees there are
  missing from the raster (the TLAS still shadows with all of them).
  Chunks, clusters and terrain nodes drop 0 everywhere. With the list
  raised to 2^26 for a test (not kept), the edge draws every tree at
  81 ms, 65 of them the flat cull; the BVH traversal (next section)
  replaced it.
  - https://developer.nvidia.com/blog/rtx-best-practices/ (compaction,
    one BLAS per object with its geometries, static TLAS)
  - https://docs.vulkan.org/spec/latest/chapters/accelstructures.html (instance records, geometry index, compaction queries)
  - https://github.com/zeux/niagara (two-phase occlusion culling)
  - https://github.com/nvpro-samples/vk_lod_clusters (instance cull feeding cluster LOD)
  - Wihlidal, "Optimizing the Graphics Pipeline with Compute", GDC 2016
    (coarse chunk culling ahead of finer culling)

## Cluster-group BVH traversal on the GPU (M2a, 2026-11-20)

Pulls virtualized geometry slice 5's hierarchical traversal forward;
slice 5's compression and streaming stay deferred.

- **Breadth-first, one indirect dispatch per BVH depth**, as
  vk_lod_clusters (and like our CDLOD terrain selection): the instance
  cull seeds `(instance, level root)` for each DAG level of each mesh
  whose band can hold the cut (the CPU reference's two tests), then
  `cluster_traverse.comp` runs one thread per queued node: skipped when
  its LOD sphere's largest self error projects at or under the threshold,
  or when its culling sphere is outside the frustum; inner survivors push
  their (up to 8) children to the next depth's queue, leaves emit work
  items of their group's clusters to the unchanged cluster cull. The
  dispatch count is the deepest leaf over all uploaded meshes plus one
  (5 with the Napf's trees and rocks; at most 12). Rejected: the
  roadmap's flat level-range iteration (vk_lod_clusters measured it ~2x
  slower than its BVH, 226-252 vs 111-118 µs for 3M instances) and
  Nanite's persistent-thread traversal (vendor-tuned thread counts;
  Nanite reports 25% over per-level dispatch, worth revisiting in M5).
- **Queues:** three node queues of `(instance, node)` entries: phase 0
  ping-pongs between queues 0 and 1 by depth parity; phase 1 starts from
  queue 2 (its instance cull's seeds plus the nodes phase 0 found
  occluded), then ping-pongs too. Counts and dispatch arguments per
  phase and depth live in `DrawCounters`. Node children are rebased into
  shared node and cluster pools at upload, so an entry needs no mesh.
- **Occlusion at node granularity:** phase 0 tests a node against last
  frame's pyramid (from last frame's view) and hands an occluded one to
  phase 1, which retests it and its subtree against this frame's; when
  phase 1's queue is full the node goes on in phase 0 instead, so nothing
  is dropped. Phase 1 culls nodes against the new pyramid. Clusters
  below a visible node keep the cluster-level retest. A node's sphere
  encloses its clusters' and its pyramid footprint covers theirs, so a
  node found occluded hides every cluster below: at the forest interior
  the traversal draws 135 fewer clusters (63,479 vs 63,614) than the
  flat path with an identical image (0 pixels differ); the flat path's
  per-cluster test samples a slightly wider footprint and keeps some
  hidden clusters.
- **A/B against the flat work items** (Release, 1920x1080,
  `--placement off`): default start and over-scatter (`--scatter 100`,
  `1119 1362 -431 -1.55 -0.25`) draw the same clusters and triangles
  (208 / 23,387 and 1,627 / 169,228) with bit-identical images, as do
  repeated runs of either path. The flat path is removed.
- **Capacities** (peak at the six verification views, Release 1080p):
  node queues 2^21 each (peak 798K, the overview's phase-0 seeds), work
  items 2^21 per phase (peak 813K, overview; was 2^19), cluster draw
  lists 2^21 per list (peak 570K, overview; was 2^20, raised for 1440p,
  where the 1-pixel cut should draw ~1.8x the clusters, the pixel-count
  ratio; not measured, the monitor was off); the node and level pools
  grow with the uploads (23.6K nodes for the placement's prototypes).
  ~150 MB more VRAM; 4.72 GB in use at the forest views (nvidia-smi),
  11.7 GB free. 0 drops in every list at every view.
- **Measured** (Release, 1920x1080, median of 5; trees and rocks all as
  clusters, no impostors or far field yet):

  | View | GPU ms | cull early / traverse / cull clusters / cull late | scene early | instances | clusters | triangles | bvh nodes |
  |---|---|---|---|---|---|---|---|
  | forest edge | 12.07 | 0.02 / 0.05 / 0.09 / 0.07 | 11.53 | 22,895 | 349,832 | 28.8M | 217,529 |
  | overview | 12.17 | 0.13 / 0.10 / 0.92 / 0.15 | 10.41 | 416,070 | 1,017,297 | 46.6M | 951,707 |
  | ridge (open view) | 4.11 | 0.07 / 0.06 / 0.32 / 0.11 | 3.25 | 140,301 | 329,253 | 14.1M | 317,162 |

  Culling at the forest edge fell from 3.0 ms (flat, mostly dropped) to
  0.23 ms with every tree drawn; the cost is now the cluster raster of
  distant trees, which the impostors and voxel far field (Tasks 8-11)
  take over. The cluster cull's 64-thread workgroups mostly idle on
  small leaf groups (0.92 ms at the overview; backlog).
  - https://github.com/nvpro-samples/vk_lod_clusters (per-level BVH,
    traversal per depth, flat vs BVH timings)
  - https://github.com/nvpro-samples/vk_lod_clusters/blob/main/docs/lod_generation.md
  - Karis, "Nanite: A Deep Dive", SIGGRAPH 2021 (persistent-thread
    hierarchy traversal, two-pass occlusion)

## Geometry pools grow with the content (M2a, 2026-11-20)

- **Grow on upload, by at least 1.5x, with a GPU copy.** The shared
  bump-allocated pools (vertices, indices, clusters, cluster vertex
  references and triangles, BVH nodes, DAG levels) start at 1 MiB each.
  When `uploadMesh` needs more, `grownPool` allocates a buffer of
  max(needed, 1.5x) bytes before recording; the used range is copied into
  it in the same submit as the mesh's data. Only after that submit, and
  a device idle (frames in flight may still read the old pool), does it
  replace and free the old one, so a failed allocation leaves every pool
  as it was. Nothing caches a pool's address: SceneData is
  written every frame, the index buffer is bound every frame, and a BLAS
  keeps no reference to its build inputs. Chosen over sizing from totals
  the game would compute before uploading, which needs a new API and a
  pass over every model before any upload; growth keeps `uploadMesh`
  self-contained, and with few growths (log1.5 of the final size) the
  copies cost little at load.
- **1.5x, not doubling**: doubling left the vertex pool at 2 GiB for
  1050 MiB of vertices (`--scatter 100` with placement, Debug): 6696 MiB
  of device memory against 5697 MiB at 1.5x. Slack stays below a third of
  each pool.
- **Trimmed before placement** (G1, 2026-11-24): with the voxel pools the
  slack reached 1.79 GB at `--trees` (6279 MiB allocated, 4491 MiB used),
  and the far-field splat's 1.5 GB transient then went past the 16 GB
  RTX 4080, so the driver paged and the splat took 55 s. `setPlacement`
  first copies every pool into an exact-size buffer (`trimPools`; models
  are loaded by then; a later upload grows again): device memory 14481 ->
  12932 MiB, splat 55 s -> 0.33 s. One pool at a time, each old one freed
  before the next is allocated: trimming all ten in one submit exceeded
  device memory and VMA (`VMA_MEMORY_USAGE_AUTO`) silently placed 2.2 GB
  of voxel and cluster pools in system memory (forest edge 1440p 24.6 ->
  42.3 ms, found by bisection). `createBuffer` now warns when a GPU-only
  buffer lands outside device memory.
- **Limits that remain** are structural and checked in `uploadMesh`, each
  with its own error: work items pack a first cluster index into 26 bits
  (`kMaxClusterIndex`, cull.glsl); vertex offsets are int32 and other
  offsets uint32; the mesh table is a fixed `kMaxMeshes`; the traversal
  dispatches at most `kBvhDepths` levels; and no pool may exceed the
  device's `maxBufferSize` (Vulkan 1.3; buffer device addresses are not
  bound by `maxStorageBufferRange`).

## Motion clarity: DLSS 4.5, Ray Reconstruction, TAA v2, grading (2026-11-20)

Spec: `docs/superpowers/archive/specs/2026-11-20-motion-clarity-design.md`.

**The smear is in the TAA history, not in the motion vectors.**
- Measured at 1080p, eye-height ground view, motion/still Laplacian:
  forward 5 m/s 0.49, sideways 0.91, backward 0.95.
- Every frame resamples the history at a fractional offset (Catmull-Rom)
  and keeps 90% of it, so the blur compounds.
- Forward motion also magnifies older, coarser history.
- The reprojection residual has a sharp minimum at motion-vector scale
  1.0, so the motion vectors are right.

**Temporal accumulation stays.**
- Without TAA, shimmer between consecutive frames rises from 1.9/1.7 to
  19.1/17.0 (/255, foliage crown and shadow), and the 1-ray-per-pixel
  sun penumbrae dither.
- SSAA and MSAA cost too much or don't fix shading and shadow noise.
- Forza Horizon 5's MSAA lost to its later TAA because trees shimmered.
- Source: https://www.computerbase.de/2022-06/forza-horizon-5-series-9-update-taa-test/

**DLSS 4.5 sits behind an `Upscaler` interface, in an optional,
separately linked module (`MERIDIAN_DLSS`).**
- It uses direct NGX on Linux (Streamline is Windows-only), with NGX
  default presets except M for DLAA, Quality and Balanced (see below), so
  an SDK update is a version bump plus a preset re-measure.
- The core stays free of NGX and licence-clean: the DLSS licence forbids
  making the SDK subject to an open-source licence and requires NVIDIA
  attribution.
- Source: https://github.com/NVIDIA/DLSS
- The interface (`upscaler_vk.h`) is one `Upscaler::evaluate` per frame
  with an `UpscalerFrame`: render-sized colour, depth, motion and
  transparency mask in SHADER_READ_ONLY (depth is now stored after the
  late pass for it), jitter in render pixels, unjittered camera-relative
  matrices, reset; it writes a display-sized RGBA16F `output` that the
  histogram, tonemap and `.pfm` captures read. An optional
  `UpscalerProvider` passed to the `Renderer` constructor adds its
  instance extensions, the device extensions the GPU has, and backends.
- TAA is the core's backend (`TaaUpscaler`). It keeps its own ping-pong
  history; taa.comp writes the resolve there and sharpen.comp writes
  `output` from it, so the sharpen can't feed back into the history. No visual
  change: Release 1080p ground still and forward 5 m/s mean diff
  0.0000/255 vs the previous build; the `aa` pass median of 5 is 0.17 ms
  vs `taa` 0.17 ms (interleaved runs; re-measure at 1440p).
- The DLSS module (`engine/dlss`, `meridian_dlss`) uses NVIDIA DLSS SDK
  v310.9.16, the newest 310.x tag on 2026-11-20, pinned by its commit
  tarball (9981c6350dc58c0ce21f024fd65e31e851c31689, SHA-256 in the
  CMake file; a git clone would fetch every release's binaries). It
  links the SDK's `libnvsdk_ngx.a` and ships the release snippets
  `libnvidia-ngx-dlss.so.310.9.16` and `libnvidia-ngx-dlssd.so.310.9.16`
  (Ray Reconstruction) next to the binary, with the guide's section 9.5-9.6
  copyright and third-party notices; the dev snippets (watermark) and
  Frame Generation are not shipped. NGX init uses a fixed project id
  (custom engine), the binary's directory as the feature path, and
  reports at startup on driver 620.70 (RTX 4080): Super Resolution
  available, Ray Reconstruction available.
- The feature is created at the render size NGX gives for the mode (DLAA:
  the display size) with IsHDR, MVLowRes (no MVJittered: our motion
  vectors carry no jitter), DepthInverted and AutoExposure;
  `InMVScale` = render size. Exposure A/B, Release 1080p, DLAA, default
  preset: a constant 1.0 R32F exposure texture (colour is pre-exposed)
  vs NGX's AutoExposure measured forward 5 m/s ground 0.4600 vs 0.4596,
  beech forward 0.751 vs 0.757, shimmer crown 0.311 vs 0.312, shadow
  0.304 vs 0.305, the same within noise. AutoExposure needs no texture,
  and presets L and M always use it (programming guide 3.9).
- Jitter sign: NGX's `InJitterOffset` is the offset the jitter moved the
  image by, the negative of `UpscalerFrame.jitter` (each pixel samples at
  its center + jitter; `perspective` in mat4.h flips y with
  m[1][1] = -f). Release 1080p DLAA ground still Laplacian: both negated
  0.1803, x only 0.1743, y only 0.1743, neither 0.1738; frame 120 vs 121
  mean diff 0.048 vs 0.066 (y flipped) and 0.071 (neither).
- DLSS (DLAA + Ray Reconstruction) is the default (2026-11-20, "DLSS DLAA
  with Ray Reconstruction is the default", below); `--aa taa` selects
  TAA v2, which measures forward 0.94 at 0.17 ms. Super Resolution DLAA
  preset M (`--rr off`) costs 2.36 ms at 1440p.
- When DLSS is selected, the Super Resolution preset is M for DLAA, Quality and
  Balanced and NGX's own (also M) for Performance (`defaultPreset` in
  dlss.cpp); `--dlss-preset` overrides for A/B. NGX 310.9.16 defaults
  DLAA, Quality and Balanced to K, which fails the motion gates; M is the
  second-generation transformer (DLSS 4.5). An SDK bump must re-measure
  the newest preset against these numbers. Measured at 2560x1440,
  Release, `tools/motion-gates.sh` (GPU ms median of 5 ground stills):

  | | fwd 5 ground | side 5 | fwd 5 beech | pitch | shimmer crown/shadow | SSIM fwd | `aa` ms |
  |---|---|---|---|---|---|---|---|
  | DLAA, K (NGX default) | 0.44 | 1.04 | 0.77 | 1.04 | 0.27/0.30 | 0.926 | 0.75 |
  | DLAA, M | 0.96 | 1.01 | 0.90 | 0.93 | 0.80/0.40 | 0.993 | 2.36 |
  | Quality (1707x960), K | 0.49 | | | | | | 0.74 |
  | Quality, M | 0.82 | | | | | | 1.25 |
  | Balanced, K | 0.49 | | | | | | 0.76 |
  | Balanced, M | 0.78 | | | | | | 1.05 |
  | Performance, M (NGX default) | 0.75 | | | | | | 0.90 |
  | TAA | 0.49 | | | | | | 0.15 |

  Preset K (first transformer) smears forward motion like the old TAA;
  preset M passes every motion gate. Its `aa` pass costs 2.36 ms at 1440p,
  over the 1.0 ms SR/DLAA slot (the fallback when RR is unavailable).
  TAA v2 measures forward 0.94 at 0.17 ms. Upscaled modes' ratios are against their own stills (sharper
  by the mip bias).
- Debug builds: VK_NVX_binary_import (an NGX device extension) makes the
  driver start CUDA, which needs ASan's `protect_shadow_gap=0`; NGX leaks
  in its extension queries, init, feature creation and first evaluate
  sit in a `DriverLeakScope`. NGX clears the output on reset, so the
  output image has TRANSFER_DST.
- `MERIDIAN_DLSS=OFF` builds, tests and runs TAA with no NGX symbol in
  the core; with DLSS on, a missing snippet gives `DLSS: Super Resolution
  unavailable (0xbad00004)` and one `DLSS unavailable: ...; using TAA`
  line, and renders the TAA image exactly (mean diff 0.0000/255).

**Ray Reconstruction denoises the shadows and is the default (Task 1.5
and "DLSS DLAA with Ray Reconstruction is the default", below).**
- It takes the noisy lit colour plus guides (diffuse and specular albedo,
  normal + roughness).
- Its samples must be hashed white noise, not interleaved gradient noise.
- NRD SIGMA is not used: it is under the NVIDIA RTX SDKs licence.

**The fallback is TAA v2 plus a port of FidelityFX's shadow denoiser
(MIT).**
- Shortening the history alone doubles or triples shadow shimmer: current
  weight 0.2 gives 3.3/255, 0.3 gives 4.9.
- Source: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/

**TAA v2 (`taa.comp`, `sharpen.comp`): the history is kept shorter where
it would blur, and rescaled when the exposure changes.**
- Current weight `mix(0.1, 0.3, moving * max(subpixel))`: `moving` =
  motion length / 0.25 render pixels (clamped to 1); `subpixel` =
  4f(1-f) of the reprojected position's fraction (1 halfway between
  texels, where Catmull-Rom blurs most). Root cause 1.
- Magnification (root cause 2): J = det(I + d(motion px)/dpixel); where
  J < 1, current weight >= 1 - 0.9 J^6. Each derivative is the smaller
  of the two one-sided differences, not the central one: a depth edge on
  one side (leaf against sky) otherwise reads as a huge magnification and
  drops the AA there (beech 1.15, sparkling). This helps at edges with
  one smooth side only: a 1-pixel twig with sky on both sides can still
  read as magnified.
- Clip gamma stays 1.0. The spec's 0.75 in motion pulls the history
  toward the blurry neighbourhood mean: softer in every view (final
  parameters, 1080p: forward 0.92 vs 0.95, sideways 0.89 vs 0.99, pitch
  0.85 vs 0.91, beech 0.97 vs 1.02).
- The history is pre-exposed by the previous frame's exposure:
  `ExposureState.previousExposure` (written by exposure.comp before it
  adapts) gives the factor exposure / previousExposure, applied before
  the clip. Sunset view (`--sun 0.9 0.05 -0.4`) frame 30, while the
  exposure still adapts: without it the image lags darker by 0.78/255
  mean (0.49 while turning at 0.3 rad/s); no ghost trail either way.
- Whatever writes `exposure` must also write `previousExposure`, or TAA
  mis-scales the history for a frame.
- Output sharpen: FidelityFX CAS (MIT, commit 9fabcc9), sharpen-only with
  better diagonals, on a reversible tonemap c / (1 + max(c)) because CAS
  wants [0, 1] input (Karis 2014). It reads the history and writes
  `output`, so it never feeds back. It is off (a copy): CAS sharpness
  0.25 / 0.5 drops the forward SSIM to 0.969 / 0.966 and raises still
  shimmer to 2.9/2.5 and 3.0/2.7 (1080p); its weakest setting gives SSIM
  0.972 and shimmer 2.3/2.4 at 1440p (1.7/1.5 off). The ratio gates pass
  without it. Re-tune after the shadow denoiser (backlog).
- Sweep (Release 1080p, sharpen off; forward / sideways / pitch / beech
  motion-over-still Laplacian, then forward SSIM):

  | config | fwd | side | pitch | beech | SSIM |
  |---|---|---|---|---|---|
  | central J^8, 1 px, gamma 0.75 | 1.01 | 0.90 | 0.79 | 1.15 | 0.977 |
  | same, gamma 1 | 1.04 | 1.00 | 0.79 | 1.17 | 0.979 |
  | no magnification term | 0.79 | | | 0.81 | 0.984 |
  | central J^2 / J^4 | 0.80 / 0.89 | | | 0.96 / 1.06 | |
  | one-sided J^4 / J^8 | 0.87 / 1.02 | | | 0.95 / 1.00 | 0.985 / 0.980 |
  | one-sided J^6, 1 px | 0.95 | 0.99 | 0.79 | 0.98 | 0.983 |
  | one-sided J^6, 0.25 px (final) | 0.95 | 0.99 | 0.91 | 1.02 | 0.983 |
  | final, max weight 0.2 | 0.95 | 0.97 | 0.83 | 0.95 | 0.983 |

- Gates, Release 2560x1440, `tools/motion-gates.sh`: forward 5 m/s
  0.94, sideways 1.00, beech 1.00, pitch 0.88, SSIM 0.983, shimmer
  crown/shadow 1.7/1.5 (a still camera keeps weight 0.1, so shimmer is
  today's); `aa` median of 5 0.17 ms at 1440p, of which the sharpen's
  copy is 0.03 ms (0.14 ms with its dispatch skipped). The beech's 1.00
  is inflated by leaf sparkle: after a 1 px blur (or a 2x downsample)
  the beech ratio is 0.94, and the ground's 0.99. At 1080p: 0.95, 0.99,
  1.02, 0.91, 0.983, 2.1/1.5, `aa` 0.24 ms. The supersampled history
  was not needed. The beech's leaves still sparkle in forward motion
  (backlog).
- Sources: https://github.com/GPUOpen-Effects/FidelityFX-CAS ;
  Karis 2014, "High Quality Temporal Supersampling" (SIGGRAPH Advances).

**The grade is scene-referred and parametric.**
- White balance, CDL and per-hue OKLCh grading are baked with the DRT
  into a 65^3 LUT, indexed after AgX's inset by a log2 shaper over
  -12.47..+8 EV, so one grade serves SDR and HDR and a negative exposure
  offset recovers highlights.
- 33^3 misses the identity gate: the CPU reference (`grade.h`, 10000
  random colours, 2^-12..2^4) measures a mean of 0.47/255 against the
  analytic AgX (max 9.1); 65^3 gives 0.11/255 (max 3.9), and 0.06/255 for
  highlights up to 2^6.
- White balance is CAT02 from the CIE daylight locus (Planckian below
  4000 K, blended over 4000..5000 K so the slider has no jump), with tint
  as Duv along the Planckian isotherm, to D65 as the daylight locus gives
  it at 6504 K (so the default is exactly identity).
- Sources: CIE 15:2004 (daylight locus x(T), y(x)); Krystek 1985, "An
  algorithm to calculate correlated colour temperature" (Planckian uv);
  CIE 159:2004 CIECAM02 (CAT02); Ottosson 2020,
  https://bottosson.github.io/posts/oklab/ (OKLab).
- The grade defaults to identity.
- The GPU bake (`grade_lut.comp`, `Renderer::setGrade`) mirrors the CPU
  reference operation for operation and runs only when the grade changes;
  the tonemap is one trilinear fetch. Its readback differs from
  `bakeGradeLut` by at most 1.1e-3 (Debug check, limit 2e-3; also for a
  non-identity grade). Measured against the analytic AgX at 1920x1080,
  Release: mean 0.15/255 on the ground view and on the sunset view (sun
  disk), max 1/255; tonemap pass median 0.04 ms (was 0.06 ms; gate 0.08 ms
  at 1440p, 0.045 ms at 1080p; re-measure at 1440p).
- Reference starting set, for tuning after the lighting work: CDL power
  1.10, saturation 1.10, greens 10 degrees toward yellow x1.15, blues
  10 degrees toward cyan.
- AgX itself already matches the Chlorophyll reference's rolloff and
  greens.
- Sources: https://dev.epicgames.com/documentation/en-us/unreal-engine/color-grading-and-the-filmic-tonemapper-in-unreal-engine
  and https://gdcvault.com/play/1024253/High-Dynamic-Range-Color-Grading

**The scene renders at a render size, the resolve and later passes at
the display size** (DLSS programming guide).
- Render-sized: depth, HDR, motion and the depth pyramid; the scene
  viewport, jitter and `inverseResolution` (aerial perspective, cull, LOD
  pixels) use it. Display-sized: the upscaler's output and TAA history,
  histogram, exposure, tonemap, captures. `--render-scale S` sets render = display x S
  (rounded); TAA reads the current colour bilinearly at display pixel
  centers and its neighbourhood and motion at the render texel under
  them (quality below scale 1 is not gated: DLSS is the upscaler).
- Jitter phases = max(8, ceil(8 (display/render)^2)), at least 32 for
  Ray Reconstruction: about 8 samples per display pixel per cycle.
- Texture mip bias = log2(render/display), minus 1 when upscaling (0 at
  DLAA), on material samples (`texture` bias; `textureGrad` gradients
  scaled by 2^bias).
- Scale 1 is unchanged: Release 1080p ground still mean diff 0.0000/255,
  forward 5 m/s 0.0106/255 vs the previous build, accepted as noise
  (shader codegen: a same-size path fetching the identical texels gives
  the same diff; the forward/still Laplacian ratio stays 0.4974).
- Source: https://github.com/NVIDIA/DLSS (DLSS_Programming_Guide_Release.pdf,
  sections on resolution, jitter and mip bias)

**The reference's motion/still ratio is about 0.9** (0.8-1.0, low
confidence), estimated from the YouTube video. Gates: forward 5 m/s
>= 0.85 minimum, >= 0.95 stretch, on every path.

**A transparency mask flags blended pixels for DLSS.**
- R8_UNORM, render-sized, scene attachment 2: the blended pipeline writes
  its `coverage` with blend op MAX (the most covering surface wins); every
  other scene pipeline (mesh, masked, terrain, sky) has write mask 0 for
  it. Cleared to 0 in the early pass, loaded in the late pass.
- Alpha-tested (masked) geometry is not flagged: it is opaque per pixel.
- The depth buffer needs no transparent-pixel fix: it clears to 0 (reverse-Z
  far) and the sky pipeline has no depth test and so writes none, while
  blended surfaces write no depth.
- The cloud composite (compute, storage image) raises it to the cloud
  coverage over the scene, `max(mask, 1 - transmittance)`: thin cloud over
  terrain is a blend whose motion vector describes neither layer.
- Consumed by `UpscalerFrame.transparencyMask` (the DLSS module).

**Ray Reconstruction guide buffers and white noise (Task 1.4).**
- The scene pass writes three more attachments (layout: CLAUDE.md,
  Conventions): diffuse albedo = `diffuseColor` (base colour x (1 - metallic)),
  specular albedo = `mix(dfg.xxx, dfg.yyy, f0)` (the pre-integrated specular
  reflectance already computed for shading, without the multi-scatter
  compensation), normal + roughness = world-space shading normal and
  perceptual roughness (the clamped value the BRDF uses). Sky: diffuse 0.5,
  specular 0, normal 0. Blended surfaces write no guides (the surface behind
  keeps its values), as for depth and motion.
- RR wants these at render size: diffuse and specular albedo, normals with
  roughness packed in alpha (integration guide, "Specular Albedo",
  "Normals", "Roughness"). Source: DLSS RR Integration Guide in the SDK doc/
  directory (DLSS SDK 310.9.16).
- Guide images are only created when `Upscaler::wantsGuides()`; otherwise the
  attachments get null views. Dynamic rendering allows a null view only
  with the `dynamicRenderingUnusedAttachments` feature
  (VK_EXT_dynamic_rendering_unused_attachments; VUID-08912 otherwise), so
  it is a required device extension (supported on the RTX 4080 / driver
  620.70). Without it validation reports VUID-08912 and the driver
  crashed in draw.
- `SceneData.whiteNoise == 1` replaces the interleaved gradient noise of the
  sun-disk sample by a PCG3D hash of (pixel, frameIndex). The guide's
  "Sampling and Noise" section: RR assumes independent samples with minimal
  spatial and temporal correlation and recommends high quality hash
  functions and white noise (PCG: Jarzynski and Olano 2020, JCGT 9(3)).
  IGN is a deliberately structured spatial pattern, which TAA integrates
  but RR does not expect. AO is not stochastic, so only the sun sample
  and the cloud march jitter (per quarter/half-res texel) change.
  `whiteNoise` is 1 exactly when Ray Reconstruction runs; 0 leaves TAA
  output unchanged.
- Measured (Release, 2560x1440, TAA, `--scatter 100` beech, frames
  120/121, island_tree_01.old assets because the recooked island_tree_01
  fails to load, see backlog): shimmer crown/shadow 1.71/1.51 with IGN,
  1.71/1.67 with whiteNoise forced on; mean diff IGN vs white noise
  0.19/255. Informational.
- Scene-pass cost with guides forced on (Release, 2560x1440, scene early
  median): ground +0.14 ms (1.39 vs 1.25), beech +0.14 ms (2 runs). With
  null views (TAA): ground 1.22 vs 1.25, beech 2.47 vs 2.44 (first set
  2.50), diff 0.0000/255.

**Ray Reconstruction backend (Task 1.5), measured against the then-ratio gates:
it fails the motion gates.**
- `DlssUpscaler` runs the DLSSD feature (`NGX_VULKAN_CREATE_DLSSD_EXT1` /
  `NGX_VULKAN_EVALUATE_DLSSD_EXT`) per the RR integration guide (SDK doc/,
  rev. 08): noisy pre-exposed colour, diffuse/specular albedo, world-space
  normals with roughness packed in alpha (`Roughness_Mode_Packed`),
  hardware depth, motion vectors, camera-relative unjittered
  `worldToView` / `viewToClip` (column-major `M v` is NGX's row-major
  `v M^T`, so `Mat4` passes as is), IsHDR | MVLowRes | DepthInverted, no
  exposure input (guide 3.7), the SR path's jitter sign, a render-sized
  R16F specular hit distance cleared to 65504 (the guide's sky value:
  every specular lobe reflects the sky or the sun, nothing is ray-traced
  in reflection), render size from the DLSSD optimal-settings callback
  (guide 5.5), at least 32 jitter phases, and `whiteNoise` = 1.
- Version: SDK 310.9.16 is the newest NVIDIA/DLSS release
  (`git ls-remote --tags`, 2026-11-20); it ships
  `libnvidia-ngx-dlssd.so.310.9.16`, copied next to the binary. Preset F
  is pinned explicitly (`kRayReconstructionPreset`): the newest RR model
  in that SDK ("Default model RR2", nvsdk_ngx_defs_dlssd.h; added in guide
  rev. 08, June 2026, "the latest preset - F"), so an OTA default change
  can't swap it silently. The guide recommends the default preset; F is
  pinned because the user requires the newest model (deviation from the
  plan's "NGX's default RR preset"). Startup prints
  `DLSS: SDK 310.9.16, ..., Ray Reconstruction available (preset F)`.
- Cost (Release, 2560x1440, `aa` median of 5, ground; beech the same
  within 0.02 ms): RR DLAA 4.71 ms, over the 3.0 ms slot, so RR's mode
  defaults to Quality (1707x960): 2.35 ms.
- Gates (Release, 2560x1440, `tools/motion-gates.sh`, `--placement off`;
  TAA v2 on the same build for reference):

  | gate | RR Quality | RR DLAA | TAA v2 |
  |---|---|---|---|
  | forward 5 ground >= 0.85 | 0.711 | 0.791 | 0.944 |
  | sideways 5 ground >= 0.95 | 0.925 | 0.896 | 0.995 |
  | forward 5 beech >= 0.85 | 0.845 | 0.864 | 1.020 |
  | pitch 0.02 ground >= 0.85 | 0.726 | 0.835 | 0.878 |
  | shimmer crown <= 2.0 | 0.59 | 1.38 | 2.35 |
  | shimmer shadow <= 2.0 | 0.41 | 0.46 | 1.97 |
  | SSIM forward 5 ground >= 0.98 | 0.958 | 0.983 | 0.978 |

- RR removes the shadow and foliage shimmer (3-5x below TAA), but fails
  the motion ratios in both modes. The ratio is relative: RR's stills are
  much sharper than TAA's (ground Laplacian 0.40 Quality / 0.34 DLAA vs
  0.24) and its forward-motion frames are still sharper than TAA's (0.28
  / 0.27 vs 0.23); it loses more of its detail in motion. Inputs are not
  tuned beyond the guide. The ratio gate was dropped for absolute motion
  sharpness and RR became the default (below); the fallback shadow
  denoiser (Tasks 3.2a-3.2b) serves the fallback paths (backlog). The TAA column
  also misses crown shimmer and SSIM on this build (the new Megascans
  ground; Task 3.1 measured them passing before it).
- Debug captures, RR Quality and DLAA: 0 validation errors.

## Visible quality pass (2026-11-20)

Fixes from the asset-fidelity diagnosis (Cycles references of the same
assets, `quality-q1` branch): the assets were fine, the frame was dark,
flat and grey.

**Exposure puts the mean at 18% grey.** The EV100 rule (K = 12.5, 1.2
saturation factor) maps the histogram's mean log luminance to 0.104 of
white, 0.8 EV under grey; `kMeanToGreyEv` in exposure.comp adds those
0.8 EV. Start view display luminance mean 102 -> 125 of 255, p99 159 ->
193; forest edge 103 -> 125.

**The game starts with a grade of contrast 1.4, saturation 1.4, greens
rotated 12 degrees toward yellow and 1.15x chroma** (not the engine
default, which stays identity), aimed at the user's look target, the
Chlorophyll Engine frames in `assets/reference/chlorophyll/`: bright,
saturated sunlit greens, deep blue sky. AgX alone reads flat and
desaturated, and the cooked leaf colour leans blue. Display stats,
Release 1440p (luminance std, mean chroma (max-min)/max): references
std 50-70, chroma 0.37-0.49; start view std 28 -> 40, chroma 0.37 ->
0.53; forest edge std 39 -> 37, chroma 0.25 -> 0.45 (leaf transmission
included). The remaining gap is depth inside crowns (no ambient
visibility for foliage; backlog).

**Deferred from this pass** (rows in backlog.md): the warm ground bounce
(with today's brown placeholder terrain it turned foliage olive) and a
per-mesh leaf LOD error multiplier as a stop-gap for bare far trees (it
needs about 4x to restore canopies, which costs 6.5x the triangles at
the summit view and overflows the cull lists in the forest).

**Poly Haven AO.** Their glTFs bind the `_arm` pack (R occlusion, G
roughness, B metallic) only as metallicRoughness; the cook now binds the
same texture info (texture transform included) as occlusion when the
image file name holds `_arm`. 14 of 16 models gain AO; the two
rock_moss sets ship a plain roughness map and have none.

**Sun shadows test the TLAS before the terrain march.** Both must pass,
so the order changes no pixel (image diff 0), but inside canopies the
ray query stops within meters while the height-pyramid march walks
toward the sun first. Release 1440p, with leaf transmission (below):
forest edge 33.2 -> 21.8 ms, start view 5.70 -> 5.21 ms.

**Leaves transmit light (KHR_materials_diffuse_transmission).** Only the
factor is read (`MaterialData::diffuseTransmission`); `meridian-cook
--tree` sets 0.35 on leaf materials. Back-lit sun adds
`diffuseColor / pi * sun * -NoL * visibility * factor`, with its own
shadow ray offset toward the sun's side of the leaf, and the back face's
sky/ground irradiance adds the same share of the diffuse colour with the
usual occlusion. Why: the Cycles beech crown with 35%
translucency was 1.8x brighter than without, and Meridian had only
`sun * NoL`. Forest edge foliage/sky luminance 0.225 -> 0.278 (+24%),
chroma 0.32 -> 0.34; the extra shadow ray cost 6.5 ms there (Release
1440p) before the TLAS-first reorder above. Trees cooked before this must be
re-cooked (`fetch-sample-assets.sh` skips cooked trees: delete
`assets/source/megascans/tree_*/budget` first).

## Forest species mix favours the mature beech (2026-11-20)

Trees read as skinny, sparse "baby trees". The cook is not the cause:
it keeps 95% of the beech's leaf pieces and all of the oak's and maple's,
with leaf area preserved exactly (table above). The imported variants
are: beech A 26.6 m tall, 13 m crown, 533 m^2 of leaves (mature);
English oak *forest* A 17.7 m, 4 m crown, 68 m^2 (a forest-grown pole);
maple A 15.5 m, 8 m crown; saplings 3 m. Variant A is already the
largest of each set (oak B-D 8.5-14.9 m, maple B-D 3.8-14.5 m, beech B
23.6 m), so importing other variants would not help, except beech B
for variety. The old mix (beech 0.4, oak 0.25, maple 0.2, saplings
0.15 of the forest's slots) made 60% of forest stems poles or bushes.

- Forest: beech 0.7, maple 0.2, oak 0.1; no saplings (they took canopy
  slots; an understory would be its own layer). Lone trees: beech 0.75,
  maple 0.25; the forest-grown oak never stands alone.
- Forest edge view (`600 1205.6 -400 -1.55 0.02`, Release 1440p):
  41.1M -> 47.2M triangles, GPU 19.9 -> 26.4 ms (scene early 18.9 ->
  25.3); start view 18.2M -> 19.0M triangles, 5.63 -> 5.87 ms. The cost
  is the beech's 4M triangles at near range; the leaf LOD work and the
  far field address it.

## Terrain material: Megascans ground by role (2026-11-20)

The ground read as brown desert: Poly Haven soil and needle litter on all
flat ground and rock from 26 degrees. The look target (Chlorophyll Engine
frames, `assets/reference/chlorophyll/`) is bright green meadow, forest
floor under trees, rock only on cliffs.

- **Surfaces by what they show, not their names** (contact sheet:
  `.superpowers/ground-assets/contact-sheet.png` in the lane's worktree;
  roles in docs/assets.md). 15 of the 17 Megascans surfaces downloaded (14 after the rock anti-tiling entry below);
  dropped: `wild_grass_sfknaeoa` (very dark olive, a third of the other
  grasses' luminance, big dark blotches that repeat) and
  `rough_sandstone_rock_xi4uccxjw` (flat saturated orange, low relief,
  off the Napf's palette, 2x1 m).
- **Roles, stacked bottom to top with height blends** (`TerrainRole`,
  `terrainSurface` in mesh.frag): grass (3 variants), clover (flat
  hollows), mud (flat valley bottoms), soil (2; ridges and the ~7 degrees
  below rock, in patches), forest path (the forest mask's outer edge),
  forest floor (2; inside the forest mask), gravel (channels), scree (the
  foot of rock), rock (3, triplanar). Several variants per role: a ~200 m
  noise picks one per region and height-blends the next near its border,
  instead of blending every layer everywhere (cost).
- **Inputs:** slope from the per-pixel normal; curvature as the Laplacian
  of height over ~24 m from the normal map's divergence (4 extra fetches;
  Napf flat ground's 10th/90th percentiles about -0.02/+0.017 per m); the
  forest mask is placement.glsl's integer noise at the pixel (bit-exact
  with the trees', so floor and trees agree; 0 outside the crop, where
  nothing is placed); ~5 m noise perturbs every weight so borders are
  irregular; the Mishkinis height blend keeps stones above soil and grass
  filling hollows.
- **Rock only on cliffs:** `kRockNormalY` 48 -> 58 degrees (was 26 -> 41).
  At 2 m, 38% of the Napf is steeper than 30 degrees, 12% than 42, 5% than
  48, 1% than 58. Boulders follow the same values, so there are fewer
  (placed 2.92M -> 2.65M).
- **Colour matching:** the cook writes each base color's mean linear RGB
  (`albedo.txt`); the game scales each layer 85% of the way (in log) to
  its role's target mean, a uniform per-channel scale that keeps the
  texture's detail and contrast and a hint of its own tint. On top, ~400 m
  and ~160 m noise varies brightness (+-15%) and hue (+-5% red/blue), so
  no stretch of one layer looks uniform.
- **Resolution:** base color and normal at the scans' full 4K, ARM and
  height at 2K. Texel density: 2 m tiles 0.49 mm, 1 m 0.24 mm, 0.39 m
  gravel 0.1 mm, 3 m rock 0.73 mm per texel.
- **Normal maps:** the Megascans UE exports here are OpenGL (green up), as
  checked by correlating each map with its height map's gradient
  (+0.6 to +0.95), so no conversion. `layered_rock_vl0fdhdo` has no
  height map; its luminance (auto-levelled) stands in for the blends.
- **Cost** (Release, 2560x1440, `--placement off` so scene early is the
  terrain, 3 runs): full-ground start view 1.27 -> 1.53 ms, open view
  1.22 -> 1.33, high overview 2.76 -> 3.22. The first version cost 2.5x:
  `sampleRole` indexed `layers[]` of a local copy of `Terrain` with a
  dynamic index, which spills the whole struct to scratch memory; reading
  `pc.scene.terrain.layers[i]` straight from the buffer fixed it.
  Unrolling the role loop was slower (more live registers).
- **Memory and load:** device memory 3779 -> 4472 MiB (the textures
  ~+755 MiB; fewer boulders saved ~60 MiB of TLAS), about 53 MiB per
  layer (BC7 after transcoding: 4K with mips 21 MiB, 2K 5.3 MiB). Release
  startup with `--placement off` 2.2 -> 10.9 s, the 30 4K UASTC textures'
  transcode (backlog).

## Rock anti-tiling at distance (2026-11-20)

- **Second sample at 4x the tile size** in the triplanar rock's base
  color, mixed in up to 70% as a pixel covers 1-6% of a tile, and the
  rock's normal-map detail fades over the same range (the hex-tiled layers
  fade at 2-20%): its tile-sized bumps were what drew the grid. The rock's 2-3 m tiles otherwise
  repeat as a grid on cliffs seen from 100 m and more. 4x keeps a whole
  number of repeats per wrap period (256 tiles), so the wrapped offsets
  stay seamless. Hex-tiling the three triplanar projections was the
  alternative: 3x the taps, and its row period (256.3 tiles) doesn't match
  the triplanar wrap on the y axis.
- **`layered_rock_cliff_thficeas` dropped** from the rock role: its evenly
  spaced strata repeat as a brick grid that the 4x sample can't hide
  (they repeat at both scales). Rock keeps `rock_cliff_tg1mcgbl` and
  `layered_rock_vl0fdhdo`. 14 surfaces in all.
- **Cost** (Release 1440p, `--placement off`, 3 runs): open view 1.33 ->
  1.43 ms, high overview 3.22 -> 3.56 (distant cliffs take the second
  sample), full-ground start view unchanged (1.53). Device memory 4472 ->
  4216 MiB with one rock layer fewer.
- Triplanar on steep slopes (the Whiteout blend) was already in place
  since the first terrain material; with rock from 48 degrees only,
  every planar layer is under ~50 degrees, stretched at most 1.5x.

## Trees hidden by default (2026-11-20)

The Megaplants trees look bad (untextured leaves and bark, thin crowns),
so the game neither places nor loads them unless run with `--trees`. The
engine capabilities they exercise (cluster LOD and BVH, placement, TLAS,
the voxel far field plan) stay; no engine code, cook tool, asset or shader
was removed. Without `--trees`: no tree prototypes are loaded (startup
and memory drop), the forest and lone-tree layers are skipped (rocks stay,
21281 instances, placement hash 0x7e391ac57b8792dd), the terrain
material's `ForestMask` frequency stays 0 so the forest floor and path
mask is off and that ground gets the meadow layers, and `--scatter`
excludes the saplings, `island_tree_01` and the Megaplants. `--trees`
restores the previous behaviour exactly (2645325 instances, hash
0x8322bae25df9e142).

## DLSS DLAA with Ray Reconstruction is the default (2026-11-20)

*Superseded by "Super Resolution DLAA is the default, RR an A/B", then "Native 1440p with our own TAA is the default", then "DLSS DLAA is the default anti-aliasing" (2026-11-22), which holds.*

Superseded by "Super Resolution DLAA is the default, RR an A/B" (below):
the fallbacks and gate definitions here still hold.

- Default: `--aa dlss --rr on --dlss-mode dlaa` (preset F), 4.7 ms for the
  `aa` pass at 1440p. Fallbacks, each logged once at startup: RR
  unavailable -> Super Resolution DLAA preset M (`DLSS Ray Reconstruction
  unavailable; using Super Resolution`); DLSS unavailable or
  `MERIDIAN_DLSS=OFF` -> TAA v2 (`DLSS unavailable: <reason>; using TAA`).
  `--aa taa` and `--rr off` select the fallbacks.
- Why: user decision. RR removes the ray-traced shadow shimmer (beech
  crown 1.41 vs TAA 2.41 /255, shadow 0.35 vs 1.76), which the denoiser
  fallback (Tasks 3.2a-3.2b) is only a stand-in for. Optimise from the
  most demanding configuration downwards: the 4.7 ms is the budget to
  win back, not a reason to default to the cheap path.
- Motion gate redefined: the motion/still Laplacian ratio penalised RR
  for having much sharper stills (0.40 vs TAA 0.24) while its motion
  frames are as sharp or sharper. The gate is now the motion frame's own
  Laplacian std (`image-gates.py laplacian`), minimum 90% of TAA v2's
  value at 2560x1440, rounded down: forward 5 ground 0.10 (TAA 0.114),
  sideways 5 ground 0.10 (0.118), forward 5 beech 0.20 (0.226), pitch 0.02
  ground 0.09 (0.108). The ratio is printed as an informational column;
  shimmer and SSIM gates are unchanged.
- Measured at 2560x1440 (Release, `--placement off`, `--trees` for the
  beech views, identical `Render 2560x1440` lines): RR laplacian 0.128 /
  0.144 / 0.214 / 0.128 (ratio 0.84 / 0.94 / 0.86 / 0.84), crown/shadow
  1.41 / 0.29, SSIM 0.983, all pass. TAA v2: 0.112 / 0.117 / 0.226 /
  0.107 (ratio 0.94 / 0.97 / 1.02 / 0.89), crown/shadow 2.41 / 1.76 (crown
  fails), SSIM 0.979 (fails). RR is 5% below TAA on the beech forward
  frame and above it on the ground views.
- Startup: the window's size is the monitor mode when fullscreen
  (`Window::framebufferSize`; Wayland reports the tiled size until the
  first presented frame), and `Renderer` takes the `AaSettings` in its
  constructor, so the swapchain, render targets and upscaler are built
  once: one `Swapchain: ... 2560x1440` line. `--windowed` keeps the
  framebuffer size.

## Micro and contact shadows (2026-11-20)

- **Micro shadows**: the material AO also darkens the direct sun light
  (`microShadow` in mesh.frag, every surface including the terrain; not
  the leaf transmission, not ambient). The AO is read as the aperture of
  the cone the cavity leaves open around the normal; the sun is hidden as
  N.L drops below that aperture, `clamp(N.L * aperture)^2` (Chan,
  "Material Advances in Call of Duty: WWII", SIGGRAPH 2018, as in
  Filament). Uncharted 4 used the cruder `saturate(N.L + 2 ao^2 - 1)`
  (Brinck and Maximov, SIGGRAPH 2016). Why: texture-only cavities (AO
  baked into the maps, no geometry for the shadow rays) otherwise get the
  full sun. Per fragment, cheap and deterministic, so Ray Reconstruction
  sees no extra noise. Sources:
  https://google.github.io/filament/Filament.html ("Micro-shadowing"),
  https://www.activision.com/cdn/research/MaterialAdvancesInWWII.pdf
  On the terrain the term reads the AO relative to each layer's mean
  (its last mip; `terrainMicroOcclusion`): the Megascans ground AO also
  bakes the scan's large-scale occlusion (means 0.5-0.7), and fed raw it
  hid a 14 degree sun across open ground in large blotches (full-ground
  view, `--sun 0.9 0.25 -0.4`: 10.4/255 mean darker, 9-19% in sRGB per
  band; normalised, 4.8/255 with crevice detail kept, contact shadows
  included). Ambient AO keeps the raw value. Meshes keep the raw AO.
- **Contact shadows**: a compute pass (`contact_shadows.comp`) after the late
  scene pass and before the upscaler. Shading is forward: when a fragment
  shades, the depth buffer is incomplete (early-pass objects shade before
  the late ones), and last frame's reprojected depth would lag and ghost
  and needs a depth copy. With the complete depth the pass marches 16 steps
  toward the sun over `clamp(0.03 d, 0.25, 1.5)` m (d the view distance),
  start jittered per pixel (IGN, or pcg3d white noise under Ray
  Reconstruction, as the sun sample), in displayed space (the curvature
  warp is negligible over 1.5 m). A sample is occluded when the scene is in
  front of it by more than `0.01 + 0.002 d` m plus the depth step to the
  next pixel (a grazing surface's own slope) and less than
  `0.3 + 0.01 d` m; the effect fades from 1 at 40 m to 0 at 60 m. Binary
  per pixel per frame; the upscaler accumulates the jitter. To remove
  exactly the sun, the scene pass writes each pixel's exposed sun radiance
  (after haze and the RT, terrain and micro shadows, so the result is
  multiplicative with them) into an RGBA16F attachment (8 B/pixel;
  B10G11R11 rounds up by up to 1.6% and would remove more sun than the
  pixel got, tinting the shadow), and the pass subtracts the occluded part from the HDR colour, ambient
  untouched. Limits: screen-space only (off-screen or hidden occluders are
  invisible); the thickness is a heuristic; only geometry that writes depth
  casts (blended surfaces don't cast; one in front scales the sun behind it
  by its transmittance in the attachment, blend ZERO / ONE_MINUS_SRC_ALPHA,
  so glass doesn't darken; the POM terrain's relief
  doesn't cast, as POM writes no pixel depth offset; its own sun march
  self-shadows it); normal-map relief is the micro shadows' job.
  Sources: Bend Studio, "Screen Space Shadows" (Graham Aldridge, SIGGRAPH
  2023 Advances, open source),
  https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/; Unreal
  Engine contact shadows,
  https://dev.epicgames.com/documentation/en-us/unreal-engine/contact-shadows-in-unreal-engine

## Terrain parallax occlusion mapping (2026-11-20)

Up close the 4K ground scans read flat. Options compared for eye-height
views of 1-6 cm deep ground relief:

- **POM, linear search + secant refinement** (Tatarchuk 2006; "steep"
  POM refined between the last two steps): chosen. Converges on shallow
  relief in a handful of steps, no extra data.
- **Relief mapping's binary search / interval mapping** (Policarpo 2005,
  Risser 2005): more dependent taps for the same result at 1-6 cm; the
  secant step is interval mapping's first iteration.
- **(Relaxed) cone step mapping, quadtree displacement** (Policarpo and
  Oliveira 2007, Drobot 2009): fewer steps on deep relief, but a cone or
  max-mip map per layer in the cook; ground relief here is 1-3% of a tile,
  where the linear march already needs few steps.
- **Mesh-shader displacement of near CDLOD nodes** (Nanite-tessellation
  style): true silhouettes and depth, but mm-scale triangles within ~10 m
  (millions), the blended hex-tiled height evaluated per vertex, and the
  shadow march/TLAS don't see it. Silhouettes of 1-6 cm relief only show
  at grazing angles within a few meters. Worth it later for rock (10-15 cm),
  not for this.
- **Parallax self-shadowing** (Tatarchuk's horizon march toward the sun):
  added, ~0.1 ms of the cost below.

What it does (`parallaxHit`, `terrainSurface` in mesh.frag):

- **One height for every layer:** POM marches the dominant layer's height
  (top role at weight >= 0.5, its nearer variant), and every layer is then
  sampled at the shifted world position, so height blends and parallax
  agree. Marching the blended height would sample every active layer per
  step. The hex tiles are placed once at the unshifted position for the
  march, weighted without the colour term (no base colour fetches); the
  shift is at most a few cm against 0.2-1 m hex cells.
- **Shift in world space:** a point at depth d below the surface on the
  view ray is `-V d / (N.V)` away (N the macro normal, N.V capped at 0.2:
  at most 5x the depth sideways); the planar layers read its x, z.
- **Real depths:** each surface's Megascans JSON `displacementScale` (cm)
  is the height map's full range (`TerrainLayer::heightScale`, main.cpp's
  role table): grass 1.1-2.2 cm, gravel 1.5, mud 2.5, forest floor 2.9-3.7,
  soil 3.9-5.7, rock 8.9-15.
- **Fade by pixels, not meters:** the largest shift in pixels (over the
  pixel's longer axis) fades parallax out from 1.5 to 0.5 pixels and sets
  the steps (one per ~2 pixels, 2-12; the shadow march half that, at least
  3). With a 1440p 60-degree view that is ~40 m for 3 cm of depth, ~10 m
  for 1 cm grass; most of the cost stays within a few meters.
- **No POM on the triplanar rock** (and it fades with the rock weight):
  three projections triple the march, and the macro normal is a poor
  tangent frame on cliffs.
- **No pixel depth offset:** depth, motion vectors and the RR guides stay
  the raster surface's; the guides (normal, albedo) come from the shifted
  samples, so they agree with the colour.
- **Cost** (Release, 2560x1440 DLAA + RR, `--placement off`, scene early
  pass, median of 5): start view 1.56 -> 1.79 ms, ground close-up at eye
  height (pitch -0.5) 1.86 -> 2.20 ms; the sun march is ~0.1-0.15 ms of
  that. A fixed 8-15 m distance fade with 6-24 steps cost 0.9 ms on the
  close-up; the pixel-based fade and steps brought it here.
- **Motion** (ground close-up moving forward at 1.4 m/s, Laplacian ratio
  motion/still): DLAA + RR 0.862 -> 0.870, TAA 0.871 -> 0.859; shimmer
  unchanged (1.84 -> 1.82, 2.24 -> 2.27).

## Sun glare: energy-conserving multi-scale bloom (2026-11-20)

- Problem (user feedback): the sun reads as a small, hard circle. Its
  size is right (`sunAngularRadius` 0.00465 rad, 0.53 deg); a real sun
  looks bigger and edgeless because the eye's and lens's point spread
  function spreads its luminance (~1e5 times the sky's) into a glare,
  and the engine had no glare at all.
- Options: (1) threshold glow: not physical, makes bright surfaces
  hazy; (2) energy-conserving multi-scale bloom (Jimenez 2014, "Next
  Generation Post Processing in Call of Duty: Advanced Warfare"; UE's
  default bloom, Unity HDRP, Godot, Bevy): a downsample chain with a
  13-tap filter and a tent upsample that mixes the levels, a sum of
  Gaussians with a heavy tail, ~0.1 ms; (3) FFT convolution with a
  measured PSF (UE Convolution Bloom): the most faithful, ~1-2 ms and
  much more code; (4) physically-based glare with streaks (Spencer et
  al. 1995, Ritschel et al. 2009 temporal glare): the eye's corona and
  ciliary streaks, a look choice on top of (2) or (3).
- Choice: (2), no threshold, energy-conserving: `output` =
  mix(output, bloom, intensity), so a fraction of every pixel's energy
  moves into the kernel. The sun's FP16-capped value (sky.frag
  `kMaxExposedLuminance` 60000; measured 57888 at a high sun, where the
  physical pre-exposed value is ~4-5e4, so the cap barely binds) is
  enough: 1.5% of it spread over the chain still clips a wide core, so
  the disk outline disappears at noon; at dusk the reddened, dimmer
  sun keeps a visible disk with a small halo. Ordinary surfaces lose
  only `intensity` of their contrast.
- Where: after the upscaler on the display-sized `output` (the glare is
  wider than any upscaler footprint and must not be fed into its
  history), before the luminance histogram, so the auto-exposure meters
  the frame the eye sees. Half-display RGBA16F mip chain, 8 levels at
  1440p (down to 10x5).
- Tuning (`Bloom` in renderer.h, `--bloom INTENSITY SCATTER`): intensity
  0.015, scatter 0.7 (level weights (1 - s) s^k). 0.02 gave a slightly
  larger glare but dropped the start view's Laplacian std 2.2%
  (0.1338 -> 0.1308); 0.015 drops it 1.6% (-> 0.1316), within the 2%
  budget (1.7%, 0.1393 -> 0.1369, with the terrain POM and micro
  shadows).
- Cost: bloom pass 0.12 ms at 2560x1440 (RTX 4080, Release).

## In-game overlay: Dear ImGui (2026-11-20)

- **Dear ImGui 1.93.0** (FetchContent, pinned tag) with its GLFW and Vulkan
  backends, the latter on dynamic rendering (`PipelineInfoMain`, backend-owned
  descriptor pool, since 1.92). Immediate-mode widgets are the standard for
  engine debug and tuning tools; no alternative was weighed beyond that.
- **Where it draws**: after the tonemap, into the sRGB swapchain, in its own
  `overlay` pass (a second `vkCmdBeginRendering`, load op LOAD). Hidden, the
  game never calls `Renderer::beginOverlay()` and the pass does not exist,
  so there is no cost. Captures include the overlay. The swapchain encodes
  sRGB on write while ImGui colours are sRGB, so the style colours are
  linearized once at init.
- **Pass count**: it varies per frame now, so the timestamp readback keeps
  the count per frame slot.
- **Grade file**: `$XDG_CONFIG_HOME/meridian/grade.json` (default
  `~/.config/meridian/grade.json`), one flat JSON object, field names of
  `Grade`, vectors as `[r, g, b]`, bands nested, plus `sunAzimuthDegrees`
  (clockwise from north, -z), `sunElevationDegrees`, `haze`,
  `bloomIntensity` and `bloomScatter` (Reset: 0.015 / 0.7). Written by
  `gradeToJson`, read by a tiny tolerant parser (missing keys keep the
  default). Loaded only by interactive runs, so capture gates stay
  deterministic; `--grade FILE` loads it anywhere for A/B captures.
  Explicit `--sun`, `--haze` and `--bloom` win over the file.
- **Performance overlay**: F3, passive (no input while the cursor is
  captured). x% low = 1000 / mean of the slowest ceil(x% n) frame times, at
  least one frame; average = 1000 / mean frame time (not the mean of FPS);
  max = 1000 / fastest frame (`summarizeFrameTimes`, unit tested). Frame
  time is present to present: wall time between loop starts, so it includes
  every stall. CPU ms = that minus `Renderer::cpuWaitMs()`. The 10 s window's
  stats refresh 4x per second. F4 / `--benchmark` write per-frame CSV
  (`frame,frame_ms,cpu_ms,gpu_ms,<passes>`; GPU columns are the newest
  finished frame, 2 behind); a benchmark skips 60 warm-up frames.

## Live effect toggles (2026-11-20)

Parallax, its self-shadow, micro shadows, contact shadows and bloom are
subtle enough that "is it doing anything" needs a live A/B, so `Effects`
(renderer.h) sets them per frame through four floats at the end of
`SceneData` (parallax depth multiplier, 0 = POM off; self-shadow flag; micro
shadow strength, `mix(1, microShadow, s)`; contact ray length multiplier).
Contact shadows and bloom off skip their dispatches so F3 shows the cost
saved. Defaults reproduce the previous image exactly (TAA capture diff 0).
The grade file stores them as `effect*` numbers; the panel's A/B box and
Reset cover them; `--effects` is the CLI twin.

## Natural ground transitions (2026-11-20)

The user found the ground layer borders "griddy". Cause: every role weight
(soil, clover patches and the ~5 m border noise) came from one octave of
value noise on the square 40 m-cell lattice, cut by narrow smoothsteps and
a sharp height blend, so patches had stair-stepped, axis-aligned outlines
(`before/ground30.png` in the lane's artifacts). Slope and curvature were
already per pixel (normal map), not the vertex grid.

- **Noise:** `groundPatches` warps the domain once (two ~80 m octaves,
  +-30 m) and reads both the patch noise (~40 m) and the edge noise (~5 m)
  from `rotatedFbm`: two octaves, the second on the lattice turned 45
  degrees (scaled by sqrt 2), which maps the wrap lattice onto itself so
  the noise stays periodic, while the two grids' axes no longer line up.
  6 value-noise lookups instead of 2; a first version (warped 3-octave fBm
  per field, 18 lookups) cost twice as much for no visible gain.
- **Landform:** soil patches lean onto convex ground and clover into
  hollows (curvature added to the patch threshold), instead of random
  blobs.
- **Height blend depth per pair:** 0.3 for the soft ground roles (grass to
  forest floor), 0.1 kept for gravel, scree and rock (sharp stone edges),
  0.2 between variants of a role: wider, still height-driven borders.
- **POM border:** the march reads the dominant layer only, so the relief
  jumped where the dominant role or variant changed; its depth now fades
  to 0 toward those switches (dominant weight 0.75 -> 0.5, any role above
  0.25 -> 0.5, variant blend 0.25 -> 0.5), continuous on both sides.
- **Cost** (Release 2560x1440, `--placement off`, scene early, median of 5,
  interleaved A/B): start 1.88 -> 1.96 ms, open view 1.68 -> 1.75, high
  overview 3.96 -> 4.09, ground from 30 m 2.87 -> 3.08 (wider blend bands
  sample two layers on more pixels).

## Non-repeating cliffs (2026-11-20)

The user saw clear repeated patterns on the cliff sides: the triplanar
rock's 2-3 m tiles drew a grid from ~50 m out, which the 4x far sample
("Rock anti-tiling at distance") only softened.

- **Hex-tiled sides:** the two side projections (uv (z, y) and (x, y)) use
  the planar layers' hex tiling (Mikkelsen 2022; `sampleHexSide`: the same
  luminance-weighted blend, normals as rotated surface gradients) at every
  distance. The top projection keeps plain tiles plus its 4x far sample:
  its uv is (x, z), and the hex rows' period (256.3 tiles) can't match a
  wrap on both axes. Rock layers therefore wrap the camera offset's y by
  the hex row period and x, z by the column period (renderer_frame.cpp),
  so the sides stay seamless when the offset wraps. Projections under 1%
  of the blend are skipped. Heitz-Neyret histogram-preserving tiling was
  the alternative: a per-texture Gaussianised copy and LUT in the cook for
  the same visual result here.
- **Two rocks mixed at large scale, with a scale break:** the variants
  cross-fade over ~40 m noise instead of ~200 m regions, so one cliff shows
  both, and the second is sampled at 2x the tile frequency (a whole
  multiple keeps the hex wrap seamless), so their features never line up.
- **Macro variation (`weatherRock`):** strata as +-10% brightness and a
  slight warm/cool shift along a noise-warped height (value noise, not
  sines: regular stripes read as paint), and moss (up to 60%) in the
  texture's AO hollows and on ledges, in ~10 m patches.
- **Trade-off:** distant cliffs read flatter than the old grid (the hex
  blend averages the tile-sized contrast that drew it).
- **Cost** (Release 2560x1440, `--placement off`, scene early, median of 5,
  interleaved A/B, before the transitions change): start 1.87 -> 1.99 ms,
  open view 1.68 -> 1.81 (both have cliffs in view); up to 2 side axes x
  3 hex taps x 4 maps per rock variant, both variants on ~40% of the rock.
## Weather state and wind (M4 clouds, 2026-11-19)

Spec: `docs/superpowers/archive/specs/2026-11-19-clouds-weather-design.md`.

- **Weather state** (`weather.h`): coverage, type (0 stratus/overcast ..
  1 cumulus, above 1 reserved for cumulonimbus), precipitation (carried,
  unused in M4) and the 10 m wind (direction it blows toward, speed,
  gustiness). `Weather` blends to a target with a smoothstep (value and
  slope continuous, so C1 with the steady states around it); the wind
  direction turns along the shorter arc by angle (a vector lerp passes
  through zero on a reversal). Presets: clear (coverage 0), fair (0.35,
  cumulus, 3 m/s; the default), broken (0.65, type 0.6, 6 m/s), overcast
  (1.0, stratus, 8 m/s).
- **Determinism:** `update` takes the absolute game time; the wind offset
  and travel integrate in fixed 0.25 s substeps (midpoint rule) and are
  extrapolated from the last substep to the exact time with the same
  midpoint velocity, so offsets are continuous and the state at a substep
  time is bitwise the same for 1, 4 or 60 updates per game second or a
  600x time-lapse (tested). Blends are analytic in game time.
- **Fronts (Approaches 2):** a change is born on a front line perpendicular
  to the current wind, 60 km upwind of the world origin, and crosses the
  world at wind speed. The state at p is the history sample whose travel
  (integral of the 10 m speed) is the current travel minus p's downwind
  distance from the line, blended between neighbours; upwind of the line
  the current state, beyond the history the oldest sample. Calm wind stops
  travel (no division by speed anywhere), so the weather on the ground
  stays where it was. History: one sample per 1 km of travel, 256 samples
  (~255 km, the spec's ~250 km: the weather map window plus the horizon
  from 5 km up), kept oldest first in a fixed-capacity vector (dropping
  the oldest moves 4 KB once per km, and the GPU gets one contiguous
  array). The GPU copy stores travel relative to now (<= 0) so float
  precision doesn't degrade as total travel grows; `weatherAt`
  (weather.glsl) binary-searches it with the same rule.
- **Wind profile (Approaches 5):** speed by the neutral-stability power
  law from 10 m (exponent 1/7, altitude clamped to >= 1 m); direction
  veers clockwise seen from above with altitude (Ekman spiral, northern
  hemisphere) linearly from 0 at 10 m to 0.35 rad (20 deg) at 1 km, the
  top of the boundary layer, constant above. Both are time-invariant, so
  the offset at any altitude is the 10 m offset scaled and rotated the
  same way and only one integral is stored. The GPU `Wind` block (this
  frame's and last frame's, for motion vectors) carries direction, speed,
  gustiness, the 10 m offset, the gust phase and the profile constants;
  `gust` is value noise in 40 m cells carried by the 10 m wind, evolving
  along a phase (0.25 cells per game second, periodic every 256 cells so
  the wrap is seamless), amplitude = gustiness. Altitude means above the
  ground for vegetation and above sea level for clouds.
  - Ghost of Tsushima wind: https://gdcvault.com/play/1027350/Blowing-from-the-West-Simulating
  - UE WindDirectionalSource: https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/WindDirectionalSourceComponent
  - Power-law wind profile (1/7 for neutral stability): https://en.wikipedia.org/wiki/Wind_profile_power_law
  - Ekman spiral: https://en.wikipedia.org/wiki/Ekman_spiral

## Cloud noise and the weather map (M4 clouds, 2026-11-19)

- **Noise volumes (Approaches 1, Density):** generated on the GPU once at
  load (`cloud_noise.comp`, one dispatch, then box-filtered 3D mips by
  blits, `lod` in `cloudDensity` picks them). Base 128^3 RGBA8: R
  Perlin-Worley (Perlin fBm dilated by Worley fBm, `remap(perlin, 0, 1,
  worley, 1)`), GBA Worley fBm at 4, 8, 16 cells per tile; detail 64^3
  RGBA8: Worley fBm at 2, 4, 8 cells; curl 128^2 RGBA16F: the curl of 2D
  Perlin fBm (divergence-free, so the detail lookup swirls without
  bunching). World tiles: base 8192 m (64 m texels, ~2 km cells, sampled
  at twice the vertical frequency), detail 256 m (4 m texels), curl
  4096 m (sizes: "Cloud look development" below). Every lattice wraps, so all of them
  tile seamlessly.
  - Schneider, Real-time Volumetric Cloudscapes of Horizon Zero Dawn (2015): https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
  - Hillaire, TileableVolumeNoise (Perlin-Worley construction): https://github.com/sebh/TileableVolumeNoise
- **Density (`clouds.glsl` `cloudDensity`):** weather coverage as a
  plain multiplier times the cut of vertical profile x base shape (HZD:
  `remap(R, worleyFbm - 1, 1)`, stretched and cut as in "Shape" under
  Cloud march below), then eroded by the curl-distorted detail (150 m of
  distortion fading toward the top). Coverage is thresholded once, in
  the map; a second HZD-style `remap(shape, 1 - coverage, 1)` on top of
  it would punch holes in overcast (R spread over [0, 1] at coverage 1)
  and shrink fair well below its coverage, so coverage stays out of the
  cut. Erosion depends on type like the
  profile: cumulus wispy at the base and billowy above, stratus wispy
  throughout, carving up to 0.6 (cumulus) and 0.5 (stratus). The eroded density is scaled by 3
  and saturated at 1 (crisp edges, full cores). Output is in [0, 1]; the
  march scales it to extinction.
  `world.y` is the altitude on the curved shell, so the caller decides
  the curvature. Profiles are analytic smoothsteps by normalized height
  (stratus a soft slab, cumulus a flat base thinning upward; the base
  ramp multiplies after the shape cut, the thinning before it) instead of
  the spec's small type texture: two curves don't need a texture, and
  the type axis stays continuous. Layer per type: stratus 1.8-3.0 km,
  cumulus base 2.5 km with tops from 3.3 km rising to 4.5 km with
  coverage (towers), interpolated by type.
- **Weather map (Approaches 2):** every frame (`weather_map.comp`, pass
  `weather`, before the atmosphere LUTs), 512^2 RGBA8 over 60 km
  (117 m texels) centered on the camera, its corner snapped to whole
  texels in double so it doesn't swim. Per texel: the local state from
  `weatherAt` (history rule), weather noise at `xz - windOffset(3 km)`
  with a time coordinate (one lattice unit per 30 game minutes, so cells
  form and dissolve without wind), coverage as a saturating soft
  threshold `R = saturate((n - lo) / w)`, `lo = (1 - c)(1 + w) - w`,
  `w = 0.15` (coverage 0: R = 0; coverage 1: R = 1 everywhere, a solid
  overcast; in between cells grow from their cores; this replaces the
  spec's `remap(n, 1 - c, 1)`, which left R spread over [0, 1] at
  coverage 1), type the local type +-0.16 of 32 km Perlin variation,
  clamped to [0, 1] like the RGBA8 map stores it (the fallback too, so
  the edge matches; cumulonimbus types above 1 need a wider format
  later), precipitation the local value. The noise: inverted Worley fBm in 4 km cells times
  Perlin fBm 16 km wide (`mix(0.5, 1.5, p)`), mapped to about uniform
  with a logistic approximation of the normal CDF (mean 0.46 and
  single-octave deviation 0.18 measured on dumps; the deviation scales
  like a weighted mean of independent octaves), so the soft threshold
  covers about `c (1 + w) - w / 2` of the sky with R > 0.5 for either
  octave count. Measured fraction of texels with R > 0.5: clear 0,
  fair (0.35) 0.331, broken (0.65) 0.648, overcast (1.0) 1.0 (R = 1 on
  every texel).
  - Logistic approximation of the normal CDF (1.702): https://doi.org/10.3926/jiem.2009.v2n1.p114-127
- **Fallback beyond the window:** `weatherMapAt` reads the map inside its
  texel centers, beyond them the same noise with one octave instead of
  three. The map's outer 64 texels (7.5 km) blend to that one-octave
  value, so its edge texels equal the fallback and the boundary is
  continuous (checked: two maps one window apart differ across the
  shared edge as much as neighbouring texels do). Calibrating each
  octave count to the same distribution keeps coverage statistics equal
  on both sides.
- **Precision:** all cloud noise tiles with 2^18 m (262 km; the map's
  Worley cells, the base, detail and curl periods divide it), so the
  cloud wind offset is uploaded modulo 2^18 m, reduced in double
  (`Weather::windOffsetModulo`): positions keep 1.6 cm resolution after
  any amount of play instead of degrading with the unbounded float offset
  (6 cm after 1000 km). The time coordinate wraps the same way at its
  period (64 units, 32 game hours). The gust noise period now reaches
  wind.glsl through `GpuWind::gustPhasePeriod`, one constant instead of
  two copies.
- Cost: `weather` 0.11 ms at 2560x1440 (per-texel history lookup and
  3D Worley); noise generation at load is one dispatch.

## Cloud march, lighting and haze (M4 clouds, 2026-11-19)

- **March (`clouds_march.comp`, pass `clouds` after the late scene
  pass):** quarter resolution (640x360 at 1440p), half when the camera is
  inside the local layer or within 250 m of it (`cameraInCloudLayer`; the
  local layer is the type at last frame's camera through `stateAt`, with
  the tallest cumulus top). Separate targets per resolution (Task 4 keeps
  a history per resolution). Rays are straight in the displayed frame
  and the layer is the shell of the type envelope (1.8-4.5 km) around
  the planet center below the camera, so clouds curve under the horizon
  with M1's curvature warp; shell intersections use the stable quadratic
  with `c = (h - a)(2R + h + a)`, since `|o|^2 - r^2` loses everything in
  float at planet scale. Steps: 1.2% of the distance (about a
  quarter-res pixel), at least 60 m, widened only where the steps left
  (of at most 128) couldn't reach the segment's end; an earlier rule,
  the segment / 64, gave ~1 km steps at 30 km and striped mid-distance
  clouds. Each sample sits at a per-frame jittered fraction of its own
  step (the step boundaries don't depend on the jitter; Task 4 fix: with
  only the start jittered, later samples moved less and less where the
  steps split the remaining segment, so accumulation left static
  horizontal bands on mid-distance clouds), jittered by interleaved
  gradient noise per frame (no blue
  noise texture in the engine yet; IGN is the engine's existing
  low-discrepancy per-pixel sequence); the march stops at the scene
  depth, the sea-level sphere, 300 km, or transmittance 0.01.
- **Lighting:** extinction 0.06/m at density 1, albedo 0.99 (no edge
  darkening; "Cloud look development" below); sun transmittance by a 6-step light
  march (20 m doubling, 1.26 km); sun color from the transmittance LUT at
  the sample, zero in the planet's shadow (the LUT alone returns the
  transmittance to the ground there, which made clouds glow after
  sunset; the light march is skipped there and stops once it leaves the
  layer); dual-lobe Henyey-Greenstein (0.8 and -0.3, 30% backward);
  Wrenninge octaves, 4, extinction, contribution and eccentricity each
  x0.5; ambient = clear-sky irradiance / 2 pi (isotropic in-scattering from
  a uniform upper hemisphere) times a 0.35-to-1 bottom-to-top gradient,
  plus the ground bounce / 2 pi times the mirrored gradient;
  Frostbite's energy-conserving integration per step. Mean depth
  weights each step by its opacity contribution; 0 = no cloud.
- **Haze:** at the mean depth d, `L = L_haze(0..d) (1 - T) + T_haze L_cloud`
  (the background already carries the haze in front of the clouds, so
  the composite's `L + T x background` counts it once). Within the
  aerial perspective LUT's 128 km from it (as mesh.frag); beyond, the
  LUT's last value extended by the transmittance between 128 km and d
  (Bruneton's ratio of transmittances to the atmosphere's edge, taken
  upward) and the sky-view luminance, capped at the sky's own.
- **Shape (clouds.glsl):** the base Perlin-Worley shape measured only
  about 0.78-0.92 (6% below 0.8, median 0.84, 4% above 0.9), so the
  product coverage x profile x shape gave flat-topped, vertical-walled
  slabs. The shape is stretched over that range, lifted toward 1 for
  stratus (`shape = mix(shape, 1, 0.6 (1 - type))`; overcast had holes
  without it), and thinning x shape is cut at 0.6 for cumulus (gaps,
  domed tops where the profile thins) and 0.1 for stratus, interpolated
  by type: `density = coverage x baseRamp x saturate(remap(thinning x
  shape, cut, 1))`, then eroded. Coverage stays a plain multiplier (the Task 2
  ruling: thresholded once, in the map).
- **Composite:** replaced by the temporal resolve and depth-aware
  composite below.
- Cost (`clouds` GPU ms, 2560x1440, Debug build, one run each): fair
  ground 0.89, above the layer 0.84, inside (half res) 1.37; clear 0.46,
  the floor of marching empty layer through the weather lookups (the
  analytic fallback beyond 30 km, with its history search and Worley
  noise, on every step); sun below the horizon 0.47 (no light march).
  Levers: step growth and the step cap, a cheaper weather lookup beyond
  the window, fewer light steps.
  - Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite (2016): https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
  - Wrenninge, Kulla, Lundqvist, Oz: The Great and Volumetric (SIGGRAPH 2013 talk): https://doi.org/10.1145/2504459.2504518
  - Schneider, Nubis Evolved (2022): https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf
  - Bruneton, Precomputed Atmospheric Scattering (transmittance between two points): https://ebruneton.github.io/precomputed_atmospheric_scattering/
  - Jimenez, Next Generation Post Processing in Call of Duty: Advanced Warfare (2014, interleaved gradient noise): https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/

## Cloud temporal resolve, composite and motion (M4 clouds, 2026-11-19)

- **Resolve (`clouds_resolve.comp`, at the march's resolution):** each
  march texel is reprojected by its transmittance-weighted mean depth
  (clear texels: the deepest cloud in their 3x3, else at infinity) and
  the wind displacement since last frame, into last frame's view (each
  camera through its own curvature warp). History: 9-tap Catmull-Rom,
  clipped toward the 3x3 neighborhood's mean +- 1 sigma in RGBA
  (luminance and transmittance; Salvi's variance clipping as in
  taa.comp), blended with 10% of the current frame, 25% inside or near
  the layer, where clouds sweep across the view. A plain blend (no
  luminance weighting): clouds are smooth, so flicker suppression isn't
  needed and would bias bright edges. Depth is blended by opacity.
  Histories are ping-ponged per resolution and restart when the
  resolution changes (HZD/Frostbite-style reprojection; rejected:
  HZD's 1-of-16 updates, see the spec).
- **Disocclusion:** where scene geometry cut the march short (in front
  of the layer, or inside the marched segment) the march stores its
  depth d as -1 - d (-1: no cloud before the surface), so the clouds in
  front of the surface are kept but the texel is flagged as knowing
  nothing behind it. History whose bilinear footprint touches a flagged
  texel is rejected where this frame's march isn't cut, and a restarted texel
  takes its 3x3 tent-weighted mean (texels cut alike) instead of the single
  jittered sample, which otherwise left a noisy strip behind moving
  silhouettes for ~20 frames (wall corner strafe at 10 m/s, inspected).
  Neighborhood statistics use only texels cut like the current one, so a
  silhouette's box can't keep clear air over clouds.
- **Wind displacement:** `scene.clouds.windDisplacement` is computed on
  the CPU from the cloud wind offset modulo 4096 m (reduced in double,
  float-precise to ~0.25 mm), differenced with last frame's and
  re-wrapped (`std::remainder`); the 2^18 m noise offsets jump where
  they wrap, and the full offset loses float precision with play time.
- **Composite (`clouds_composite.comp`, full resolution, compute):** joint
  bilateral upsample (Kopf et al.) in its simplest, classifying form.
  Where all 4x4 texels around the pixel agree with its surface,
  Catmull-Rom weights (bilinear's slope breaks at texel centers showed as
  a static 4-px grid on smooth cloud gradients); otherwise the 2x2
  bilinear taps, which can't ring across a silhouette, each kept only
  where it agrees with the pixel's surface: a flagged (cut) texel is skipped for a sky pixel and read at
  its decoded depth for a geometry pixel; a cloud texel deeper than the
  pixel's surface counts as clear. A sky pixel whose four taps all saw geometry
  (thin gaps) uses the surrounding 4x4 texels that saw the sky. Blended
  premultiplied over HDR (read-modify-write as a storage image, which
  is why HDR and motion gained STORAGE usage). Where cloud opacity
  exceeds 0.5 the motion target gets the clouds' motion (previous -
  current UV) from the opacity-weighted depth with the current and
  previous camera plus the wind displacement, so TAA (and later DLSS)
  moves clouds with the clouds; reads depth, writes HDR and motion only.
- Measured (Debug, 2560x1440, Laplacian moving / still, clouds on vs
  `--clouds off`; whole frames are dominated by terrain, so crops
  isolate the clouds): fast turn (1 rad/s) whole frame 0.735 vs 0.735,
  sky rows 0-800 0.698 vs 0.685, horizon band rows 600-1000 (clouds,
  ridges, the wall's top) 0.728 vs 0.733; toward the layer (60 m/s)
  whole 0.757 vs 0.755, sky 0.996 vs 0.995, band 0.909 vs 0.908; across
  the base (250 m/s up) whole 0.709 vs 0.710. Silhouette in front of
  clouds drifting across the view (summit, `1119 1356 -431 2.5 0.12`,
  the 3 km wind 80% lateral; crop 1000x340+900+560, still camera so
  clouds-off is 1 by construction and the clouds-on ratio is the
  evidence): drifting / frozen 1.001 at ~10 m/s (`--time-scale 1.5`) and
  1.001 at ~100 m/s (`15`); crops show no smear or halo at the
  ridgeline. Strafing past the marble wall at 10 m/s with drifting
  clouds: whole 0.738 vs 0.738, wall-corner crop clean. Cloud motion vectors vs the sky's
  rotation-only motion: no measurable difference at normal speeds
  (clouds are kilometers away), +5% sky Laplacian at a 200x time-lapse
  (~1.4 km/s lateral drift), which also confirms their sign. Fly-through
  sequence across the quarter -> half switch: frame-to-frame MAE stays
  at its neighbors' level (no pop). `clouds` GPU ms: ground 0.82-0.97,
  above 0.87, inside 1.48-1.6 (Task 3: 0.89, 0.84, 1.37).
- Static bands and grid (Task 4 fix round 2): diagnosed on a frozen
  mid-distance cumulus (`1119 1356 -431 2.5 0.12 --time-scale 0`): the
  pattern didn't change between frames 120 and 121 (crop MAE 9/65535),
  survived 400 frames with 2% current weight and no clipping (so not the
  resolve), and grew without jitter: the march's start-only jitter was
  the cause. Per-step jitter removed the bands; the Catmull-Rom upsample
  cut the 4-px row structure (mean |2nd derivative| by row phase,
  max-min / mean: 0.38 -> 0.16, flat sky 0.02-0.07). Cost (Debug, median
  of 3, `clouds` ms): ground 0.91 -> 1.08, above 0.85 -> 1.05, inside
  1.49 -> 1.67 (about half each: the exact last step and the 16-tap
  upsample). The thin dark ribbons that remained on the undersides came
  from the density field (base ramp inside the shape cut); they are gone
  with Task 7's shape (flat-base profile, finer noise).
  With per-step jitter the single-frame noise alternates between
  neighbouring columns (IGN), so a restarted texel's 3x3 box mean showed
  vertical stripes behind a strafed silhouette; restarts use a 1-2-1
  tent instead, which cancels that alternation.
- Known gap: the cloud history is pre-exposed and not rescaled when the
  exposure changes (as TAA's).
  - Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite (2016): https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
  - Schneider, The Real-time Volumetric Cloudscapes of Horizon Zero Dawn (2015): https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
  - Salvi, An Excursion in Temporal Supersampling (GDC 2016, variance clipping): http://behindthepixels.io/assets/files/TemporalAA.pdf
  - Kopf, Cohen, Lischinski, Uyttendaele: Joint Bilateral Upsampling (SIGGRAPH 2007): https://johanneskopf.de/publications/jbu/

## Cloud shadow map (M4 clouds, 2026-11-19)

- **Beer shadow map (Approaches 4):** every frame in pass `weather`
  after the weather map (`cloud_shadow.comp`), RGBA16F 1024^2 over
  30.72 km across the sun (30 m texels), orthographic along the light,
  centered on the camera and snapped to whole texels in the sun's frame
  (in double, like the weather map), so shadows don't swim as the camera
  moves (high oblique view moving 100 m/s laterally: shadow-edge crop
  Laplacian moving / still 0.92 with cloud shadows vs 0.90 with clouds
  off, i.e. TAA's own loss, no swimming). Per texel one line through the
  layer envelope (1.8-4.5 km, flat world: the scene is lit in it), 32
  steps at their centers, no jitter (a static map has no history to
  average it): R front depth along the light (km from the camera's
  plane), G mean extinction from the first to the last cloudy step, B
  total optical depth, A 1 (0: not marched); `T(d) = exp(-min(maxOD,
  meanExt max(0, d - front)))`. Only lines that reach a receiver the map
  serves are marched: between sea level and the layer top, within half
  the map (15.36 km) horizontally from the camera, plus the 3 texels the
  blur and the bilinear read reach (a texel across the sun moves the
  line's ground track by its size / sin elevation). At a low sun most of
  the map's plane would otherwise serve terrain far along the sun,
  through the expensive analytic weather fallback (2 degree sun: weather
  pass 3.41 ms unbounded). The front depth stays within ~15 km + 4.5 km /
  sin elevation, ~100 km at 3 degrees, where fp16 rounds it to +-31 m
  along the light (~2 m vertically). `cloud_shadow_blur.comp` blurs G
  and B with a 5x5 binomial weighted by front-depth similarity (1 km),
  so clouds at different depths along the light don't mix; front stays
  the texel's; unmarched texels (A 0) don't contribute, and the read
  divides by the filtered A. Rejected: exponential shadow maps
  (over-darken over long distances, the spec's choice).
- **Low sun:** the map's weight fades with the sun, `smoothstep(sin 3,
  sin 6 degrees, sun.y)`, into the fallback, and below 3 degrees it
  isn't marched. Lower, its 32 steps per line span over 50 km of layer:
  the noise mip that step length selects erases the cumulus (fair, 2
  degrees: mean map T 0.98 against 0.06 with the mip capped at 1), and
  finer mips alias into blocky self-shadow on the clouds (seen at mip
  caps 1, 2.5 and 3.5). The fallback is continuous through the fade and
  holds its 3 degree value below it.
- The CPU twin (`beerShadowTexel` / `beerShadowTransmittance`) is tested
  against a brute-force optical depth: on a uniform column, optical
  depth within 5% of the column's at 10 depths from before it to 1.5 km
  past it; on a cumulus-like column (soft top, thinning base),
  transmittance within 0.05 at 10 depths through it and at 5 and 8 km
  (both fail without the max optical depth clamp). The
  linear-optical-depth model overestimates where a column thins (up to
  7% of its total optical depth on that column), the known cost of the
  format.
  - Hillaire, Physically Based and Scalable Atmospheres / clouds in Unreal Engine (SIGGRAPH 2020 course; Beer shadow map: front depth, mean extinction, max optical depth): https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf
  - UE volumetric cloud component (cloud shadow map): https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine
  - Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite (2016, cloud shadows on the scene from a transmittance map along the sun): https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
- **Basis (`cloudShadowBasis`, weather.h):** Frisvad's orthonormal basis
  from the sun direction (right x up = sun, forward = the light's
  travel), on the axes cycled (x, y, z) -> (z, x, y) so its one
  singular direction (n.z = -1) is the nadir, where the sun never lights
  anything. It neither flips nor spins as the sun crosses the zenith or
  the north horizon (tested in 0.5 degree steps: east to west through
  overhead, along the horizon through north, and up through north from
  -10 to 10 degrees; neighbouring axes within 0.999), and it stays
  orthonormal at 90, 45, 2, 0, -5 and -90 degrees for any azimuth
  (tested; a `cross(sun, worldUp)` basis is undefined overhead and flips
  across it).
  - Frisvad, Building an Orthonormal Basis from a 3D Unit Vector Without Normalization (JGT 2012): https://doi.org/10.1080/2165347X.2012.689606
- **Use:** `cloudShadow(world)` (clouds.glsl) multiplies the sun at
  every surface (mesh.frag, terrain too: the sun transmittance, so the
  ground bounce term is dimmed as well). The map fades out over the
  outer 10% of its half-size (1.5 km), across the sun and horizontally
  from the camera (13.8 to 15.4 km), into the fallback beyond (1 above
  the layer). One crossing of the layer passes `1 - k R`, R the weather
  coverage (`weatherMapAt`: the map inside its 60 km window, the
  one-octave analytic fallback beyond) where the sun ray crosses the
  middle of the layer. k is calibrated, not derived: coverage R = 1 is
  cloudy over only part of its area (the shape cut and erosion carve
  gaps), more so for cumulus; binned by type, k falls steeply only
  toward pure cumulus (the stratus shape floor fills the gaps below type
  ~0.8), so `k = 1 - 0.72 type^2.6` (refitted to the cloud look's
  shape, see "Cloud look development"). A low sun's ray runs S = layer
  height / tan elevation through the layer and crosses about n = S / L
  independent clouds; the n - 1 beyond the first pass the regional mean
  `1 - k(local type) c'`, c' the local state's mean coverage under
  weatherFromNoise's soft threshold over an about uniform noise
  (`c (1 + 0.15) - 0.075`). L falls with the local type, 4.9 km for
  stratus to 1.55 km for cumulus (`kCloudShadowCrossingLength`), fitted
  at 6.5, 10 and 20 degrees. Calibration: top-down captures at
  9 km (map weight 1 on every pixel), mean map T vs mean fallback T on
  the same pixels, 3 positions per sun elevation (residuals of the
  current fit: "Cloud shadow fallback refit"). On high oblique views
  (6500 m, pitch -0.25) swept from 0 to 20 degrees of sun, the mean
  applied transmittance 11.5-13.8 km and 15.4-17.5 km from the camera
  differs with the cloud pattern either way, except at 4-5.5 degrees,
  where in the one view measured per side (fair, looking toward and
  away from the sun) the near band was brighter by 0.03-0.22; the view's
  mean changes by at most 0.10 per half degree.
  Only the sun is dimmed: no local occlusion of ambient (spec). The
  cloud march's 1.26 km sun march adds the optical depth from where it
  ends (unless it left the layer) of `cloudShadow`'s blend: the map's
  near the camera, the fallback's mean beyond ~15 km horizontally and
  below 6 degrees of sun, so far and dusk clouds keep their
  self-shadowing (with the map's term alone they rendered too bright:
  sunlit tops in the sunset-from-above view 3.1k -> 2.1k cd/m^2). `--clouds
  off` turns the shadows off too (shadow size 0).
- Verified: shadows sit against the sun (top-down broken view, shadow
  mask with the sun 45 degrees east vs west: shifted 54 px each way,
  ~1.6 km, the cloud base-to-ground height at 45 degrees); overcast
  ground view loses the sun term (only sky light remains; exposure
  adapts); clear unchanged (MAE 0 vs Task 4); dusk (2 degrees) and sun
  overhead render without NaN.
- Cost (`weather` / `clouds` GPU ms, 2560x1440): Debug, before (Task 4)
  -> after: clear ground 0.09 / 0.61 -> 0.23 / 0.63, fair ground
  0.09 / 1.00 -> 0.34 / 1.02, broken above the layer 0.10 / 0.94 ->
  0.52 / 1.04, overcast 0.09 / 0.49 -> 0.48 / 0.44. Release (median of
  5): fair ground 0.34 / 1.08, above 0.40 / 1.00, inside 0.34 / 1.66,
  clear 0.24 / 0.60. By sun elevation (Debug, 1920x1080, weather pass,
  ground view): 40.5 degrees 0.28, 6.5 degrees 0.37, 4.5 degrees 0.52
  (the most texels marched over the longest lines), 2 degrees 0.12 (map
  not marched). Over the ~1 ms budget; levers, not applied: amortize the
  map (a quarter of the texels per frame), fewer steps (16), or a
  cheaper analytic weather lookup.

## Sky capture and ambient (M4 clouds, 2026-11-20)

- **Capture (spec: "Sky + cloud capture and ambient"):**
  `sky_capture.comp` marches the clouds from the camera into an RGBA16F
  octahedral map of the whole sphere, 64^2 (~2.8 degree texels; full
  sphere so downward reflections and a camera above the layer see the
  clouds below), in pass `weather` after the cloud shadow map. It stores
  only the clouds: in-scattered luminance with the haze in front (rgb,
  sun illuminance 1) and transmittance (a). Readers composite `sky x a +
  rgb` with the full-resolution sky-view LUT (`skyCaptureAt`,
  clouds.glsl), so a clear sky reads exactly the LUT (MAE 0 against the
  sky-view LUT alone) and keeps its horizon resolution. Coarse march: the
  view march's code (clouds_march.glsl), steps from 250 m growing 3% of
  the distance, at most 32, one fixed jitter per texel (no flicker, no
  history). Time-sliced like UE's real-time sky light capture: one texel
  of every 2x2 block per frame (all when the history resets: first
  frame, swapchain rebuild), so it lags at most 4 frames. Chosen over 6
  x 32^2: one 2D image in the bindless table, no cube faces or seams,
  bilinear-continuous over the upper hemisphere.
- **Approximations:** the haze in front of capture clouds is `sky (1 -
  T_haze)` from the sky-view and transmittance LUTs (the aerial
  perspective LUT covers only the screen), which assumes uniform
  in-scattering along the sky's whole path and so over-counts haze in
  front of near clouds toward the horizon; `transmittanceBetween` is
  grey (the channel average).
- **Ambient:** `sky_irradiance.comp` integrates the sky-view LUT with
  the capture over it (256 cosine-weighted directions) into
  `skyIrradiance.irradiance` (scene ambient, mesh.frag and terrain), and
  the LUT alone into `clearIrradiance`, which lights the clouds (the
  layer's top sees the clear sky; lighting clouds with the cloud-dimmed
  irradiance under them would feed back and darken overcast twice). The
  specular sky fallback in mesh.frag reads the capture in the reflected
  direction (costs under 0.02 ms). No SH-L1: directional ambient would
  change the clear image, and nothing reads it yet.
- Overcast needs no special case: from the ground the capture is dim
  cloud, ambient irradiance is ~1/3 of the clear sky's (0.025 vs 0.076
  x sun, green), the sun is in the shadow map's shadow, and auto
  exposure lands at EV100 12.2 (clear 15.2; sunny-16 tables put heavy
  overcast at EV 12-13). Fair and broken cumulus raise the irradiance
  (fair 0.106, broken 0.126 vs 0.076: bright cloud sides replace blue
  sky, the known broken-cloud enhancement of diffuse light). The capture
  is taken at the camera: above the layer the scene under an overcast
  deck gets the clear sky's ambient (backlog).
- Cost (2560x1440, GPU fast clock state; ~1.8x in the slow one):
  capture + irradiance 0.07-0.16 ms (fair ground 0.14, overcast ground
  0.07 since opaque rays stop early, overcast above 0.16). Clouds +
  weather are over the ~1 ms budget (fair ground 0.43 + 0.80); capture
  levers: 1/8 or 1/16 of the texels per frame, 16 steps.
  - Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite (2016; clouds and sky captured into a low-resolution cubemap, updated over frames, for the scene's ambient): https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
  - UE Sky Light, Real Time Capture (sky atmosphere and volumetric clouds captured and time-sliced over frames into the sky light): https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-lights-in-unreal-engine
  - Cigolle et al., A Survey of Efficient Representations for Independent Unit Vectors (JCGT 2014, octahedral mapping): https://jcgt.org/published/0003/02/01/

## Cloud look development (M4 clouds, 2026-11-20)

Matched side by side against the Chlorophyll Engine frames
(`assets/reference/chlorophyll/`: fair cumulus with bright tops and
flatter, light-grey bases, a high cirrus streak layer, a hazier
stratocumulus sky) and a sunset photo (`assets/reference/clouds/cloud2`),
under the game's grade; the earlier photo metrics
(`tools/cloud-compare.sh`, `tools/cloud-metrics.py`) stay for numbers.
The photos' tops measure 5.9-13x the sky beside them and their bases
2.1-3.4x (display-referred, whites clipped): bases are lit, not dark.

- **Material:** albedo 0.99 (Frostbite: rho = 1, sigma_s = sigma_t;
  Hess 1998 cumulus sigma_t 0.05-0.12 /m), extinction 0.06 /m at density
  1, no edge darkening (the octaves and the diffuse term replace HZD's
  powder). Dual-lobe HG 0.8 / -0.3, 30% backward (Frostbite eq. 18).
  Octaves: 4, extinction, contribution and eccentricity each x0.5
  (Wrenninge's defaults; contribution <= extinction factor is Frostbite's
  energy rule, which holds the sum to ~1.9x single scattering as tau ->
  0, so "energy-conserving" is approximate).
- **Diffuse multiple scattering:** the octaves still decay exponentially
  with the sun's optical depth, so cores and bases went grey and an
  overcast deck took only the blue sky ambient (it read as clear sky).
  Light that diffuses through a thick cloud falls off algebraically
  (two-stream: transmittance 1 / (1 + 0.75 (1 - g) tau)); an isotropic
  term `0.8 / (4 pi (1 + 0.3 tau))` (g_eff 0.6) is added to the octave
  sum. It made fair cumulus white with light-grey bases, as in the
  frames, and the overcast deck a neutral grey. With g_eff 0.85 (slope
  0.11) and weight 1 every sunset cloud lit flat orange with no shadowed
  side; 0.6 keeps the shading.
- **Ambient:** the clear sky's irradiance at the top, falling to 0.35 of
  it at the base, and to 0.05 under full coverage (`groundCoverage`):
  a deck's base sees other clouds, not the blue sky. Ground bounce: the
  atmosphere's ground albedo (Lambertian) under the sun through the local
  coverage's gaps plus the clear sky, E / 2 pi, 1 at the base to 0.35 at
  the top (HZD: ambient rises with height; Frostbite biases its height
  gradient for ground bounce).
- **Scale and edges:** base noise 8192 m (128^3, 64 m texels) at twice
  the vertical frequency: fewer, larger cells, 1-3 km cumulus with wide
  gaps, closer to the frames than the 4096 m field of many small cells.
  Detail noise 64^3 over 256 m (4 m texels; was 32^3 over 512 m) and
  cumulus erosion 0.6 (stratus 0.5) give cauliflower lobes and crisp
  inner structure up close at no measurable cost. Density gain 3 (eroded
  density scaled and saturated, Nubis' remap): 6 gave hard silhouettes
  and cost motion sharpness (see the trade below). Cumulus cut 0.6.
  Superseded by "Sharp cloud detail" below (billows, gain 6, cut 0.3).
- **Flat bases:** the cumulus base ramp multiplies after the shape cut,
  so every cloud starts at the layer bottom. Extruding the shape below
  10-25% of the layer gave straight-walled boxes, dropped (twice).
- **Self-shadowing:** 6 sun steps doubling from 20 m (1.26 km; was 5
  from 40 m): sharper lobe shading, +0.04 ms.
- **Stratocumulus / overcast:** the stratus shape floor is 0.4 (was 0.6):
  the deck keeps thickness variation, which the diffuse term turns into
  brighter and darker patches, without opening holes at coverage 1.
- **Cirrus (`cirrusLayer`, clouds_march.glsl):** a 2D sheet at 9 km
  composited behind the low layer in the view march (not in the sky
  capture, not shadowing). Gradient-noise streaks 16 km along by 1 km
  across the wind, bent by a 3 km warp, in 32 km patches; amount
  `smoothstep(0.05, 0.6, coverage)` (none in clear weather; broken and
  fair show it, overcast hides it); optical depth 0.6 x density over the
  slant path, single scattering with HG 0.75 forward (ice) and 30%
  isotropic, plus the sky ambient. The streak noise turns with the wind,
  so it doesn't tile with the wind offset's period (jumps once per
  262 km of travel). From 9 km up the sheet is not drawn. A tiled base-noise slice gave cellular blotches
  (mackerel sky) and long straight streaks a zoom-blur of rays.
- **March resolution:** half instead of quarter changed nothing visible
  on a near cumulus at 1:1 (the detail is in the noise, not the
  sampling) and cost +0.65 ms (fair 0.87 -> 1.51), so the march stays at
  quarter resolution.
- **Motion vs density gain (user decision):** fast turn (1 rad/s, view
  `1119 1355.9 -431 -1.55 0.15`), moving/still Laplacian ratio over the
  sky crop, Release 2560x1440 (before the finer detail): gain 6: 0.848,
  3: 0.869, 1.5: 0.890; clouds off 0.935. A higher gain sharpens edges
  in a still frame that the temporal resolve then cannot keep in motion.
  Gain 3 is the middle; with the finer detail noise the ratio is 0.880.
  Superseded by "Sharp cloud detail" below (gain 6, measured under RR).
- **Cloud shadow fallback refit:** the Task 5 procedure (top-down
  captures at 9 km, mean map vs fallback T where the map weight is 1, 3
  positions per sun elevation): `k = 1 - 0.72 type^2.6`, L = 5.5 km.
  Fallback minus map: fair -0.009 (40.5 deg), +0.026 (20), +0.006 (10),
  -0.053 (6.5); broken -0.008, +0.120, +0.077, +0.040 (the stale fit was
  -0.07..-0.16 fair, -0.06..+0.17 broken). L = 3.5 km made fair at 6.5
  deg and broken at 10 deg worse (-0.13, -0.08): the remaining error is
  the step from one crossing to many at ~26 deg, not L. Superseded by
  "Cloud shadow fallback refit" below (L by type).
- **Measured (Release, 2560x1440):** `clouds` ms, fair from 1500 m
  0.91 (median of 3; 0.83 at the rebase, before this round), cirrus view
  1.04, broken 1.16, overcast 0.60, sunset from above 0.99; `weather`
  0.35-0.8. Fast turn sky crop 0.881 (clouds off 0.935), band crop 0.890
  (off 0.898).
- **Contour bands at full render scale** (Task 5 note): the stacked
  shelves came from the old profile x shape product under the steep
  shape stretch, not from sampling; the flat-base profile and finer noise
  removed them. What remains is the quarter-resolution march's texel
  stair-step on small distant clouds (backlog).
- **Catmull-Rom upsample overshoot:** clamping to the central 2x2 range
  changed the sunset-from-above view by MAE 1.4e-4 with no visible ring
  removed, so the composite is unchanged. Superseded by "Cloud blue
  fringes" below.
  - Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite (2016, §5): https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
  - Wrenninge, Kulla, Lundqvist, Oz: The Great and Volumetric (2013, multiple-scattering octaves): https://fpsunflower.github.io/ckulla/data/oz_volumes.pdf
  - Schneider, The Real-time Volumetric Cloudscapes of Horizon Zero Dawn (2015): https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
  - Schneider, Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima Engine (2017): https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf
  - Patapom, Real-time Volumetric Rendering course notes (Revision 2013): https://patapom.com/topics/Revision2013/Revision%202013%20-%20Real-time%20Volumetric%20Rendering%20Course%20Notes.pdf
  - GameDev.net, Horizon: Zero Dawn cloud system thread (coverage vs density, height gradients): https://gamedev.net/forums/topic/680832-horizonzero-dawn-cloud-system/
  - Hess, Koepke, Schult, Optical Properties of Aerosols and Clouds (1998): https://lweb.cfa.harvard.edu/hitran/HITRAN2012/Aerosols/papers/hess_opac_1998.pdf

## Sharp cloud detail (M4 clouds, 2026-11-20)

User review of the look: clouds blurry and lacking detail next to the
Chlorophyll frames; smooth grey interiors, detail only at the rim, a
stair-stepped underside on a large backlit cloud, many identical small
cells toward the horizon. Views recreated from the user's screenshots
(the default start, `--weather fair` and `broken`, looking toward the
sun); sheets in the clouds worktree's
`.superpowers/sdd/2026-11-19-clouds-weather/artifacts/t7/sharp/`.

- **Cause of the blur:** the noise spectrum, not the march resolution.
  The base shape (Perlin-Worley, 8 km period) has nothing finer than
  ~500 m and the detail volume (256 m period) nothing coarser than
  ~128 m, and the detail only erodes where the shape is already thin:
  the gain saturates the rest, so bodies were smooth isosurfaces with
  smooth, height-only ambient. Half-resolution march on the same view:
  no added detail (less speckle) for +0.9-1.3 ms, so it stays quarter.
- **Billows:** the detail volume sampled again at 4x its period (lobes of
  ~100-500 m, vertically squashed with the base shape) erodes the base
  shape across the whole body (`kBillowErosion` 0.8, cumulus only:
  scaled by type), fading out over march lods 1-3 (coarse mips would
  only thin far clouds). It removes volume, so the cumulus shape cut
  drops 0.6 -> 0.3 (fair sky cloud fraction 0.224 -> 0.252 on the fair
  view; broken 0.759 -> 0.693).
- **In-cloud ambient occlusion:** one density tap 100 m above each
  lit sample scales its sky ambient by exp(-0.5 x optical depth over the
  probe), so crevices between billows and undersides darken instead of
  following the height gradient alone. This is the cloud's own ambient,
  not the scene's (backlog "Cloud occlusion of ambient" stays). Under
  overcast it stacks with `kAmbientBottom`'s darker bases.
- **Density gain 3 -> 6:** with billows to show, the higher gain gives
  crisp lobe surfaces and visible sunlit-vs-shadowed contrast. It trades
  motion: fast turn (1 rad/s, view `1119 1355.9 -431 -1.55 0.15`, DLSS
  DLAA + RR) sky-crop moving/still Laplacian ratio 0.916 -> 0.878
  (clouds off 0.978), band crop 0.936 -> 0.932 (off 0.944). Gain 4 was
  visibly softer inside. User decision, like the earlier gain trade-off.
- **Stair-stepped underside:** the cumulus base ramp (7% of the layer)
  times the gain was a hard plane, and the wispy (uneroded Worley)
  erosion near the base cut its rim into stacked ridges; finer march
  steps (30 m, 256 steps) left them unchanged. The ramp now spans 20% of
  the layer and cumulus erode billowy (inverted Worley) at every height.
- **Cost** (Release 2560x1440, DLSS DLAA + RR, `clouds` pass, single
  runs, spread between runs ~0.2 ms): fair from 1500 m 0.91 -> 1.28-1.54,
  broken 1.38 -> 1.15-1.21, start view toward the sun fair 1.06 ->
  1.22-1.49, broken 1.54 -> 1.26-1.33, overcast 0.65 -> 0.70, sunset from
  above 0.96 -> 1.07. The billow fetch and the ambient tap (a full
  density sample) cost; the higher gain ends rays sooner.
- **Cloud size variety:** the weather noise blends in a second Worley
  field with cells 2x larger where a coarse gradient mask (8 weather
  cells across) is high (smoothstep 0.5-0.7), rescaled about the mean
  0.46 by 1 / |(1 - m, m)| so the blend keeps one field's deviation and
  the coverage threshold stays calibrated. Each field is computed only
  where its weight is nonzero. The horizon now mixes regions of larger
  cumulus with fields of small ones instead of one cell size.
- A grey smudge in the sunset-from-above sky under DLSS RR only (absent
  with TAA, and before this change with RR) is RR reconstructing the
  cloud-free sky (backlog).

## Cloud altitudes (M4 clouds, 2026-11-20)

The layer was 1.8-4.5 km above sea level, only ~450 m
over the Napf ridges (~1.35-1.4 km). Fair-weather cumulus bases sit at
the lifting condensation level, ~125 m above ground per degree C of
surface dew-point spread; a Swiss summer afternoon's 10-15 degrees
puts them 1.2-2 km above ground. Now cumulus (type 1) 2.8 km base, top
growing with coverage up to 4.8 km (fair: ~1.2 km thick); stratus
(type 0) 1.7-2.8 km, above the terrain's high points; broken
(type ~0.6) lands at ~2.4 km base. Types blend linearly as before
(`cloudLayer`). Cirrus stays at 9 km.

## Cloud cost trims (M4 clouds, 2026-11-20)

No visible change on the fair and broken views:

- The sun march's steps after the first two and the ambient probe use a
  coarse density (the detail erosion's mean instead of the curl + detail
  fetches); behind transmittance 0.25 a sample's sun march is all coarse
  and skips the probe; the billow fetch is skipped where the unbillowed
  shape is already clear air (billows only erode); the cloud shadow map
  marches half its rows per frame (alternating; the blur runs over all of
  it); view-march step growth 0.012 -> 0.016 of the distance.
- Measured (Release 2560x1440, DLSS DLAA + RR, one run each):
  `clouds` + `weather` ms, fair from 1500 m 1.39 + 0.53, broken 1.44 +
  0.78, start view toward the sun fair 1.60 + 0.47, broken 1.56 + 0.75,
  overcast 0.80 + 0.50, sunset from above 1.37 + 0.42. Weather split on
  the fair view: map 0.14, shadow map + blur 0.21, sky capture +
  irradiance 0.16.

## Cloud shadow fallback refit (M4 clouds, 2026-11-20)

- **Why the map darkened at a low sun:** after the sharp-detail round
  the shadow map's mean transmittance on the fair top-down view at 10
  degrees was 0.65, against 0.89 when the fallback was last fitted.
  Measured on one view (fair, 10 degrees, (1119, -431)), changing one
  thing at a time: every row marched each frame 0.655 (same as half
  rows), the old 1.8-4.5 km layer 0.658, PNG instead of `.pfm` readout
  0.645, density gain 3 instead of 6 0.664, gain 3 and shape cut 0.6
  instead of 0.3 0.923. The lower shape cut (kShapeCut, "Sharp cloud
  detail") grows each cumulus taller and wider, so a slanted sun line
  crosses far more cloud, while a high sun sees about the same cover
  (40.5 degrees: 0.90). The darkening is real; the fallback's single
  crossing length (5.5 km) was fitted to the narrower clouds.
- **Refit:** the crossing length now depends on the local type,
  `mix(4.9 km, 1.55 km, type)` (`kCloudShadowCrossingLength`): fair
  cumulus (type 1) needs ~1.55 km and broken (type 0.6) ~2.9 km, from
  the fallback's response to L = 5.5 and 2.5 km at each elevation; one
  L can't fit both. k (`1 - 0.72 type^2.6`) is unchanged: the high-sun
  residuals didn't move. Fallback minus map, Task 5 procedure (3
  positions, Release 2560x1440): fair -0.008 (40.5 deg), -0.048 (20),
  -0.024 (10), +0.004 (6.5); broken -0.045, -0.027, -0.036, +0.002 (the
  stale fit: fair -0.013, +0.053, +0.223, +0.303; broken -0.037, +0.121,
  +0.194, +0.143 on 2 positions). Single positions spread by up to 0.1
  either way with the local cloud pattern (the third position's broken
  map is 0.06 brighter even at 40.5 deg, where L doesn't apply).

## Streamed placement: ground cover around the camera (M3, 2026-11-20)

The user rejected the procedural mesh-shader grass blades ("horrendous")
and set the target to the Chlorophyll Engine meadow frame
(`assets/reference/chlorophyll/flower-meadow-cumulus.png`): dense,
knee-high scanned grass tufts with flowers. Static placement over the
whole crop can't hold that: 4-6 clumps/m^2 over ~100 km^2 of grassland is
400-600M instances, against the 24-bit id space (16.7M) and the 1.5M
budget.

Practice:
- **Horizon Zero Dawn** (van Muijden, "GPU-Based Procedural Placement in
  Horizon Zero Dawn", GDC 2017): placement in compute, on tiles around
  the player, from density maps and a dither pattern (density = keep the
  pattern points under the threshold), regenerated as the player moves.
  Meridian's static placement already is this, applied to the whole crop
  at once.
- **Ghost of Tsushima** (Wohllaib, "Procedural Grass in Ghost of
  Tsushima", GDC 2021): grass on a tile grid around the camera, generated
  every frame, thinned with distance and shrinking blades before they are
  removed so nothing pops.
- **UE5 PCG runtime generation** (Epic docs, "Using PCG Generation
  Modes"): partitioned grids generated within a generation radius
  around generation sources, cleaned up past radius x a cleanup
  multiplier, one radius per grid size.
- **Chlorophyll Engine**: nothing public on its ground cover beyond the
  video. (Update 2026-11-21: his replies add that grass is alpha tested,
  left out of the shadow pass and shaded by contact shadows alone, and
  goes through the voxel path far away: baseline F17, F23, F26.)

Choice: any placement layer can be **streamed**
(`PlacementLayer::streamFullDensity > 0`). It is placed by the same
integer rules, dither patterns and hash as static layers. The difference
is that it covers a W x W window of 64 m cells around the camera, written
into a fixed ring of instance slots after the static ones. Only cells
that enter the window are regenerated. Inside the window, the instances
thin with distance: the dither threshold against a keep fraction (full
density, then 1/d^2), shrinking before they go (Ghost of Tsushima).
Being a prefix of a progressive pattern, a thinned set stays evenly
spread. Static and streamed instances share one id space and one cull
and draw path. Debug builds check every regenerated cell against
`evaluatePoint`. Streamed instances are not in the TLAS. They receive
sun shadows, but cast only screen-space contact shadows. Whether they
should be in the TLAS is open (plan task 4). Plan:
`docs/superpowers/archive/plans/2026-11-20-streamed-placement.md`.

- **Seamless thinning (2026-11-20):** the mat must leave no edge, ring
  or pop. The keep fraction is full density, then (fullDensity/d)^2,
  faded to 0 over the last 50% of maxDistance (was 20%), and an
  instance's scale is `smoothstep(0, 0.3, keep - threshold)` (was 0.15),
  so tufts shrink toward their base pivot over a wide band before they
  drop. Per instance and continuous in camera distance, no per-cell term
  (CPU reference, GLSL mirror and tests changed together). The fade
  factor goes from `loadInstance` (`LoadedInstance.fade`, 1 for
  non-streamed) through mesh.mesh / mesh.vert (flat varying, location 9)
  to mesh.frag, whose plant path blends the shading normal toward the
  terrain's (`terrainNormal()`, now outside the TERRAIN block) and the
  micro occlusion toward 1 as it goes to 0, so the last tufts shade like
  the ground. No prefetch margin: the slot update runs in the frame that
  draws it, before the cull, so a window of maxDrawDistance + bounds
  margin already holds every cell in reach at any speed (unit test along
  a 2.5 m/frame diagonal path); a 32 m margin tried first only grew the
  window to 7x7 (ring 4.21M -> 8.25M slots, ~230 MB) for nothing. Fading
  leaves keep direct sun on their back faces, since the blended normal
  turned off their transmission and they went dark.
- **Ground following:** near the camera the raster terrain adds up to
  0.3 m of detail noise that placement (on the bilinear heightfield)
  doesn't see. Short plants sank under it, and only their TLAS shadow
  showed: the "black clover". Streamed instances are lifted by the same
  displacement in `loadInstance` (`terrain_detail.glsl`).
- **Pattern cost:** best-candidate dither patterns take 8 i candidates
  for point i, which is quadratic. A 0.45 m footprint (~20k points per
  cell) took 90 s in Release. The candidate count is now capped at 512.
  Later points of dense patterns are slightly less even, and the static
  hashes changed.
- **Meadow setup** (Release 2560x1440, open view `1119 1356 -431 -1.55
  -0.02`), matched by eye to `assets/reference/target-meadow.png` (no
  ground showing in the clearings):
  - a short mat (the low clumps, 0.2 m footprint (0.14 m since
    2026-11-21, twice as dense; ring 6.8M slots, 2.2M live), full density to 30 m,
    gone at 120 m)
  - tall seed-head tufts in 15 m-scale patches (0.5 m)
  - hawkbit in clumped drifts (0.3 m, scaled 2-2.8x so the heads stand
    above the mat)
  - sparse clover

  Window 5x5 (7x7 since the prefetch margin), ring 4.21M slots, ~1.1M live, ~41k drawn instances, 3.3M
  triangles. Scene early 5.2-5.5 ms against 2.0 ms without the meadow,
  so the meadow costs ~3.3 ms. The stream pass is 0.00 ms when no cell
  changes.
- **Far field matched:** the terrain's grass role target colour is set
  so the bare terrain renders as bright as the mat. The measure is
  exposure-normalized mean radiance (.pfm, divided by a sky region's
  ratio), with and without placement, over the same pixels. Mat/terrain
  went from 0.50/0.46 (R/G, mid band) to 0.97/0.94, and from 0.42/0.38
  (near) to 0.87/0.90. Blue stays ~0.7 (haze). The terrain grass target
  went from linear (0.085, 0.12, 0.024) to (0.027, 0.034, 0.006): the mat
  is darker than its albedo because of self-occlusion.
- **Grade toward the target:** `Prototype::tint` multiplies a placed
  prototype's base colour (RGB unorm8 of tint / 2 in `PrototypeInfo`,
  passed flat from mesh.mesh). Without it the scanned grasses read olive.
  The target-meadow.png mat has linear R/G 0.76; ours had 1.09 and was
  2.4x darker in G, though part of that is exposure and the grade. Grass
  tint is (1.15, 1.9, 0.95), and the hawkbit tint (1.1, 1.0, 0.6) turns
  the white heads cream-yellow. The terrain grass target was re-matched
  to (0.037, 0.058, 0.0104). Exposure-normalized mat/terrain: R/G 0.95/0.98
  in the mid band and 0.83/0.89 near; blue 0.57 to 0.59, from haze on the
  bare terrain.

## Clouds and weather measured (M4 clouds, 2026-11-20)

- **GPU ms** (Release 2560x1440, DLSS DLAA + RR, `--placement off`, median
  of 5, `weather` + `clouds` passes / frame total): fair from the ground
  (`1119 1355.9 -431 -1.55 0.15`) 0.49 + 1.49 / 9.00; overcast (`1119
  1356 -431 -1.55 0.25 --weather overcast`) 0.50 + 0.81 / 8.26; above
  the layer (`1119 6500 -431 -1.55 -0.25`) 0.48 + 1.29 / 11.41; inside
  the layer (`1119 3200 -431 -1.55 0`, half-resolution march) 0.48 +
  1.89 / 10.82. Over the spec's ~1 ms (2 ms inside) on every view but
  overcast; the sharp-detail round targeted ~2 ms fair. Levers left:
  the 16-tap composite as a 9-tap bilinear one, an adaptive shadow-map
  step count, fewer sun steps far from the camera.
- **Motion gates** (`tools/motion-gates.sh`, same session, main 899fcba
  vs clouds): laplacian forward 5 ground 0.1286 vs 0.1284, sideways
  0.1397 vs 0.1395, forward 5 beech 0.2083 vs 0.2050, pitch 0.1288 vs
  0.1283; shimmer crown 1.42 vs 1.57, shadow 0.30 vs 0.28; SSIM 0.9815
  vs 0.9806. All pass; the ground views look down (little sky), so this
  checks that the clouds pass, the cloud shadow and the capture ambient
  cost no sharpness. 0 validation errors in every run.

## Plant micro occlusion (2026-11-20)

The Megascans plants have no AO texture, so streamed grass looked flat
under the sky ambient. Choice: a procedural height occlusion, `0.25 +
0.75 * smoothstep(0, 0.85, h)` with h the vertex height over the mesh
bound's top (mesh.mesh, object space; plants have their origin at the
ground), only for materials with diffuse transmission. It multiplies
`s.occlusion` (ambient, back irradiance, specular follow) and the sun
diffuse at 50% (self-shadowing inside a tuft). The micro shadow Effects
toggle and strength scale it (`micro=0` turns it off). Terrain and other
materials are unchanged. Contact shadows already act on grass (any depth):
the A/B at the open view shows clear grass-on-grass shadows, so the march
is unchanged (0.09 ms). The bust still shadows the grass through the TLAS
and the terrain march.

## Dither pattern disk cache (M3, 2026-11-20)

The dense streamed layers' best-candidate patterns cost ~10 s Release and
~59 s Debug at every startup ("Placed ... in N ms"). They are pure functions
of (layer id, point count, the algorithm's constants), so each is cached as
`assets/placement-cache/dither-<id>-<count>.bin`, like the `.clusters`
cache: header with magic, format version, seed, count, candidates per point
and the candidate cap, then the points (thresholds follow from the rank, so
they are not stored). Any header mismatch or wrong size is a miss; a miss
rebuilds and rewrites through a temp file and a rename (parallel runs never
read a partial file); an unwritable directory warns. The directory is passed
to `Renderer::setPlacement` (the engine has no asset root of its own; the game
uses `MERIDIAN_ASSET_DIR`), empty = no cache. Alternative rejected: a
cheaper algorithm, which would change every placed object's identity.
Result: Placed 10.5 s -> 63 ms Release, 58.9 s -> 128 ms Debug; placement
hash 0x341d51799d224d3c, placement and stream checks 0 mismatches.

## Terrain sun visibility per masked cluster (2026-11-20)

At a sun under ~1 degree the meadow made scene early 57 ms (open ridge view
into the sun, `1119 1356 -431 1.57 -0.02 --sun 0.9 0.0 -0.4`, Release
2560x1440 DLSS RR; midday ~6 ms). Bisect: the terrain march off took it to
6.7 ms, the TLAS query off left it at 55 ms, and without placement the same
view was 5.2 ms. So the cost was the heightfield march (Terrain shadows:
heightfield march) run by every alpha-masked fragment: grazing rays skim the
ridges and take the full 256 steps, and masked grass overdraw paid that many
times per pixel. The cost jumps between sun elevation 0.6 and 1.1 degrees
(scene early 62 -> 11 ms).

- **Choice:** a small alpha-masked cluster (bounding radius <= 1 m: grass,
  flowers, leaves at fine LOD) gets its terrain visibility from
  cluster_cull.comp, one march per drawn cluster from its bounds center
  (lifted to the bilinear height plus the shadow bias, like an object's
  march origin) through a point of the sun's disk fixed per cluster (a
  hash of instance and cluster): a per-frame point would flip whole tufts
  between lit and shadowed, a flicker the upscaler can't average like
  per-pixel noise, so the penumbra is a stable mix of lit and shadowed
  clusters. Blade tips and bases share the center's value (at 0.6 degrees
  0.25 m of height is 24 m of ray run). The result rides in the
  draw list entry's top 8 bits (0 shadowed, 1 lit, 255 = march per
  fragment); mesh.frag multiplies the TLAS query's visibility by it. The
  TLAS query stays per fragment. A cluster is about the size of a terrain
  shadow's penumbra (0.53 degrees times the occluder distance, 0.9 m at
  100 m) or smaller. Opaque surfaces (early-Z, about one march per pixel),
  the terrain and larger masked clusters keep the per-fragment march.
- **Rejected:** the march in mesh.mesh per cluster (32 lanes through 32
  disk points, averaged): scene early 17 ms, 7 ms of it the mesh shader
  waiting on the march latency (1 lane cost the same); in the cull pass
  the same work is 1.6 ms. A terrain shadow-height (horizon) map would
  make every surface's terrain shadow one fetch at any sun angle; it is
  bigger work for the remaining grazing cost (backlog).
- **Measured** (Release, 2560x1440, DLSS RR, scene early / cull clusters,
  ms): sun elevation 0 degrees 57.81 / 0.07 -> 9.92 / 1.86, 0.6 degrees
  61.74 -> 9.86 / 1.65, 2.9 degrees 10.44 / 0.07 -> 7.56 / 0.26, 5.7
  degrees 7.63 -> 5.43 / 0.24; full-ground view at 0 degrees 65.33 ->
  12.39 / 1.68; midday open view 5.93 -> 4.23 / 0.22. Images match the
  per-fragment march (mean difference 0.28-0.30/255 at 0, 0.6 and 2.9
  degrees). Frame-to-frame shimmer at 0.6 degrees, static camera, frozen
  time (mean |difference|/255 of two consecutive frames): near meadow
  0.445 -> 0.442, mid meadow 0.633 -> 0.639.

## Cloud march sub-texel jitter (2026-11-20)

User report (low sun, RR): clouds shimmer, blocky edges, dashed lines. The
quarter-res march cast the same ray through each texel's center every
frame, so the resolve could only accumulate along the ray: edges kept the
4-px stair-step and the cirrus streaks (finer than a texel toward the
horizon) broke into dashes, under RR, TAA and DLSS SR alike.

- **Choice:** each frame offsets every march texel's ray within the texel
  by the R2 sequence (`fract(0.5 + n (0.7549, 0.5698))`, n = frame mod
  64), and the resolve accumulates it into the texel unchanged (as TAA
  does with the camera jitter): each texel converges to its area's mean,
  so edges and streaks are antialiased at quarter resolution (no added
  detail; full-resolution reconstruction stays rejected, backlog). The
  along-ray jitter is interleaved gradient noise under RR too: RR's
  white-noise rule is for samples it denoises, and it sees only the
  resolved clouds. The variance clip widens from 1 to 2 sigma, which the
  spatial jitter's extra per-frame spread otherwise turned into flicker.
- **Measured** (Release, 2560x1440, RR unless noted; flicker = two
  consecutive frames of a static camera at `--time-scale 0`, sky rows
  0-45%, 4x4 box-blurred, p99 / p99.9 of |difference| /255; the mean is
  exposure drift): low sun (`1119 1356 -431 1.57 -0.02 --sun 0.9 0.01
  -0.4`) 2.13 / 5.75 -> 1.81 / 5.00; same view at midday 1.67 / 6.02 ->
  1.33 / 4.97; jitter alone (white noise, 1 sigma) 2.63 / 7.86. TAA,
  low sun: 1.10 / 4.01 -> 1.33 / 5.01. Sky Laplacian (rows 0-600) under
  a 1 rad/s turn unchanged (midday 0.0192, low sun 0.0176 -> 0.0175);
  still frame 0.0199 -> 0.0188 and drifting-cloud crop 0.0292 -> 0.0282
  (the stair-steps' edges gone). `clouds` ms unchanged (1.3-1.4).

## Cloud blue fringes (2026-11-20)

User report: blue dashed outlines along cloud undersides. Seen from above
at sunset (`1119 5500 -431 -1.55 -0.2 --sun -0.998 0.05 0.03 --weather
broken`) as saturated dark-blue texels where a lit cloud top meets the
dark underside of a cloud behind it. Both Catmull-Rom filters overshoot
there: the composite's 4x4 upsample, and the resolve's history after the
variance clip (a box of mean +- 2 sigma reaches below zero at such
edges). A channel below zero was clamped on its own, which left the
blue one.

- **Choice:** the composite clamps its Catmull-Rom result to the range of
  the central 2x2 texels (where it uses Catmull-Rom; the bilateral
  bilinear fallback can't overshoot), and the resolve clamps the history
  to >= 0 after the clip instead of before it. The fringes are gone in the
  view above; flicker and turn sharpness as in "Cloud march sub-texel
  jitter" (measured with both).

## Opaque clouds hide the sun disk (2026-11-20)

The sun disk (clamped to 60000 exposed, ~1e5 times the sky) showed
through thick cumulus and overcast: the march stops once the
transmittance falls below `kOpaque` (0.01) and kept that residue, and
60000 x 0.01 still tonemaps to white. The early-out now sets the
transmittance to 0 (the dropped in-scattering is under 1%). Broken
weather time-lapse with the sun behind a cloud (`1119 1356 -431 0.895
0.70 --weather broken --time-scale 60`, frame 12 of 20-frame steps): max
value around the sun 0.988 -> 0.675; overcast: the disk is gone. Thinner
cloud edges (transmittance above 0.01) still pass a dimmed disk, as real
thin cloud does.

## Super Resolution DLAA is the default, RR an A/B (2026-11-20)

*Superseded by "Native 1440p with our own TAA is the default", then "DLSS DLAA is the default anti-aliasing" (2026-11-22), which holds.*

Supersedes the default in "DLSS DLAA with Ray Reconstruction is the
default". User decision: raster rendering in the style of Chlorophyll
Engine (no ray tracing, art-directed), for image clarity, motion clarity
and performance. RR's reason to be the default was the ray-traced sun
shadow's per-pixel noise; the raster sun shadows that replace the ray
(spec `superpowers/archive/specs/2026-11-20-raster-sun-shadows-design.md`) have none to denoise.

- **Choice:** `--rr` defaults to off: DLSS Super Resolution DLAA, preset
  M (the SR default). `--rr on` stays as an A/B. Under SR the upscaler
  doesn't want guides, so the guide images aren't created, the jitter
  sequence has 8 phases instead of 32, and `SceneData.whiteNoise` is 0
  (interleaved gradient noise for the sun-disk sample, the cloud march and
  contact shadows).
- **Measured** (Release, 2560x1440, `tools/motion-gates.sh`, same build,
  `--rr on` vs off): `aa` 4.65 -> 2.35 ms, frame GPU 9.31 -> 6.84 ms (ground
  still, `--placement off`); Laplacian fwd5 ground 0.115 / 0.116, side5
  ground 0.127 / 0.119, fwd5 beech 0.209 / 0.214, pitch 0.114 / 0.113 (all
  pass); shimmer crown 1.79 / 1.73, shadow 0.29 / 0.36 (pass); the
  ray-traced shadow shimmers more without RR, as expected until the raster
  shadows land. SSIM fwd5 ground 0.959 / 0.960 fails the 0.98 gate under
  both, so it predates this change (backlog).

## Global illumination: terrain-following probes + screen-space bitmask (2026-11-20)

User direction: GI as a screen-space + voxel hybrid without ray tracing,
the architecture Chlorophyll Engine states. Spec
`docs/superpowers/specs/2026-11-20-gi-design.md`, behind `--gi
off|probes|hybrid` (default off until the look is accepted).

- **Options weighed:** per-pixel world traces with screen probes and a
  surface cache (Lumen's software path, AMD Brixelizer GI): the quality
  bar, but a deferred renderer's design whose spatial-temporal noise is
  what shimmers under foliage. Voxel cone tracing (VXGI, CryEngine SVOGI):
  noise-free, but every mesh voxelized into a clipmap, thin-wall leaks,
  cones per pixel. Radiance cascades: proven in screen space (2D/2.5D),
  world-space 3D cascades are memory-heavy. Probe volumes traced in
  software (DDGI's irradiance probes with Godot SDFGI/HDDAGI's
  no-RT traces): world-space, so stable in motion, and a few filtered
  fetches per fragment in a forward renderer.
- **Choice:** a camera-centred clipmap of SH L1 probes (5 levels, 2-32 m)
  whose vertical axis follows the terrain, traced against what Meridian
  already holds as exact voxel-like structures: the heightfield's max
  pyramid (`terrainOccludes`' march, now returning the hit distance) and
  the far-field world bricks of the trees (`farFieldTransmittance`). Ground
  hits read a 2D ground radiance cache (albedo from the rendered image,
  relit by the sun with terrain, tree and cloud shadows plus the probes'
  own irradiance, so bounces accumulate); misses read the sky-view LUT and
  the sky capture (clouds stay in ambient). Near detail below the probe
  spacing comes from a half-resolution visibility-bitmask pass (Therrien
  et al. 2023), which replaces the occluded share of the probe light by
  the bounce it sees on screen.
- **Why not a mesh voxel clipmap:** grass, flowers and the terrain
  material are colour on a 2.5D surface at the GI's scale; trees already
  have voxels; rocks are the only gap and the screen pass covers them near
  the camera (rock voxels are a follow-up).
- Sources: Therrien, Levesque, Gilet 2023 (arXiv 2301.11376); Wright et
  al., Lumen, SIGGRAPH 2022 Advances; Majercik et al. 2019 (DDGI, JCGT
  8(2)); Godot SDFGI and HDDAGI (PR #86267, probe traces 2.43 -> 0.93 ms
  on a GTX 1650); AMD FidelityFX Brixelizer GI; Tevs et al. 2008.
- **First version (`--gi probes`, 2026-11-20):** probes in a plain storage
  buffer (96 B header + 64 B per probe, stamped with the world column they
  were traced for, so a scrolled-in slot or a teleport retraces instead of
  showing stale light) and sampled with manual trilinear filtering in
  `giSample`, instead of the spec's 3D textures (no descriptors to add).
  The column base is the terrain under the probe, not the column's
  maximum (the maximum floated coarse levels' probes above slope
  receivers: sky leak). Ground hits: fixed meadow albedo (0.06, 0.09,
  0.03) lit by the sun with the terrain march, far-field trees within
  300 m and the cloud shadow, plus the sky irradiance; no ground cache or
  multi-bounce yet. Measured (Release, 2560x1440, `--placement off`,
  DLSS SR DLAA): `gi probes` 1.00 ms open view, 1.33-1.36 low sun, 1.38
  sunset; scene early +0.1-0.3 ms; low-sun shadow/sunlit ground
  luminance 0.118 -> 0.143. With `--trees` (forest interior, RR) 2.96 ms
  and the forest turns blue (backlog).

## Raster sun shadows: cached cascades (2026-11-20)

*Superseded by "Raster sun shadows are the default" (2026-11-21), which holds.*

User direction: raster rendering, no ray tracing. Replaces the per-pixel
TLAS shadow ray for objects (see "Shadows" at the top, which chose RT);
the terrain march, cloud shadow, contact shadows and the far path stay.
Spec `docs/superpowers/archive/specs/2026-11-20-raster-sun-shadows-design.md`.

- **Choice:** cached, camera-centred cascaded shadow maps (a coarse
  clipmap), not virtual shadow maps. VSMs cache per page and match screen
  resolution, but page marking, allocation and per-page culling are a
  large system, and Epic's docs name foliage and alpha-masked geometry as
  their expensive case. Shipped open worlds cache sun cascades: Insomniac's
  CSM scrolling (SIGGRAPH 2012), Crytek (Kasyan 2013) and Cyberpunk 2077
  (Dybisz 2021) refresh distant cascades periodically.
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine
  - https://advances.realtimerendering.com/s2012/insomniac/Acton-CSM_Scrolling(Siggraph2012).pdf
  - https://history.siggraph.org/learning/shadows-optimizations-in-cyberpunk-2077-by-dybisz-and-witanowski
- **Design:** 5 cascades, radius 20 * 4^i m, 2048^2 D32, windows snapped
  to whole texels of a fixed light-space grid (double precision), LOD cut
  at 1 texel (orthographic, view independent). The casters are static (no
  vertex wind), so a cascade re-renders only when the camera leaves half
  its margin (along u, v or depth) or on a sun or instance change: same
  texels, same LOD, so a re-render reproduces the overlap and the map
  doesn't swim. Rendering reuses the GPU cull chain on a SceneData copy
  (`shadowView`); the lookup is a 3x3 tent PCF from 9 gathers.
- **Default stays rt** for now: at the motion gates (2560x1440, SR DLAA,
  same build) raster vs rt: Laplacian fwd5 ground 0.116 / 0.116, side5
  0.119 / 0.119, fwd5 beech 0.198 (fails 0.20) / 0.214, pitch 0.113 /
  0.113; shimmer crown 1.71 / 1.73, shadow 0.42 / 0.36. Not better on
  shimmer yet, so `--shadows raster` is opt-in until tuned (bias, filter,
  resolution; backlog). GPU (one run each, Release, default placement):
  frame 11.6 / 10.2 ms at the ground view, 14.5 / 12.6 at low sun, 10.1 /
  10.4 at sunset; a full re-render of the 5 cascades on the first frame.

## Alpha-test coverage mips keep clear of the threshold (2026-11-20)

`coverageMip` (tools/cook/ktx2.cpp, every alpha-tested texture the cook
encodes) rescales each mip's alpha so the fraction of texels above 0.5
matches mip 0 (Castano). It took the smallest scale that reached the
target, which leaves the texels it just lifted at alpha 128: UASTC/BC7
error and bilinear filtering flip them back below the cutoff. A uniform
mip (a leaf atlas region coarser than its detail) then vanished entirely:
the textured test rowan's leaves disappeared beyond ~5 m, at every
geometry LOD (`--lod-error 0` too).

- **Choice:** every scale up to the next coverage step keeps the same
  texels above 0.5, so take the middle of that plateau (a second binary
  search for its upper end), capped at 1.2x the smallest scale (the
  lifted texels land at ~0.6, not saturated, so bilinear filtering
  doesn't grow the mask between texels). Coverage is unchanged by
  construction.
- Unit test: uniform alpha 0.87 lands at 148-158 in every mip (was 128);
  a holes pattern keeps 75% coverage while the holes are resolved.
- Already-cooked plants keep their files until they are re-cooked; their
  atlases have large transparent regions, so few of their mips were
  uniform.

## Textured tree import: Unreal glTF export, UVs kept (tree-import, 2026-11-20)

The Megaplants USD on Fab has no textures (flat colours, hence the tint
palette and Bark012 above); the textures live only in the Unreal asset.
The user opens the Megaplants in Unreal 5.7+ (Procedural Vegetation
Editor plugin, "Nanite Foliage (Experimental)") and exports them with
`tools/unreal/export_trees.py`.

- **Format: glTF.** Unreal's built-in exporter (`unreal.GLTFExporter`,
  `GLTFExportOptions`: PNG textures, material inputs baked to glTF PBR)
  carries materials and textures and needs no new dependency; the cook
  already reads glTF. USD with textures also works: usd2gltf now carries
  UsdPreviewSurface inputs connected to `UsdUVTexture` files (diffuse
  colour, its alpha as opacity with `opacityThreshold` as MASK cutoff,
  normal, roughness/metallic when they read G/B of one file, occlusion
  from R) as glTF textures that reference the files where they are.
  FBX only as a fallback if both fail: assimp via FetchContent would be
  the route; not added.
- **What a Megaplants asset is in Unreal is unknown until the first
  run:** the PVE docs say its Export node bakes Nanite Foliage (a skeletal
  mesh with micro-instanced assembly parts, Dynamic Wind compatible) or a
  plain static mesh; no Python API for assembly parts was found. So the
  script's first run is diagnostic (inventory.json: every asset in the
  pack folder, class, LODs, Nanite settings, material instance parameters
  and their textures, any level actors placing the pack's meshes), then
  exports each mesh (`export_source_model` first, since the render data
  of a Nanite mesh is the fallback mesh) plus every referenced texture as
  PNG with a role guessed from the parameter name. Placements of placed
  instanced meshes go to placements.json in glTF axes. Wind (bones,
  weights) is listed, not exported.
- **Cook: a material with a base color texture keeps its UVs.** Leaves are
  still simplified per piece and pruned with area compensation (pruning
  and scaling about the centroid never touch UVs); simplification adds UV
  as two attributes weighted by the piece's texel density, sqrt(surface
  area / UV area) / its largest extent: a UV error costs as much as the
  position error of the same length on the surface (meshoptimizer
  normalises positions by the extent and applies attribute weights as
  given). Robust to tiling bark UVs and atlas islands, unlike the UV
  span; degenerate UVs get weight 0. Rejected: simplify only the wood and prune whole leaf
  pieces, which keeps UVs exactly but would prune ~90% of the beech's
  pieces (46M source triangles for a 4M budget). Measured on the
  textured test rowan: the same 0.289 ratio and 0.94M leaf triangles as
  without UVs (Megaplants leaf UVs are near-affine over flat leaves).
- **Maps:** each textured material's maps are re-encoded to KTX2 (base
  colour, normal, metallicRoughness, occlusion; one texture per source
  image and use). A leaf base colour with alpha becomes alpha MASK at
  0.5, its alpha remapped piecewise-linearly so the source cutoff lands on
  0.5 (Unreal's default opacity mask clip is 0.333), with coverage mips;
  only when the source isn't OPAQUE (an opaque leaf's alpha may hold
  something else). A textured material without TEXCOORD_0, or reading
  another UV set (also through a texture transform), is an error.
  Leaves transmit by the source's `KHR_materials_diffuse_transmission`
  (its texture averaged over the opaque texels: the engine has no
  transmission map) or 0.35. Bark: the asset's own textures when the
  wood material has them; Bark012 (`--bark-texture`) only for untextured
  wood. Leaf detection: double-sided, alpha MASK, or a foliage-like name
  (leaf, foliage, needle, two-sided) when an exporter drops both flags.
  The tree parser accepts KHR_texture_transform (carried through),
  KHR_mesh_quantization and the material extensions the engine reads.
- **Untextured path unchanged:** the rowan sapling, beech saplings and
  beech cook byte-identically to the committed cooks (glTF, buffers,
  textures).
- **Test:** the rowan sapling USD with a layer adding UsdUVTexture inputs
  (a checker leaf atlas with alpha holes and a 0.3 cutoff; Bark012
  colour, normal and a packed ORM), through usd2gltf and the cook at 1M
  triangles: the checker keeps square cells on every leaf, holes cut
  through, and against a full-detail cook (3.46M) of the same input the
  tree's crop scores SSIM 0.9926 at 6 m and 0.9920 at 2.7 m (Debug,
  DLSS RR). Unit test: leaves of 4x4 quads with one UV cell each,
  simplified and pruned: each leaf keeps one affine UV map (equal
  surface/UV area ratio on every triangle) and its full UV cell.
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/GLTFExporter
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/GLTFExportOptions
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-vegetation-editor-in-unreal-engine
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-assemblies
  - https://openusd.org/release/spec_usdpreviewsurface.html
  - https://meshoptimizer.org/#simplification

## Native 1440p with our own TAA is the default (2026-11-21)

*The native 1440p resolution stands; the own-TAA default is superseded by "DLSS DLAA is the default anti-aliasing" (2026-11-22). The own TAA is the fallback.*

User direction: native 2560x1440, no upscaler, as the Chlorophyll Engine
renders (native 1440p, no upscaler, on an RTX 3080). `--aa taa` at render
scale 1 is the default; DLSS (Super Resolution DLAA, Ray Reconstruction)
stays behind `--aa dlss` as an A/B, so its backlog rows are A/B-only.

- The motion gates (`tools/motion-gates.sh`) are re-anchored at native
  TAA, Release 2560x1440, rt sun shadows, same build (the minimums are
  90% of each, rounded down):

  | gate | native TAA | minimum |
  |---|---|---|
  | Laplacian fwd5 ground | 0.101 (motion/still 0.93) | 0.090 |
  | Laplacian side5 ground | 0.101 (0.92) | 0.090 |
  | Laplacian fwd5 beech | 0.221 (1.04) | 0.198 |
  | Laplacian pitch0.02 ground | 0.098 (0.90) | 0.087 |
  | shimmer crown / shadow | 2.62 / 1.38 | target <= 2.0 |
  | SSIM fwd5 ground | 0.954 | target >= 0.98 |

  The ground Laplacians are lower than TAA v2's first anchor (0.114,
  0.118) because the Megascans ground replaced the old terrain material,
  not because TAA got softer. Crown shimmer and SSIM miss their targets;
  the limits stay as targets (backlog) for the TAA work that follows.
- `aa` costs 0.18 ms at 2560x1440 (ground view, 0.17 at the beech).
- Note 2026-11-21 (later): this entry's rationale ("as the Chlorophyll
  Engine renders, no upscaler") rests on press reports. His own replies
  state 1440p and no hardware RT but never "no upscaler"; he follows
  DLSS/FSR guidance and adds an alpha-grass mip bias and a motion smear
  mask (baseline F9, F16), so his AA method is unknown. The decision
  stands as the user's direction (native, own TAA); only the claim about
  his engine changed.

## Crown sky visibility (2026-11-21)

*Superseded by "GI: crown visibility only without GI (#30, 2026-11-23)", which holds.*

Foliage took the full sky and ground ambient at every leaf, so crowns read
flat and pale: forest edge foliage OKLCh L median 0.51 (p10 0.44) against
the Chlorophyll frames' 0.32-0.42 (p10 0.21-0.26), no pixel of the view
under L 0.3. The references' crowns are dark inside with sunlit tops.

- **Choice: an analytic crown medium, per vertex.** mesh.mesh treats a
  mesh taller than 4 m (half height > 2 m) as a crown: the ellipsoid
  inscribed in its bounding box (`MeshInfo::halfExtent`, new) filled with
  a uniform leaf medium. A leaf's sky visibility is the transmittance
  `exp(-2.5 t)` to the ellipsoid's surface (t in units of the half
  extents) toward the zenith and toward up-and-outward, averaged; leaves
  on the top and sides see the sky, those deep inside or under the crown
  see ~5-20%. mesh.frag multiplies `s.occlusion` (ambient, back
  irradiance, specular occlusion) by it for leaf materials only, scaled by
  the micro shadow strength (`--effects micro=0` is the A/B). The sun is
  untouched: its shadow ray already darkens the interior. It multiplies
  the plant height occlusion (which all leaf meshes keep); tufts (half
  height <= 2 m) get only that.
- **Why not a cook-time bake or ray-traced sky visibility:** a bake needs
  a vertex channel (the 48 B vertex has none free) and a re-cook of every
  tree, which is changing source (Megaplants); RT is off the default path
  (raster direction). The ellipsoid is what SpeedTree-style per-vertex
  crown AO approximates, needs no data, and costs nothing measurable (forest
  edge, Release 1440p TAA, back to back: 30.6 / 28.3 ms main against 30.1
  ms, within run-to-run noise). The Chlorophyll Engine bakes per-vertex
  occlusion in its LOD build (assumed from its look; unpublished).
  Update 2026-11-21: he states instead a per-asset voxel opacity volume
  giving each crown's light optical depth (baseline F21).
- **Result** (forest edge `600 1205.6 -400 -1.55 0.02 --trees`, Release,
  `--aa taa`): foliage L median 0.51 -> 0.42, p10 0.44 -> 0.26; image L
  p5 0.43 -> 0.25, std 0.14 -> 0.20 (references 0.15-0.25); dark pixels
  (L 0.15-0.3) 0 -> 8.5% at hue 146 (references 10-18% at 129-147).
  Exposure rises with the darker foliage (sky L 0.87 -> 0.92).
  Motion gates: crown shimmer 2.62 -> 2.70 (target 2.0, already
  missed at native TAA; the darker interior adds leaf contrast), the
  rest unchanged (ground views have no crowns).
- Left (backlog): far-field voxels and bark get no crown term.

## Masked depth prepass (perf, 2026-11-21)

Alpha-masked clusters (grass, flowers, leaves) were shaded in one pass
with `discard`, which turns off early depth testing: every overlapping
grass fragment ran the full shading (sun ray query, terrain march,
ambient, haze), and so did the terrain drawn first under the grass. At
the start view the masked shading was 2.6 of the 5.0 ms early scene pass
(measured by stripping it), the terrain under the grass ~2.1 ms.

- **Practice:** a depth prepass for alpha-tested geometry, then a shading
  pass with depth EQUAL and no discard, is the standard fix (UE's
  "masked in depth prepass", Frostbite and Decima foliage; the
  alternative, a visibility buffer, is VG slice 4).
- **Choice:** each scene phase draws the masked lists depth-only first
  (`mesh_masked_depth.frag`, the alpha test alone, no colour writes), then
  the terrain and the opaque lists, then the masked lists again shaded
  with compare EQUAL, depth write off, early fragment tests. Coverage comes
  from the prepass. Both passes use mesh.mesh, whose `gl_Position` is
  `invariant`, so the depths match bit for bit.
- **Measured** (Release 2560x1440, `--aa taa`): start view GPU 7.83 ->
  5.44 ms (scene early 5.06 -> 2.77), looking down into the grass 8.40 ->
  5.66 ms, forest interior with `--trees` 10.32 -> 9.19, start view with
  `--shadows raster` 9.18 -> 5.92. The forest edge (26.6 ms, 47M
  triangles) is geometry bound and unchanged. Captures differ from before
  by 0.02/255 mean.

## Exposure keeps the highlights (2026-11-21)

Mean-log metering alone raised the exposure as foliage got darker (crown
sky visibility): the forest edge's sky and clouds went to OKLCh L 0.92 /
0.96, where the Chlorophyll frames hold sky at 0.72-0.82 and cloud tops at
~0.87. exposure.comp now also finds the histogram's 95th percentile and
never lets it map above 2.0 pre-exposed (AgX white is ~3): the target is
the darker of the mean-to-grey EV and that bound. Views with plenty of
sky are unchanged (start view p95 0.69, image diff 0.008/255); the forest
edge goes 0.26 EV down (EV100 10.55 -> 10.81): image L mean 0.56 ->
0.53, foliage L median 0.42 -> 0.40 (references 0.50-0.55, 0.32-0.42).
A bound of 1.4 took 0.75 EV and left the image at L 0.46, darker than
every reference. Cost: one comparison per bin in a one-thread pass.

## Starting grade: neutral saturation, stronger greens (2026-11-21)

Measured offline (the tonemap pass replayed on captured `.pfm`s with
`bakeGradeLut` / `sampleGradeLut`, which match the GPU to 0.34/255) against
the Chlorophyll frames (display OKLCh, phone-capture borders cropped):
their sky has chroma 0.04-0.07 and a top-of-frame blue/red of 1.5-2.9;
at identity ours is already 0.04 / 2.0, but the starting grade's global
saturation 1.4 took it to 0.06 / 2.9-4.5 (summit view). Their foliage is
chroma 0.055-0.12 at hue 123-133, grass hue 117-125; ours at identity is
chroma 0.047 at hue 145 (AgX's inset mixes the other channels into each,
and the leaf colour leans blue), grass 118. So `kStartingGrade` becomes
contrast 1.4, saturation 1.0 (the user's own saved grade had already
turned it down to 1.0), greens -10 degrees and 1.5x chroma (was -12 /
1.15). Summit view: sky chroma 0.062 -> 0.051, blue/red 4.52 -> 3.45;
foliage hue 124-126, grass 111; forest edge foliage chroma 0.079 at hue
139 (references 0.055-0.086 at 123-133). -15 degrees pushed the grass to
112, off the references; the greens band (140 +- 45) can't rotate leaves
further without rotating grass too. The engine default stays identity.
Interactive runs load the user's grade file, which overrides this.

## Textured beech from Megascans scans, no Unreal step (assets, 2026-11-21)

*Superseded by "Tree leaves as opaque geometry, traced from the atlas" (2026-11-22), which holds.*

The Megaplants' own leaf and bark textures exist only in Unreal; Megascans
scans already downloaded hold a leaf atlas and beech bark.
`tools/usd2gltf/texture_tree.py` maps them onto the usd2gltf glTF:

- **Leaves:** Megaplants leaves are modelled outlines with UVs into an
  atlas we don't have. Each leaf piece (connected component; vertices
  sharing a position connect) is fitted to one leaf of a Megascans atlas:
  PCA long axis -> the atlas leaf's vertical, the petiole end (a run of
  thin width bins at one end, else the end the area centroid leans to)
  -> the atlas leaf's base, the blade's extent -> the atlas leaf's box
  (found as connected components of the opacity map). Then every vertex
  at one position gets one UV: the source's few petiole seams per leaf,
  multiplied over 140k leaves, locked 78k vertices in the cluster LOD
  and cost 15M extra triangles at the forest edge (62.0M vs 47.4M); welded,
  51.6M. Atlas for the beech: `unidentified_tree_leaves_pecjv2` (ovate,
  smooth margin); roughness factor = the atlas's mean roughness (0.57).
  Opaque leaves (the geometry is the outline), the atlas's normal map.
- **Bark:** Megascans `beech_bark_thspcaes` (olive-grey, lichen; B, N,
  ORM as glTF base colour, normal, ARM), replacing Bark012's pink-red.
  The smooth light-grey `wmohcgfn` read white in sun.
- **Measured** (Release, 2560x1440, `--aa taa --trees`, one run each):
  forest edge 30.4 -> 31.7 ms (47.4M -> 52.6M triangles); open view
  toward the summit 8.65 -> 9.54 ms (13.3M -> 17.2M; the cook keeps
  146k of 147k leaf pieces against 140k untextured, and they reduce less
  at coarse cluster levels); the forest overview looks the same (beech
  crowns there are already gone, below). Only `--trees` runs pay it.
  Look: grey-olive trunks instead of orange-red, leaves with venation
  and per-leaf colour from the scan instead of a flat tint.
- **Maple and oak stay untextured.** Textured the same way (assorted
  leaves `ojioj2` palmate leaves, oak `qgkpd2`, bark `vmhkcdsg`) they look
  better up close but fewer of their crowns survive at 300-600 m in the
  forest overview (meshoptimizer's sparse simplification drops whole leaf
  pieces at coarse cluster levels, and the textured cooks prune
  differently), so the swap would regress the far look.
- **Not solved by any texture:** past ~200 m every Megaplants tree is a
  bare pole: the cluster LOD removes the modelled leaves. That is the
  mid tier's job (rasterized voxels, far-m lane), or card-leaf trees
  (Megascans Trees packs, Unreal only); `--trees` stays off by default.

## Cloud temporal stability under TAA (2026-11-21)

User request: volumetric clouds free of flicker while drifting
(`--time-scale 1`) and turning, at native 1440p TAA. Flicker metric
(`tools/cloud-flicker.py`): static camera, `--time-scale 1`, 240 frames,
the last 120 scored, consecutive-frame |luma difference| /255 after a 4x4
box blur, p99 over the sky crop (rows 0-45%) at the ground view
`1119 1355.9 -431 -1.55 0.15`, the whole frame in the layer
`1119 3200 -431 -1.55 0` (its terrain alone, `--clouds off`: p99 0.29).
Bar: the 2.5D path's p99 0.55.

- **Cause, measured:** a static ground camera sees clouds 5-15 km away
  drift ~0.05 px per frame, so reprojection is not the source: the
  resolve's output moves by its current weight w times the current
  sample's distance from the history. Halving w (0.1 -> 0.05) halved the
  ground p99 (1.33 -> 0.64). A heat map of the mean frame difference put it on the
  silhouettes, where the R2 sub-texel offset over the whole texel swings
  a texel between clear and opaque from frame to frame.
- **Choice:** w 0.05 (0.125 in the layer, from 0.25) and the sub-texel
  offset over the texel's central half (`0.25 + 0.5 R2`): the texel still
  integrates an area (no stair-steps at 2x zoom), the edge swing halves,
  and the clouds read a little crisper. The variance clip (2 sigma) keeps
  turns from ghosting. With the longer history the cloud history is now
  rescaled by exposure / previousExposure (it is pre-exposed), as
  taa.comp does.
- **Rejected:** per-texel golden-ratio along-ray jitter from a hashed
  start (worse: ground p99 1.53, layer 2.30, it maximises the
  frame-to-frame step); a 20 m minimum step in the layer (no change:
  0.86).
- **Measured** (Release, TAA, 2560x1440): ground p99 / p99.9 1.33 / 6.23
  -> 0.51 / 2.90; in the layer 1.86 / 6.27 -> 0.85 / 3.49 (at
  `--time-scale 0` 0.76; with the along-ray jitter fixed at 0.5 it drops
  to 0.50 but bands, so the in-layer remainder is the along-ray jitter
  over long near-horizontal steps). Cloud crop Laplacian, still 0.0781
  -> 0.0783, 1 rad/s turn 0.0203 -> 0.0191. `clouds` 1.74 -> 1.70 ms at
  the ground view.

## Crown sky visibility for bark (2026-11-21)

*Superseded by "GI: crown visibility only without GI (#30, 2026-11-23)", which holds.*

Under the darker crowns the trunks and branches stood out pale pink-white
(forest edge), where the references' trunks are dark. A placed tree's
non-leaf meshes now take the crown of the prototype's leaf mesh (mesh.mesh
finds it once per cluster), and mesh.frag applies the crown term to every
material (1 where there is no crown). The visibility gained a third, low
ray (elevation ~17 degrees), and the outward direction leans on the
vertex normal, so a trunk on the crown's axis looks out the way it faces:
with two rays a lone tree's trunk went black on its shadow side. Forest
edge image L mean 0.53 -> 0.50 at the same exposure (EV100 10.81),
dark pixels (L 0.15-0.3) 11% -> 13%; the summit view's lone tree keeps a
brown shadow side.

Far-field voxels keep occlusion 1: the vertical transmittance through the
bricks above the voxel hit (farMarchCell straight up) changed the forest
overview and a 1500 m view by 0.15/255 mean, under 1% of pixels by more
than 3/255. The hit sits at opacity 0.5 near the canopy top, so little is
above it, and far cells are kilometres away under haze.

## TAA: sharpen in proportion to motion (2026-11-21)

Native TAA loses 7-10% of its Laplacian in motion (forward 5 m/s 0.93 of
the still, pitch 0.90): each frame resamples the history at a fractional
offset (Catmull-Rom), and the blur compounds. A still frame has none of
that loss, and sharpening it only amplifies frame-to-frame noise (TAA v2:
CAS's weakest lobe raised still shimmer from 1.7/1.5 to 2.3/2.4).

- **sharpen.comp scales CAS by the pixel's motion**: strength
  `0.25 * clamp(motion px / 0.5, 0, 1)` of CAS's weakest lobe (-1/8);
  a still pixel is copied, as before. UE4's TAA answers motion blur by
  shortening the history with speed (TAA v2 already does, up to 0.3);
  this compensates the output instead, so nothing feeds back.
- Release 2560x1440, rt shadows, gates (before -> after): Laplacian fwd5
  ground 0.101 -> 0.108 (motion/still 0.93 -> 0.99), side5 0.101 ->
  0.108 (0.92 -> 0.99), beech fwd5 0.221 -> 0.229, pitch 0.098 -> 0.103
  (0.90 -> 0.95); still shimmer 2.62 / 1.38 unchanged; SSIM fwd5 0.954
  unchanged; `aa` 0.18 -> 0.20 ms. Strength 1 (full weakest lobe at 2
  px) overshoots: motion/still 1.35 and SSIM 0.945; 0.2 gives 0.98.
- Tried and dropped: depth-dilated motion (nearest depth in 3x3, Karis
  2014) lowered forward / sideways sharpness to 0.097 / 0.096 (the
  neighbour's motion misregisters continuous ground); restricted to
  silhouettes (10% depth step) still 0.0995 / 0.099, since grazing far
  terrain reads as a silhouette. No gate showed an edge benefit.
- SSIM fwd5 (0.954) is not an exposure artefact: the forward run ends at
  EV 13.81 vs the still's 13.92 (+8% brightness), but matching the means
  only lifts it to 0.955. It is the grass texture's fine detail.
- Sources: https://github.com/GPUOpen-Effects/FidelityFX-CAS ; Yang et
  al. 2020, "A Survey of Temporal Antialiasing Techniques" (EG STAR).

## Mid tier: rasterized prototype voxels (M2a Task 11, 2026-11-21)

*To be superseded by the DAG voxel foliage plan (branch `g1`) once it merges; until then this entry holds.*

- **Chosen (user, 2026-11-20): rasterize the prototype voxels the Nanite
  Foliage way**, replacing the per-instance box + fragment DDA (measured
  6.7-85 ms, gate 1.5). Epic (UE 5.7 Nanite Foliage docs): voxels are 4^3
  bricks in the same hierarchy as the triangles, binned separately,
  depth-bucketed and rasterized, no ray march. Chlorophyll generates its
  voxels inside its meshoptimizer LOD build.
- **How.** cull.comp lists a tree past the switch (its prototype's finest
  voxel, height / 128, under 2 px; both representations over a 10% band
  with complementary per-pixel dither) as (instance, mip), the mip the
  coarsest whose voxel is <= 2 px (`kVoxelMipPixels`). voxel_bricks.comp,
  one workgroup per tree, frustum-tests each occupied brick, marches the
  terrain's sun visibility once per brick, and lists the voxels that
  draw, compacted 64 per mesh workgroup. A cube of edge v seen along unit
  d (prototype space) covers v^2 S on screen, S = |dx| + |dy| + |dz|, and
  its mean chord is v / S, so each voxel is a camera-facing quad of that
  area drawn with opacity `1 - exp(-sigma v / S)`, decided by a hash of
  (tree, voxel): stable from frame to frame (no temporal noise for TAA to
  smear), and over many voxels the coverage is the baked extinction's.
  Voxels whose baked optical depth toward the camera exceeds 3.5 (< 3%
  visible) are skipped: the build stores per voxel the optical depth from
  its center out of the box along the 6 axis directions (4 bits each,
  0.5 steps, in the occlusion word), blended by d^2 like an ambient cube.
  The same depths toward the sun shade the crown itself; the sun ray then
  starts where it leaves the box (cascades or TLAS, as other surfaces).
  Fragments shade flat at the voxel center (normal, palette albedo, baked
  sky occlusion), exact depth and static motion.
- **Measured** (Release 2560x1440, `--aa taa --trees --shadows raster`,
  one run each, clusters = `--voxel-trees off`, frame GPU ms): forest
  overview 14.68 -> 13.41 (scene early 9.64 -> 3.73, voxel trees 3.73,
  cull early +1.55; 5.19M voxels from 1.57M bricks of 151.7k trees),
  forest edge 21.05 -> 20.53, summit 11.34 -> 11.44 (run-to-run noise;
  0.56 cull early + 0.71 voxel trees against 1.4 ms less scene early), high
  overview 12.55 -> 11.84. With `--shadows rt` (one TLAS ray per voxel
  fragment from where the sun leaves the box) the overview cost 15.84 ->
  17.14 with the untextured beech (voxel trees 6.59); after the textured
  beech (heavier clusters) voxels win there too: overview 18.97 -> 17.31,
  edge 31.75 -> 28.97 (2 px switch). The steps
  that got there at the overview (voxel pass): a mesh workgroup per brick
  at 1 px voxels 32.6 ms with the list capped; 2 px mip and the
  hidden-voxel skip 15.7; compaction to 64 drawn voxels per workgroup and
  the terrain march per brick instead of per fragment 7.6; no discard in
  the main pipeline (early depth) 3.7; one list reservation per 64 bricks
  cut the expansion 2.0 -> 1.6 ms. The skip did not pay alone while every
  brick was a workgroup (the workgroups were the cost, not the quads).
- **Motion** (summit view `1119 1356 -431 -1.55 -0.02`, raster, TAA,
  forest crop 1560x300+1000+600): Laplacian turning at 0.3 rad/s 0.424
  (still 0.432) against clusters 0.315 (still 0.302); shimmer between
  frames 120 and 121 1.41% against 1.38%. The coverage hash is fixed per
  tree and voxel, so the voxels hold still under TAA. A fly-toward
  sequence across the switch shows no popping.
- **Look:** where clusters thin to bare trunks (overview, summit ridges at
  0.3-1.5 km), the voxels keep the canopy continuous
  (`.superpowers/farm/out` captures in the far-m worktree). At a 1 px
  switch trees at ~150-260 m (the prototypes' finest voxels 0.12-0.21 m)
  were still bare clusters; the switch is now where the finest voxel is
  2 px (`kVoxelSwitchPixels`), half that distance, and the voxels there
  draw at mip 0. Measured after the textured beech landed (raster, frame
  GPU ms, clusters / 1 px switch / 2 px switch): edge 23.23 / 20.73 /
  21.07, summit 10.29 / 8.41 / 8.92, overview 18.14 / 13.48 / 13.48; the
  2 px switch's cost is voxel_bricks.comp over the larger near bricks
  (cull early 0.69 -> 1.75 at the edge).
- **Mips keep coverage** (2026-11-21): averaging the 8 children's
  extinction covers more at every coarser level (1 - exp(-mean depth) >=
  the mean of 1 - exp(-depth)): a beech's projected coverage grew 0.27 ->
  0.34 by its 3.4 m level, an oak's 0.19 -> 0.34 by 4 m, so distant trees
  thickened and each mip switch stepped. A coarse voxel now keeps its
  children's mean transmittance along each axis (the 4 two-voxel columns
  through it), averaged over the axes: 0.27 -> 0.29 and 0.19 -> 0.24
  (the rest is correlation along longer columns). Forest overview: 5.19M
  -> 4.54M voxels, voxel trees 3.73 -> 3.28 ms, the canopy unchanged to
  the eye (mean luminance of the forest within 1%). Load prints each
  prototype's coverage per level.
- **State on main 5fb418e** (Release 2560x1440, `--trees --shadows
  raster`, back to back, frame GPU ms clusters -> voxels): forest edge
  18.49 -> 15.47, summit 9.99 -> 8.73, forest overview 17.60 -> 13.05,
  high overview 13.17 -> 11.60; far horizon from the summit (clouds off)
  14.67 -> 10.50. Earlier single runs above were taken with five lanes on
  the GPU and read 2-5 ms high.
- **Tried, not taken:** a 4 px switch (~65 m): crowns on the oaks and
  maples that still thin at 65-130 m, edge 15.47 -> 14.86 ms, but a
  nearby tree in the band shows its 4 px squares (summit view); for the
  user's eye. Voxel trees as cascade casters (cascades 0-3; cascade 4's
  ~1M trees overflow to clusters): no visible gain at the overview, whose
  receivers are in cascade 4 (branch `far-m-shadow`). The per-brick
  terrain march costs ~0.15 ms at the overview (within run noise).
- **Rejected:** a mesh workgroup per brick (the workgroup count, not the
  quads, cost 9-13 ms); voxels at 1 px (~4x the voxels for the same
  coverage, list overflow); per-voxel band dither in place of the
  per-pixel discard (independent masks lose up to ~16% coverage mid band);
  per-brick Hi-Z (a dithered canopy leaves holes in the min-depth pyramid,
  so it would mainly cull bricks behind terrain, which the instance test
  already does).
- **Voxel generation on the CPU at cook time (research, 2026-11-21):**
  a prototype (branch `farm-cpuvox`, tools/cpuvox) rasterizes each
  prototype's triangles into the bake's ray grid (4x4 rays per voxel face
  per axis, nearest hit) and matches brute-force ray-triangle tests (0
  mismatches over 2100 rays); 0.16-0.45 s per 1-4M-triangle Megaplant on
  one thread (~1.6 s for five), 0.5 MiB zstd for five at level 0. That
  removes the bake's BLAS/ray-query dependency and caches with the
  `.clusters` file, as Chlorophyll does in its LOD build. Decided: move it
  there when the BLAS goes (raster direction) or the textured Megaplants
  land, whichever is first; it needs a CPU alpha decode of the BC7/Basis
  base colour (the current trees' leaves are alpha 1) and keying by the
  geometry hash and a bake version (backlog "Far-field bake needs ray
  queries").
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage
  - https://www.youtube.com/watch?v=VC_NUxbeA0E (Chlorophyll Engine)
  - Loubet and Neyret 2017, https://hal.archives-ouvertes.fr/hal-01468817v1
    (mean chord and projected area of volume LoDs)

## TAA: a longer history when still (2026-11-21)

At native 1440p the meadow shimmers with the camera still: alpha-tested
grass blades thinner than a pixel flip coverage with the jitter, and the
contact shadows' per-frame jitter flips their binary hit. With a
current weight of 0.1 the history leaves ~23% of a per-frame flip
visible (noise std ~ sqrt(a / (2 - a))); 1/16 leaves ~18% and averages
two full 8-phase jitter cycles.

- `kCurrentWeight` 0.1 -> 1/16 (still pixels only: motion still raises
  it toward 0.3, and the variance clip handles disocclusion).
- Grass view `1119 1355.9 -431 -1.55 -0.1` (placement on, frames 120 vs
  121, /255): near 2.32 -> 1.63, mid 1.01 -> 0.73, whole frame 1.13 ->
  0.81. Motion gates (same build otherwise): fwd5 0.108 -> 0.108, side5
  0.107 -> 0.106, pitch 0.103 -> 0.103, SSIM fwd5 0.973 -> 0.972; beech
  crown / shadow shimmer 2.01 / 0.97 (2.62 / 1.38 before the textured
  beech and the exposure change landed, so not a clean A/B). No ghost at
  the marble wall's disoccluded edge strafing at 5 m/s.
- Tried and dropped: a contact-shadow jitter fixed in time (IGN of frame
  0) took the near grass shimmer only 1.63 -> 1.52 and left a visible
  static dither on shadowed model sides (it would swim in motion).
  `--effects contact=0` measures the pass's whole share (2.32 -> 1.77 at
  weight 0.1); the fix is the wavefront march in the backlog.

## Terrain shadow-height map (2026-11-21)

- **Problem:** every terrain and object fragment marched the height
  pyramid toward the sun (`terrainOccludes`); at a terrain-filled view that
  was ~1.3 ms of the early scene pass, more at a low sun.
- **Choice:** a shadow-height (horizon) map. For a sun of horizontal
  direction d and elevation E, a point p is lit by the terrain iff p.y >=
  h(p.xz), h(x) = max over t > 0 of H(x + t d) - t tanE. Along a line toward
  the sun with samples a_k this is max_{a_k < a}(H_k + a_k tanE) - a tanE:
  a prefix max, so one line sweep gives every texel on it (Max 1988 horizon
  mapping keeps a horizon angle per texel and direction; with one sun
  direction it collapses to one height; Timonen & Westerholm 2010 sweep
  height fields along lines the same way). Built by terrain_shadow.comp:
  2048^2 RG32F texels of 4 m (8.2 km) in a sun-aligned window texel-snapped
  around the camera; one 256-thread workgroup per line, exact bilinear
  heights every 2 m in the window and every 4 m upstream as far as the
  terrain's relief / tanE (at most kShadowRayLength) into shared memory,
  per-thread chunk maxima, a Hillis-Steele prefix max over the threads,
  then a second chunk walk writing every second window sample (strictly
  upstream samples only, so a receiver never shadows itself). Each texel
  stores h and the winning occluder's distance t*. Receivers
  (`terrainShadowVisibility`, lighting.glsl) fetch it bilinearly and return
  clamp((y - h) / max(w, 0.05) + 0.5, 0, 1), w = t* 2 tan(sun radius) /
  cos^2 E (the occluder line's height change across the sun disk): a
  deterministic penumbra instead of the per-pixel disk-sampled march.
  `traceSunVisibility` now returns objects x terrain. Outside the window or
  with the sun under 1 degree the march stays.
- **Rebuild:** the build costs 0.43 ms at the default sun and 0.79-0.91 ms
  at 2.9 and 1.5 degrees (longer upstream lines), over the 0.15 ms budget
  for every frame, so it rebuilds only when the sun turns more than 0.02
  degrees or the camera moves more than 1 km from the window centre (like
  the far shadow map). A capture with an always-rebuilt map and one with the
  gated map match exactly (diff 0.0000).
- **Measured** (Release, 2560x1440, `--aa taa`, rt shadows; before ->
  after, terrain shadow pass 0 in steady state): terrain-filled `1119 1420
  -431 -1.55 -0.6`: GPU 5.92 -> 5.48 ms, scene early 4.14 -> 3.70 (diff
  0.056/255); start view: 5.80 -> 5.59, 2.88 -> 2.79 (0.011); sunset `--sun
  0.9 0.05 -0.4` (2.9 deg): 7.70 -> 7.31, 5.49 -> 5.00 (0.055); same at 1.5
  deg (`--sun 0.9 0.026 -0.4`): 7.96 -> 7.34, 5.60 -> 5.14 (0.041). Ridge
  shadows land in the same places, no acne or banding. Terrain sun
  visibility still costs ~0.8 ms of the terrain-filled view's scene early
  (all of it off: 2.88): the TLAS query for object shadows on the terrain
  and the receiver's heightfield lift (`terrainHeight`,
  `terrainCellAbovePlane`) remain; the map fetch alone is ~0.3 ms.
- **Culls read it too:** cluster_cull.comp's per-cluster and
  voxel_bricks.comp's per-brick terrain sun test compare the map's soft
  visibility with their disk sample's hash (the same penumbra on
  average), so the pass now runs first in the frame, before any cull.
  Start view `cull clusters` 0.20 -> 0.10 ms, forest edge with `--trees`
  `cull early` 1.76 -> 1.52 (diff 0.006/255). gi_probes.comp still
  marches; below 1 degree the march remains (backlog).
- Sources:
  - N. Max, "Horizon mapping: shadows for bump-mapped surfaces", The
    Visual Computer 4(2), 1988
  - V. Timonen, J. Westerholm, "Scalable Height Field Self-Shadowing",
    Computer Graphics Forum 29(2) (Eurographics 2010)
  - Parallel prefix scan: Hillis & Steele 1986; Blelloch 1990

## Raster sun shadows are the default (2026-11-21)

User direction: raster, no ray tracing on the default path. `--shadows
raster` (the cached cascades of "Raster sun shadows: cached cascades")
is the default; `--shadows rt` stays as an A/B (the TLAS and BLAS stay:
the far-field bake and `--far-shadows tlas` use them).

- **Cost fix:** `cascadeVisibility` copied the `SunShadows` struct (five
  cascades) into a local per fragment; reading the fields in place took
  ~1.8 ms out of the early scene pass at the ground view. It, not the tap
  count, was raster's deficit against rt.
- **Filter:** a 3x3 tent from 4 hardware depth-compare taps (Castaño
  2013, "Shadow Mapping Summary", The Witness: bilinear PCF taps placed
  and weighted to integrate the tent) instead of 9 textureGathers. The
  cascades are bound with a compare sampler (`shadowSampler`,
  GREATER_OR_EQUAL for reverse depth) and read through a
  `sampler2DShadow` view of the bindless binding.
  - http://www.ludicon.com/castano/blog/articles/shadow-mapping-summary-part-1/
- Cascades 0-1 cull at a 0.25-texel LOD error (1 texel beyond): at one
  texel the near trees' leaf clusters simplify away and their crowns let
  through too much sun.
- The depth range reaches the terrain's highest sample + 150 m (was a
  2500 m Napf constant). Trees past the voxel switch still cast as
  clusters: the shadow views skip the voxel list.
- Measured (Release 2560x1440, native TAA, same build; raster / rt):

  | gate | raster | rt |
  |---|---|---|
  | shimmer crown / shadow (/255) | 1.95 / 0.09 | 2.01 / 0.97 |
  | Laplacian fwd5 ground / side5 / pitch | 0.107 / 0.106 / 0.102 | 0.108 / 0.106 / 0.103 |
  | Laplacian fwd5 beech | 0.231 | 0.245 |
  | SSIM fwd5 | 0.972 | 0.972 |
  | GPU frame, start view, placement on | 5.56 ms (scene early 2.72) | 5.90 ms (2.80) |
  | GPU frame, forest edge `--trees` | 16.6 ms | 28.1 ms |

  (Start view re-measured after rebasing onto the terrain shadow-height
  map.) Shadow
  shimmer drops 10x. The beech's forward Laplacian is 6% lower because
  its dappled shadow on the ground, 12-25 m below the camera, falls in
  cascade 1 (~10 cm texels): softer than the per-pixel ray, closer to
  the soft shadows of the Chlorophyll frames, and stable. Accepted.
- Re-render hitch: the first frame's five cascades cost 0.37 ms at the
  start view, but 51 ms at the forest edge with `--trees` (every tree in
  cascades 3-4 as clusters; backlog).

## Cloud outline and motion-adaptive resolve (2026-11-21)

*Superseded by "Cloud outline off by default" (2026-11-23), which holds.*

User request: crisper volumetric clouds, "almost a pseudo outline", on the
outer silhouette and on the lobes inside it, from the resolved clouds,
subtle by default, an Effects strength with 0 = off; still flicker free.

- **Outline (clouds_composite.comp):** an unsharp mask of the upsampled
  resolved clouds against a tent of radius 2 low-res texels (continuous
  across texel borders, like the Catmull-Rom upsample), applied as two
  scales of the premultiplied colour: the opacity sharpened (clamped to
  0..1) hardens the silhouette, and the luminance per unit opacity (the
  cloud's own shading) sharpened as a ratio (0.5..2) raises the steps
  between lit and shaded lobes. Only where every tap agrees with the
  pixel's surface (no halos on ridges). Rejected on the way: sharpening
  the four channels apart (dark blue rims where opacity and colour
  diverged) and clamping to the 4x4 range (the range is constant per
  texel, so it showed the texel grid as flat blocks). Strength:
  `Effects::cloudOutline`, default 1 (0..3), a `CloudConstants` push
  constant, `--effects cloud-outline=X`, the G panel, `effectCloudOutline`.
  Default 1 because it matches the reference: median edge width (step /
  steepest gradient across sky-cloud edges) at the ground view 5.85 px
  before this session, 5.54 at strength 0, 4.11 at 1, 3.52 at 2; the
  Chlorophyll `flower-meadow-cumulus.png` cumulus 4.02 px.
- **Resolve weight by motion:** sharpening amplifies the resolve's
  residual noise (ground p99 0.50 -> 0.71 at strength 1), so the still
  weight drops to 0.03 (0.04 in the layer) and rises to 0.1 (0.25) at
  0.5 texel per frame of reprojection motion, where the resampled history
  would blur (a turn, close clouds in the layer). Drifting clouds at
  `--time-scale 1` from the ground move ~0.05 px per frame: still.
- **Along-ray jitter:** interleaved gradient noise in space plus a
  golden-ratio step per frame per texel, instead of IGN's own per-frame
  offset, whose recent-weighted mean left IGN's diagonal pattern as combed
  stripes in the layer under the long history; flicker unchanged or lower.
- **Measured** (Release, TAA, 2560x1440, strength 1, flicker p99 /
  p99.9 /255 as in "Cloud temporal stability under TAA"): ground 0.46 /
  2.88, in the layer 0.49 / 2.18 (before this session 1.33 / 6.23 and
  1.86 / 6.27). Cloud crop Laplacian under a 1 rad/s turn 0.0191 ->
  0.0262; summit-silhouette drift at `--time-scale 1.5` 0.0351 vs its
  still 0.0353 (no smear). `tools/cloud-compare.sh` image metrics at
  strength 0 -> 1 (ground, wide, sunset): cover 0.468 -> 0.466, 0.552 ->
  0.548, 0.792 -> 0.794; tops, mids and bases within 3%: the look holds.
  `clouds` 1.41 ms at both strengths (cloud-compare's ground view).

## Frame cost measured, perf lane (2026-11-21)

Release, 2560x1440, native TAA, main at 8ce025b, GPU ms:

- **Targets** (Chlorophyll: 7.7 ms on a 3080 with forest; ours a 4080):
  start view 5.64 (scene early 2.76, clouds 1.39, weather 0.44); full
  ground `-0.25` pitch 5.65 (without placement 4.16); forest interior
  with `--trees` 9.82 rt / 7.40 raster; forest edge 28.1 rt / 15.7
  raster.
- **Forest edge, raster:** scene early 10.4 = ~3.5 geometry (all mesh
  shading stripped), 3.7 the object sun visibility (cascade PCF: with it
  forced to 1, 6.7), ~3 shading sub-pixel tree triangles; `cull early`
  1.5 (voxel_bricks.comp), voxel trees 1.1, clouds 0.7.
- **Clouds** (ground view): the quarter-resolution march is ~0.9 ms even
  looking down (the sky part), the composite 0.14, the resolve ~0.1.
- **Denser meadow headroom:** the grass mat at half the footprint area
  (0.2 -> 0.14 m, 2x the instances: 2.16M live, ring 6.8M slots, 5.9M
  triangles drawn) costs +1.05 ms at the start view (5.64 -> 6.69).
  Adopted (orchestrator, 2026-11-21: closer to the reference meadows; the
  meadow budget moves to 7.0 ms): start view 6.61, full ground 6.87 with
  raster shadows the default. The static placement hash is unchanged (the
  mat is streamed).
- **Dead ends:** an opaque depth prepass (opaque lists depth-only, then
  EQUAL): forest edge rt 27.4 -> 25.1 but raster 18.8 -> 21.5, start
  -0.27, dropped (the opaque lists are geometry-heavy and already early-Z
  tested). A lean depth-only mesh shader for the masked prepass (no crown,
  normals, previous clip): no change (10.39 -> 10.15 forest edge, 2.76 ->
  2.82 start; the driver already drops outputs the depth pass doesn't
  read). Re-cooking the meadow plants after the alpha-mip fix (19ac9d1)
  changed neither cost nor triangle counts measurably (7.81 vs 7.83 ms).
  Skipping the cloud march for texels with scene depth: clouds 1.20 ->
  1.17 ms at the full-ground view (0.64 both at pitch -0.6): the march
  already ends where the depth cuts the ray before the layer
  (`cloudLayerSpan` clamped by the scene distance), so its cost is the
  sky texels'.
- **After raster shadows became the default** (513ddb9, with the dense
  mat): forest edge `--trees` 13.17 ms (scene early 8.26, cull early 1.51,
  voxel trees 0.93, clouds 0.75), forest interior 6.81.

## Haze as a boundary layer over the valley floor (2026-11-21)

The default `summer` haze is now an aerosol layer anchored at the terrain:
the Mie density is `exp(-h / H)` cut by a smoothstep top, plus a thin
free-troposphere background (`Atmosphere::mieLayerTop`, `mieLayerFade`,
`mieBackground`, `mieBackgroundScaleHeight`; `sampleMedium` in
atmosphere.glsl, so every LUT and the aerial perspective follow). Engine
defaults (top 0, background 0) are the old plain exponential, so other
planets and `clean` / `summer-hazy` are unchanged.

- Layer (main.cpp `summerLayerHaze`): floor = 5th percentile of the
  terrain heights (Napf: 0.75 km), top 1.2 km above it (1.95 km, the
  Plateau's summer convective boundary layer, Collaud Coen 2014), fade
  0.25 km, H 2 km inside (near well mixed); AOD(550) 0.03 in the layer
  plus a background (H 2 km, not cut by the top) of 0.01 above the top and
  ~0.008 inside, ~0.05 over the floor in all: a clean summer day after a
  front, the low end of Ingold 2001's Swiss range. The sweep's AODs below
  are labelled the same way (layer + background above the top). Same OPAC continental optics as before.
- Why so little aerosol: dark shadows 1-3 km away are lifted by single
  scattering in proportion to the optical depth of the path alone, not
  to where the aerosol sits. At the forest overview (`700 1450 -400
  -1.55 -0.5 --trees`, image L p5 / shadow hue L 0.15-0.3 and 0.3-0.42):
  old summer 0.347 / 240, 217; the first try, AOD 0.10 in a 0.45 km
  layer over the floor, made it worse, 0.370 / 245, 229 (the views look
  down into the valleys, where it concentrates the haze); AOD 0.05 + 0.02
  in a 1.2 km layer 0.320; 0.03 + 0.02 0.309; 0.03 + 0.01 (chosen) 0.300
  / 207, 179; clean 0.246 / 190, 162; Chlorophyll aerials 0.13-0.23 /
  136-147. The rest of the gap is not haze (exposure meters this dark
  frame 1.4 EV brighter than the meadow; forest shadow ambient, backlog
  "Sky occlusion").
- High overview (`1119 4354 -431 -1.55 -0.35`): p5 0.341 -> 0.324 (clean
  0.295); the sharp top shows as the haze sitting in the valleys. Ground
  views: zenith blue/red unchanged (3.55 at the open view), horizon haze
  kept; sunset view unchanged in character. Atmosphere pass 0.02 ms
  before and after.

## Coarsest shadow cascade casts far cells (2026-11-21)

- **Chosen:** cascade 4 (5.12 km radius, ~6 m texels) skips every cell's
  trees in its cell cull and draws each cell's far-field canopy box,
  marched along the sunlight to opacity 0.5 (voxel.frag's `SHADOW`
  variant, depth only), instead of ~1M trees as clusters at a 1-texel LOD.
  A texel there is about an L0 voxel (4 m); the far cells are the
  instance-less aggregate the far field already keeps.
- **Measured** (Release 1440p, forest edge `--trees`, the first frame's
  five cascade renders, `--frames 3`): sun shadows 51.3 -> 47.1 ms; alone,
  cascade 4 costs 3.35 ms, cascade 3 0.92, cascade 2 3.76, so cascades 0-1
  (LOD 0.25 texel for thin foliage) carry ~39 ms of the hitch (backlog).
  No visible change at the forest overview: tree shadows on the ground
  there don't show from cascade 4 with either caster (rt shows them).
- **Rejected:** voxel trees (the mid tier's prototype voxels) as casters
  in cascades 0-3: no change in the hitch (47.7 ms; cascades 2-3 are
  cheap) and none on the overview's ground, which shows no tree shadows
  with clusters, voxel trees or far cells as casters (branch
  `far-m-shadow`); cascade 4's ~1M trees would be ~70M voxels.


## TAA: sharper history resampling, shorter blend in motion (2026-11-21)

Motion shimmer had no gate: the still pair (frames 120 vs 121) only sees
a still camera. Two runs of 120 and 121 frames that move (fixed 1/60 s
steps ending at `--camera`) end at the same view, so their difference is
the shimmer in motion: walk 1.4 m/s and turn 0.3 rad/s at the meadow
view `1119 1355.9 -431 -1.55 -0.1` (placement on; mean |diff| /255 over
the near ground, bottom 40%; Laplacian of the motion frame). DLSS DLAA,
for reference, measures walk 10.1 / 0.459 and turn 6.1 / 0.455: some of
the difference is content (LOD cut, stream fade), not anti-aliasing.

| native TAA | walk shimmer / Laplacian | turn shimmer / Laplacian |
|---|---|---|
| before (Catmull-Rom, max weight 0.3) | 14.8 / 0.512 | 10.9 / 0.458 |
| max weight 0.15 | 13.5 / 0.474 | 7.6 / 0.371 (visibly smeared) |
| max 0.15, sharpen 0.5 | 14.4 / 0.505 | 8.1 / 0.393 (smeared) |
| Keys a = -0.75 | 15.2 / 0.532 | 11.3 / 0.489 |
| a = -0.75, max 0.2 | 14.3 / 0.506 | 9.1 / 0.443 |
| **a = -1, max 0.2 (chosen)** | 14.8 / 0.529 | 9.7 / 0.480 |

- A shorter blend alone (lower maximum current weight) trades shimmer
  for smear: each frame resamples the history at a fractional offset and
  Catmull-Rom's blur compounds. The history is now resampled with a Keys
  cubic at a = -1 (sharper than Catmull-Rom's -0.5; same 9-tap scheme,
  w1 and w2 stay positive), which keeps detail per resample, so the
  maximum current weight in motion drops 0.3 -> 0.2: turn shimmer -10%
  and sharper, walk equal and sharper. The variance clip bounds the
  cubic's overshoot; no ringing at the marble wall's edge against the sky
  while turning.
- Motion gates (same build otherwise, raster shadows): fwd5 0.107 ->
  0.114, side5 0.106 -> 0.106, beech fwd5 0.231 -> 0.242, pitch 0.102 ->
  0.109; still shimmer crown / shadow 1.95 / 0.09 unchanged; SSIM fwd5
  0.972 unchanged. A still camera reprojects at whole texels, where the
  cubic is the identity.
- Source: Keys 1981, "Cubic convolution interpolation for digital image
  processing" (IEEE TASSP 29(6)).

## GI: ground cache, canopy light, cheaper probe reads (2026-11-21)

Follow-up to "Global illumination" above (`--gi probes`, still default off).

- **Ground radiance cache (plan Task 5):** probe rays that hit the ground
  read a camera-centred cache (4 levels of 256^2 texels, 1-8 m), whose
  albedo is scattered from the rendered diffuse albedo of pixels within
  1.5 m of the terrain (1 pixel of every 4x4 per frame, atomics, a 10%
  blend) and which `gi_ground.comp` relights (a quarter of the texels per
  frame: sun with terrain, tree and cloud shadow, plus the probes' own
  irradiance, so bounces accumulate). The meadow's green now comes from
  what is drawn, not a constant.
- **The blue forest was not thin canopy.** A diagnostic that painted
  canopy-blocked probe rays red showed the forest floor fully covered;
  the blue came from (a) the sharp sky reflection, which nothing occluded,
  turning visible once the diffuse ambient under the canopy dropped and
  exposure rose, and (b) leaves that probe rays hit lit only by the sun
  reaching the probe (near 0 under a canopy). Fixes: the sky reflection is
  scaled by the probes' rough irradiance over the open sky's (a
  probe-derived specular occlusion, as Frostbite's and DDGI's
  irradiance-ratio occlusion); leaves hit along a ray take albedo
  (0.09, 0.14, 0.04) for reflected + transmitted light and see the sun
  with `mix(probe's canopy transmittance, 0.5, ray direction y)` (a
  crown's upper leaves are about half sunlit). Forest floor: blue-grey ->
  green and darker; aerial forest p5 (L*, canopy crop, `.superpowers/gi/
  shade.py` in the lane worktree): off 0.221, probes 0.171; the
  references measure 0.06-0.10 with the same script.
- **Probe reads:** the shading read 12-15 dependent 64 B probe records
  per fragment (scene early +2.9 ms at the forest edge). Tried: one probe
  sample per cluster in mesh.mesh (worse: the workgroup waits on thread
  0's loads, +0.5 ms on the meadow); a 3D texture with hardware trilinear
  (fast, but the copied volume read back garbage with trees on; not
  pursued further, backlog). Kept: gi_probes.comp also writes each
  probe's SH as half floats, 24 B per probe in probe order, and the
  shading reads the 4 columns x 2 slices with the slices counted from the
  terrain under the point (no dependent base read, no stamps: every probe
  in a window is traced the frame it enters). Coarser levels retrace
  every 16th (level 1) and 32nd (2-4) frame instead of every 8th.
- **Cost (Release, 2560x1440 native TAA, GI on vs off):** meadow start
  view (placement) 5.79 -> 6.61 ms (gi probes 0.33, ground 0.10, albedo
  0.11, scene early +0.27); forest interior 6.62 -> 8.14 (probes 0.91,
  ground 0.28, scene early +0.30); forest edge 13.23 -> 15.57 (probes
  1.04, ground 0.30, scene early +1.04); forest overview 11.53 -> 13.62
  (probes 1.06, ground 0.29, voxel trees +0.82: per-fragment probe reads
  on voxel quads). The probe trace with `--trees` (far-field march per
  ray) and the per-fragment reads on dense foliage are what is left.

## Megascans Trees (card leaves) from Unreal replace the Megaplants (2026-11-21)

*The import path stands; the card leaves are superseded by "Tree leaves as opaque geometry, traced from the atlas" (2026-11-22).*

The Megascans Trees packs (Black Alder, European Beech; Fab, Unreal only)
are what the Chlorophyll frames look like up close: card leaves on an 8K
atlas, scanned bark, several variants per kind (Forest, Field, Sapling).
They are exported headless from the user's Unreal project
(`tools/unreal/export_trees.py`, geometry glTF + the Summer textures as
PNG), turned into textured glTFs by `tools/unreal/cook_ready.py` and
cooked with `meridian-cook --tree` (docs/assets.md, "Megascans Trees from
Unreal": channel layouts, slot order, measured normal convention).

- **No material bake.** Unreal's glTF exporter bakes nothing headless;
  the MA_Foliage inputs are rebuilt from the textures instead: base
  colour + Mask R as alpha (alpha test), Mask G roughness, metallic 0,
  Mask B (translucency) averaged over the leaf texels into the
  material's diffuse transmission (0.35 x 0.42 for the alder, 0.35 x 0.62
  for the beech), the normal map as is (it is already OpenGL: measured
  by the sign of its curl against a known-GL map; flipping it, the first
  attempt, made bark look melted and lit from below), and Unreal's
  texture colour adjustments (the beech atlas: brightness 0.9, hue -5
  degrees, in linear HSV as Unreal applies them), which the PNG exporter
  leaves out. Impostor slots are dropped: the voxel tier replaces them.
- **Every variant is a prototype** (main.cpp `addUnrealTrees`, equal
  shares within a species): forests 60% beech Forest_01-08, 25% hornbeam
  Forest_01-10, 15% alder Forest_01-09 (27 prototypes); lone trees 50%
  alder Field_01-05, 30% hornbeam Field_01-04, 20% beech Field_01 (10);
  saplings in equal parts beech 01-04, hornbeam 01-08, alder 01-06 (18)
  in a new layer 12 under the canopy (15 m footprint,
  150 m draw distance, not in the far field). The pick is the placement
  hash, so it is stable per instance. The Megaplants stay the fallback
  when no Megascans Trees are cooked.
- **Voxel tier:** the far-field and prototype-voxel bakes alpha-test the
  cards (`farfield_bake.comp` confirms a candidate hit only above the
  material's cutoff; masked BLAS geometry is non-opaque), so distant
  crowns stay see-through, not solid blobs (forest-edge hill at
  300-800 m, alder-only capture).
- **Texture encode cache:** a species' variants share their maps, so
  the tree cook caches KTX2 encodings by image content in
  `<species>/.ktx2-cache` (first tree ~90 s, the others ~20 s).
- **Measured** (Release, 2560x1440, `--aa taa --trees`, GPU ms):
  Megaplants (same build, Megascans Trees hidden) forest edge 12.43,
  forest overview 11.54, summit view 8.90; alder only (4.5 m forest
  footprint) 7.86 / 10.06 / 9.81 (6 m); beech + alder (6 m forest, 15 m
  saplings) 18.70 / 14.27 / 12.40; with the hornbeam (smaller trees) in
  the mix forest edge 16.28, overview 12.93 (beech + alder forest edge 36.9M triangles; the beech is 48 m tall
  with 0.1-2M triangles per variant, and its voxels (height / 128 =
  0.38 m) switch in only at ~500 m). Release startup with the beech
  ~140 s, 76 s of it building the far field (3.26M level-0 bricks for
  the beech's 53 m crowns).
- **Debug checks** with the beech: two of 37k far-field voxels differed
  only in their octahedral normal bytes across the fold (the same normal,
  or 1.8 degrees apart), so the check now compares normals by angle
  (3 degrees, about 2 steps where the encoding is coarsest); two of 2852
  far shadow texels put the first 0.5-opacity point a tree later (659 m)
  where a sun ray's opacity creeps along 0.5 through sparse crowns, so
  that check allows 1% of such texels, as it already did for hit/empty
  mismatches.
- **Open:** leaves and bark read silvery in backlit and sky-lit views.
  Not the metallic channel (B is 0; an explicit `metallicFactor` 0 A/B
  changed the image by 0.0002 MAE) and not the albedo (the alder's leaf
  texels average sRGB (0.22, 0.26, 0.08), the colour of the reference's
  crowns). It is specular: our F0 is a fixed 0.04, MA_Foliage's
  Specular 0.2-0.3 is F0 0.016-0.024, and Unreal's leaves are glossier
  still (Constant Roughness Leaves 0.2), so the sheen there is held
  down by the lower F0 and its specular occlusion. Next:
  `KHR_materials_specular` (specularFactor 0.4-0.6) in model.cpp and
  mesh.frag; beech triangle budget and far-field build time.

## Benchmark route and ledger (2026-11-21)

- **Choice:** performance is judged on `--benchmark 60 out.csv --route
  tools/routes/parity.route --trees` (five labelled segments: meadow,
  grass, forest-edge, forest-interior, aerial), summarised by
  `tools/bench-route.py`: per-segment average and 1% low fps, plus the
  passes that dominate. Single static shots hide the transitions, the
  streaming and the 1% lows that decide whether a frame rate holds in
  play; a route average over real frame times does not. Lines: 180 fps
  route average, 105 fps in the heaviest segment (programme spec, Phase
  0). Compare only between identical `Render 2560x1440` lines.
- **Baseline (Release, main at 17dee80 with the Megaplants trees, before
  3f26df5 brought the Megascans Trees: re-run on the new trees; GI off, raster shadows
  default):** two runs, route average 84.6 and 87.6 fps (3.5% apart;
  segments differ by up to 10% in `aerial`, where the 2 s hop from the
  forest interior is part of the segment).

```
segment          frames  avg_fps  low1_fps  avg_ms  top passes (ms)
meadow             1138     94.9      79.6   10.54  scene_early 4.37, cull_early 1.33, clouds 1.27, far_field 1.14  BELOW 105
grass               927     77.5      24.2   12.90  scene_early 5.23, far_field 1.52, cull_early 1.43, clouds 1.07  BELOW 105
forest-edge         893     74.3      19.8   13.45  scene_early 6.89, cull_early 1.51, sun_shadows 1.41, voxel_trees 0.93  BELOW 105
forest-interior    1218    101.2      25.9    9.88  scene_early 3.71, cull_early 1.42, voxel_trees 1.08, clouds 0.83  BELOW 105
aerial              902     75.2      56.4   13.30  scene_early 4.26, far_field 3.26, clouds 1.56, cull_early 1.49  BELOW 105
route              5078     84.6      23.7   11.81  scene_early 4.79, cull_early 1.43, far_field 1.36, clouds 1.06  BELOW 180

segment          frames  avg_fps  low1_fps  avg_ms  top passes (ms)
meadow             1165     97.1      81.4   10.30  scene_early 4.38, cull_early 1.33, clouds 1.27, far_field 0.85  BELOW 105
grass               961     80.2      23.5   12.46  scene_early 5.22, cull_early 1.40, clouds 1.09, far_field 1.03  BELOW 105
forest-edge         898     74.8      20.2   13.38  scene_early 6.91, cull_early 1.50, sun_shadows 1.43, voxel_trees 0.95  BELOW 105
forest-interior    1240    103.3      26.0    9.68  scene_early 3.67, cull_early 1.43, voxel_trees 1.15, clouds 0.81  BELOW 105
aerial              992     82.6      60.5   12.11  scene_early 4.10, far_field 2.11, clouds 1.55, cull_early 1.49  BELOW 105
route              5256     87.6      23.9   11.42  scene_early 4.75, cull_early 1.42, clouds 1.06, voxel_trees 1.02  BELOW 180
```

## Camera field of view: 70 degrees horizontal, configurable (2026-11-21)

The camera was fixed at fovY 1.2 rad (~101 degrees horizontal at 16:9), so
everything looked small and fine detail was lost. The matched views of the
five reference frames estimate his field of view at ~60-70 degrees, so the
default is 70 degrees horizontal. `--fov`, the grade file's `fovDegrees` and
the G panel slider (50..110) set it; fovY comes from `verticalFov(horizontal,
aspect)` each frame, so a resize keeps the horizontal angle. Culling, LOD
error, jitter and reprojection all derive from the projection matrix, so none
needed a change. The motion gate minimums in `tools/motion-gates.sh` were
anchored at the old field of view and must be re-anchored on the next gate run
(not run with this change); captures from before it are not comparable.

## TAA stability over sharpness (2026-11-22)

The user's G19 complaint (smear, jitter, shimmer in motion) is mostly
instability, and they prefer a stable image to a sharp one. Measured with
`tools/motion-clarity.sh` (same pose reached on frame 120 and 121 while
moving: their mean difference is the jitter; frame 120 against the still
is the error; 2560x1440, Release): a still camera differs by ~1.2/255
between frames, a moving one by 8-16 under our TAA and 8-9 under DLSS
DLAA, which is also sharper when still, so the resolve, not only the
input, falls short.

- Tried on the forest edge and the meadow (sideways 5 m/s, turn 1 rad/s):
  - Catmull-Rom (-0.5) history resampling instead of Keys -1: jitter
    -4..-19%, still image unchanged. Keys -1's overshoot compounds every
    frame in motion (the moving frame had up to 1.8x the still's Laplacian)
    and the variance clip turns it into flicker.
  - The 3x3 jitter-aware reconstruction of the current frame (Gaussian,
    sigma 0.47 px, Karis 2014; WIP branch `native`): jitter -16..-23%,
    still 21% softer; at equal still sharpness a plain blur scores the
    same, so it is a softening that buys stability, accepted as such.
  - Motion sharpening off: jitter -3..-7%.
  - Dropped: no motion-raised current weight (worse), variance gamma 1.5
    (error up to +31%).
- Choice: all three kept together as `Effects::stableTaa` (default on;
  `--effects taa-stable=0` and the G panel give the previous resolve):
  jitter -27..-35%, error -10..-22% against the previous resolve, still
  ~21% softer; `aa` 0.20 ms. DLSS DLAA still leads (forest sideways:
  jitter 7.8 vs 9.2, error 6.5 vs 13.5).
- The motion gates' Laplacian minimums (`tools/motion-gates.sh`) were
  anchored to the sharper resolve (not re-run here); the clarity table is
  the gate for this work.

## DLSS DLAA is the default anti-aliasing (2026-11-22)

Supersedes "Native 1440p with our own TAA is the default (2026-11-21)".
That entry rested on press reports that the Chlorophyll Engine uses no
upscaler; his own replies say he follows DLSS/FSR guidance (baseline F16).
At the motion clarity table DLSS DLAA is sharper when still and steadier
in motion than our TAA even with the stable resolve (forest sideways 5 m/s:
jitter 7.8 vs 9.2, error 6.5 vs 13.5; meadow: 9.4 vs 11.2, 11.6 vs 12.6),
and DLAA is how DLSS is commonly shipped as pure anti-aliasing.

- Choice (user, 2026-11-22): `--aa dlss` is the game's default, Super
  Resolution DLAA at native 1440p (preset M); our TAA, with the stable
  resolve, stays the fallback (non-NVIDIA GPUs, the licence-clean build)
  and an A/B (`--aa taa`).
- Cost: DLAA ~2.3 ms at 1440p against TAA's 0.2 ms. If the 180 fps
  target needs it back, DLSS Quality (render scale ~0.67) is the lever;
  judged at the parity route.
- Input-side instability stays: even under DLAA a moving frame differs
  ~6x more than a still one (alpha-tested foliage), so the foliage work
  (mip bias, geometry leaves) and the cloud march's own history still
  apply.

## Tree leaves as opaque geometry, traced from the atlas (2026-11-22)

Status: implemented 2026-11-23 (spec
`2026-11-21-materials-fidelity-design.md` section 2, D1 and D2; plan
`superpowers/archive/plans/2026-11-22-leaf-geometry.md`).

- **Problem:** the Megascans Trees' leaves and decoration are
  alpha-tested cards. Their cut-out edge depends on a texture fetch whose
  mip moves with distance and motion, so under a pixel it flickers even
  under DLAA (moving frames differ ~6x more than still ones, entry
  above). A card's UV box is 85-94% transparent (Black Alder Forest_01:
  6,298 cards, 50k triangles, 61 atlas regions, each a sprig of 3-23
  pieces), so the masked depth prepass also discards most of what it
  draws.
- **Practice:** Epic's Nanite foliage guidance is to cut masked cards
  into opaque geometry (masked Nanite pays programmable raster and
  overdraw; UE 5.7's Nanite Foliage builds on geometric leaves plus
  voxels); tools do it by contouring the alpha, triangulating and
  projecting onto the card (SideFX "Converting alpha cards to opaque
  geometry" with the Trace SOP, Atlas2Mesh, CardCutter, the "Masked
  Material to Nanite" Geometry Script tool). CE: trees fully triangles or
  voxels, only grass and small ground cover alpha tested (baseline F17),
  "alpha grass still smears a little, trees don't" (F16).
- **Choice:** in `meridian-cook --tree`, trace the cooked atlas alpha once
  per material (marching squares at the 0.5 cutoff, Douglas-Peucker ~1
  texel, earcut triangulation), then clip that atlas-space mesh to every
  card triangle through its UVs (Sutherland-Hodgman, repeat tiles for UVs
  outside [0,1]) and map by barycentrics, so leaves follow the card's
  bend; the existing piece split, simplification (leaves ~2 mm, wood
  keeps 1 cm: a 1 cm error polygonizes a 5-8 cm leaf up close, and the
  cluster LOD already simplifies by screen error at distance) and pruning
  follow.
  Leaf and decoration materials become opaque, double-sided, without
  alpha. Rejected: per-card tracing (same result, thousands of traces),
  an offline Houdini/Blender step (outside the cook), tight-fit masked
  cards (overdraw only, the shimmer stays). Megaplants leaves are already
  geometry.
- **Budget:** tracing gives ~150-400 triangles per card against 4-6
  (est.). Frame cost follows the cluster LOD cut, not source triangles;
  memory does not: 55 tree prototypes hold 13.7M triangles at ~110-125 B
  each with cluster data, so 4M per tree (CE's figure, F10) would be
  ~25 GB. Chosen: ~30 prototypes (5 Forest per species, 6 Field, 3
  Sapling per species) and a ~30M total cap (~3.5 GB est.): CE's own
  trade of few dense assets, heavily instanced, over pruning leaves into
  fewer, bigger ones (close-up quality) or keeping 55 at ~60M (VRAM).
  Re-checked on real counts after the first alder and beech trace; if
  ~2 mm outlines don't fit, cut variants before coarsening outlines; past
  that, cluster compression and streaming (VG slice 5).
- **Not solved by geometry:** back faces (G13), leaf material (G15, G18),
  translucency (G3), dapple resolution (G5), forest edge cost (G10), and
  leaves stay flat in their card's plane.
- **Gates:** motion jitter lower than with cards (`motion-clarity.sh`,
  forest edge and interior), crown coverage within 5% from 30 m to the
  voxel switch (baseline on cards first; fallback: area-preserving
  pruning per cluster DAG level, then an earlier voxel switch), forest
  edge GPU ms not above today's, tree memory under the cap.
- **Implemented:** the trace is a filled marching-squares mesh of the
  alpha at the cutoff, simplified by meshoptimizer (planar, absolute
  error), not Douglas-Peucker + earcut: the same outline tolerance, holes
  and nesting for free, no new dependency. Leaf outlines are coarsened
  2 -> 5 mm before pruning leaves to fit a tree's budget. 30 variants
  (assets.md), 28.7M triangles. `meridian-cook --coverage` measures crown
  coverage across the cluster LOD cut.
- **What it took to hold the crowns:** (1) simplification drops disjoint
  leaves, thinning a crown with distance (alder Forest_01 0.62 of its
  full-detail coverage at the voxel switch): each group's simplified
  pieces are scaled about their centres to keep the group's leaf area
  (stochastic simplification's area rule, Cook et al. 2007), at most 4x;
  (2) that move must count as LOD error, or the cut draws scaled levels
  where they are wrong by tens of pixels (sprigs grew into metre-wide
  shards from a 16x cap on, pale plates at 4x); with the move added to
  the group's error, monotonic up the DAG, every traced tree keeps >= 0.95
  of its coverage up to its switch (beech Field_01 0.97 at 169 m,
  Forest_01 0.95 at 345 m) at the cost of a finer cut near the switch
  (beech Field_01 471k triangles at 169 m, cards 225k); (3) the trace
  must keep each card's winding: on cards with mirrored UVs (every beech
  and hornbeam card) the leaves came out wound against their normals,
  double-sided shading then turned the normal away from the camera and
  grazing Fresnel made whole crowns white-blue at distance. Loading also
  exposed MikkTSpace's O(n^2) handling of degenerate triangles (the
  hornbeam's outline slivers): tangent generation skips them.
- **Measured** (Release, DLAA, 2560x1440, main with GI probes, cards
  then traced): GPU ms forest edge 28.5 -> 27.3, interior 11.6 -> 11.5,
  open view toward the summit 19.3 -> 20.1, forest overview 18.0 -> 18.5;
  the overview's crowns are dense instead of sparse cards. Motion
  (`motion-clarity.sh` forest rows): jitter side5 7.55 -> 6.79, side30
  9.38 -> 8.82, turn1 4.40 -> 4.27; error side5 6.20 -> 5.97, side30 7.44
  -> 7.54, turn1 4.51 -> 4.66; the still camera's jitter rises 1.02 ->
  1.35 (sub-pixel opaque leaves under DLAA's jitter; backlog). Debug
  checks pass, 0 validation errors.

## Cloud shadow map grid moves with the clouds (2026-11-22)

Cloud shadows advanced in ~29 m jumps (G20, #23). Measured on a still
camera at 2400 m looking down, `--weather broken --time-scale 1`, 30 s
captured every 10 frames, each frame divided by a `--clouds off` still to
keep the cloud shadow alone: a kymograph (one image column over time) of a
shadow edge was a staircase, one map texel every ~1.7 s. The cause is the
sampling, not the row interleave: the map's texels were fixed in the world
(snapped around the camera) while the clouds drift through them, so a sharp
cloud edge sampled at texel centres reconstructs (bilinear, after the blur)
at texel positions only, and jumps a texel when it crosses a centre. The
alternating-row refresh lags half the rows one frame, ~0.2 m of wind, too
little to see.

Fix: snap the map in the cloud frame instead of the world, the camera
position minus the cloud wind's displacement summed in double since startup
(`cloudWindTravel`, from the per-frame `windDisplacement`). A texel then
keeps marching the same part of a cloud, and the lookup, which takes the
camera's sub-texel offset in that frame (`shadowCamera`), glides the shadow
continuously. Only the wind's horizontal displacement projected on the map's
right and up axes matters: the part along the sun moves a cloud along its own
shadow line. No shader change. Interleaved rows stay valid while the grid
keeps its texel; on a frame the grid steps (camera or wind crossing a texel,
also the old misregistration when the camera crossed one) the whole map is
re-marched, ~5 frames per second at 150 m/s flight and once every ~2 s from
the wind alone. On average this costs nothing: the weather pass stays 0.67 ms
at 1440p, still or flying at 150 m/s. Same sequence after: the kymograph
edge is a straight diagonal. The ~29 m outline resolution remains (backlog:
finer near cascade or bicubic lookup, a look decision).

## Alpha cut-out: +1 mip bias (G19 step c, 2026-11-22)

- **Problem:** under DLAA a moving frame still differs ~6x more than a
  still one; alpha-tested foliage is the largest source. The cut-out edge
  of the masked depth prepass (mesh_masked_depth.frag; the shaded pass
  only matches its depth) follows the alpha of whichever mip the
  sub-pixel jitter and motion pick.
- **Practice:** CE "tweaked the mip bias on alpha-tested grass" (baseline
  F16). A coarser mip averages the cut-out over more texels, so the edge
  moves less frame to frame; coverage holds because the cook builds
  coverage-preserving alpha mips (ktx2.cpp).
- **Measured** (`motion-clarity.sh` rows, Release, DLAA, 1440p; jitter =
  frame 120 vs 121 at the same pose, error = motion vs still, 0-255):

  | case | jitter 0 / -1 / +1 | error 0 / -1 / +1 |
  |---|---|---|
  | ground side 5 m/s | 9.41 / 10.21 / 8.41 | 11.62 / 12.32 / 11.04 |
  | ground turn 1 rad/s | 4.97 / 5.20 / 4.59 | 7.42 / 7.74 / 6.99 |
  | forest side 5 m/s | 7.84 / 8.03 / 7.71 | 6.53 / 6.74 / 6.21 |
  | forest turn 1 rad/s | 4.80 / 4.92 / 4.63 | 5.33 / 5.55 / 5.27 |

  -1 is worse everywhere. +1: still ground Laplacian -5% (0.646 ->
  0.615), mean luminance unchanged (no visible coverage loss); forest
  edge GPU +1.1 ms (25.7 -> 26.8: the cut-out depth occludes less, ~6%
  more instances drawn), ground unchanged.
- **Choice:** `Effects::cutoutMipBias` (`--effects cutout-bias=X`, G
  panel, grade file `effectCutoutMipBias`, -2..2), default +1, added to
  the texture mip bias in the masked depth pass only (stability over
  sharpness). The cascades' alpha test (shadow_masked.frag) keeps its
  own LOD: the shadow map doesn't move with the camera jitter. Once tree
  leaves are opaque geometry (entry above) the bias touches only grass,
  flowers, ferns and the meadow, and the forest cost goes with it.

## Cloud outline off by default (2026-11-23)

User decision: `Effects::cloudOutline` defaults to 0 (was 1, "Cloud
outline and motion-adaptive resolve" above). The user's own saved grade
already ran with it at 0. Evidence that it costs more than it gives: with
a still camera under DLAA the outline drives most of the sky's pops
(cloudpop worktree, `1119 1356 -431 -1.55 0.35`: 21.3 pops per frame at
1, 9.7 at 0; 12.9 / 8.3 with frozen clouds), it amplifies the resolve's
residual noise, and it is a suspect for the blocky distant clouds (G16).
The effect stays (`--effects cloud-outline=1`, the G panel slider,
`effectCloudOutline`) for A/B; a grade file without the key now gets 0.

## Exposure: shade lift floor (G7, 2026-11-23)

Measured (research/2026-11-23-G7-grey-image.md): mean-to-grey metering
lifted forest views 3.4-4 EV over the open start view (5.5 with GI
probes), so every forest came out at OKLab L ~0.55 with no darks (p5 0.43;
the Chlorophyll frames 0.15-0.24), whatever the canopy did to the light.
`exposure.comp` now also bounds the target from below: never more than
`kMaxShadeLiftEv` = 3 over the EV of an open landscape of reflectance 0.09
lit by `openIlluminance` (sky_irradiance.comp: sun through the atmosphere
and the cloud shadow at the camera, plus the sky with clouds, on
upward-facing ground). An incident-light bound, like a photographer
metering the open light and opening up for shade; it follows the weather
and time of day and never binds at night. Reflectance 0.09 makes the
model agree with the open start view's own meter (13.39 vs 13.56); 0.18
put the bound a full EV lower than the chosen look.

The 3 EV is the user's look choice among emulated caps on the GI interior
(4 EV: L 0.32 / p5 0.18; 3 EV: 0.21 / 0.10, "most photoreal", trunks very
dark). Readable shade at that cap is local exposure's job (follow-up), not
a looser cap. Open views are unchanged (the bound sits ~3 EV under their
meter); forests darken: interior EV100 10.21 -> 10.60, edge 9.57 -> 10.60.
Rejected: percentile-only bounds (a closed canopy with no sunflecks or sky
in view has no bright pixels to anchor on) and per-biome authored clamps
(the light already says how open the place is). Cost: one log2 in a
one-thread pass, a few ALU ops in sky_irradiance.comp.

## GI reads material albedo (2026-11-23)

Principle (materials fidelity spec): the engine lights whatever material it
is given, no asset-specific constants. `gi_probes.comp` blended foliage
hits toward a fixed `kLeafAlbedo` (0.09, 0.14, 0.04) and uncached ground
hits used a fixed meadow albedo, so any other leaf asset, autumn palette or
planet lit as this beech/meadow. Now `farFieldTransmittanceAlbedo`
(farfield.glsl) returns, besides transmittance, the mean palette albedo
along the ray weighted by what each voxel absorbed: per segment
`T (1 - exp(-sigma dt))` times the voxel's palette entry, summed and divided
by `1 - T`. It shares the march with `farFieldTransmittance` (and
`farMarchCell`): the body is `farFieldMarch` / `farMarchCellImpl` with a
`const bool wantAlbedo`, false in the existing wrappers, so their code is
unchanged after constant folding. The palette's alpha now holds each slot's hit-weighted mean
`diffuseTransmission` (renderer_farfield.cpp, same weighting as the colour;
0 for non-leaf materials; voxel.frag and voxel_instance.frag read only
`.rgb`), and the march accumulates the scatter albedo `rgb * (1 + a)`:
reflected plus transmitted, as mesh.frag lights a leaf's back with
diffuseColor * diffuseTransmission. The first version used reflectance alone
and dropped the transmitted light (the old constant stood for both). The heuristic (half the crown's leaves
sunlit, 0.5 weights) stays; it is #30's. Ground fallback (no cache texel
yet) is now neutral grey 0.079, the Rec.709 luminance of the old meadow
colour; the terrain material offers no cheap mean albedo in these passes,
and the cache supplies the hue once it has data. Measured (Release, --trees --gi probes, 2560x1440, before -> after, with
transmission): edge OKLab mean L 0.256 -> 0.241, p50 0.099 -> 0.082, `gi
probes` 1.26 -> 1.33 ms; interior mean L 0.204 -> 0.158, p5 0.102 -> 0.067,
p50 0.175 -> 0.124, 1.13 -> 1.17 ms; EV100 unchanged (10.60). Reflectance
alone gave interior 0.152, so the transmitted term recovers little: the
baked leaf palette is simply darker than the old constant (0.09, 0.14,
0.04). Edge trunk with micro=0 rgb/Y 0.71 1.10 0.86 -> 0.78 1.06 1.07. Whether
the shade is too dark is the lighting heuristic's (#30) and the albedo
audit's question, not tuned here.

## GI: crown visibility only without GI (#30, 2026-11-23)

Under `--gi probes` the tree ambient was the probe irradiance, which
already traces the canopy through the far-field voxels, multiplied again by
the per-vertex crown sky visibility (mesh.frag `crownOcclusion`): the
canopy occluded twice, so forest-edge leaves and trunks went near-black
(#30). The crown term now applies only with GI off, where it is the only
canopy occlusion; the plant-base micro occlusion stays in both modes.
Release, 2560x1440, `--trees --gi probes`, after #31: edge mean L 0.241 ->
0.310, p5 0.000 -> 0.073; trunk rgb/Y 0.78 1.06 1.06 (neutral; it read
olive before #31); interior 0.158 -> 0.162 (the floor has no crown term;
its shade is 1.5-2.7% of the sunflecks, inside the 1.5-5% measured for
closed beech canopies). GPU unchanged (gi probes 1.18 / 1.33 ms).

## GI probes on by default (2026-11-23)

User decision after the #31 and #30 fixes. Without GI the forest floor gets
the open sky's ambient (10-17% of the sunflecks, blue-grey); with the
probes it sits at 1.5-2.7%, inside the 1.5-5% measured under closed beech
canopies, and nothing else in the engine gives canopy occlusion or bounce.
`--gi off` stays as the A/B and the fallback for weaker GPUs (crown sky
visibility is then the only canopy occlusion). Release, DLAA, 2560x1440:
- Cost: forest interior +1.7 ms, edge +2.2 ms (gi probes 1.1-1.3, gi
  ground 0.3, gi albedo <0.1, scene early +0.2-0.45); the gate views
  (meadow, beech scatter, no forest) +0.5 ms (probes 0.34, ground 0.09).
- Motion (tools/motion-clarity.sh forest rows, GI off -> on): sharpness
  ratio to the still 0.90 / 0.88 / 0.91 -> 0.89 / 0.86 / 0.91 (side 5,
  side 30, turn 1); jitter and error lower with GI, partly because the
  frame is darker.
- Motion gates with GI on: laplacian fwd5 ground 0.1271, side5 0.1283,
  fwd5 beech 0.2837, pitch 0.1306; shimmer crown 1.57, shadow 0.25: all
  pass. SSIM fwd5 ground 0.972 stays below its 0.98 minimum, the
  pre-existing failure under DLSS (backlog row; 0.959-0.960 recorded
  there).

## Leaf transmission per KHR_materials_diffuse_transmission (#32, 2026-11-23)

The engine imports `KHR_materials_diffuse_transmission` but lit a leaf's
front with the full base colour and its back with base colour x factor.
The extension (Khronos, release candidate) defines the factor as the share
of diffuse light transmitted instead of reflected, and the back's colour as
its own `diffuseTransmissionColor` (factor x texture, default white),
independent of the base colour: diffuse = mix(BRDF(baseColor),
BTDF(diffuseTransmissionColor), factor). Engine-stands-alone principle:
honour the standard the engine imports; asset semantics belong to the cook.

- Engine: `MaterialData`/`GpuMaterial` gain the transmission colour factor
  and texture (model.cpp imports both; the texture shares the material's
  UV transform). mesh.frag scales the front's diffuse colour by (1 - f) and
  lights the back with f x transmission colour (x tint, x (1 - metallic)):
  `addTransmittedSun` / `addTransmittedAmbient` (lighting.glsl), shared with
  the voxels. The diffuse albedo guide is the reflected (1 - f) albedo.
  `addTransmittedAmbient` reuses `addAmbientLight`'s probe sample (`Shading.gi`,
  evaluated toward -N): a second `giSample` cost the voxel trees pass ~1 ms
  on the forest overview.
- Far field: the bake also sums each slot's transmission colour; the
  palette's first half is (reflected rgb = mean base x (1 - f), f), the
  second half (transmitted rgb = mean transmission colour x f, 0). The GI
  scatter albedo is reflected + transmitted. voxel.frag and
  voxel_instance.frag now transmit the sun and ambient through back-lit
  voxels (before: front only), as the clusters do, so the switch matches.
- Cook: sources give "the back transmits t times the base colour" (Unreal's
  translucency in tools/unreal/cook_ready.py, the plants' ORT translucency,
  the 0.35 default). `khrLeafTransmission` (tree.h) re-encodes that exactly:
  base colour x (1 + t), also the transmission colour texture, factor
  t / (1 + t); the front still reflects the source colour, the back
  transmits t times it. Texels whose scaled colour passes 1 clamp (they
  cannot both reflect ~100% and transmit): goldenrods 4.95% of channels,
  clovers 0.55%, poppy 0.45%, the trees <= 0.02%.
- Verified look-neutral (Release, DLAA, 2560x1440, --trees, old binary and
  cooks vs new): OKLab mean L edge 0.303 / 0.303, overview 0.569 / 0.569,
  meadow ground 0.571 / 0.571; overview p5 0.334 -> 0.341 (back-lit voxels);
  mean differences 1.0-1.6/255 (re-encoded textures, voxel back light).
  GPU edge 27.74 -> 27.67 ms, overview 18.57 -> 18.12, ground 20.97 ->
  20.89.
- Calibration (Megascans Trees only): `cook_ready.py` writes t = 1.1, the
  measured transmittance over reflectance of mature beech leaves over the
  visible range (reflectance 5.7%, transmittance 6.4%, EGU2019-6557; young
  spring leaves ~2, PAR ~2 in Pilarski 2008), instead of 0.35 x the Unreal
  translucency mask (0.22 on beech). The textures show summer leaves, so
  the transmission has to be theirs; seasons are a runtime material
  question, not a cook value. KHR factor 0.524, leaf colour x 2.1, clamping
  <= 0.03% of channels. Forest edge (Release, DLAA, 2560x1440): mean L
  0.303 -> 0.361 (default sun), 0.473 -> 0.561 (sun behind, EV100 9.86 ->
  9.87); the canopy shade reads green instead of near-black, backlit crowns
  glow. t = 2 was tried and rejected: lime backlight and trunk B/G 0.37.
- Open: trunks under the canopy take a yellow-green cast from the GI bounce
  (central trunk B/G 0.63 -> 0.47 with GI probes; 0.99 / 0.95 with `--gi
  off` before and after, so the bark and the direct light are neutral).
  Closed-canopy shade light is ~0.62 (Chiao 2000). Whether the probes
  over-saturate the canopy's bounce is for the reference path tracer
  (spec 2026-11-23), not tuned by eye.

## Reference path tracer (2026-11-23)

Spec `superpowers/archive/specs/2026-11-23-reference-path-tracer-design.md`, plan
`superpowers/archive/plans/2026-11-23-reference-path-tracer.md`. Every lighting
question had been judged against photos and measured ranges, which can
reject an outlier but cannot say whether an approximation is right (#32's
trunk tint). Unreal and Unity HDRP ship a path tracer for this; we had the
pieces (TLAS of every placed instance with full-detail BLAS, ray queries,
bindless materials, the heightfield march).

- Approach (user choice among three): a compute path tracer inside the
  renderer (`reference.comp`, ray queries), sharing material evaluation
  with the raster path: mesh.frag's material and terrain functions moved
  to `surface.glsl` behind a `SurfaceInput` (raster output unchanged: mean
  OKLab L equal, pixel differences inside run-to-run noise). Rejected: a
  ray-tracing pipeline (no infrastructure, nothing gained with bindless
  materials) and an offline renderer export (independent of our material
  code but a large exporter; later, to validate materials themselves).
- Scope (user decision): surfaces only. Sky, haze, sun transmittance,
  cloud shadow and the sky capture are the raster path's lookups, used as
  incoming light.
- Path: camera ray jittered in the pixel; closest hit in the TLAS (alpha
  tested, both caster masks) or the heightfield; next-event estimation of
  the sun disk with a TLAS + heightfield shadow ray; the raster BSDF (Lambert
  front, KHR transmission back, GGX with energy compensation), one lobe per
  bounce picked by albedo; Russian roulette from bounce 3, 16 bounces (256
  for the furnace checks: 64 lost 3-5% in a white canopy); misses read the
  sky LUT behind the sky capture's clouds; aerial perspective on the camera
  segment; the raster's occlusion approximations left out.
- Checks: albedo of the primary hits vs the raster albedo capture within 2%
  per cell on the forest edge; white furnace 0.994-0.999, half-transmitting
  leaf furnace 0.995-1.007, sun estimate vs closed form 0.999; noise halves
  from 16 to 64 samples; at 1024 samples 0.07-0.94% noise (2% backlit),
  ~23 ms per sample on the gate views.
- It found an engine bug at once: the DFG table's descriptor binding was
  fragment-only, so compute shaders read 0 and GGX energy compensation
  exploded (~80x on sunlit ground); the binding now includes compute.
- First comparison, forest edge (Release, 2560x1440, `--effects pom=0
  --bloom 0 0`, reference 1024 spp): the shaded central trunk is 0.0175
  with B/G 0.84 in the reference, 0.0373 / 0.61 with GI probes (2.1x
  bright, too green), 0.0282 / 1.00 with GI off. #32's trunk tint is the
  probes' error: they over-brighten and over-saturate the canopy bounce.
  Backlit (`--sun -0.9 0.4 0.15`) the raster canopy is 1.8x too dark and
  trunks 1.2x, so through-leaf light is under-transmitted in the raster.
  Overview albedo: far voxel trees 2-12% brighter in the reference.
- Known differences: the reference terrain is the bilinear heightfield
  without the near detail displacement (raster buries object bases up to
  0.3 m deeper), blended materials trace as opaque, streamed meadow layers
  are absent (`--streamed off` on both sides), the sky in primary misses
  is the 64^2 capture's clouds.

## GI probes: foliage lit where the ray meets it (2026-11-23)

Spec `superpowers/archive/specs/2026-11-23-gi-canopy-radiance-design.md`, plan
`superpowers/archive/plans/2026-11-23-gi-canopy-radiance.md`. The reference path
tracer measured the forest edge's shaded trunk at 2.1x its luminance and too
green (B/G 0.61 vs 0.84) under the probes. An ablation against the
reference (one term neutralized per variant, Release, 2560x1440, `--trees
--effects pom=0 --bloom 0 0 --streamed off`, reference 1024 spp) placed it:
with the probes' foliage radiance zeroed the trunk was 0.87x the reference
(occlusion of sky and ground right), so the foliage radiance itself was ~10x
too bright. `albedo (1 + t) / pi (sun 0.5 leafSun sun.y + 0.5 sky)` lit every
leaf a probe ray meets as an outer, sunlit, sky-exposed one, while the leaves
seen from below are the shaded underside; its sky half alone was 4.6x too
bright. Reflectance-only albedo and a 1 m ray start changed nothing.

Now (`gi_probes.comp`, `farFieldTransmittanceScatter` in farfield.glsl):
the albedo march also returns the absorbed-weighted mean reflected rgb,
transmitted rgb and hit distance; from that point the sun's transmittance
(far-field march, at most 64 m) and the sky's (straight up). The sun term
uses the area scattering phase of bi-Lambertian leaves with a spherical
leaf-angle distribution (Shultis & Myneni 1988, over pi G with G = 1/2, so
it integrates to rho + tau; `leafPhase` in gi.h is the tested CPU
reference): looking toward the sun shows transmission, away reflection; a
volume, so no `sun.y` factor. The sky term is isotropic, omega / (2 pi)
E_sky T_up. Multiple scattering in the crown by the spectral invariant
(p-theory, Knyazikhin, Huang et al. 2007): source / (1 - p omega), p = 1 -
T_up. `leafSun` and the per-probe canopy sun march are gone. Rejected: the
far shadow map for the sun term (8 m, binary at 0.5 opacity), a lighting
volume baked over the voxels (most code, stalls on sun motion), the
prototype voxels (the 4 m world voxels already land within ~10%).

Measured against the reference (ratio reference / raster per 4x4 cell):
- Edge front: mean |ln ratio| 0.145 -> 0.066, 14 of 16 cells within
  0.9-1.1 (before 7); trunk 0.47 -> 0.97 of the reference's luminance, B/G
  0.61 -> 0.89 (reference 0.84). Left: canopy against the bright
  background 0.84 (cell 3,0, the foliage term overshoots there), foreground
  leaves 1.09-1.14 (also with the foliage zeroed: leaf shading, backlog).
- Edge backlit (`--sun -0.9 0.4 0.15`): probe-lit columns 2-3 0.223 ->
  0.217; the frame stays 0.38 because the sunward side is 1.4-2.5x too dark
  with or without GI (the direct path, backlog). The old over-bright
  foliage had partly masked it.
- Multiple scattering: front 0.0642 vs 0.0633 without (a wash), backlit
  0.391 vs 0.403; kept.
- Interior: the reference misses the terrain around that view (backlog), so
  judged on the raster alone: floor shade 5.4% -> 2.4% of the sunflecks
  (closed beech canopy 1.5-5%), now neutral (B/G 0.99) rather than olive.
- Cost: `gi probes` 1.40 -> 1.57 ms median on the edge view. The sun march
  to `kGiTreeDistance` (300 m) cost +0.59 ms (it runs until the ray clears
  the highest canopy anywhere); capping it at 64 m (`kGiLeafSunDistance`)
  measured no worse against the reference (128 m: 1.74 ms, front 0.062,
  backlit 0.41).

## Terrain relief test: real 0.5 m vs 2 m (2026-11-23)

Question (terrain triage, plan step 1): does real sub-2 m relief make
eye-height ground look more real? Test: a 4x4 km swissALTI3D patch at
0.5 m and 2 m (same tiles, same year, `tools/fetch-relief-patch.sh`),
captured from four eye-height views at midday and with an 8 degree side
sun, against 2 m with and without the detail noise
(`--effects terrain-detail=0`, new; `--terrain FILE`, new). Result
(research/2026-11-23-terrain-triage.md, "Step 1 result"): yes, clearly
under grazing light; real relief is anisotropic and slope-aligned (~0.1 m
std on pasture, metres on ravine walls) and also sharpens hillsides
50-500 m out, while the isotropic detail noise reads as artificial. The
engine already renders 0.5 m data unchanged. The next choice is how to get
such relief everywhere: stored 0.5 m data near the player (a height
clipmap) or synthesis trained on the real patch.

Follow-up, same day (user approved): the detail noise is off by default
(`Effects::terrainDetail = false`; `--effects terrain-detail=1` or the G
panel brings it back for A/B). On the Napf's pasture the bare 2 m surface
was as real as the 0.5 m data at midday, and the noise's isotropic bumps
read as artificial under a low sun. Next in the lane: geo-maps in the cook
and causal placement (triage plan steps 2-3), developed on the 0.5 m patch;
a height clipmap for 0.5 m everywhere when the showcase needs the full
area.

## Backlit canopy: leaf sun side and crowns without tuft occlusion (2026-11-23)

Spec `superpowers/archive/specs/2026-11-23-backlit-direct-design.md`. The reference
put the backlit forest edge (`--sun -0.9 0.4 0.15`) at 1.4-2.5x the raster
on the sunward side, the same with `--gi off`. Ablation (Release,
2560x1440, `--trees --effects pom=0 --bloom 0 0 --streamed off`, references
1024 spp, a one-bounce reference for the direct part):

- Ambient is right: the reference's indirect light matches the raster's
  ambient within ~15% per cell. The error was direct sun, 90-100% of it in
  3-9% of the pixels, at 10-40x the cell mean.
- Not the cause: the sun cascades (`--shadows rt` is darker still), voxel
  trees, LOD error; forcing the through-leaf sun unshadowed overshoots most
  cells and still leaves those pixels under 25%.
- Normal mismatch: where a leaf's shading normal and geometric normal
  disagree about the sun's side, the reference evaluated the front lobe for
  light arriving behind the face (a leak) and the raster gave no sun at all
  (transmission needed both normals facing away). Now both follow pbrt's
  rule: the lit side is the geometric normal's, the BSDF is evaluated in the
  shading frame, a thin surface's Lambert lobe on that side takes |N.L|, GGX
  only where both normals face the sun. Rejected: gating only the reference
  (raster leaves stay unlit there), geometric normals for leaf lighting
  (flat). Furnace checks unchanged (white and leaf 0.999-1.004, sun 0.9994).
- Tuft occlusion on crowns: `plantOcclusion` (2026-11-20, for grass tufts)
  applied to every leaf material, so a crown's lower leaves lost up to 75%
  of their ambient and, through the micro shadow, most of a low sun. Crowns
  (leaf meshes over 4 m tall, the crown visibility's test) now send height
  1; their occlusion is the crown visibility or the GI probes'. Rejected:
  taking plant occlusion out of every plant's sun (tufts keep their look).

Measured (ratio reference / raster per 4x4 cell, mean |ln ratio|):
- Backlit: 0.368 -> 0.130 (0.288 of the old raster against the new
  reference, so most of it is the raster); sunward column 1.90 / 2.41 /
  2.43 / 2.02 -> 1.10 / 1.21 / 1.38 / 1.20.
- Front: 0.066 -> 0.068 (noise); foreground leaves 1.09 / 1.13 -> 1.01 /
  1.06; canopy against the bright background (cells 2,0-3,0, 2,1) now 9-19%
  bright: the tuft darkening was hiding the GI sky term there (backlog).
- Contact shadows off on top: sunward column 1.08 / 1.05 / 1.14 / 0.95 but
  other cells overshoot (0.82); kept (backlog).

## Terrain march: rays that start on a sample line (2026-11-23)

The reference missed the terrain around the forest interior: the camera
(85, -53) lies exactly on a 2 m sample (odd-metre coordinates do), so at s = 0
`terrainRayDistance` took the cell on the positive side of the boundary; for a
ray toward -x or -z that cell exits at once, which the march reads as an edge
cell (exit 1e30) and solves its bilinear patch over the whole ray: a hit at 0
or a miss. The finest GI level's probe columns (2 m, centres at odd metres)
start every ray there too. Fix: the start is nudged across any boundary it lies
on, as the march already does after each step. A Python twin of the march over
napf.height: 42 of 300 random downward rays from the interior camera and 124 of
300 from the default start disagreed with a 0.25 m brute march, 0 after. Edge
view against the reference: backlit 0.128 -> 0.121 (GI), front 0.069 either way.

## Geo channels and causal placement (2026-11-24)

Terrain lane steps 2-3 (research/2026-11-23-terrain-triage.md; spec and
plan `2026-11-23-geo-channels-causal-placement*`). Placement decorated the
terrain with noise masks; real ground puts things where they grow or fall.

- Channels: `meridian-cook --geo` writes 8-bit channels beside the
  heightfield (`.geo`, meridian/geo.h, generic to the engine; meaning and
  encodings in tools/cook/geo_channels.h): topographic wetness index (Beven
  and Kirkby 1979) and specific catchment area from FD8 multiple flow
  direction (Freeman 1991, exponent 1.1; D8 stripes) over a
  Priority-Flood+epsilon filled DEM (Barnes et al. 2014) smoothed to ~4 m
  (Lidberg et al. 2021); topographic position at 6 m and 40 m; annual
  direct sun relative to flat open ground with a 16-azimuth terrain horizon
  at 47 N; rockfall reach below ground steeper than 45 degrees within a 32
  degree energy line (Heim's Fahrboschung), from the source's foot. Own C++
  (tested on analytic surfaces) rather than WhiteboxTools: no external
  binary, runs on any heightfield. Patch 5 s, Napf 15 s.
- Rules: up to four integer channel ramps per `DensityRule` and an
  optional ramp over an earlier layer's raw density
  (`PlacementLayer::parent`, one level), bit-exact on CPU and GPU.
- Forest truth: swissSURFACE3D Raster minus swissALTI3D (canopy over 3 m
  covering 20% of an 8 m cell), `--score-forest` reports Cohen's kappa and
  F1 (the patch is 69% forest: everything-forest scores F1 0.82, kappa 0).
  The truth is an evaluation, never an input (user).
- Result: the best rule is the Napf's land use: forest on slopes from ~14
  degrees, closed by ~34, minus the sunniest slopes (pasture). Kappa -0.225
  -> 0.461, F1 0.403 -> 0.829; wetness, cluster noise and slope variants
  scored lower. Other layers: field trees in the forest's open ground,
  saplings in its gaps, ferns in moist shade, meadow plants by sun and
  wetness, scree rocks along the runout.
- Forest floor: baked per geo texel at setPlacement (placement.comp
  `PLACEMENT_FOREST`) and read with one bilinear fetch; per-fragment
  evaluation cost +6.5 ms.
- Cost: the realistic forest (67% of the patch vs 41%) raises scene early
  where it stands (ravine view 9.8 -> 21 ms). Parity route with `--trees`
  (new baseline): 41.1 -> 27.9 fps average, 1% low 13.6 -> 10.4, scene
  early 12.6 -> 20.3 ms, every segment slower (meadow most: its ground is
  now partly forest). Release startup 26.4 -> 26.8 s. The user kept the
  extent; the performance lane takes the cost (backlog).

## Performance gate restated (2026-11-24)

Phase 2 architecture assessment, delegated by the user to Fable (the
assessment file is `.superpowers/fable-review/architecture-assessment.md`
in the main checkout, git-ignored; no GPU run was possible that session,
so every ms estimate there is a projection from recorded measurements).

The programme's gate was 180 fps average (5.5 ms) and ~105 fps (9.5 ms)
in the heaviest views, scaled from the Chlorophyll Engine's reported
numbers. Chosen instead, as GPU ms on the benchmark route, native 2560x1440
DLAA, GI on:

- route average <= 8.3 ms (120 fps);
- heaviest segment <= 11.1 ms (90 fps);
- 1% low >= 60 fps in every segment, hop frames excluded (new: the line
  the user's "stable" priority needs).

180 fps stays a stretch figure. It is still proof of technique, not a
merge gate (parity programme call 1). Why the old figure goes: the fixed
per-frame costs (aa ~1.4-2.5, GI ~1.9, weather 0.44, clouds 0.3-1.5, post
~0.2 ms: 4.2-5.4 ms) are settled choices, so 5.5 ms is out of reach by
arithmetic before any terrain, tree or shadow is drawn; and the figure
does not transfer: his 130 fps has no stated internal resolution or AA
cost (baseline F16), his figure with GI on is ~120 average and never
below ~90 (F12), and his clouds are 2.5D (F6) where Meridian keeps
volumetric by the user's call.

Also decided: match the result, not the architecture. Programme rule 3
("Chlorophyll's approach by default") becomes: adopt his technique where it
is his stated answer to a problem Meridian measurably has (visibility
buffer, ground cover out of the shadow maps, semi-static shadows). The
deliberate divergences (volumetric clouds, native 1440p DLAA, probe GI)
stay, and the gate pays for them.

Projection after the ordered work (judgment, wide error bars): forest
segments 11-13 ms, meadow and aerial 7-9 ms, route average 9-10 ms. The
first GPU session therefore starts with the measurement repair (split
"cull early", hop frames out of the 1% lows, the R5-1 forest walk, aa 2.45
vs 1.36 ms, the scene-early split re-measured on current trees).

## Visibility buffer is unconditional (2026-11-24)

Parity programme call 3 made the visibility buffer conditional: start it if
the forest edge still exceeds 9.5 ms and sub-pixel shading is over ~1.5 ms
of it after the voxel switch lands. That trigger is met today by a wide
margin (scene early 20.3 ms on the route average; the only breakdown on
record, "Frame cost measured, perf lane", puts about a third each on
geometry, per-fragment cascade PCF and sub-pixel triangle shading, and
predates opaque traced leaves) and will very likely still be met after g1.

Decision: no longer conditional. Its spec is written right after g1
merges, before any new forward per-fragment lighting pass (GTAO first of
all); cluster geometry and voxel quads go through it, terrain stays
forward and is drawn after on the resolved depth. Estimated saving 8-12 ms
on forest segments (PCF, GI reads and material once per pixel; the voxel
overshading goes with it). Risk is high (every material path, motion
vectors, DLSS guides, texture LOD from barycentrics, previous-frame wind
for motion vectors), effort 1-2 weeks at the project's pace.

Consequences: the GTAO spec's forward ambient-radiance attachment (its
decision 2) is dropped, the spec is superseded and rewritten after the
visibility buffer spec, which also chooses GTAO against the visibility
bitmask. The shading math stays in shared functions so the resolve reuses
it (call 3's existing rule). The cascade changes (drop the sun-axis
re-render trigger, relax the 0.25 px foliage caster LOD, static ground
cover out of the cascades, voxel casters) come before it and must not close
the door on static far cascades plus a small dynamic near map (wind).

## VRAM budget (2026-11-24)

New standing gate beside frame time; no spec or gate covered memory before.
Evidence (g1 lane report of 2026-11-24, three tree species, 2-4K placeholder
textures): main holds 12.6 GB before the far field loads and peaks at 14.4
GB on the 16 GB card; g1 holds 14.4 GB and peaks at 16.1-16.7 GB, which
pages to system memory (far-field splat 0.36 s -> ~56 s). The per-pool
breakdown is being measured by the g1 lane.

Chosen (medium confidence on the numbers):

- Resident <= 12 GiB (12288 MiB) and transient peak <= 14 GiB (14336 MiB),
  device-local (VMA's blocks on the DEVICE_LOCAL heaps, the figure the
  capture runs print first), at the showcase views,
  printed by every capture run. Every spec states its VRAM delta. g1 may
  not merge above main's peak.
- Geometry compression (backlog "Cluster vertices are 48 B uncompressed",
  about 4x on tree memory) moves ahead of any new content pack. The
  broadleaf forest floor cook waits for it or for measured headroom.
- To check: how much the TLAS and the full-detail BLAS hold on the default
  raster path. Done: "TLAS and BLAS only for ray-traced modes" (1958 MiB
  at the forest edge).

Rejected: no budget (the g1 overshoot would have merged unnoticed), and a
lower resolution or texture cap as the first lever (it trades the look the
gate protects).

Measurement point (2026-11-25, step 4): the resident figure is the
`Loaded: resident` line, printed once at the first draw, after every load
(the far field included). The `TLAS:` line's "device memory allocated"
comes before the far field and understated it: main at eb10736, forest
edge `--trees`, 2560x1440, read 13282 MiB there, 13956 MiB on the
`Loaded:` line and 13988 MiB at the end of the run (a temporary print;
the first frames allocate the last 32 MiB). The
peak stays the `TLAS:` line's load peak and the `Far field:` line's peak.

## Cull list caps from measured need (G1, 2026-11-24)

- **Caps = the most any evidence view needs at 1440p, plus 50%**, not
  powers of two (nothing in the code needs them): measured with the caps
  raised and every frame's counts (the first frame, without occlusion
  history, needs the most). Voxel quads 24660000 (fly-toward 16435665); the
  other three from the high overview (below). The margin is wide because a
  full voxel list leaves holes where main drew triangles (backlog).
- **The high overview (1119 4354 -431)** first needed 13615592 voxel
  clusters per phase: a far tree drew its terminal groups, 4-8 clusters per
  voxelizing mesh, because regions on either side of the mesh origin never
  merge by halving their keys. The last few clusters now merge into one per
  mesh (`finish()`, cluster.cpp; kVoxelizerVersion 10): 4431087 voxel
  clusters, 5964036 work items, 3640526 nodes, and the caps cover it +50%
  (work items 8950000, nodes 5460000, voxel clusters 6650000): 0 dropped,
  48.3 ms against main's 47.2, device-local peak 14261 MiB. One cluster per
  tree across meshes is the far tile tier's job.
- **Paid for by smaller far-field splat slices**: `kFarSliceBytes` 1 GiB
  -> 256 MiB (slices 7/2/1 -> 30/5/1, splat 335 ms, image unchanged within
  repeat-run noise); device-local peak 14671 -> 13874 MiB before the caps,
  14014 MiB with them (all heaps 14110, main 14743).
- **Memory figures are device-local** (VMA blocks on DEVICE_LOCAL heaps;
  the all-heaps total in brackets), and the peak now covers model load
  (sampled when a pool grows: 13114 MiB, below the far-field peak).

## Voxel clusters cast into the cascades (G1 Task 8, G11, 2026-11-24)

- **The same voxels, seen from the sun, in cascades 0-3**: cascade 4
  (`shadowFarCells`) switches every cell with trees to its far cell
  (cell_cull.comp skips the pixel test in a shadow view), so placed trees
  cast there as far-cell voxels and only hand-placed instances reach the
  cluster cull. In cascades 0-3, cluster_cull.comp lists voxel clusters as
  in the main view, voxel_bricks.comp keeps a voxel
  where the stable coverage hash falls under its opacity along the sun (and
  drops those hidden deeper than kVoxelHiddenDepth toward it), and
  voxel_instance.mesh draws each as an orthographic quad facing the sun,
  depth only. No cascade path disables LOD pruning: the cascades keep
  their orthographic LOD (1 texel, 0.25 in cascades 0-1).
- **Self-shadow counted once** (superseded inside cascades 0-3 by "Voxel
  sun visibility from the cascades", 2026-11-25): voxel_instance.frag already attenuates by
  the voxel's optical depth out of its model toward the sun and traces from
  where the sun ray leaves the mesh's box, so the cascades add only other
  trees. A caster moved half a voxel away from the sun changed nothing
  (edge 21.11 -> 21.27), so the voxels' own leaves were not the darkening.
- **Coverage against the ray-traced truth** (`--shadows rt`, full BLAS),
  1440p, region mean: forest edge (rows 200-1000) g1 21.11, main ray traced
  21.41, main raster 28.95; overview crop g1 97.97, ray traced 99.19, main
  raster 106.75. Main's coarse cascades under-shadow (thin leaves simplify
  away at one texel); g1 lands within 1.5% of the truth.
- **Cost** (all five cascades in one frame, `sun shadows:` max): edge 84.7
  ms against main's 80.6, overview 27.0 against 40.7.

## Voxels inside the cluster DAG (G1, 2026-11-24)

- **What**: tree models carry voxel levels inside their cluster LOD DAG
  (vendored clusterlod.h hook, `cluster.cpp`); distant foliage switches per
  cluster group from triangles to voxels drawn as camera-facing quads lit
  through the leaf model, cast into the sun cascades (entry above), and the
  far field's templates come from the same voxelizer. Spec
  `specs/2026-11-24-dag-voxel-foliage-design.md`, plan and ledger beside it.
- **Measured against main, 1440p** (plan ledger, evidence index): forest
  edge 24.5 ms against 37.1, summit 20.4 / 32.0, overview 22.4 / 25.2,
  interior looking up 18.1 / 21.8, fly-toward 24.4 / 32.9, far horizon 26.0
  / 31.7; high overview 48.8 / 47.2 (main's image drops 463,757 entries, the
  criterion is waived there); device-local peak 14261 MiB (all heaps 14357,
  main 14743); warm startup 81.2 s / 85.5 s; forest still jitter 0.49 / 2.66.
- **Far-field palette, an accepted deviation (user)**: one palette entry
  per mesh slot (`kFarSlots` = 8, reflected and transmitted colour), not
  per-voxel albedo and transmission as the plan's Task 6 said; revisit with
  the far tile tier.
- **The darker forest is accepted as correct shadowing (user)**: against
  main ray traced (`--shadows rt`) on fly frame 010, OKLCh, g1's residual is
  at most L 0.021, C 0.001, h 1.5 deg per region, while main's raster is
  brighter by L 0.021-0.135 and less blue by up to 93 deg of hue in the
  understory (more shadow raises the sky's share of the light). Overview
  ground (main raster's lit gaps): g1 96.67, ray traced 101.21, main raster
  144.84; there g1's mask reads lit 6.9%, crown 25.5%, shadowed 67.6%
  against 17.3 / 11.5 / 71.2 ray traced: voxel crowns cover more of the
  gaps from above, the shadowing matches. Brightness is a lighting and grade
  matter, not a g1 change.

## Caster LOD of one texel in every cascade (G1, 2026-11-24)

- **Why**: with the voxel casters and the cascade counters read back, the
  sun cascades dropped entries at five of the seven evidence views (forest
  edge 12.85M). Per cascade at the edge (`--shadow-refresh`), cascade 1
  saturated both lists: drawn clusters at kMaxDrawnClusters (2^21, also
  near the GPU's 2^22 mesh dispatch limit) and voxel quads at 24.66M, while
  cascades 2-3 at one texel needed 7-18k triangle clusters. Cascades 0-1
  kept a 0.25-texel caster error "for thin foliage": several casters per
  texel of a 2048^2 map that filters over +-1.5 texels.
- **Chosen**: `lodErrorPixels` 1 in every cascade, for triangle clusters
  and voxel levels alike (they share the threshold). Pulled forward from
  resume.md step 3 ("relax the 0.25 px caster LOD").
- **Measured, 1440p** (`--shadow-refresh`, every cascade every frame):
  drops 0 at edge, interior looking up, fly-toward, overview within the
  committed caps (max cascade drawn clusters 939289, quads 15.5M); GPU
  109.6 -> 78.6, 85.6 -> 60.3, 92.8 -> 64.8, 49.2 -> 47.3 ms; edge without
  the flag 24.7 ms; peak unchanged (14261 MiB device-local).
- **No thinner shadows against the ray-traced truth** (main `--shadows
  rt`): overview ground mask mean 96.66 (0.25: 96.67; truth 101.21), classes
  unchanged; OKLCh edge canopy/understory L -0.005/-0.007 (0.25:
  -0.004/-0.007), fly-toward canopy L +0.005 (0.25: +0.011). Near dappled
  shade, canopy from 40 m above: mean 84.78 -> 85.42, look side-by-side
  `.superpowers/g1/c1/sbs_canopy_025_vs_1.jpg`.
- **User, 2026-11-24: keep one texel in every cascade** (saw the side by
  side, found the two nearly identical).

## Voxel sun visibility from the cascades (voxel-look fix 1, 2026-11-25)

- **Problem**: DAG voxels read darker and bluer than the triangles they
  replace: OKLab L in the voxel mask -0.047 to -0.061 at 60-12 m, hue
  +14 to +22 deg. Cause (voxel-look diagnosis): the box-exit design
  ("Self-shadow counted once") shades a voxel through exp(-selfDepth), its
  optical depth out of the whole model; per pixel at 30 m that is mean
  0.158 (p10 0.009, p50 0.093), and with the cascade term 0.123, where a
  forced-triangle leaf seen at the same pixels gets 0.353 (p10 0.013,
  p50 0.262, p90 0.870). That rule predates voxel casters in the cascades.
- **Chosen**: where cascades 0-3 hold the voxels as casters (inside
  cascade 3's radius before its blend into the far-cell cascade 4, which
  holds the same model as far cells), the voxel takes the sun as a leaf
  does: traceSunVisibility at the voxel plus one voxel edge toward the sun,
  no exp(-selfDepth). Beyond, the box-exit sample with exp(-selfDepth)
  stays. Triangles lit through the same voxel casters had already matched
  the ray-traced truth (L 0.461 against 0.463), so the casters carry the
  crown's own shadow.
- **Measured, 1440p, voxel mask, against `--voxel-lod near`**: sun
  visibility at 30 m mean 0.389 (p10 0.006, p50 0.284, p90 0.962) against
  the triangles' 0.353. Delta L 60/45/30/20/12 m: -0.052/-0.047/-0.060/
  -0.058/-0.061 before, -0.001/-0.003/-0.026/-0.010/-0.022 after; hue gap
  +15 to +22 deg -> -8 to +4 deg. Backlit sun at 30 m -0.075 -> -0.039,
  high sun -0.085 -> -0.025. Local contrast at or under the triangles'
  (30 m 0.0912 against 0.0979): no acne from the coverage hash. Frame time
  unchanged (forest edge 24.62 -> 24.52 ms, overview 22.62 -> 22.36 ms),
  0 dropped.
- **Variant rejected**: one global clumping factor, exp(-0.4 selfDepth)
  (calibrated offline to the triangles' mean): delta L -0.012/-0.009/
  -0.033/-0.022/-0.032, worse at every stop, and it compresses the
  distribution (p10 too high, p90 too low) where the cascade lookup keeps
  its shape. The 4-bit 0.5-step depth quantization does not show in local
  contrast (clump 0.0909 against default 0.0909 at 30 m).
- **Not accepted yet**: 30 m and 12 m stay outside +-0.015, and backlit and
  high sun too, while the sun visibility already exceeds the triangles'.
  The rest of the gap is in the voxel's shading terms (aligned share,
  spherical-leaf phase, specular), not its visibility. `--shadows rt` keeps
  the box-exit path (no cascade radii there): unchanged, L 0.415.

## Solid parts voxelize no finer than their model (voxel-look fix 2, 2026-11-25)

- **Problem**: the beech's trunk-base mesh (8,990 triangles, 2.91 m tall,
  overlapping the bark from 1.57 m) went to voxels at 4-18 m with
  3.9-15.6 mm edges, while its leaves switch at 68-80 m and its bark at
  66-148 m: a voxel pattern on the trunk at walking distance, z-fighting
  against the bark in the overlap (local contrast 0.035 against 0.003 as
  triangles at 12 m) and grass through holes in the voxel shell at 60 m.
- **Cause**: not a stalled simplifier. buildClusterLod floors the voxel
  edge at the mesh's own height / 512 (the comment's intent was the
  model's: "~117 m for a 25 m beech"). 2.91 / 512 = 5.7 mm floors to
  2^-8 m = 3.9 mm, exactly the finest dumped edge, so the trunk base
  switched as soon as its triangles' error reached millimetres. The holes:
  a voxel on a solid surface keeps itself by the coverage hash against its
  opacity, which a thin shell at mm edges leaves under 1.
- **Try (rejected)**: every primitive of a voxelizing model takes the
  model's height. Foliage meshes shorter than their model then switched
  later too: voxel pixels at the stops halved (30 m 0.121 -> 0.063 of the
  frame).
- **Chosen**: only solid (non-foliage) parts take the model's height
  (`MeshData::voxelModelHeight`, set by loadGltf from the same foliage
  test voxelizesModel uses); foliage keeps its own. No asset names, no
  distance constant. kVoxelizerVersion 10 -> 11 (the cluster cache files).
- **Measured, 1440p**: trunk bases are triangles at 60, 30, 12 and 6 m
  (tint frames, `.superpowers/voxel-look/cap/tint_fix2_sbs.jpg`), so the
  overlap z-fight and the 60 m holes are gone at these stops; voxel
  pixels 0.121 -> 0.116 at 30 m (crowns kept). In the old voxel mask,
  with fix 1, delta L against `--voxel-lod near` +0.006 at 30 m and
  -0.004 at 12 m: solid branch pixels that voxels covered are triangles
  again; in the new mask (still voxels) -0.024 and -0.020, fix 1's
  remaining gap. Cost: forest edge 24.52 -> 25.51 ms (+2.1M triangles),
  overview 22.36 -> 22.40 ms; 0 dropped, cascades included.
- **Still open**: a solid part voxelizes beyond its model's switch with
  the same thin-shell coverage, so a far trunk can still show holes.

## Ray-traced shadows: voxels trace from themselves (voxel-look item 4, 2026-11-25)

- **Problem**: fix 1 gave voxels the sun from the cascades at the voxel
  only inside cascade 3's radius; `--shadows rt` fills no cascade radii,
  so under it voxels kept the box-exit sample (exp(-selfDepth) times a
  ray from where the sun leaves the mesh box), and the ray-traced A/B no
  longer took the same path as the raster default.
- **Chosen**: under `--shadows rt` the voxel takes the first branch at
  every distance: one voxel toward the sun, then traceSunVisibility, whose
  ray meets the model's own triangles in the TLAS as a leaf's ray does
  (one condition, voxel_instance.frag). Beyond kFarShadowDistance the far
  branch is the same one the triangles take.
- **Measured, 1440p, voxel mask, against `--voxel-lod near` under the
  same shadow mode**: RT 30 m -0.047 -> -0.026 (raster -0.025), 12 m
  -0.048 -> -0.021 (raster -0.019): the RT gap now mirrors the raster one.
  Overview from above (700 1450 -400): RT voxel-tree pass 19.05 -> 21.46 ms
  (one run; debug path only), L 0.577 -> 0.578. Raster default unchanged
  (the condition is false there). 0 dropped, 0 validation errors.

## Voxel crown residual after fixes 1-2: what it is (voxel-look round 3, 2026-11-25)

- **Coverage was sky, not pale branches**: the albedo guide marks sky with
  0.498 grey; the round-2 "pale branch" pixels (B/G 1.0-1.5) were sky seen
  through crown gaps. Share of the voxel mask: `--voxel-lod near` 6.7%
  against voxels 1.9% at 30 m, 2.6% against 1.1% at 12 m. With sky taken
  out, albedo matches (near/voxels 0.986x, 0.979x).
- **Split of delta L** (voxels - near, png OKLab, class mix vs within):
  30 m -0.023 = coverage (sky gaps) -0.012 + within leaf -0.0045, grey
  -0.0052, sky edge -0.0018; 12 m -0.018 = coverage -0.003 + leaf -0.0076
  + grey -0.0063.
- **Shading terms per albedo** (linear, sky-normalised, voxels/near): sun
  diffuse 0.704x (30 m) and 0.792x (12 m); ambient 0.973x and 1.009x;
  specular 1.29x and 1.12x (absolute). The sun visibility is already
  higher (0.389 against 0.353), and leafPhase = Gamma / (pi G) matches the
  visible-leaf integral (backscatter 2 rho / 3 pi), so the sun-diffuse gap
  is in the voxel's inputs (lobe axis, alignment, transmission bake) or in
  how its one aligned leaf stands for many: a design question, no try
  made (backlog).
- **Handover at cascade 3**: voxel luminance over distance (start view
  raised to 1700 m, sky-normalised) steps 0.2165 (1050-1075 m) -> 0.1543
  (1100-1125 m); the box-exit path is ~0.72x the cascade path at the same
  distance (cascades everywhere: 0.219, box exit everywhere: 0.154). A
  visible seam ring at ~1.1 km; backlog.


## Voxels take the sun from the cascades at every distance (voxel-look seam fix, 2026-11-25)

- **Problem**: fix 1 gave voxels the cascades inside cascade 3's blend
  start (~1088 m) and kept, beyond it, the model's optical depth toward
  the sun times a trace from the mesh box's exit. The two disagree: crowns
  stepped darker and bluer at ~1.1 km, a ring seen from above (round 3:
  Y/sky 0.2165 -> 0.1543).
- **Premise**: past cascade 3 the cascades still cover the voxel:
  cascade 3 blends into cascade 4 up to 1280 m, and cascade 4 (5120 m)
  casts every cell's far cell, the bricks marched along the sun to opacity
  0.5, its own tree's included. So the voxel's own model shades it there
  too, coarsely (5 m texels): the self-shadow keeps the cascade path's
  level, not the box exit's. Beyond kFarShadowDistance (4000 m)
  traceSunVisibility takes its far path either way.
- **Checked against `--shadows rt`** (valid since 295d8ed), two views from
  1700 m spanning the seam (`1119 1700 -431` yaw 0 and 1.55, pitch
  -0.25), linear captures normalised by clear sky, voxel pixels by
  distance (OKLab of the normalised linear): step of crown L from
  950-1088 m to 1088-1180 m, view yaw 0 / yaw 1.55: ray traced +0.014 /
  +0.029; box exit (before) -0.034 / +0.015; cascades everywhere +0.017 /
  +0.039; the variant, box exit with selfDepth x 0.19 (matched to the
  cascades in 1088-1180 m in both views), +0.018 / +0.040. Hue across the
  step (yaw 0): ray traced 184 -> 189, before 153 -> 201, cascades
  153 -> 153, variant 153 -> 152; p10/p90 follow the ray-traced step with
  the cascades (p90 +0.014 against +0.014), the variant compresses them
  (p10 +0.038, p90 -0.000). Images: `.superpowers/voxel-look/cap/r4/seam_{n,e}/sheet.jpg`.
- **Chosen**: cascades everywhere; the box-exit branch, `voxelBoxExit` and
  the voxel's mesh-space centre varying go. No tuned constant; the
  variant matched the mean but not the spread.
- **Still off, not the seam**: inside 1088 m the cascade path is already
  brighter and greener than the ray-traced one at ~1 km (view yaw 0:
  +0.034 L, hue 153 against 184; backlog), and stays so beyond.
- **Cost**: overview (`700 1450 -400 -1.55 -0.5`), two runs each: GPU
  22.57 / 22.41 -> 22.10 / 22.07 ms, voxel trees 7.75 / 7.66 -> 7.30 /
  7.28 ms; 0 dropped, cascades included.

## Voxel look round 4: target switch, far trunks, small trunks, overview hue (voxel-look, 2026-11-25)

- **Switching tree**: forest hornbeam Forest_05 (instance 1621093, scale
  1.05; prototype ids checked against the glTF bounds), approached from
  45 m to 10 m with a clear corridor. Its leaves switch between 45 and
  30 m (tint: voxel share of the target 0.90 at 45 m, 0.01 at 30 m), not
  at the dump's 17-24 m (that is the 1 px estimate; voxels switch at
  kVoxelPixels x the threshold). Stops, target-crown mask, voxels against
  `--voxel-lod near`: 45 m dL -0.032 (p10 -0.008, p50 -0.051, p90
  -0.034), C 0.041 against 0.050, h +6.7; 30/20/12 m (triangles) within
  0.009. Walk at 5 m/s, frame pairs 3 frames apart: while the target is
  voxels (30-46 m) frame-pair |dL| exceeds the triangles' by up to
  +0.046 and L by -0.062 at ~37 m; across the switch (24-30 m) up to
  +0.022; after it +0.001. The bar fails on this tree.
- **Far trunks**: at 120-140 m (lone alder view) the trunks are still
  triangles inside voxel crowns; at 200-300 m voxel trunks are 1-2 px
  with no visible holes, in a frame half covered by a near branch.
- **The voxel trunk at 22 m**: a beech Forest_08 (7.6 m model): fix 2's
  floor gives its solid parts 15 mm voxels, switching at ~17-20 m.
  Proposal (backlog): floor a solid cluster's edge at its own thickness.
- **Overview hue** (raster h 126.6 against ray traced 136.6 for the mean
  linear colour of the voxel pixels, 97% under 1088 m): specular is the
  same in both (h 291, 3.6-3.7% of Y); the diffuse term carries it (h
  123.7, C 0.038 against 130.6, 0.033): the cascades give far crowns
  more sun than the ray (backlog).

## TLAS and BLAS only for ray-traced modes (step 4, 2026-11-25)

The raster default traced one ray query: in `traceSunVisibility`
(lighting.glsl) a receiver past `kFarShadowDistance` (4 km, with the far
shadow map on) skipped the cascades and queried the TLAS for the non-tree
casters (rocks, scree, saplings, hand-placed models). Those casters are not
drawn past 1500, 600 and 150 m, so in raster mode a far receiver now takes
objects from the far field alone. The ray query now runs only when
`scene.shadowMode` is 1 (`--shadows rt`), a value uniform over every draw,
and in reference.comp, which only `--reference` dispatches; both modes
build the TLAS, so no shader executes a query on a null TLAS address.

- `Renderer::setRayTracing(bool)` (default off, before `addInstance` and
  `setPlacement`; the game sets it for `--shadows rt` or `--reference`)
  builds the prototype BLAS, the hand-placed meshes' BLAS, the TLAS, its
  scratch and record buffers. Off, setPlacement uploads the instance table
  and prints `TLAS: off`. `ShadowMode::Rt` or `beginReference` without it
  throws. `--far-shadows tlas` needs no TLAS of its own: in raster mode it
  turns the far shadow map off and the cascades shade receivers to 5.12 km.
- The earlier claim ("Raster sun shadows are the default") that the far
  field's bake uses the TLAS and BLAS no longer holds: renderer_farfield.cpp
  bakes from the meshes' voxels. The GI probes never used them either.
- Measured (Release, 2560x1440, `Render 2560x1440, display 2560x1440`;
  main eb10736 + the `Loaded:` print, then this change):

  | forest edge `--trees` | main | this |
  |---|---|---|
  | `Loaded: resident` (MiB) | 13956 | 11998 |
  | end of run (MiB, temporary print) | 13988 | 12030 |
  | peak at models loading / far field (MiB) | 13054 / 14160 | 13086 / 12411 |

  The pools held TLAS 598, scratch 261, records 209, 97 prototype BLAS 800
  and 15 mesh BLAS 3 MiB (1871 MiB); VMA's blocks drop 1958 MiB. Start view
  (no trees): `TLAS:` line 3712 -> 3608 MiB. Resident is now 290 MiB under
  the 12288 MiB gate.
- Look: forest overview `700 1450 -400 -1.55 -0.5` and a high view to the
  horizon `700 1800 -400 -1.55 -0.12`, both `--trees`, main against this:
  mean difference 0.616 and 0.2295/255, SSIM 0.9978 and 0.9981; two runs of
  main differ by 0.607 and 0.2291/255 (SSIM 0.9979, 0.9981). No difference
  above run-to-run noise.
- `--shadows rt` (forest edge) and `--reference 16` (start view) still run
  with 0 validation errors.

## Debug views and report bundle (debug-tools round 1, 2026-11-25)

The user judges by eye, the lanes by numbers; between them was a
256-colour screenshot and a hand-rebuilt pose. Engines close that gap
with buffer visualisation modes (Unreal's Buffer Visualization and Nanite
Visualize: Clusters/Triangles/LOD; Frostbite and id Tech debug views) and
an editor pick that reads a per-pixel object id. Round 1 builds the
minimum of that: an id target, a report key and two views.

- Premise: there was no per-pixel instance or cluster id. The
  architecture's "visibility buffer" is per-instance occlusion flags, not a
  deferred id buffer; scene attachment 6 was already the sun light (the
  doc's "6-7 reserved" was stale), 7 was free.
- Id: scene attachment 7, RGBA32UI (kind << 24 | 24-bit instance id,
  cluster index or far entry, distance, material), written by mesh.frag
  (clusters through a mesh.mesh varying, and the terrain),
  voxel_instance.frag, voxel.frag and sky.frag through one include
  (debug_id.glsl). Mesh and material names are the glTF names loadGltf
  passes to `Renderer::setMeshName`/`setMaterialName`; the cluster's DAG
  level comes from a CPU table built at upload from each level's BVH.
- First try: the id written always, to a null attachment when off
  (VK_EXT_dynamic_rendering_unused_attachments). Forest edge `--trees`
  (Release, Render 2560x1440 DLSS): scene early 10.73 -> 13.44 ms, GPU
  23.96 -> 26.70 ms. A null attachment does not make the export free.
- Kept (the variant): DEBUG_ID shader variants and a second set of the
  eight scene pipelines with attachment 7, made only while a view or the
  report is on (`createScenePipelines(true)`; one hitch on toggle). Off,
  the pass has seven attachments and main's shaders: scene early 10.89 ms,
  GPU 24.07 ms (single-frame numbers, run noise). `Loaded: resident 11998
  MiB` before and after; the id target (59 MB at 1440p), the debug albedo
  (only when no diffuse albedo exists) and the level table exist only
  while on.
- Views bypass the upscaler: debug_view.frag replaces the tonemap and reads
  attachments 3 and 7 at the nearest render pixel. Report: shot (the frame
  without view or crosshair), every view of the same frame rendered
  offscreen from the kept attachments, pixel.json at the centre pixel,
  look.json and a repro line (`--game-time` added so an interactive
  moment's clock can be matched).
- Measured: trunk view `1320.78 1292.08 -468.17 0.369 0.6 --sun -0.207
  0.819 0.535 --trees`, report at frame 60: pixel.json names
  `SM_BlackAlder_Field_04.gltf`, triangles, DAG level 0, 1.22 m; its
  repro.txt's capture differs from the shot by 0.5735/255 mean, two runs
  of the same repro line by 0.4909/255 (this `--trees` view is not
  deterministic to 1e-6). Forest edge with `--debug-view lod`: a European
  beech (Forest_03) at 19.9 m, level 1. 0 validation errors in every run
  (Release with asserts and validation).
- Pre-merge (2026-11-26): the user's interactive F5/F6 test passed (18
  bundles; triangles, voxels to 1.27 km, terrain picked right). Debug
  (ASan/UBSan, sync validation) start view with `--placement off
  --debug-view lod --report-at 20`: 0 validation errors, no sanitizer
  report. Far-cell pick: `--camera 6000 2500 6000 -0.84381 -0.08295 --trees`
  (aimed at a far-cell pixel of the lod view): far cell 5902, level 1,
  17.98 km; looking along the grid's diagonal, voxel clusters (level 11)
  cover everything to ~8.6 km and far cells only show near the horizon.
  The lod legend (cli.md, legend.txt) is printed from the shader's
  formulas.

## Debug tools round 2 (2026-11-26)

Round 1 named what a pixel shows; round 2 says why it looks that way:
which light it gets, how its texture sits, how bright it is, and what it
looks like alone. Engines ship the same set: Unreal's view modes
(Lighting Only, Reflections, Required Texture Resolution / texel density),
its HDR luminance visualisation and camera-style false colour (EV bands
around 18% grey), editor isolate/hide of the selection, and a paused game
clock with a free camera.

- Light split: the scene pass's eight colour attachments are all in use
  in the debug set (NVIDIA's maxColorAttachments is 8), so the debug
  fragment shaders store the split in a buffer instead: 32 B per render
  pixel (depth; sun diffuse, sun specular, ambient diffuse, ambient
  specular as exposed halves; stretch). Fragment stores have no raster
  order and run before the late depth test (discard, written depth), so
  the store sits in a `pixel_interlock_unordered` section
  (VK_EXT_fragment_shader_interlock) that keeps the nearest depth by
  hand. Sun = the light before `addAmbientLight` (leaf transmission
  included), ambient = the rest; both after haze transmittance, before
  contact shadows. Check: at the user's alder pixel the four parts sum to
  the HDR value within 0.2% (R 261.8 vs 261.9 cd/m^2).
- Stretch: the singular values of the base colour texture's texel map in
  the surface plane (texels/m), from screen derivatives of UV and
  position; the view colours their ratio over an 8 x 8 checker, the pick
  reports the geometric mean and the ratio.
- Luminance: the HDR attachment (pre-exposed, so 0.18 is grey) in EV,
  eleven bands; the pick also reads HDR and the exposure the frame used.
  HDR gains TRANSFER_SRC for that texel.
- Solo/hide: `SceneData.debugFilter`, applied in the debug mesh shaders
  (clusters, also in the masked depth pre-pass so a hidden surface leaves
  no depth; terrain; voxel quads by gl_CullPrimitiveEXT); far cells
  discard. Shadows, GI and blended surfaces are not filtered (a hidden
  tree still shades its surroundings).
- Cost on (forest edge `--trees`, Release, Render 2560x1440 DLSS, the sun
  view): first try, filter by discard in the fragment shaders, GPU 62.02
  ms, scene early 32.31 ms: the stores are side effects, so the opaque,
  terrain and voxel-quad shaders lost early depth testing and shaded
  every hidden fragment (ray-queried sun included). Kept (the variant):
  filter in the mesh shaders, `early_fragment_tests` declared in those
  debug fragment shaders: GPU 32.53 ms, scene early 16.45 ms (off 24.25 /
  10.89). The remaining +5.6 ms in scene early (id target, stores,
  interlock; not split apart) exists only while a view, the report or a
  filter is on.
- Time freeze: the game clock stops (wind, clouds, weather); the renderer
  takes only exposure adaptation and upscaler timing from frame time.
- Off: no buffer, no filter, main's shaders; SceneData +16 B, the
  interlock and fragmentStoresAndAtomics features enabled at device
  creation, HDR + TRANSFER_SRC. Forest edge `--trees` before (main
  4f01f72) / after: `Loaded: resident 11998 MiB` both; GPU 24.43 / 24.25
  ms, scene early 11.03 / 10.89 (single runs, noise); images differ by
  0.2451/255 (the `--trees` floor, backlog).
- First use, the user's alder bundle (`~/meridian-reports/2026-11-25_23-26-21`,
  its repro line + `--report-at 120`): the centre pixel is the
  `Decoration` material (a masked, double-sided shell over the bark), in
  shade: sun 0; ambient diffuse (118.3, 106.3, 91.5) cd/m^2, ambient
  specular (143.5, 161.5, 192.9), 55% of the pixel and sky-blue. Hiding
  the material (`--debug-filter hide-material 60`) shows the `Tileable`
  bark 5.8 cm under it with ambient specular (26.6, 31.4, 35.2), 19%.
  Its ORM roughness mean is 0.154 against the bark's 0.367
  (`meridian-cook --stats`). Over all shaded bark in the frame (from the
  views, the neutral tonemap inverted) specular is 29% of luminance and
  the ambient diffuse over the albedo leans green (G/R 1.07, B/R 1.01).
  So the wet gloss is the Decoration shell's low roughness reflecting the
  sky, and the tint is that blue reflection plus a slightly green
  ambient over a brown albedo; no sun involved. Fix deferred (backlog).

## Trace only cards that transmit (trunk-fix round 6, 2026-11-25)

- **Problem:** the tree cook traced every alpha-tested, double-sided
  material into opaque geometry (`isLeafMaterial` && alpha &&
  !OPAQUE), so the Megascans "Decoration" cards (lichen, knots on the
  bark) became opaque shells. On the lone alder at 1.2-3.7 m they were
  the pale patch, the white shards and the dark shell; hiding the
  Decoration material removed all three (round-6 premise captures).
- **Practice:** glTF MASK is the standard for alpha-tested cards; the
  case for tracing (decisions.md "Tree leaves as opaque geometry") is leaf
  shimmer in a crown seen through thousands of cards. Decoration sits
  flat on the bark: few cards, no see-through crown.
- **Choice:** a generic rule by glTF material property, not by name:
  trace a leaf material only when it transmits light
  (KHR_materials_diffuse_transmission with factor > 0, or no extension,
  which the cook already reads as the default leaf transmission). Every
  other masked material whose base colour has alpha is written as glTF
  MASK with its source cutoff (0.5 in the Unreal export). The engine
  already draws MASK through the masked pipeline and the voxelizer
  already tests base alpha against the cutoff for MASK materials
  (voxelize.cpp `surfaceAt`), so no engine change.
- **Leaf safety:** the leaf rule is unchanged, not the leaf bytes: leaves
  and decoration share one triangle budget per tree, so the traced
  decoration's triangles (e.g. alder Field_04: 117k of 1.29M) no longer
  push the leaves' simplify ratio down (0.931 -> 1.0). Proven: leaf
  textures and leaf material JSON byte-identical in all 30 cooked trees,
  the 10 trees without decoration byte-identical end to end, and the
  1112aa1 cook reproduces the shared cooked trees byte for byte.
- **Cost:** decoration cards are alpha-tested again, so their edges can
  shimmer in motion the way traced leaves no longer do (few cards, on the
  bark). Leaves keep their full budget, so the trees carry more
  triangles; accepted by the user. Measured at the forest edge with
  `--trees` (2560x1440, DLAA, on main 0724245): resident 11998 ->
  12061 MiB (+63), models-loaded peak 13167 MiB, 18605468 -> 18725035
  drawn triangles, GPU 24.37/24.48 -> 24.77/23.95 ms (noise). Motion on
  the lone alder at 6 m (`tools/motion-clarity.sh` forest cases moved
  there, the 1112aa1 cook before): jitter still 0.81 -> 0.81, side 5 m/s
  6.29 -> 6.29, side 30 m/s 6.74 -> 6.75, turn 1 rad/s 3.40 -> 3.40;
  the decoration shimmer does not show at frame scale.

## Multi-view capture in one launch (debug-tools round 3, 2026-11-26)

A capture with `--trees` spends ~18-20 s loading for a few seconds of
rendering, and the lanes' look sheets were one launch per view, each
waiting on the shared GPU lock. `--capture-views FILE DIR` loads once and
renders every line of FILE. Engines handle a teleport the same way: a
camera cut (Unreal's `bCameraCut`) resets the temporal upscaler (DLSS's
reset flag, TAA/TSR history), eye adaptation and the GI history, and the
cached shadow pages are invalidated.

- `Renderer::cut()` waits for the GPU and puts back what a fresh launch's
  first frame finds: `historyValid` off (the DLSS reset flag, TAA, cloud
  temporal and sky capture, previous matrices and wind), the GI buffers
  zero-filled as at load (the reset flag alone re-traces the probes but
  the ground cache keeps every albedo it has seen: after the overview the
  alder trunk at 1 m came out 1.26/255 darker, green-weighted, and 60
  settle frames before the cut made it 3.16, since they only fed that
  cache), `shadowStale` (every sun cascade re-rendered), the terrain shadow map
  and the far shadow map rebuilt (both are sun-keyed caches, so a view
  whose sun differs from the last by less than their rebuild threshold
  would otherwise keep the old one), `pyramidValid` off, the cloud wind
  displacement, the exposure state back to its initial EV100 15, and the
  jitter phase and noise frame index counting from 0 again (`cutFrame`;
  the frame-in-flight slots keep counting), the cloud shadow grid's
  travel and the previous camera position. The streamed meadow ring
  refills all its slots in the first frame after a jump without help.
  Settle frames: 0 (none needed once the reset is complete).
- Equivalence (Release, `--trees`, 120 frames; mean |difference|/255 of
  the multi-view PNG vs a single launch of the same view, `image-gates.py
  diff`), views in list order: overview (first) 0.607, its own
  single-vs-single 0.608; alder trunk 1 m 0.365 (single-vs-single 0.216;
  a residual mean +0.35 in G remains, below the noise); the same with
  `--debug-view sun` 0.0002; forest edge 0.299 (backlog noise 0.25-0.49).
- Exposure is reset, not settled: a fresh capture adapts from the initial
  EV over its frames, and only a reset makes a view's frames repeat that.
- Per-view options are the ones that change without a reload; each line
  is parsed by the same parser as the command line, appended to the
  launch's arguments, so the launch's flags are the defaults and a line
  overrides them. Reload flags on a line are an error.
- The report bundle's repro.txt carries the shot as a view line too
  (named after the bundle), and the parser skips repro command lines, so
  bundles concatenate into a list.
- Speed (Release, `--trees`, 2560x1440): 18 views in one launch held the
  GPU lock 169.7 s, against the usual ~20-30 s a single launch holds it
  (estimate, not measured: the 18-launch timing was skipped).

## Voxel switch distance floor (crown lane, 2026-11-26)

- Problem: the voxel switch is per cluster by projected size
  (`kVoxelPixels` 2), so trunks voxelize at ~33 m and crowns at 45-55 m
  (user bundles 2026-11-25 23-25-29, 23-28-08, 23-25-19), and the user sees
  trees twitch as they cross it. `kVoxelPixels` 1 moved the switch hardly at
  all, made voxels finer and overflowed the cull lists (round 5, rejected).
- Choice: a distance floor per instance. `Renderer::setVoxelNear(M)` /
  `--voxel-near M`: in the main view, an instance whose origin is within M
  m of the camera keeps its triangles (the voxel-LOD-off path of
  cluster_cull.comp, cluster_traverse.comp and cull.comp: no voxel cluster
  draws, the triangles voxels replace are terminal); beyond M the pixel
  rule applies unchanged. One mechanism, `voxelLodAt` (common.glsl) reading
  `SceneData::voxelNear`; the CPU reference is `voxelsAt` plus
  `selectClustersBvh(..., voxels)`, tested against a flat cut
  (cluster_test.cpp). The cascades keep their voxel casters, so the light
  is the default's and only the main view's geometry changes (as the old
  near mode did). `--voxel-lod near` is now `--voxel-near 150`: the old
  `kVoxelLodNearDistance` was 150 m, so its meaning is unchanged.
- Measured (Release, 2560x1440, `Render 2560x1440, display 2560x1440`,
  `--trees`, two runs each, `GPU N ms` of the `Rendered 120 frames` line):

  | forest edge `600 1205.6 -400 -1.55 0.02` | M 0 | 60 | 100 | 150 |
  |---|---|---|---|---|
  | GPU ms | 24.47, 24.63 | 24.25, 24.30 | 25.60, 26.46 | 31.25, 31.00 |
  | triangles | 18.6M | 18.9M | 26.6M | 57.8M |
  | scene early ms | 10.9 | 10.9 | 13.0 | 19.7 |
  | main view drawn clusters (cap 2^21) | 281995 | 282175 | 310903 | 542319 |

  `Loaded: resident 11998 MiB` and the peaks (13086 MiB while models load,
  12411 MiB at the far field) are the same at every M; 0 dropped by full
  cull lists, cascades included, 0 validation errors. The forest overview
  `700 1450 -400 -1.55 -0.5` is 22.1-22.6 ms at every M: no tree is within
  150 m of that camera.
- Default stays 0 (the pixel rule alone) until the user picks 60 or 100 m by
  eye on a walk; 150 is ruled out (3x triangles, +6.6 ms).

## Vertex compression (2026-11-26)

- **Problem:** cluster vertices were 48 B (float position, normal,
  tangent, uv) in the shared vertex pool, the BLAS and every vertex read
  in the mesh, shadow and ray hit shaders. The trees dominate the pool,
  and the trunk-base fade needs one more channel (COLOR_0 alpha).
- **Choice:** a 20 B GPU vertex (`PackedVertex`, vertex_pack.h, five
  uint, std430 stride 20), packed at upload only; the CPU `Vertex` stays
  float (plus a float alpha). Layout: position 3 x snorm16 over the
  mesh's bounds (`MeshInfo` center and half extent; mesh-local, so no
  world-scale precision loss), COLOR_0 alpha unorm16, normal octahedral
  2 x snorm16, tangent octahedral 2 x 15 bit with the bitangent sign in
  bit 31, UV 2 x unorm16 over the mesh's UV range (`MeshInfo` uvOffset,
  uvScale: 64 -> 80 B). One GLSL decode, common.glsl `loadVertex`, read
  by mesh.vert, mesh.mesh, shadow.mesh, lighting.glsl
  `alphaTestCandidate` and ray_hit.glsl; it takes `SceneBuffer` rather
  than a vertex buffer parameter, since glslang refuses to pass a member
  of a readonly block ("cannot drop memory qualifier").
- **Calls accepted by the user:**
  1. The BLAS reads the first 8 bytes as R16G16B16A16_SNORM (stride 20,
     w ignored), with the dequantization in each geometry's
     `transformData` (a BLAS holds several meshes, one geometry each,
     so the TLAS instance transform cannot carry it). The transforms
     live in a host-visible buffer freed after the build.
  2. UV unorm16, not fp16: the fp16 ulp at |uv| = 1 is ~1e-3, 2-4 texels
     on 2k-4k bark. Max |UV| across the cooked trees is 39.5 (beech
     Field_01 Tileable, range 40.52 on a 2048 texture): unorm16 steps
     1.27 texels there (error <= 0.63), fp16 would step 0.031 UV = 64
     texels. Hornbeam Field_03 steps 1.26; the rest less.
  3. Trunk-base fade is a hard cut: base alpha x COLOR_0 alpha against
     the cutoff, no dither (stability over sharpness). Vertex alpha is a
     varying only where a cutout runs: the masked depth prepass
     (mesh.frag ALPHA_MASK && DEPTH_ONLY; the main masked pass uses
     early tests), shadow_masked.frag, `alphaTestCandidate`, and the
     voxelizer's `surfaceAt` (same coverage). mesh.vert (blended) has
     no alpha output.
- **COLOR_0 rules:** the loader reads COLOR_0 alpha (model.cpp). The
  tree cook keeps it only on a fade material (`isFadeMaterial`: MASK,
  single-sided, base colour without alpha; cook_ready.py marks Trunk
  MASK); the Megascans plant import drops COLOR_0 (its wind data would
  cut fronds). A MASK mesh counts as foliage for the voxel switch only
  without vertex alpha < 1, so a faded trunk keeps the wood switch
  height. Of every cooked asset only common_fern_rhdso (MI_rhDso, 2955
  of 3480 vertices under the cutoff) carried MASK + COLOR_0 alpha < 1;
  it is recooked with the 30 trees. A fade material only fades when a
  cooked vertex has alpha < 1: the beech and hornbeam Trunk slots carry
  COLOR_0 alpha 1 everywhere, so they are written opaque without COLOR_0
  (the first recook marked them MASK, which paid the masked pipeline
  and, with no vertex alpha < 1, counted them as foliage). Fading
  Trunks: alder Field_01, Field_04, Forest_03, Forest_04.
- **Measured** (forest edge `--trees`, 2560x1440 DLAA, Release, warm
  caches; main 9146661 with the same cooks except the 9 non-fading
  Trunks still MASK): resident 12124 -> 10959 MiB (-1165), models-loaded
  peak 12860 -> 10644 MiB, far-field peak 12537 -> 11372 MiB; GPU
  24.56 -> 23.96 ms. Trunk MASK against forced OPAQUE (the 4 fading
  alder Trunks, one launch each): 23.96 against 24.17 ms, noise, so the
  base clusters stay in the Trunk material (no split). With `--shadows
  rt` resident is 12730 MiB, GPU 32.34 ms. Validation build (-O3,
  assertions and validation on): 0 errors in both runs, 0 dropped.
- **Look:** the alder fork at 1 m (mesh junctions, decoration over bark)
  shows no hairline seams; the trunk-base cut at 2-6 m is judged by the
  user. kCacheVersion 8 -> 9.

## Voxel coverage seed from geometry only (2026-11-26)

- **Problem:** each voxel cluster's coverage hash (voxel_bricks.comp) was
  seeded from the cluster cache key, which hashes the raw `Vertex` bytes
  and `kVoxelizerVersion`. Vertex compression (52 B CPU vertex,
  voxelizer 11 -> 12) therefore re-rolled every tree's stable voxel
  dither: byte-identical voxel caches, a different pattern, so every
  dappled voxel shadow moved (alder meadow at 2 m: 15.4/255 against main,
  sun light 20.8; with main's seed forced: 1.09 and 0.29, the rest the
  alder fade).
- **Rule:** the seed (`seedVoxels`, cluster.cpp) hashes the model's
  positions and indices only, mixed with the mesh's index; never cache
  or voxelizer versions or the Vertex layout. A format change keeps the
  look; models still differ. Tested (cluster_test.cpp "voxel seeds follow
  positions and indices only"). This change re-rolls the pattern once.

## Voxel colour prefilter: tried, no effect (crown lane round 2, 2026-11-26)

- Tried: `voxelize` summed each voxel's material (albedo, transmission,
  normal moment, roughness) over its 26 neighbours, a Gaussian of variance
  4/12 voxels^2 weighted by hits, occupancy untouched. Dropped: it does not
  move the far-crown speckle. Don't retry it for that.
- Measured at the Part B pose (Release, `Render 2560x1440, display
  2560x1440`, main b40f1f6 vs the try, the same 256 spp reference):

  | metric | before | after | reference |
  |---|---|---|---|
  | albedo residual (log) | 0.244 | 0.249 | 0.149 |
  | crown contrast | 0.183 | 0.179 | 0.174 |
  | tree mean logL - frame median | -0.713 | -0.706 | -0.638 |
  | GPU ms | 29.48 | 29.39 | |

  The sheets matched by eye.
- Why: the per-voxel colour premise was wrong (it came from the branch
  mesh, misread as leaves). In the cached levels of beech and hornbeam
  Forest_01, mesh 0 (leaves, the only one with transmission) is smooth:
  beech log-albedo residual against the 26-neighbour mean 0.092 / 0.072 at
  0.5 / 1 m, neighbour lobe axes |dot| 0.875 (hornbeam 0.68-0.72; 0.5 is
  random). Mesh 1 (branches, no transmission) is two-toned (log-albedo
  1st/99th percentile -3.0 / -2.0, residual 0.22 at 1 m), and its levels
  above 0.25 m come from mergeVoxelGrid, which a voxelize filter never
  reaches. The far-crown speckle's source is still open (backlog).

## Wood voxels fold into the leaf voxel of their cell (crown lane round 5, 2026-11-26)

- Problem: far crowns speckle with pale blocks. Inside the crown mask at the
  Part B pose 27.6% of raster pixels were wood-coloured against 6.5% in the
  256 spp reference (backlog "Voxel crown speckle: wood voxels").
- Premise, checked offline before the change: the wood *amount* is about
  right; its *form* is wrong. (a) Mean linear crown albedo, raster / reference
  1.051 on the common mask (main b40f1f6). (b) The coverage-to-cube rule
  (voxel_bricks.comp: each mesh's voxel an opaque cube with probability its
  view opacity, meshes rolled independently) simulated over beech and
  hornbeam Forest_01's levels (`.superpowers/crown5/sim.cpp`): the visible
  share of wood cubes matches the share of wood that is first hit on the
  triangles within ~2 points (beech 0.5 m: x 0.33 / z 0.30 / top 0.11
  against 0.31 / 0.31 / 0.12). The reference blends sub-pixel twigs into
  leaf-coloured pixels; the raster drew them as cubes the size of a voxel.
- Choice: in `bakeModelAxisDepths` (the model-level pass over every mesh's
  voxels by lattice cell), an opaque voxel (no diffuse transmission) in a
  cell that also holds a transmissive one folds into the transmissive voxel
  with the most coverage, as coarse levels already combine coincident
  voxels (`combineVoxels`, the combineVoxelGrids rule: optical depths add,
  albedo, transmission, normal moment and roughness weighted by coverage).
  The folded voxel is dropped. Cells with wood only (trunks, bare
  branches) keep it. Generic: the rule reads transmission, not mesh names.
- Level: every voxel level. Wood in mixed cells is 1.4-3.4 cm thick (beech,
  coverage-weighted median to upper quartile of its side coverage x edge
  at 0.0625-0.125 m; hornbeam alike), under the finest voxel edge (6.25 cm),
  so no level has it filling the cell.
- Measured at the Part B pose (Release, `Render 2560x1440, display
  2560x1440`, main b40f1f6 vs the fold, the same reference;
  `.superpowers/crown5/`):

  | metric | before | after | reference |
  |---|---|---|---|
  | wood-coloured crown px (crown3/classify.py) | 0.276 | 0.149 | 0.065 |
  | albedo residual (log) | 0.244 | 0.209 | 0.149 |
  | mean crown albedo, raster / reference | 1.051 | 1.035 | |
  | thin runs >= 3 px (coverage-exact mask) | 2074 | 2051 | |
  | GPU ms | 29.48 | 27.71 | |

  Voxel bricks drawn 11.56 M -> 8.22 M (cull clusters 11.33 -> 9.71 ms). The
  thin-structure metric of analyze.py rose (36638 -> 62095 runs) only because
  its 0.02 albedo mask drops blended voxels whose colour lies near the
  ground's; the pixels where the terrain shows exactly are unchanged
  (0.3737 vs 0.3736 of the frame).
- Not done: wood-only cubes still read pale (the rest of the 0.149); a
  leaf and a wood mesh whose LOD cuts differ at a distance can show a cell
  twice or not at all (the axis depth bake assumes the same). Measured on
  the CPU cut up to 33% of wood voxel coverage misplaced at a level
  transition (backlog "Leaf and wood meshes of one model choose voxel
  levels independently"); lone beech and hornbeam captures at 40-170 m
  (2560x1440, main 8793aa7 vs the fold) show no wood blinking or lost
  beyond main's own.

## Opaque wood voxels at half coverage always draw (crown lane round 6, 2026-11-26)

- Problem: at 60-110 m lone-tree trunks show holes with the background
  through them at the base and a ragged silhouette (user, the 60 m band
  capture). The trunk base there is voxels only: on the CPU cut at 60 m no
  wood triangle cluster of beech Field_01 reaches its lowest metre (edge
  0.0625 m), and the `--report-at` pixels on the three trunks are voxels
  (beech Field_01 trunk mesh at 61 m, hornbeam Field_01 at 90 m, black
  alder Field_02 at 107 m).
- Cause: the coverage hash draws a voxel with probability equal to its
  opacity along the view. At 1-2 px per voxel (voxels switch at
  kVoxelPixels) a dropped voxel is a hole the size of a pixel or two, and
  nothing averages it. Partly covered wood cells sit at the base and the
  silhouette: chance that no cube draws in a trunk column (CPU, views along
  +-x and +-z, `.superpowers/crown6/trunk/trunk.txt` in meridian-crown):
  base (lowest 0.5 m) 0.22-0.25 beech, 0.44-0.46 hornbeam, 0.34-0.36
  alder; silhouette 0.30-0.36 / 0.07-0.56 / 0.29-0.57; interior 0.006-0.025.
- Choice: a voxel without transmission (wood) whose opacity along the view
  is at least `kOpaqueVoxelFloor` = 0.5 always draws; below it, and for
  every leaf voxel, the hash stays. Derived, not tuned: drawn or dropped
  whole, coverage f errs by 2 f (1 - f) of the quad on average through the
  hash and by 1 - f drawn always, and the second is smaller from f = 1/2.
  Voxels are a pixel or more at every drawn level (and cascades draw a
  one-texel caster LOD), so the per-voxel error is what shows. Leaves keep
  the hash in this try: their gaps are partly real, and the fold put wood
  in leaf cells into the leaf voxel.
- Measured (Release, `Render 2560x1440, display 2560x1440`, main 68f54bb
  vs this change; `.superpowers/crown6/trunk/`): non-tree pixels inside
  each row's trunk span in the lod view at the 60 m pose (`--camera 5203.38
  752.23 5950.74 0 0 --trees`): beech 0.0975 -> 0.0722, alder 0.1310 ->
  0.1037, hornbeam 0.1256 -> 0.1006; frame tree pixels 929004 -> 929889,
  thin tree pixels (<= 2 of 4 neighbours) 101936 -> 101529; Part B forest
  view wood-coloured crown px (crown3/classify.py) 0.1487 -> 0.1511
  (reference 0.0647); GPU 14.95 -> 15.01 ms (60 m view), 27.71 -> 27.99 ms
  (forest view, voxel trees 5.41 -> 5.48).
- Not done: about three quarters of the trunk holes remain (cells under
  half coverage, which a column of several such cells still leaves open,
  and on the hornbeam trunk holes where triangle and voxel clusters of the
  same mesh interleave, which the CPU cut at 90 m does not reproduce);
  backlog.

## Opaque wood stays triangles until thinner than its voxel edge (crown lane round 7, 2026-11-26)

- Problem: at 60 m the beech's lowest trunk metre was voxels while the
  trunk above was triangles (user). Cause, from the v13 caches: the
  voxel switch rule (`edgeFor`), not the simplifier (the triangle-only
  DAGs end at 40-83 triangles) or the small-mesh final level. A group goes
  to voxels at the largest power-of-two edge under its simplified error,
  not under the model's height / 512 (1/32 m for the 23.7 m beech). The
  trunk base is its own mesh (`Trunk`, 9577 triangles); its groups reach
  1/32 m within the first passes, and an error of 1/32 m projects to 1 px
  at 57 m (1828 px per metre at 1 m: 70 degrees horizontal at
  2560x1440, fovY 0.7504; the round 5-6 tools assumed fovY 1.2, 1052).
  Most branch groups also switch at 57 m, some at 114 m.
- Practice: UE 5.7 Nanite Foliage has the same trunk holes when the whole
  tree is voxelized; Epic's assembly example keeps trunk and large
  branches as ordinary geometry and instances only the twigs.
- Choice: a solid part (voxelModelHeight > 0, bark) goes to voxels at an
  edge only once it is thinner than that edge: at least `kThinVoxelShare`
  = 0.5 of the group's voxels there must cover under `kOpaqueVoxelFloor`
  of a face along every axis. Cylinders voxelized in random orientations
  show 0.37-0.41 thin from 1.25 edges across up, 0.45 at 1 edge, 0.51 at
  0.75 (`.superpowers/crown7/cyl.txt` in meridian-crown), so wood goes to
  voxels at about 0.8 edges across, under the pixel the edge projects to
  at the switch. A literal; 0.45 would be the one-edge value (director's
  ruling: keep 0.5). It also goes only at an edge its children's error
  doesn't exceed: the first try (voxelizer 14) let a held-back group join
  at a finer edge, its cell's error rose (1/32 m voxels with self error
  0.123 instead of 0.0625) and the pooled neighbours' finer voxels drew
  to twice the distance (beech wood voxels at 400 m 5509 -> 27595).
  Voxelizer 15; clusterlod's voxel_edge hook now gets the group's
  triangles. Leaves unchanged. Both tests are heuristics: thinness is
  measured on the group's current simplified triangles, while the voxels
  it would emit come from its source (original) triangles; and it takes
  each voxel's largest per-axis opacity, while the runtime floor
  (`kOpaqueVoxelFloor`, voxel_lod.glsl) applies to the opacity along the
  view. A thick group in the stuck path or clodBuild's final
  single-cluster path stays a terminal triangle group drawn at every
  distance.
- Measured (Release, `Render 2560x1440, display 2560x1440`, main 5b657d1
  vs this change, `.superpowers/crown7/` in meridian-crown): CPU cut
  (selectClustersBvh, 1 px) wood voxels per tree at 100/200/400 m beech
  196547/36007/5656 -> 176030/32115/5188, hornbeam 27072/10401/2362 ->
  12972/7234/2078, alder 12771/5204/1269 -> 0/3232/1130 (`cut15.txt`);
  background inside the trunk spans at the round 6 60 m pose beech 0.0722
  -> 0.0610, alder 0.1037 -> 0.0798, hornbeam 0.1006 -> 0.0800; forest view
  wood share 0.1511 -> 0.1702 (reference 0.0647), triangles 94062 ->
  15912939, GPU 27.88 -> 30.85 ms; forest edge 23.33 -> 25.05 ms; overview
  21.01 -> 23.34 ms; resident 10930 -> 10890 MiB; 0 dropped by any cull
  list; cluster caches 2245400242 -> 2202588085 bytes (30 models). No pop
  above same-build repeat noise in walk-speed passes across 57 and 114 m.
- Open: wood share moves away from the reference and forests cost 1.7-3
  ms more (backlog); sub-pixel wood kept as triangles (backlog).

## A held-back wood group joins the voxels at the next power of two (crown lane round 8, 2026-11-26)

- Problem: round 7 (voxelizer 15) kept sub-pixel wood as triangles far
  past its 1 px distance: in the CPU cut (selectClustersBvh, 1 px,
  2560x1440, 70 degrees horizontal) 3880 wood triangles of the three
  Megascans field trees (beech 2328, hornbeam 335, alder 1217) sat at 400
  m in clusters mostly thin at the 1 px edge (0 with voxelizer 13). A
  solid group joined the voxels only at a power-of-two edge between its
  children's error and its simplified error; with none between them it
  waited for a later pass. Replayed from the v15 caches
  (`.superpowers/crown8/dag.cpp`, REPLAY=1, in meridian-crown): waits hold
  2689 of the 3880 at 400 m, 2324 of them in passes that added no error,
  in chains of up to seven passes (beech: four and five), and held sub-pixel
  wood triangles to 800 m (hornbeam: 410 there, all waiting). Waits only follow
  a thick hold (a group's children's error above the finest edge). Not
  the cause: stuck groups (none in the three trees) and the final single
  cluster (one per trunk in beech and hornbeam, 68 and 65 triangles); the
  alder Trunk's alpha mask (thick() voxelizes with the real materials; the
  alder's thick-majority triangles are all in its unmasked Tileable mesh).
- Choice: when the largest power of two under the simplified error is
  under the children's error, the group takes the next power of two above
  the children's error, as a stuck group already does, and goes to voxels
  there if it is thin at it. Its voxel group's error is that edge, at or
  above its children's; its triangles draw at most to twice the distance
  where the children's error is 1 px. Voxelizer 16. Tried first (the
  director's text): only passes that add no error take the next power of
  two; worse at 400 m (4317 thin-cluster triangles: hornbeam 335 -> 1708).
  Lattice edges come from the float's exponent (`powerOfTwoAtOrAbove`,
  `powerOfTwoAtOrBelow`): a float log2 rounds a value one ulp off a power
  of two onto it, so the next power of two could land under the children's
  error (fix check). The three trees' v16 caches are byte-identical with
  and without it.
- Measured on the CPU (in-process build, identical to the game's cache for
  v15): thin-cluster wood triangles at 400 m beech 2328 -> 1867, hornbeam
  335 -> 0, alder 1217 -> 103 (3880 -> 1970); at 200 m 7384 -> 5534, 1740
  -> 2882, 4996 -> 4805; all wood triangles in the cut at 100/200/400/800 m
  105304/21125/5217/616 -> 102803/19713/3182/246. Wood voxels per tree at
  100/200/400 m beech 177936/32560/5271, hornbeam 20621/9283/2270, alder
  0/3292/1137 (voxelizer 13: 196547/36007/5656, 27072/10401/2362,
  12771/5204/1269). The lowest trunk metre stays triangles at 60 m (beech
  2545, hornbeam 1817, alder 1332 triangles, no voxels). Alder's Tileable
  mesh now ends in a 113-triangle final cluster drawn at every distance.
  Beech cook 47.3-47.8 s (v15) vs 47.4-47.9 s, same machine state.
- GPU (Release, `Render 2560x1440, display 2560x1440`, main de21d6d vs
  7bda3ff, round 7's poses, `.superpowers/crown8/` in meridian-crown):
  Part B forest 30.94 / 30.98 -> 29.74 / 29.63 ms (two runs each),
  triangles 15912939 -> 10310916; forest edge 25.27 -> 24.84 ms; overview
  23.29 -> 22.61 / 22.54 ms (voxelizer 13: 27.88, 23.33, 21.01); 0
  dropped by any cull list, pass or cascade; resident 10890 -> 10893 MiB;
  trunk background at the 60 m poses unchanged (beech 0.0602, hornbeam
  0.0665, alder 0.2091). The overview's mean colour is unchanged (sRGB
  111.83/121.77/104.09 vs 111.93/121.72/104.10); its 4.2/255 mean
  difference is per-pixel voxel coverage speckle, 1.9/255 over 8x8 blocks.
- Open: forests still cost 1.0-1.8 ms over voxelizer 13 (backlog).

## Leaf voxels draw shrunk quads on their jitter phases (leaf lane, stage 2, 2026-11-26)

- Problem: trees drawn as voxels read as "pixels erased, low resolution"
  (user). A voxel was one whole quad of 1 to 2.6 px drawn with chance f
  (its opacity along the view) by a hash fixed for all time, so partial
  coverage reached the screen as missing blocks that the jitter and DLAA
  cannot average (per-voxel covered-area error 2 f (1 - f) of the quad).
  Spec `docs/superpowers/specs/2026-11-26-voxel-leaf-coverage-design.md`.
- Choice (user, from stage 1's sheets and clips): `area-pixel-phase` for
  leaf voxels. The quad shrinks to area fraction a = min(max(f, 1/q), 1)
  (q the full quad's pixels, so no quad goes under one pixel) and draws on
  floor(P p) of the P jitter phases (p = f / a), plus one more when its
  stable hash is under the fractional part of P p, spread evenly from a
  stable-hash shift: expected coverage a p = f for every voxel, and a
  still camera converges over one cycle to a voxel's a draws / P, which is
  f on average over voxels (stage 1 measured the covered-area error at
  about 0.002 against 0.77 for the hash at Part B, with draws counted as
  round(P p); that count never drew a voxel with P p < 1/2 and drew one
  with 1/2 <= P p < 3/2 once per cycle, -33% to +100% of its p; found by
  stage 2's fix check). The cycle P is the jitter cycle, at most
  `kMaxVoxelCoveragePhases` (256: the phase's 8 bits in
  `SceneData.voxelCoverage`). The shrunk quad sits at a stable-hash point
  inside the full quad, not at its centre. Wood voxels and the cascades keep the hash with
  `kOpaqueVoxelFloor`, identical to main; `--voxel-coverage hash` keeps
  main's rule for leaves too (A/B). Rejected in stage 1: `area`,
  `area-offset` (need 72-90M list entries at the far views), `mixed`,
  `phase` (looks like the hash), `area-pixel` (holes stay). Wood: every
  area rule shrinks a voxel about its centre, so a trunk 1.2-2.3 voxels
  wide becomes a two-column lattice of 1 px dots that DLAA greys into a
  ladder (trunk contrast at 116 m: triangles 0.623, hash 0.572,
  area-pixel-phase 0.468; wood at a = f 0.466).
- Offset (stage 2): with every shrunk quad at its voxel's centre, voxels of
  one lattice row project to one screen row and the about-1 px quads line
  up in horizontal dashes with empty rows between (beech_130m: the voxel
  crowns behind the lone beech; distance not measured). The raw
  frame shows them, `--aa taa` averages them, DLAA keeps them; pitching
  the camera by 0.12 rad does not remove them. Crown crop mean |dI/dy| /
  mean |dI/dx| (`.superpowers/leaf/streak.py`, 400x240 crops of
  `crops.json`): beech_130m centred 1.226, offset 1.136, triangles 1.139,
  hash 1.062; beech_170m 1.205 / 1.137 / 1.129; hornbeam_160m 1.160 /
  1.079 / 1.071; Part B 1.069 / 1.052 (hash 1.020). The offset is drawn
  inside the full quad, so it moves no coverage out of the voxel; it is
  fixed per voxel, so a still camera does not boil and motion vectors stay
  the voxel centre's. A per-phase offset was not tried.
- Measured (Release, `Render 2560x1440, display 2560x1440, jitter phases
  8, mip bias 0, DLSS`, 0 validation errors, 0 dropped by any list, pass
  or cascade in every run): list need, most in a frame, near views 8.2M to
  11.2M quads, forest edge 25.7M (cascades 15.1M), overview 26.6M, Part B
  31.8M, summit 29.0M, walk 9.9M, pan 30.5M, fly toward the forest 37.2M
  (the maximum), so `kMaxVoxelQuads` 24.66M -> 55.87M (1.5x, rounded up).
  Resident 10893 -> 11370 MiB (+477), peak 11782 MiB; the 12 GB gate
  holds. GPU frame edge 29.16, overview 27.76, Part B 35.99 ms (hash at
  stage 1: Part B voxel pass 5.05 -> 10.24 ms). Cascades: the ground
  shadow under a crown keeps its shape and mean (109.54 vs 109.18) with
  hash casters, so the cascades stay on the hash. Wood equals main: with
  every leaf voxel dropped (throwaway patch), area-pixel-phase vs `hash`
  draw the same quads (main view 470175 at beech_170m and 1391156 at the
  forest edge; cascades 140325 and 1349383) and differ by the same-build
  repeat noise (0.3464 vs 0.3469/255, 0.3724 vs 0.3725/255); `hash` vs
  main 99266f0 at beech08_116m: quad need 4730674 both, 0.5114 vs repeat
  0.5115/255.
- Draw count fix re-measured (0502671 against 7a54e2e, same 14 views and
  the fly-toward, same settings): list need moves -2.3% to +0.2% (Part B
  31841226 -> 31831375, overview 26764300 -> 26766167, fly-toward
  37245138 -> 37047773), 0 dropped, resident 11370 MiB and peak 11782 MiB
  unchanged; crown streak ratio 1.192 -> 1.188; far canopy crops
  unchanged in mean and noise (overview 113.99 / 12.33 -> 114.07 / 12.33,
  Part B 119.72 / 5.79 -> 119.61 / 5.79; `.superpowers/leaf/canopy.py`).
  A leaf voxel has p < 1 only where f < 1/q (its quad held at one pixel);
  how many voxels that is at these views was not measured.
- Motion (`tools/motion-clarity.sh`, 0502671 against main 99266f0's code,
  run twice for the forest rows): frame 120/121 jitter at the forest edge
  moving sideways 5 m/s 4.3114 / 4.3120 against 4.2896 / 4.2874, 30 m/s
  5.1393 / 5.1395 against 5.0985 / 5.0990 (+0.6%, +0.8%, repeats within
  0.003); still and turning forest rows and all ground and sky rows within
  0.008. Near crown (beech_130m pose, crown rect, 5 m/s sideways) 7.10
  against 8.54, still 1.14 against 1.86. Ruling (director, 2026-11-26):
  the forest-edge jitter (+0.5% to +0.8%) is acceptable against the
  near-crown gain; which of the rule and the offset adds it is not
  measured and no separation test is planned.
- Not done: the 150 m grain is leaf shading (triangles show it too);
  voxel crowns pass more terrain than the path tracer under every rule
  (0.33 vs 0.23); stages 3 to 5 of the spec; thin voxel trunks have no
  rule that reads solid (backlog).

## Voxel crowns occlude their ambient by the crown with GI on (leaf lane, 2026-11-26)

- Problem: under GI a voxel skipped its crown occlusion (the probes stand
  for the canopy), so the only occlusion of its ambient light was the
  probes': the sharp sky along R by `skyOcclusion`, the diffuse by the
  probe irradiance. Outside the probes' clipmap (5 levels of 48 columns
  from 2 m: about 768 m around the camera) `giIrradiance` falls back to the
  open sky and ground, so a voxel there had no ambient occlusion at all.
  Scene lane pick (relief patch, `--haze clean --sun 0.672 0.309 -0.672`,
  voxel beech at 2157 m, original trees): ambient diffuse Y 86.3, ambient
  specular 82.0 (b/g 1.76), haze 55% of the pixel; with `--gi off` the
  crown occlusion scales both by 0.60.
- Choice: with GI on, the ambient specular passes through the model's
  optical depth along R (`voxelBoxDepth`, as the sun and sky use it), and
  the crown occlusion of the GI-off path (up and four sides) scales the
  ambient diffuse by the share outside the clipmap (`gi.remaining`);
  inside, the probes keep the diffuse, so it is not occluded twice. Both
  follow `microShadowStrength` like the GI-off path. Tried first: the
  specular alone (slope L 0.297, lit / shadow 1.36).
- Measured (Release, `Render 2560x1440, display 2560x1440`, view
  `--camera 1300 1350 1300 -0.785 -0.25` on the relief patch, `--sun 0.707
  0.325 -0.707 --haze clean`, original trees, regions
  `canopy/regions.json` a1 of the scene lane, reference 1.jpg): shadowed
  slope OKLCh L 0.327 -> 0.266 (ref 0.257), C 0.053 -> 0.059 (ref 0.031:
  the haze is a larger share of the darker pixel), lit crowns L 0.423 ->
  0.378 (ref 0.403), lit / shadow L 1.29 -> 1.42 (ref 1.57). Pick: ambient
  diffuse 86.3 -> 52.0, specular 82.0 -> 28.7. GPU 26.22 -> 26.03 ms; the
  lone beech at 150 m as voxels against triangles 0.995 -> 1.004 of the
  crown crop's luminance (no step at the switch). Motion
  (`tools/motion-clarity.sh`, against 8be2e9d's shader): forest jitter
  still 0.4801 / 0.4874, sideways 5 m/s 4.2408 / 4.3106, 30 m/s
  5.0528 / 5.1404, turning 2.1190 / 2.1518; near crown (beech_130m pose)
  moving 7.01 / 7.12, still 1.08 / 1.14: no added jitter.
- Not done: triangle leaves have the same fault (backlog); haze and
  palette are the scene lane's.

## Foliage specular AA (2026-11-27)

Leaf-scale sparkle (backlog G18). Measured on branch crown-light (leaf lane, rounds 3 and 4;
that branch also carried crown fill and canopy sky, which are not part of this change): at
view m2 the white dots on the canopy (L > 0.85, 6.1 per 1e4 px, reference 0) are all voxels,
91 % of their light is specular, essentially all of it sun specular (>= 5473 of 5528, 1e-3
exposed; sky reflection <= 26): a flat voxel's one lobe axis glints where the leaves it
stands for would spread the highlight.

`--foliage-specular-aa` (default on, `off` for A/B): geometric specular AA (Tokuyoshi and
Kaplanyan 2019, "Improved Geometric Specular Antialiasing":
alpha^2 += min(2 x 0.15 x (|dN/dx|^2 + |dN/dy|^2), 0.2)), on foliage only (materials with
diffuse transmission, and the voxels). The kernel and constants follow Filament's
normalFiltering (variance 0.15, threshold 0.2), but the derivative is taken of the shading
normal (normal-mapped, after the two-sided flip), not the geometric normal, so normal-map
detail feeds the kernel; this was the measured configuration, and switching to the geometric
normal is an open variant. Far-cell voxels (voxel.frag, fixed perceptual roughness 0.8) are
not covered. A voxel is flat-shaded, so it has none: its pixel spans 1 / voxelPixels^2 of
the change to the neighbouring voxels' lobe axes, taken as |N1 - N2|^2 = 1 (axes about 60
degrees apart in a crown whose leaves face every way, `kVoxelNeighbourNormalChange`). The
voxel term passes k / voxelPixels^2 where the triangle convention is |dN/dx|^2 + |dN/dy|^2,
i.e. the constant already stands for both axes; the kernel reaches its cap once a voxel is
about 1.2 px or smaller, so the factor matters only for voxels of about 2 to 4 px; and the
voxel's pixel size uses radial distance, which errs toward rougher off-axis. On by
default because it is a correctness fix (a pixel's highlight must cover the normals in it).

Measured on branch crown-light:
- m2: white dots 6.1 -> 0.1 per 1e4 px, |lap|/L 0.357 -> 0.348 (reference 0.251).
- s1 (`--camera 218 1408 1298 -0.785 -0.57`, a sun-facing slope with no hill or cloud
  shadow): dots 2.2 -> 0.0, |lap|/L 0.232 -> 0.215 (reference 0.251), lit-canopy specular
  share 92.9 -> 92.4 %: energy kept, only the peaks spread.
- Triangle/voxel switch (lone beech): tri/vox 0.998 on and off.
- Motion (frame 120 vs 121 at the same pose, motion-clarity.sh's method; jitter = mean
  |difference| 0-255, off / repeat of off / on): forest_still 0.480 / 0.481 / 0.454,
  forest_side5 4.241 / 4.241 / 4.034, forest_side30 5.056 / 5.054 / 4.788, forest_turn1
  2.119 / 2.118 / 1.999, near crown still 1.001 / 1.003 / 0.998, near side5 5.390 / 5.388 /
  5.374, s1 still 0.594 / 0.593 / 0.580, s1 side5 1.986 / 1.986 / 1.925: every row at or below
  baseline.
- Close range (start view, trees at 20-60 m): specular share of triangle leaf pixels 22.8 ->
  21.6 % and 25.1 -> 24.3 %, so leaves don't go dull; no footprint fade needed.
- GPU within noise (s1 25.40 -> 25.34 ms, m2 24.28 -> 24.43 ms).
No wider variant: s1's sparkle is already below the reference.

## Leaf light: adaxial-only specular and the canopy ambient (light lane round 3, 2026-11-27)

Leaves lit their back (abaxial) face with the front's specular lobe and reflected open sky
from inside a crown. Measured tree leaves disagree: Bousquet, Jacquemoud and Moya, "Leaf
BRDF and BTDF measurements and model" (ISPMSRS 2005; adaxial beech: Lambert plus
Cook-Torrance, sigma ~0.35, n ~1.5, white specular, the BTDF near Lambertian) and Habel et
al., "Physically based real-time translucency for leaves" (EGSR 2007: "the front of a leaf
has broad specularity whereas the back of a leaf is diffuse. This is mostly true for tree
leaves"). Our per-leaf parameters already match the measurement (texture roughness ~0.61,
alpha 0.37; f0 0.04); only the two cases above were wrong. Inside a canopy, spectral
invariants (Knyazikhin, Huang; already gi_probes.comp's foliage model) say a leaf sees, in
the directions its crown hides, other leaves of radiance ~omega x the light reaching them:
green and dim, not sky. No public engine text on foliage specular occlusion inside crowns
was found (Ghost of Tsushima, Horizon, SpeedTree docs).

`--leaf-light on` (default; lighting.glsl `setLeafLight`, mesh.frag, voxel_instance.frag):
- The sun highlight and the ambient specular scale by the share of the seen face that is
  adaxial, 0.5 + kLeafAdaxialUp x N.y (0.5) with N toward the viewer: blades turn their
  adaxial face to the sky, and a voxel's lobe axis has no sign, so triangles and voxels
  take the same rule.
- Ambient diffuse, specular along R and the transmitted (back) ambient: the occluded share
  1 - V sees neighbours, omega x the ambient reaching the point (omega = reflected +
  transmitted albedo, one scattering). V is the crown sky visibility for triangles, the
  model's visibility 0.5 (up + sides) for voxels, exp(-depth along R) for the reflection.
  GI off: this term replaces the crown occlusion (crownOcclusion() for triangles, the model
  occlusion for voxels); the prototype (branch far-light 5d03ce6, mode b) kept that occlusion
  on top, which applied the crown twice, and the port fixes it. GI on: the term applies at
  full weight after giIrradiance and the probes' skyOcclusion, and replaces the voxels'
  crownR / multi-bounce adjustments; the probes already trace the canopy inside the
  clipmap, so there the crown darkens leaves twice (main applied no crown term to
  triangles with GI on, and to voxels only on `gi.remaining`). Kept as is: it is how B
  looked when the user chose it (GI on). `--leaf-light gi-weighted` (A/B) weights the
  canopy term by `sh.gi.remaining`, the crown counted once; backlog row "Leaf light
  canopy term counts the crown twice with GI on".
- Triangles and voxels take their crown terms scaled by the micro shadow strength (as the
  voxels' model occlusion), so `--micro-shadows off` leaves no seam at the switch. The
  reflection term still differs: triangles use the isotropic crown visibility, voxels
  exp(-depth along R).
- Recollision (omega / (1 - p omega), the prototype's b2) and the partial modes a, a2 are
  dropped: b2's mean |diff| from off stayed within 0.02 /255 of b's at every view.

Chosen by the user by eye (B, light lane round 3). Measured on far-light, grade wb7500,
palette light1, crown fill on, summer haze (2560x1440): mean |diff| vs off h2 3.76, s1 2.49,
leaf view 9.23 /255 (floor 0.42, 0.30, 0.42); shaded crown specular share h2 32.0 -> 21.8 %;
white dots per 1e4 crown px h2 51.7 -> 44.2; motion (h2 at 5 m/s) dots 21.7 -> 19.3,
frame-to-frame |dL| 0.0371 -> 0.0346; triangle/voxel seam at equal distance (lone
hornbeam) voxel/triangle Y near 0.735 -> 0.823 vs 0.670 -> 0.791, i.e. B narrows the
switch; GPU o4 30.69 -> 30.87, h2 32.86 -> 33.02 ms. Main has no crown fill, so the port's
look differs from the one judged; the port proof compares main against the branch.
Port on main (517883d vs the branch, 2560x1440 DLAA, Release, 120 frames): GPU start 24.15
-> 24.19, o4 (relief) 27.27 -> 27.44, eye height under the canopy 23.14 -> 22.57 ms; VRAM
unchanged; 0 validation errors.

## Exposure: the shade bound holds on the displayed image (2026-11-28)

Under a closed canopy the meter wants more lift than the shade bound allows ("Exposure:
shade lift floor", 2026-11-23): at the relief patch's eye-height forest view (`595.5 989.2
495.3 -0.785 0.0 --sun -0.0714 0.574 -0.816`) the exposure converges to EV100 10.21, the
bound openEv 13.19 - kMaxShadeLiftEv 3 (open ground 71104 lux), and reads 10.43 after the
capture's 120 frames (adaptation 1.5/s from EV 15 leaves 5 %). A grade's `exposureEv`
scales the image in the LUT, after the meter, so the working look's -0.85 EV darkened that
already-bounded interior by another 0.85 EV, near unreadable to walk in.

Practice: Unreal (4.25+) separates a constant exposure compensation from an exposure
compensation curve indexed by the metered average scene EV100, so dark scenes can be
compensated differently from bright ones (dev.epicgames.com "Auto Exposure in Unreal
Engine"; Epic tech blog "How Epic Games is handling auto exposure in 4.25", 2020). Here the
curve needs no new constants: the shade bound is the user's photographic rule (open the
shade up by at most 3 EV), so it should hold on what is displayed. exposure.comp moves the
bound by the grade's exposure (`PostConstants::gradeExposureEv`): a negative grade EV lowers
the bound's EV100 by as much, so a view held at the bound is shown as bright as at grade EV
0, while views the meter puts above the bound (open meadow, aerial) keep the full grade
exposure. Between the two the compensation fades with the metered level (a max, smoothed by
the adaptation). At grade EV 0 nothing changes. A positive grade EV raises the bound, so it
cannot lift shade past the 3 EV rule on screen, and views just above the bound lose part of
its brightening. Captures and fixed grade files see the result at once; in the G panel, a
grade EV change on a view held at the bound shows at once in the LUT and then eases back
over ~1-2 s as the exposure adapts to the moved bound (open views respond at once and
stay). Rejected: moving the grade EV into the
meter's compensation (the panel's slider would ease in over a second, and the bound would
still darken), a looser shade bound (the user chose 3 EV; readable shade beyond it is local
exposure's job), and faster adaptation.

Measured (Release, 2560x1440 DLAA, 120 frames, working grade; display luma Y' 0-255):
eye-height forest p50 28.9 on main, 18.4 with the grade before this change, 34.9 after
(p5 9.9 / 7.4 / 17.2; EV100 10.43 / 10.43 / 9.62); start view and relief o4 unchanged by it
(mean |diff| 0.49 and 0.28 /255, within the `--trees` run-to-run floor 0.58); with the
previous grade file it reproduces main (start 0.59, o4 0.32, forest 0.23 /255). Walk from
the meadow into the forest (8 m/s, 600 frames, every 5th saved): median Y' rises from the
start's adaptation to a plateau of 80 and falls smoothly to 67 (main 114 to 66), one turn,
largest 5-frame step 5.6 (main 8.4): no pumping.

## Working look as the starting grade (light lane round 2, 2026-11-27)

The light lane matched o4 (`--camera -1090.3 1254.6 1245.4 0.2 -0.24 --sun 0.020 0.643
-0.766`) to Chlorophyll's 1.jpg by display-luma percentiles (p5/25/50/75/95, OKLab, per
region). On the built-in grade the near forest sat at the black level (p5 6.4 vs 20.6, p25
13.8 vs 30.4), its median at 30.2 vs 44.1. Ladder: grade EV -0.75 put the median and the sky
on the reference (44.8 / 224.5 vs 44.1 / 224.5); lowering contrast to lift the blacks
dragged the sky (1.2: p5 23.5, sky 214.7), an offset lifted them without touching it (0.006:
p5 21.9, sky 224.6); EV -0.85 + offset 0.006 gave near 20.1 29.8 48.0 67.9 87.1 (reference
20.6 30.4 44.1 64.5 93.4). White balance 7500 K then took the shaded crowns' blue cast off
(C 0.039 -> 0.027 at h 242, reference 0.030 h 241) and the near and forest hues toward the
references (o4 153 -> 134, reference 146; h2 162 -> 138, reference 148), at a cost: the far
ridge loses its blue (C 0.028 -> 0.017 h 208, reference 0.029 h 243) and the near p95 drops
94.6 -> 81.9. The user chose this "working look" (grades/wb7500.json on far-light) by eye.

`kStartingGrade` gains those three fields; contrast 1.4 and the greens band stay. With the
shade bound on the displayed image (entry above), the EV darkens the open and aerial views
it was matched on and leaves forest interiors held at the bound as main shows them. The leaf
palette (light1) and crown fill the lane also ran with are not part of it. Interactive runs
still load the user's grade file over the built-in one, and captures ignore it, so captures
and the panel's Reset take the new look. The previous built-in stays reachable as
`tools/grades/starting-2026-11-21.json` (a partial file: the three fields at their old
identity values, the rest from the built-in).
