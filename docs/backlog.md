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
| Shadows too dark (6-7.5% of sunlit vs ~10-20%) | Unrealistic black shadows | Shadow investigation 2026-11-20 | Multi-bounce AO in progress; rest below |
| Haze anchored at sea level | Ridges (1.35 km) sit above most of the aerosol: less sky light up high, thin haze from above | Shadow investigation; haze tuning | Engine: aerosol layer from the valley floor / layered profile with a sharp top |
| Bounce light / GI | No light bounced from the ground or between surfaces | Shadow investigation | Later (GI milestone not yet planned) |
| Cloud occlusion of ambient | Dropped from M4: it double-darkened with the cloud-dimmed sky capture | M4 clouds spec | Revisit with GI |
| RT reflections | No reflections beyond the specular from the sky | M2b spec non-goal | Later (with a denoiser) |
| Sky occlusion | Forest interiors and under-canopy too bright from the sky | Roadmap | M2b sky visibility |
| Haze pink cast at sunset | Slightly off color | Haze tuning | Revisit with clouds/look-dev |

## Image quality

| Item | Why it matters | Source | Target |
|---|---|---|---|
| TAA blur | Keeps only ~48% of the still image's sharpness in motion; textures soft up close | CLAUDE.md known gap; user feedback | Quality pass step 2 |
| TAA history not rescaled on exposure change | Ghosting when exposure adapts | CLAUDE.md known gap | Quality pass step 2 |
| Dither pattern static per pixel | Fine at 8-bit; an HDR/10-bit swapchain needs the dither scaled | Dither fix | When HDR output lands |
| DLSS integration | Upscaling, Ray Reconstruction (would also denoise); brings mip bias tuning. Linux: NGX SDK directly | Roadmap; terrain-material spec | Later |

## Geometry and vegetation

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Leaves vanish at distance | Cluster simplification thins disjoint leaves | User screenshots; VG spec | M2a impostors (Tasks 8-9); coverage-preserving simplification = VG slice 3 |
| RT on clusters | Shadows trace full-detail BLAS while raster draws LOD clusters | VG spec | VG slice 2 |
| Visibility buffer | Tiny-triangle shading efficiency | VG spec | VG slice 4, if measurements call for it |
| Cluster compression and streaming | Memory at larger scale | VG spec | VG slice 5 |
| Tree wind | Trees are static | Roadmap (RT BLAS can't sway with the raster) | After clouds' wind interface; with RT on clusters |
| Placed-instance rotation precision | Up to 0.6 deg error near 180 deg (xyz snorm16) | M2a Task 4 | Smallest-three encoding if it ever shows |
| Trees beyond ~1.3 km may float/sink up to ~3 m on coarse terrain | Ground fit | M1 final review, M2a Task 4 | Checked in M2a Task 5 |
| Blended instances not culled, full detail | Cost if a blended asset is scattered | CLAUDE.md known gap | When needed |
| Masked-geometry RT shadows not visually verified | Possible wrong alpha-tested shadows | CLAUDE.md known gap | M2b (ferns with opacity micromaps) |
| Grass and grass wind | Meadows are bare | Roadmap | M3, reading M4's global wind |
| Undergrowth (shrubs, ferns) | Forest floor is bare | M2a spec non-goal | M2b first layer; revisit layered dithering between placement layers then |
| Real land-cover data (swissTLM3D forest polygons as a mask) | Forests follow rules, not the real map | M2a spec non-goal | Content; the placement mask input is ready |
| Partitioned TLAS is NVIDIA-only | Vendor lock-in | M2b spec | Plain-TLAS fallback later if AMD support wanted |

## Terrain

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Ground reads as desert (rock on most steep slopes, no grass) | Biggest realism gap | User feedback | M3 meadow + terrain material |
| Grid pattern and two-tone patches on rock at distance; rock not hex-tiled | Visible repetition | User screenshots; CLAUDE.md | M3 terrain material |
| Detail displacement (+-0.3 m) casts no shadows | Missing micro-shadows near the camera | M1 Task 5 | Later |
| Residual contour shadow bands at grazing sun | Neighbor-cell relief under smooth normals | Shadow investigation | Accepted; revisit with shadow denoiser |
| Sunset (3 deg sun) shadow march costs ~1.15 ms | Unbudgeted at low sun | M1 Task 5 | M5 |
| Shadow rays limited to 20 km | Very long sunset shadows cut off | M1 | M5 if visible |
| CDLOD crack margin on very steep coarse nodes | Possible sub-meter cracks on cliffs | M1 Task 4 review | Watch |
| Normal-map sign seam on the crop's mirror line | 7.5 km out, in haze | M1 Task 4 review | Watch |

## Performance and workflow

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Debug startup ~96 s with placement | Slow iteration | M2a Task 4 | Use `--placement off`; optimize model loading later |
| Ground material costs ~1.1-1.4 ms on steep slopes | Budget | M1 | M5 |
| Renderer constructor leaks partial Vulkan state on throw | Robustness | CLAUDE.md | Later |

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
| Cumulonimbus rendering (the cloud-type axis has room) | M4 clouds spec | Later weather milestone |
| Authored weather zones (regional overrides on the analytic map) | M4 clouds spec | Later |
| Collision beyond walking on the heightfield | Roadmap | Later |
| CPU queries and editing/overrides of placed instances | M2a spec non-goal | When gameplay needs them (placement is deterministic) |
