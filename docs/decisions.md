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
  TLAS is rebuilt per frame (sized for 4096 instances).
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

- **TAA** now, **DLSS later** in the same slot. Halton(2,3) jitter over 8
  frames, motion vectors as previous - current in UV, 9-tap Catmull-Rom
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

## Materials and assets

- **Filament's BRDF**: GGX, height-correlated Smith, Schlick, Lambert, with
  multiple-scattering energy compensation from a DFG LUT (Filament
  `DFV_Multiscatter`). Specular ambient comes from the sky-view LUT until RT
  reflections exist.
- **Bindless textures** (descriptor indexing, update-after-bind, partially
  bound, 16384 slots) and a material table read by buffer address.
- **KTX2 + Basis Universal UASTC** (`KHR_texture_basisu`), transcoded to BC7
  at load (UASTC is a BC7 subset, so nearly lossless). Basis Universal is
  used instead of KTX-Software because the latter's repo needs git-lfs and
  Basis alone covers both encoding and transcoding.
  - https://docs.vulkan.org/samples/latest/samples/performance/texture_compression_basisu/README.html
- **glTF via fastgltf** (2-20x faster than alternatives) with MikkTSpace
  tangents when missing.
  - https://fastgltf.readthedocs.io/latest/overview.html
- **meridian-cook** converts PNG/JPEG to KTX2 UASTC (slower pack level,
  zstd, mips; sRGB for color, renormalized normal maps) and writes a
  standard glTF.
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

