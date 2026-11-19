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
  `--haze summer` (default, AOD(550) 0.10, H 0.5 km), `--haze
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
  `DFV_Multiscatter`). Specular ambient comes from the sky-view LUT until RT
  reflections exist.
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

## Far-field forests: per-cell voxel bricks (M2a, 2026-11-20)

- **Chosen (user, 2026-11-20): per-cell voxel bricks.** Past a per-cell
  far switch (tallest tree under ~6 px, ~4 km), each 64 m cell skips its
  tree instances and is drawn as one aggregate of sparse 4^3 voxel bricks
  (L0 4 m, L1 8 m, L2 16 m). The reference is Chlorophyll Engine's
  per-tile far field. The same bricks are the far ray field for sun
  shadows now and for sky visibility in M2b, replacing M2b's 2D canopy
  field. Impostors stay, for the mid range only. Spec:
  `docs/superpowers/specs/2026-11-20-far-field-design.md`.
- **Why:** per-instance impostors to the horizon would put ~1M+
  instances, each a quad of 1-5 px, through culling and the mesh shader in
  ridge and overview views. Those sub-pixel alpha-tested quads shimmer,
  which TAA can't resolve. An aggregate costs per covered pixel and is
  prefiltered.
- **Rejected:**
  - (A) impostors to the horizon: the cost and shimmer above.
  - (B) a 2.5D canopy aggregate (top, bottom, density, albedo, normal per
    4 m texel), cheaper, ~225 MB est. Kept as the documented fallback if
    the prototype's gates fail; the user decides after the prototype.
  - UE 5.7's per-cluster Nanite voxels: an aggregate per cluster doesn't
    reduce per-instance work, and Epic's hardware RT for it falls back to
    a mesh that users report casts poor shadows.
- **Resolution dialed down:** the user accepted no 3D crown silhouettes
  beyond ~4 km. So the finest voxel is 4 m (one pixel at the far switch;
  2 m would be ~8x the memory) and a voxel is 4 B (log extinction,
  octahedral normal, albedo palette index). Estimated ~350-450 MB resident
  over the Napf (forest ~93 km^2: ~1.1-1.45M L0 bricks), with a load-time
  peak of ~2 GB while one level's accumulators exist. No streaming.
  Distance residency for L0 (re-splat from the placed table, no disk) is
  the lever if the prototype measures over 500 MB.
- **Rays:** M2a keeps the static TLAS and chooses per ray, by the
  receiver's distance from the camera against `kFarShadowDistance` (4 km
  to start). Near receivers trace every tree
  in the TLAS (the reference). Far receivers mask placed trees out of
  the ray query (a new instance-mask bit) and march the voxels with
  Beer-Lambert. Every ray uses exactly one tree representation, so none
  is counted twice. M2b's partitioned TLAS switches to per-cell
  `cellInTlas` flags.
- **Gates (M2a Task 8 prototype; decided after it, not cut pre-emptively):**
  - far field resident <= 500 MB, load-time peak <= 2.5 GB;
  - far raster <= 1.0 ms at the overview and ridge views;
  - the voxel shadow march costs no more than TLAS shadows for far
    receivers;
  - shadow quality vs the reference (`--far-field off`, full-detail TLAS)
    at the ridge and sunset views, which the user inspects;
  - coverage: far-forest luminance within 5% of the same reference, with
    no step at the switch or between levels.
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
- TAA v2 stays the default upscaler (decided 2026-11-20); DLSS 4.5
  support is mandatory and selected with `--aa dlss`, DLAA preset M being
  the option that passes every quality gate. The DLAA preset M `aa` pass
  costs 2.36 ms at 1440p against the 1.0 ms slot, while TAA v2 measures
  forward 0.94 at 0.17 ms. Re-decide the default if the DLSS cost drops
  or the budget moves.
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
  preset M passes every motion gate. Its `aa` pass costs 2.36 ms, a measured miss of the 1.0 ms SR/DLAA slot at 1440p,
  which is why TAA stays the default. TAA v2 measures forward 0.94 at
  0.17 ms. Upscaled modes' ratios are against their own stills (sharper
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

**Ray Reconstruction first, for the shadows and AO.**
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
- Clouds, when they exist, write this mask too.
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
  changes. `whiteNoise` is 0 today and nothing sets it; 0 leaves TAA
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
  worley, 1)`), GBA Worley fBm at 4, 8, 16 cells per tile; detail 32^3
  RGBA8: Worley fBm at 2, 4, 8 cells; curl 128^2 RGBA16F: the curl of 2D
  Perlin fBm (divergence-free, so the detail lookup swirls without
  bunching). World tiles: base 8192 m (64 m texels, ~2 km cells), detail
  1024 m (32 m texels), curl 4096 m. Every lattice wraps, so all of them
  tile seamlessly.
  - Schneider, Real-time Volumetric Cloudscapes of Horizon Zero Dawn (2015): https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
  - Hillaire, TileableVolumeNoise (Perlin-Worley construction): https://github.com/sebh/TileableVolumeNoise
- **Density (`clouds.glsl` `cloudDensity`):** the plain product weather
  coverage x vertical profile x base shape (HZD: `remap(R, worleyFbm -
  1, 1)`), then eroded by the curl-distorted detail (150 m of distortion
  fading toward the top). Coverage is thresholded once, in the map; a
  second HZD-style `remap(shape, 1 - coverage, 1)` on top of it would
  punch holes in overcast (R spread over [0, 1] at coverage 1) and shrink
  fair well below its coverage (the march then cuts the product at a
  fixed, type-dependent level; see Cloud march). Erosion depends on type like the
  profile: cumulus wispy at the base and billowy above, carving up to
  0.35; stratus wispy throughout, carving up to 0.5. Output is in [0, 1]; the march scales it to extinction.
  `world.y` is the altitude on the curved shell, so the caller decides
  the curvature. Profiles are analytic smoothsteps by normalized height
  (stratus a soft slab, cumulus a flat base thinning upward) instead of
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
  float at planet scale. Steps: 64 across the segment, at least 60 m,
  growing as 1.2% of the distance (about a quarter-res pixel), at most
  128, start jittered by interleaved gradient noise per frame (no blue
  noise texture in the engine yet; IGN is the engine's existing
  low-discrepancy per-pixel sequence); the march stops at the scene
  depth, the sea-level sphere, 300 km, or transmittance 0.01.
- **Lighting:** extinction 0.06/m at density 1, albedo 0.98 lowered
  toward 0.5 below density 0.15 (edge darkening by albedo, the spec's
  choice over the powder term); sun transmittance by a 5-step light
  march (40 m doubling, 1.2 km); sun color from the transmittance LUT at
  the sample, zero in the planet's shadow (the LUT alone returns the
  transmittance to the ground there, which made clouds glow after
  sunset); dual-lobe Henyey-Greenstein (0.8 and -0.5, 30% backward);
  Wrenninge octaves, 3, extinction x0.25, contribution x0.7, eccentricity
  x0.5 (a <= b keeps it energy-conserving; b = 0.5 left sunlit tops
  grey); ambient = sky irradiance / 2 pi (isotropic in-scattering from
  a uniform upper hemisphere) times a 0.35-to-1 bottom-to-top gradient;
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
  slabs; the shape is stretched over that range, then the product is cut
  at 0.35 for cumulus (gaps, domed tops where the profile thins, cells
  shrinking with coverage at their edges) and 0.1 for stratus. Overcast
  still had holes, so stratus coverage lifts the shape toward 1
  (`shape = mix(shape, 1, 0.6 coverage (1 - type))`): a continuous sheet,
  rough only where the profile thins and the detail erodes it.
- **Composite (`clouds_composite.frag`, replaced by Task 4):** bilinear
  upsample blended over HDR (premultiplied: clouds + background x T),
  skipped where the full-resolution surface is nearer than the clouds'
  mean depth, so clouds don't draw over terrain; HDR only, no motion.
- Cost (`clouds` GPU ms, 2560x1440, Debug build, one run each): fair
  ground 0.49, above the layer 0.61, inside (half res) 0.84; clear 0.33
  at both, the floor of marching empty layer through the weather lookups
  (the analytic fallback beyond 30 km, with its history search and
  Worley noise, on every step). Levers: coarser steps or a cheaper
  weather lookup beyond the window, fewer light steps.
  - Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite (2016): https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
  - Wrenninge, Kulla, Lundqvist, Oz: The Great and Volumetric (SIGGRAPH 2013 talk): https://doi.org/10.1145/2504459.2504518
  - Schneider, Nubis Evolved (2022): https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf
  - Bruneton, Precomputed Atmospheric Scattering (transmittance between two points): https://ebruneton.github.io/precomputed_atmospheric_scattering/
  - Jimenez, Next Generation Post Processing in Call of Duty: Advanced Warfare (2014, interleaved gradient noise): https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
