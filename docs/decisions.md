# Technique decisions

Newest last. Each entry: what was chosen, why, what was rejected, and the
sources. Check here before re-researching a topic.

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
  `docs/superpowers/specs/2026-11-20-far-field-design.md`.
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
- **Limits that remain** are structural and checked in `uploadMesh`, each
  with its own error: work items pack a first cluster index into 26 bits
  (`kMaxClusterIndex`, cull.glsl); vertex offsets are int32 and other
  offsets uint32; the mesh table is a fixed `kMaxMeshes`; the traversal
  dispatches at most `kBvhDepths` levels; and no pool may exceed the
  device's `maxBufferSize` (Vulkan 1.3; buffer device addresses are not
  bound by `maxStorageBufferRange`).

## Motion clarity: DLSS 4.5, Ray Reconstruction, TAA v2, grading (2026-11-20)

Spec: `docs/superpowers/specs/2026-11-20-motion-clarity-design.md`.

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

Spec: `docs/superpowers/specs/2026-11-19-clouds-weather-design.md`.

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
`docs/superpowers/plans/2026-11-20-streamed-placement.md`.

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

Supersedes the default in "DLSS DLAA with Ray Reconstruction is the
default". User decision: raster rendering in the style of Chlorophyll
Engine (no ray tracing, art-directed), for image clarity, motion clarity
and performance. RR's reason to be the default was the ray-traced sun
shadow's per-pixel noise; the raster sun shadows that replace the ray
(spec `2026-11-20-raster-sun-shadows-design.md`) have none to denoise.

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

User direction: raster rendering, no ray tracing. Replaces the per-pixel
TLAS shadow ray for objects (see "Shadows" at the top, which chose RT);
the terrain march, cloud shadow, contact shadows and the far path stay.
Spec `docs/superpowers/specs/2026-11-20-raster-sun-shadows-design.md`.

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

Status: decided, not implemented (spec
`2026-11-21-materials-fidelity-design.md` section 2, D1 and D2).

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
