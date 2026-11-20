# Backlog: known issues and deferred work

Everything we noticed and chose not to do yet, with where it came from and
where it is meant to land. Add an entry whenever something is deferred
(a review finding parked, a known gap, a non-goal of a spec, user
feedback); remove it in the commit that resolves it. Milestones are the
roadmap's (`docs/superpowers/specs/2026-11-19-showcase-roadmap.md`).

## Lighting and shading

| Item | Why it matters | Source | Target |
|---|---|---|---|
| DLSS gets no `InPreExposure`: colour is pre-exposed by last frame's GPU exposure, so its history is not rescaled while exposure adapts (TAA v2 rescales) | Needs a CPU-side pre-exposure (e.g. a one-frame-late readback); the 310.9.16 guide says exposure affects only presets J/K, the default M is unaffected | Task 1.3 review | If DLSS becomes the default or a sun-to-shade A/B smears |
| Upscaler history reset on camera cuts: `historyValid` goes false only in `createSwapchain`, no hook for teleports or cutscenes | Predates DLSS; TAA and DLSS (and Ray Reconstruction, Task 1.5) all need it | Task 1.3 review | Before a teleport or cutscene feature |
| Fallback paths need the shadow denoiser | Under `--aa taa` and `--rr off`, soft RT sun shadows use 1 ray/pixel accumulated by TAA or DLSS SR; leaf shadows shimmer in motion (TAA beech crown/shadow 2.41/1.76 /255 at 1440p; Ray Reconstruction, the default, gives 1.41/0.35). Fallback-only work | User screenshots 2026-11-20; Task 1.5 | Motion-clarity Tasks 3.2a-3.2b, the denoiser running under TAA and DLSS SR |
| TAA path regression since the Megascans ground import | TAA v2 now misses crown shimmer (2.41 vs <= 2.0) and forward 5 SSIM (0.9796 vs >= 0.98); Task 3.1 measured both passing before the ground import. Fallback path only | Gate re-run 2026-11-20 | Investigate with Tasks 3.2a-3.2b |
| SSIM fwd5 ground fails under DLSS: 0.959 (RR) / 0.960 (SR DLAA) vs the >= 0.98 gate, on main at 64d21a6 | The forward-motion structural gate is red for every upscaler, so it no longer catches regressions | Raster lane, step 1 measurements (2026-11-20) | Find the commit that dropped it (clouds, meadow or ground changes) and re-anchor or fix |
| Ambient visibility inside crowns: foliage gets sky and ground light with occlusion 1, so crowns lack the dark depth of the reference frames | Biggest remaining foliage gap after leaf transmission and the grade | Q1 diagnosis R2c, quality pass | Crown-depth AO baked at cook, or RT sky visibility (M2b) |
| Ground bounce is grey (`kGroundAlbedo` 0.1 in mesh.frag); the shadow sides of rocks and the cliff read blue | Tinting it with today's terrain layers (mean albedo 0.20, 0.12, 0.06) turned the forest's leaf undersides olive-brown, away from the look target: the brown ground is placeholder | Q1 diagnosis R4, quality pass | With the ground lane's terrain: bounce from the terrain material's mean albedo (or RT GI) |
| Contact shadows: soft penumbra, Bend-style wavefront march, terrain terminator | The pass is a binary 16-step march along the sun's centre direction with per-pixel jitter (the upscaler averages it), so its shadows stay hard where the RT ones widen; a wavefront march over the depth buffer gives longer, cleaner rays at the same cost. Terrain pixels whose LOD triangle faces away from the sun (lit anyway, `facesSun` in mesh.frag) can be darkened by the march against that raster | Contact shadows commit | When contact shadows show noise or short rays in a gate view |
| Leaf textures (venation, normal, translucency maps) | Megaplants USD ships flat leaf colors only | Task 2 import; user feedback | Content, with leaf translucency |
| Shadows too dark (needle ground ~11% of sunlit in two measured views; real ~10-20%) | Shadows at the dark end of real | Shadow investigation 2026-11-20 | Haze profile and bounce light (rows below) |
| Haze anchored at sea level | Ridges (1.35 km) sit above most of the aerosol: less sky light up high, thin haze from above | Shadow investigation; haze tuning | Engine: aerosol layer from the valley floor / layered profile with a sharp top |
| Distant hills violet | Brown placeholder terrain albedo under blue haze: far-band OKLCh hue 277 vs 242 with a mid-green ground (reference 236-252); fix = green ground cover / vegetation albedo | Motion-clarity haze investigation | Real terrain albedo (M3 meadow) |
| Bounce light / GI: first version only (`--gi probes`, default off) | Ground hits use a fixed meadow albedo; no ground radiance cache, albedo feedback or multi-bounce; no screen-space pass (`hybrid`); probes 1.0-1.4 ms (spec 0.7) | GI lane, 2026-11-20 | Rest of `docs/superpowers/plans/2026-11-20-gi.md` (Tasks 5-8) |
| GI with `--trees`: forest interiors turn blue and the probes cost ~3 ms | The far-field voxels barely occlude the sky for probes under the canopy, so the sky dominates the ambient; the far-field march per probe ray is the cost | GI lane, 2026-11-20 | Canopy occlusion for probes (check farFieldTransmittance near the camera, a canopy term), cheaper tree traces |
| Cloud occlusion of ambient | Dropped from M4: it double-darkened with the cloud-dimmed sky capture | M4 clouds spec | Revisit with GI |
| RT reflections | No reflections beyond the specular from the sky | M2b spec non-goal | Later (with a denoiser) |
| Sky occlusion | Forest interiors and under-canopy too bright from the sky | Roadmap | M2b sky visibility |
| Haze pink cast at sunset | Slightly off color | Haze tuning | Revisit with clouds/look-dev |
| Sky capture taken at the camera | Above the cloud layer the scene under an overcast deck gets the clear sky's ambient (and specular sky); hidden by the deck from above, visible through gaps | M4 clouds sky capture | Capture from below the layer for surfaces under it, or a second capture |
| Overcast lit only by ambient | Thick decks pass no sunlight through the octave approximation (optical depth ~100), so overcast undersides show the clear sky's blue ambient instead of neutral grey diffuse sunlight; the sheet is also near-uniform (no darker/lighter patches) | M4 Task 7 look development | A diffuse-transmission term for thick clouds (slab transmission ~1 / (1 + 0.75 tau (1 - g))) and stratus structure from the base noise |
| Cloud erosion as a second threshold at low coverage | Coverage multiplies the density before the detail erosion (up to 0.5), so thin-coverage cell edges erode away instead of thinning: the effective cover falls below the map's | M4 clouds review, carried into Task 7 | With the coverage/shape model, if fair cover drifts from the map's |
| Cloud look gaps vs the Chlorophyll frames | Our clear sky is a deeper blue than the frames' pale, hazy summer sky (atmosphere/haze, not clouds; `--haze summer-hazy` is closer); the overcast deck is nearly featureless; distant clouds are smooth (detail noise is sub-pixel there); cirrus is straight-streaked (ruler-straight parallel lines from the ground too; one lit by the forward-scattering peak reads as a thin diagonal streak from the sun, gone with `--clouds off` or `--weather clear`, not bloom) and is missing from the sky capture and the cloud shadows | M4 Task 7 look development | User review of the look; next look round |
| Cloud transparency mask over solid cloud | The composite raises the DLSS mask to the full cloud opacity, also where it writes cloud motion (opacity > 0.5), so DLSS may distrust correct cloud motion; TAA ignores the mask | Clouds rebase review, 2026-11-20 | With the DLSS default switch: try marking only the blend, `1 - abs(2 opacity - 1)`, and compare the turn ratio |
| Cirrus fringe at silhouettes seen from in or above the layer | Rays that leave the layer before hitting terrain are not flagged as cut, so the composite may blend a texel without cirrus next to a ridge (1-2 texels; from below the layer terrain is cut and safe) | Clouds look review, 2026-11-20 | Flag every geometry hit as cut; check at `1119 3200 -431 -1.55 0` |
| Cloud shadow map at a low sun | Below 6 degrees the map's 32 steps per line alias into faint blocky cloud self-shadow, so it hands over to the coverage fallback (soft, no cloud-shaped shadows) by 3 degrees; at 3-6 degrees the weather pass costs up to ~0.5 ms | M4 Task 5 review | Adaptive step count or amortized map updates |

## Image quality

| Item | Why it matters | Source | Target |
|---|---|---|---|
| TAA v2 output sharpen off | sharpen.comp (FidelityFX CAS) runs as a copy: even CAS's weakest setting drops the forward 5 m/s SSIM gate to 0.972 (< 0.98) and raises still shimmer from 1.7/1.5 to 2.3/2.4 (/255, 1440p): it amplifies the frame-to-frame noise | Motion-clarity Task 3.1 | Re-tune `kSharpness` after the shadow denoiser (Task 3.2b); if it also lands at 0 there, delete sharpen.comp and its pass (the copy costs 0.03 ms at 1440p) and let taa.comp write `output` directly again |
| TAA v2 foliage speckle in motion | At 5 m/s forward the beech's leaves sparkle (motion/still Laplacian 1.00 at 1440p, 1.02 at 1080p): the shorter history averages less of the 1-ray shadow and alpha-test noise (not isolated) | Motion-clarity Task 3.1 | Shadow denoiser (Tasks 3.2a-3.2b); Ray Reconstruction removes it but is opt-in (`--rr on`) |
| DLSS RR grey smudge in the sky above the cloud layer | Sunset from above (`1119 5500 -431 -1.55 -0.2 --sun -0.998 0.05 0.03 --weather broken`) shows a soft grey blob in clear sky under Ray Reconstruction only (not with TAA); it appeared with the sharper clouds (gain 6, billows), so RR's guides there (transparency mask, albedo/normal of the sky) likely mislead it | Sharp cloud detail | With the RR guide work for clouds |
| Cloud history not rescaled on exposure change (it is pre-exposed) | Ghosting on clouds when exposure adapts | M4 clouds | With the cloud look work |
| Cloud detail limited to quarter resolution | The sub-texel jittered march (decisions.md "Cloud march sub-texel jitter") accumulates each texel's area, so edges are smooth but no finer than a quarter-res texel | M4 Task 7 look development; sunset-fix lane | Full-resolution reconstruction (HZD's 1-of-16 update) if the look needs it |
| Volumetric clouds combed or grainy up close (in the layer, half resolution) | The along-ray jitter is interleaved gradient noise; the resolve reprojects texels as clouds drift, so IGN's diagonal pattern isn't integrated and stays as combed stripes (`1119 3200 -431 -1.55 0`, any upscaler; a PCG hash removes the stripes but leaves grain under DLSS SR). Lower current weight raised frame-to-frame flicker even with clipping off (in-layer, SR, second-difference flicker sky 0.356 -> 0.427), so the history itself moves: suspect the reprojection by each frame's single noisy cloud depth | sunset-fix lane, 2026-11-20 (user screenshot) | If volumetric stays: hash jitter, reproject by the history's (or a filtered) depth, tent-filter the current frame before blending; else drop with the volumetric path |
| Volumetric cloud band at eye level inside the layer | From inside the layer the clouds at the horizon turn into a hazy band and those just below it look truncated (`1119 3200 -431 -1.55 0`); not the scene-depth cut (identical without it) nor bloom. Near-horizontal rays cross 100+ km of layer with 128 growing steps, and the haze at their mean depth dominates | sunset-fix lane, 2026-11-20 (user screenshot) | With the volumetric march's horizon handling, if volumetric stays |
| HDR output (mandatory, top quality) | User 2026-11-20: "the best of the best HDR possible", Windows acceptable. Linux path looks viable but unproven: NVIDIA 610.43 (May 2026) added HDR output and the DRM color pipeline, VK_EXT_hdr_metadata since 580.94.11 (Wayland only), Hyprland >= 0.55 does HDR (`cm = hdr`, 10-bit); open risks are NVIDIA's wp_color_management_v1 support and driver bugs (610.43.02 passes invalid metadata luminance). Dev box `vulkaninfo` lists HDR10_ST2084/HLG/scRGB, but the monitor test is pending: the session ran on Hyprland's headless fallback output (`hdr: no`). Windows (DXGI/Vulkan HDR10 + scRGB, mature) is the fallback. Engine work: HDR swapchain + hdr_metadata, display-transform split so grading stays scene-referred and shared by SDR/HDR (AgX stays the SDR path), paper-white and peak-nits settings, UI composited at paper white, dither scaled to the format | User 2026-11-20; sources: ubuntuhandbook.org/index.php/2026/05/nvidia-610-43-02-released-with-hdr-output-support-for-linux, webpronews.com/nvidias-linux-driver-leap-hdr-metadata-unlocks-new-era-for-wayland-gaming, wiki.hypr.land/configuring/core/monitors/colors, wiki.archlinux.org/title/HDR | Mandatory; spec first, test on a real HDR monitor |
| Glare checked against no photo reference | `assets/reference` has no midday-sun photo; the bloom was tuned by eye (edgeless high sun, visible dusk disk) and by the sharpness gate | Bloom lane 2026-11-20 | Compare with a real unfiltered midday-sun photo (same exposure: sky not clipped) when one is added |
| Bloom is a sum of Gaussians, not a measured eye/lens PSF; no glare streaks (ciliary corona, aperture diffraction spikes) | The heavy tail is (1 - s) s^k weights over 8 mip levels; a measured PSF (FFT convolution, UE's Convolution Bloom) or Spencer 1995 / Ritschel 2009 streaks would be closer to the eye | Bloom lane 2026-11-20 | Look-dev pass, if the glare reads synthetic |
| Dither pattern static per pixel | Fine at 8-bit; an HDR/10-bit swapchain needs the dither scaled | Dither fix | When HDR output lands |
| DLSS Frame Generation / Multi Frame Generation | Higher displayed frame rate; FG needs RTX 40, MFG RTX 50; NVIDIA recommends Streamline (Windows-only), native Linux via NGX unverified | Motion-clarity spec non-goal | After the Upscaler interface; verify Linux first |
| DLSS 5 | Newest DLSS (shipped 2026-09), RTX 50 only, no Linux evidence; dev GPU is an RTX 4080 | Motion-clarity spec | When the Linux SDK carries it and an RTX 50 target exists (a version bump behind the Upscaler interface) |
| XeSS backend | Intel's upscaler; no Linux binaries | Motion-clarity spec non-goal | When Linux binaries exist and a non-NVIDIA target needs it |
| FSR 4 backend | Better than TAA v2 on AMD RDNA 2-4 (FidelityFX SDK 2.4, MIT, Vulkan); Linux binaries unverified | Motion-clarity spec non-goal | When a non-NVIDIA target exists |
| Grade look tuning | The game starts with a grade tuned toward the Chlorophyll Engine frames (decisions.md, "Visible quality pass"); the engine default stays identity | Motion-clarity spec; quality pass | Final look after sky occlusion, crown AO and the ground lane's terrain; the user tunes it in game (G) and the grade file is the source |

## Geometry and vegetation

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Leaves vanish at distance | Cluster simplification thins disjoint leaves (sloppy collapse; the error measures leaf size, not lost coverage). A per-mesh leaf error multiplier is no stop-gap: 2x barely helps, 4x restores the summit view's canopies at 18M -> 118M triangles, 5.6 -> 17.8 ms, and overflows the cull lists at the forest views (quality pass, Release 1440p) | User screenshots; VG spec; Q1 diagnosis R1 | M2a mid voxel tier (Task 11) and voxel far field (Tasks 8-9); coverage-preserving simplification = VG slice 3 |
| RT on clusters | Shadows trace full-detail BLAS while raster draws LOD clusters (rays start the cluster's simplification error above the surface to avoid acne) | VG spec | VG slice 2 |
| Visibility buffer | Tiny-triangle shading efficiency | VG spec | VG slice 4, if measurements call for it |
| Cluster compression and streaming | Memory at larger scale (slice 5's hierarchical traversal landed in M2a Task 7); the geometry pools free retired buffers after vkDeviceWaitIdle and keep up to 1/3 slack, so streaming needs frees deferred by frame fences (and a trim) | VG spec | VG slice 5 |
| Tree wind | Trees are static | Roadmap (RT BLAS can't sway with the raster) | After clouds' wind interface; with RT on clusters |
| Placed-instance rotation precision | Up to 0.6 deg error near 180 deg (xyz snorm16) | M2a Task 4 | Smallest-three encoding if it ever shows |
| Trees beyond ~1.3 km may float/sink up to ~3 m on coarse terrain | Ground fit; not visible at 1080p on the summit ridge at 1.5 km (M2a Task 5) | M1 final review, M2a Task 4 | Recheck at 1440p with the voxel tiers (M2a Tasks 9-11); fix = snap to displayed CDLOD height |
| Every placed tree is rasterized as clusters | Wide forest views spend ~10-11.5 ms in the early scene pass (28.8M triangles at the forest edge, 46.6M at the overview) | M2a Task 5 | M2a mid voxel tier (Task 11) and voxel far field (Tasks 8-9) |
| Cluster cull workgroups mostly idle | One 64-thread workgroup per BVH leaf work item, which holds a group of ~1-16 clusters: 0.92 ms for 813K work items at the forest overview (Release, 1080p) | M2a Task 7 | Pack work items by cluster (one thread per cluster) if it shows in profiles after the far field |
| Cliff prototype shares the boulders' full random rotation | The 10.6 m sandstone cliff shows as tilted slabs sticking out of slopes | M2a Tasks 4-5 | Content tuning: yaw-only rotation or its own layer |
| Cooked assets aren't re-cooked when the cook changes | `fetch-sample-assets.sh` cooks only missing models; moss rock sets cooked before the vertical-offset bake lose rock07's scale and offsets (delete `assets/rock_moss_set_0{1,2}` and re-run) | M2a Task 5 | Tooling: stamp cooked output with a cook version |
| Blended instances not culled, full detail | Cost if a blended asset is scattered | Known gap before the backlog | When needed |
| Masked-geometry RT shadows not visually verified | Possible wrong alpha-tested shadows | Known gap before the backlog | M2b (ferns with opacity micromaps) |
| Grass and grass wind | Meadows are bare; the plants are cooked in `assets/<id>/` (docs/assets.md) | Roadmap | M3, reading M4's global wind |
| Undergrowth (shrubs, ferns) | Forest floor is bare | M2a spec non-goal | M2b first layer; revisit layered dithering between placement layers then |
| Real land-cover data (swissTLM3D forest polygons as a mask) | Forests follow rules, not the real map | M2a spec non-goal | Content; the placement mask input is ready |
| Low-end far field (pure 2D canopy, Chlorophyll-style scaling) | Lower-end GPUs and VRAM; the voxel far field targets this machine | Far-field spec, user 2026-11-20 | Later; option B's 2.5D layer is the starting point |
| Partitioned TLAS is NVIDIA-only | Vendor lock-in | M2b spec | Plain-TLAS fallback later if AMD support wanted |

## Terrain

| Item | Why it matters | Source | Target |
|---|---|---|---|
| Terrain textures add ~8.7 s to Release startup | 30 4K UASTC transcodes at load | G1 terrain material | Transcode in parallel or cache BC7 |
| No parallax on the triplanar rock; no silhouettes or depth offset anywhere | POM marches one planar layer; rock relief (9-15 cm) would show most | Terrain POM | Mesh-shader displacement of near nodes if close-up cliffs need it |
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
| Terrain march still costs ~5 ms at a sun near 0 degrees (scene early ~10 ms + cull clusters ~1.9 ms vs ~4.4 + 0.2 at midday): grazing rays take the full 256 steps on terrain and opaque pixels and in the per-cluster march | Sunset/sunrise frame time | sunset-fix lane, 2026-11-20 (decisions.md "Terrain sun visibility per masked cluster") | A terrain shadow-height (horizon) map around the camera, updated as the sun moves, would make it one fetch per surface |
| Renderer constructor leaks partial Vulkan state on throw | Robustness | Known gap before the backlog | Later |
| Frame pacing: present timing (`VK_EXT_present_timing`, fallback `VK_KHR_present_wait`/`present_id`), a refresh-locked limiter that sleeps before input (Reflex-style, `VK_NV_low_latency2`), hitch removal (pipeline compiles, uploads, TLAS rebuilds off the frame) | Smooth delivery and good 0.1%/1%/5% lows, not just average FPS; flip-metering-like behaviour | User, 2026-11-20 | After the performance overlay; research current practice and NVIDIA/Wayland support first |

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
| Trees hidden by default (they look bad) | User, 2026-11-20 | Until textured, coverage-preserving trees exist; `--trees` re-enables the Megaplants forests and lone trees for testing the tech |
