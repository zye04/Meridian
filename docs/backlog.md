# Backlog: known issues and deferred work

Everything we noticed and chose not to do yet, with where it came from and
where it is meant to land. Add an entry whenever something is deferred
(a review finding parked, a known gap, a non-goal of a spec, user
feedback); remove it in the commit that resolves it. Milestones are the
roadmap's (`docs/superpowers/specs/2026-11-19-showcase-roadmap.md`).

## Lighting and shading

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Shadow denoiser | Soft RT sun shadows use 1 ray/pixel accumulated by TAA; leaf shadows shimmer in motion | User screenshots 2026-11-20 | Quality pass step 2 (spec pending) |
| Leaf translucency (light through leaves) | Canopies read dull and dark; tree shadows too dark | M2a spec non-goal; user feedback | After M2b sky visibility |
| Leaf textures (venation, normal, translucency maps) | Megaplants USD ships flat leaf colors only | Task 2 import; user feedback | Content, with leaf translucency |
| Shadows too dark (needle ground ~11% of sunlit in two measured views; real ~10-20%) | Shadows at the dark end of real | Shadow investigation 2026-11-20 | Haze profile and bounce light (rows below) |
| Haze anchored at sea level | Ridges (1.35 km) sit above most of the aerosol: less sky light up high, thin haze from above | Shadow investigation; haze tuning | Engine: aerosol layer from the valley floor / layered profile with a sharp top |
| Bounce light / GI | No light bounced from the ground or between surfaces | Shadow investigation | Later (GI milestone not yet planned) |
| Cloud occlusion of ambient | Dropped from M4: it double-darkened with the cloud-dimmed sky capture | M4 clouds spec | Revisit with GI |
| RT reflections | No reflections beyond the specular from the sky | M2b spec non-goal | Later (with a denoiser) |
| Sky occlusion | Forest interiors and under-canopy too bright from the sky | Roadmap | M2b sky visibility |
| Haze pink cast at sunset | Slightly off color | Haze tuning | Revisit with clouds/look-dev |

## Image quality

| Item | Why it matters | Source | Target |
|---|---|---|---|
| TAA blur | Keeps only ~48% of the still image's sharpness in motion; textures soft up close | Known gap before the backlog; user feedback | Quality pass step 2 |
| TAA history not rescaled on exposure change | Ghosting when exposure adapts | Known gap before the backlog | Quality pass step 2 |
| HDR output (mandatory, top quality) | User 2026-11-20: "the best of the best HDR possible", Windows acceptable. Linux path looks viable but unproven: NVIDIA 610.43 (May 2026) added HDR output and the DRM color pipeline, VK_EXT_hdr_metadata since 580.94.11 (Wayland only), Hyprland >= 0.55 does HDR (`cm = hdr`, 10-bit); open risks are NVIDIA's wp_color_management_v1 support and driver bugs (610.43.02 passes invalid metadata luminance). Dev box `vulkaninfo` lists HDR10_ST2084/HLG/scRGB, but the monitor test is pending: the session ran on Hyprland's headless fallback output (`hdr: no`). Windows (DXGI/Vulkan HDR10 + scRGB, mature) is the fallback. Engine work: HDR swapchain + hdr_metadata, display-transform split so grading stays scene-referred and shared by SDR/HDR (AgX stays the SDR path), paper-white and peak-nits settings, UI composited at paper white, dither scaled to the format | User 2026-11-20; sources: ubuntuhandbook.org/index.php/2026/05/nvidia-610-43-02-released-with-hdr-output-support-for-linux, webpronews.com/nvidias-linux-driver-leap-hdr-metadata-unlocks-new-era-for-wayland-gaming, wiki.hypr.land/configuring/core/monitors/colors, wiki.archlinux.org/title/HDR | Mandatory; spec first, test on a real HDR monitor |
| Dither pattern static per pixel | Fine at 8-bit; an HDR/10-bit swapchain needs the dither scaled | Dither fix | When HDR output lands |
| DLSS integration | Upscaling, Ray Reconstruction (would also denoise); brings mip bias tuning. Linux: NGX SDK directly | Roadmap; terrain-material spec | Later |

## Geometry and vegetation

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Leaves vanish at distance | Cluster simplification thins disjoint leaves | User screenshots; VG spec | M2a impostors (Tasks 10-11) and voxel far field (Tasks 8-9); coverage-preserving simplification = VG slice 3 |
| RT on clusters | Shadows trace full-detail BLAS while raster draws LOD clusters (rays start the cluster's simplification error above the surface to avoid acne) | VG spec | VG slice 2 |
| Visibility buffer | Tiny-triangle shading efficiency | VG spec | VG slice 4, if measurements call for it |
| Cluster compression and streaming | Memory at larger scale (slice 5's hierarchical traversal landed in M2a Task 7) | VG spec | VG slice 5 |
| Tree wind | Trees are static | Roadmap (RT BLAS can't sway with the raster) | After clouds' wind interface; with RT on clusters |
| Placed-instance rotation precision | Up to 0.6 deg error near 180 deg (xyz snorm16) | M2a Task 4 | Smallest-three encoding if it ever shows |
| Trees beyond ~1.3 km may float/sink up to ~3 m on coarse terrain | Ground fit; not visible at 1080p on the summit ridge at 1.5 km (M2a Task 5) | M1 final review, M2a Task 4 | Recheck at 1440p with impostors (M2a Task 9); fix = snap to displayed CDLOD height |
| Every placed tree is rasterized as clusters | Wide forest views spend ~10-11.5 ms in the early scene pass (28.8M triangles at the forest edge, 46.6M at the overview) | M2a Task 5 | M2a impostors (Tasks 10-11) and voxel far field (Tasks 8-9) |
| Cluster cull workgroups mostly idle | One 64-thread workgroup per BVH leaf work item, which holds a group of ~1-16 clusters: 0.92 ms for 813K work items at the forest overview (Release, 1080p) | M2a Task 7 | Pack work items by cluster (one thread per cluster) if it shows in profiles after the far field |
| Geometry pools too small for `--scatter 100` with placement | Startup throws "geometry pools are full" (vertices 18.7M of 16M, indices 73M of 64M, cluster triangles 48.5M of 48M); each view works on its own | M2a Task 7 | Raise the pools or share prototypes' meshes with the scatter when the combined view is needed |
| Cliff prototype shares the boulders' full random rotation | The 10.6 m sandstone cliff shows as tilted slabs sticking out of slopes | M2a Tasks 4-5 | Content tuning: yaw-only rotation or its own layer |
| Cooked assets aren't re-cooked when the cook changes | `fetch-sample-assets.sh` cooks only missing models; moss rock sets cooked before the vertical-offset bake lose rock07's scale and offsets (delete `assets/rock_moss_set_0{1,2}` and re-run) | M2a Task 5 | Tooling: stamp cooked output with a cook version |
| Blended instances not culled, full detail | Cost if a blended asset is scattered | Known gap before the backlog | When needed |
| Masked-geometry RT shadows not visually verified | Possible wrong alpha-tested shadows | Known gap before the backlog | M2b (ferns with opacity micromaps) |
| Grass and grass wind | Meadows are bare | Roadmap | M3, reading M4's global wind |
| Undergrowth (shrubs, ferns) | Forest floor is bare | M2a spec non-goal | M2b first layer; revisit layered dithering between placement layers then |
| Real land-cover data (swissTLM3D forest polygons as a mask) | Forests follow rules, not the real map | M2a spec non-goal | Content; the placement mask input is ready |
| Low-end far field (pure 2D canopy, Chlorophyll-style scaling) | Lower-end GPUs and VRAM; the voxel far field targets this machine | Far-field spec, user 2026-11-20 | Later; option B's 2.5D layer is the starting point |
| Partitioned TLAS is NVIDIA-only | Vendor lock-in | M2b spec | Plain-TLAS fallback later if AMD support wanted |

## Terrain

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Ground reads as desert (rock on most steep slopes, no grass) | Biggest realism gap | User feedback | M3 meadow + terrain material |
| Grid pattern and two-tone patches on rock at distance; rock not hex-tiled | Visible repetition | User screenshots; known gap before the backlog | M3 terrain material |
| Detail displacement (+-0.3 m) casts no shadows | Missing micro-shadows near the camera | M1 Task 5 | Later |
| Residual contour shadow bands at grazing sun | Neighbor-cell relief under smooth normals | Shadow investigation | Accepted; revisit with shadow denoiser |
| Sunset (3 deg sun) shadow march costs ~1.15 ms | Unbudgeted at low sun | M1 Task 5 | M5 |
| Shadow rays limited to 20 km | Very long sunset shadows cut off | M1 | M5 if visible |
| CDLOD crack margin on very steep coarse nodes | Possible sub-meter cracks on cliffs | M1 Task 4 review | Watch |
| Normal-map sign seam on the crop's mirror line | 7.5 km out, in haze | M1 Task 4 review | Watch |

## Performance and workflow

| Item | Why it matters | Source | Target |
|---|---|---|---|
| TLAS build scratch (228 MiB) stays allocated | The TLAS is rebuilt only when instances change, yet its scratch is kept for the whole run | M2a Task 5 review | M2b (memory accounting) |
| Debug startup ~95 s with placement | Slow iteration | M2a Task 4 | Use `--placement off`; optimize model loading later |
| Ground material costs ~1.1-1.4 ms on steep slopes | Budget | M1 | M5 |
| Renderer constructor leaks partial Vulkan state on throw | Robustness | Known gap before the backlog | Later |

## Content and assets

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Hornbeam and hazel Megaplants not downloaded | Species variety | Asset run | Optional download |
| All species share one bark texture (ambientCG Bark012) | Variety | M2a Task 2 | Content |
| "Rough sandstone rock" download is a flat surface scan | Not usable as a rock | M2a Task 4 | Use as a terrain rock material (M3) |

## Weather and world (out of current milestones)

| Item | Source | Target |
|---|---|---|
| Precipitation (rain, snow), wet surfaces, puddles | M4 clouds spec | Later weather milestone |
| Lightning, fog banks, valley mist, god rays | M4 clouds spec | Later |
| Water | Roadmap | Later |
| Procedural rivers and paths (meshes/splats over the terrain, like Chlorophyll Engine's; placement exclusion along them) | User, 2026-11-20 | Later; placement mask input is ready |
| Cumulonimbus rendering (the cloud-type axis has room) | M4 clouds spec | Later weather milestone |
| Authored weather zones (regional overrides on the analytic map) | M4 clouds spec | Later |
| Collision beyond walking on the heightfield | Roadmap | Later |
| CPU queries and editing/overrides of placed instances | M2a spec non-goal | When gameplay needs them (placement is deterministic) |
| Character animation: evaluate NVIDIA generative motion. ARDY (2026, research.nvidia.com/labs/sil/projects/ardy/): autoregressive diffusion, real-time body motion with online text/trajectory control on SOMA skeletons; runtime candidate, license, speed and GPU cost unverified. Kimodo (2026, github.com/nv-tlabs/kimodo): text + constraints to body motion, offline PyTorch (~17 GB VRAM, <3 GB with the text encoder on CPU), Apache-2.0 code, NVIDIA Open Model License weights (SMPL-X R&D only); authoring tool, not runtime. Both need a rigged skeleton. Related: Audio2Face-3D (C++ SDK, MIT) for talking NPCs | Research, 2026-11-20 | When characters exist |
