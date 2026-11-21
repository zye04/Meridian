# Topic E: native anti-aliasing for dense foliage, and image formation (2026-11-21)

Verification policy: every source below was looked up online this session
(search result or page fetch) unless marked **unverified**. Cost figures are
only kept where a source gave them; otherwise "cost unmeasured". Technique
descriptions marked (memory) are my recollection of the technique, not
re-read from the source.

Baseline (decisions.md 2026-11-21): native TAA, YCoCg variance clip, Keys
a=-1 history resample, max current weight 0.2, still weight 1/16,
motion-scaled CAS. Motion shimmer walk 14.8-15.9 vs DLAA 10.1, turn 9.7-10.4
vs 6.1 (/255); decisions notes part of that gap is content (LOD cut, stream
fade), not AA. Branch `native` 3x3 reconstruction: -25% shimmer, -20% sharpness.

## Part 1. Anti-aliasing

### 1.1 Diagnosis
Sub-pixel alpha-tested blades flip coverage with the jitter: signal above
Nyquist. TAA only averages it (noise std ~ sqrt(a/(2-a)), already in
decisions). So attack the input (coverage, mips, geometry width, specular
variance) before the resolve; the turn case additionally suffers from
history-resample blur, which is a resolve problem.

### 1.2 Input-side

| Technique | Solves / how | Cost | Maturity | Source |
|---|---|---|---|---|
| Coverage-preserving alpha mips | Leaves thinning with distance: scale mip alpha so the alpha-test coverage matches mip 0. We already cook "coverage mips". | 0 runtime | Standard | Castano, "Computing Alpha Mipmaps", blog (2010) [S1] |
| Hashed / stochastic alpha | Alpha-tested geometry vanishing with distance; per-pixel hashed threshold gives stable noise for TAA to resolve. Adds noise rather than removing it, so only for fades/LOD, not as the shimmer fix. | cost unmeasured | Published, used | Wyman, McGuire, I3D 2017 [S2] |
| Geometric specular AA | Normal-map / sub-pixel-geometry sparkle: widen roughness by the footprint's normal variance (NDF filtering; derivative-based estimate in projected half-vector space) | cost unmeasured, ALU only | Published; Unity HDRP ships a version [S5] | Kaplanyan, Hill, Patney, Lefohn, HPG 2016 [S3]; Tokuyoshi, Kaplanyan, I3D 2019 [S4] |
| Blade width clamped to the pixel footprint + opacity fade | Triangles < 1 px alias by definition; widen and fade rather than flicker (memory; no source fetched) | cost unmeasured | Common practice, **unverified** | none |
| Filtered opacity in the far voxel tier | G1 in the audit; biggest forest shimmer source | engine work | n/a | audit |
| Alpha-to-coverage + MSAA | Exact edge coverage, but needs multisampled targets for the HDR + motion + guide attachments of our GPU-driven path (reasoning, not a measured cost) | high bandwidth | Not recommended | n/a |
| VRS | Shading cost only; not an AA tool | n/a | skip | n/a |

### 1.3 Resolve-side

| Technique | What it offers | Source |
|---|---|---|
| Karis TAA (YCoCg, variance clip, luminance-weighted blend) | Our baseline (memory) | Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014 Advances course [S6] |
| Pedersen INSIDE TAA | Motion dilation by closest depth (we tried and dropped it: decisions), velocity-weighted feedback (memory) | Pedersen, GDC 2016 [S7] |
| Filmic SMAA T2x (Activision) | Morphological + temporal supersampling + temporal filtering; sharpness and stability aim; same group's later "Dynamic TAA and Upsampling in CoD" (2020) [S8b] | Jimenez, SIGGRAPH 2016 [S8] |
| TSR (UE5) | Doc confirms thin-geometry handling as a first-class problem (detail thinner than a pixel disappears between frames) and a "shading rejection heuristic" deciding reuse vs reject; "async compute offsets ~0.5 ms of TSR cost" on PS5 Fortnite (only cost figure published on the page) | Epic UE docs [S9] |
| TAA survey | Systematic split into sample accumulation and history validation; also a unified TAA/upsampling formulation | Yang, Liu, Salvi, Computer Graphics Forum 2020 [S10] |
| FSR 3.1 NativeAA | Official 1.0x "NativeAA" mode; Vulkan (GLSL 4.50) backend; open source; reactive mask (0-1, recommended max ~0.9), transparency/composition mask and a pixel history "lock" to protect thin features. Runs on NVIDIA under Vulkan. Cost at 1440p NativeAA: not given in the fetched manual section (**unmeasured**) | AMD GPUOpen, FSR 3.1.4 manual [S11]; announcement 2024 [S12] |
| FSR 4 / Redstone ML | RDNA 4 only (AMD page and press); community Vulkan/Linux ports (OptiScaler) not verified (page returned 403) so **unverified**; not shippable on NVIDIA | [S13] [S14] |
| XeSS (DP4a path) | Runs on non-Intel GPUs via DP4a; no native-AA mode or cost verified this session (**unverified**) | Intel XeSS SDK, https://github.com/intel/xess (**unverified**) |
| DLSS DLAA | Our A/B reference (2.3 ms, repo) | decisions |

Reading: FSR's reactive mask and lock, and TSR's thin-geometry handling, are
the published answers to our problem: keep history for thin or high-frequency
features longer while still rejecting disocclusion. FSR 3.1 is open source
and Vulkan-capable so its source can be studied or run as an A/B backend
behind `UpscalerProvider`; it is not a proven shimmer win over our TAA (no
data), so treat as an experiment.

### 1.4 Motion blur as a clarity tool
Tile-max / neighbour-max reconstruction-filter motion blur (McGuire et al.,
I3D 2012 [S15]; Jimenez, CoD:AW 2014 [S16]) hides per-frame coverage flips
along the motion direction. It lowers measured Laplacian by construction, so
it conflicts with the "maximum clarity" gates. Optional, camera-only,
small shutter fraction for turns; not a TAA repair. Cost unmeasured.

## Part 2. Image formation

### 2.1 Exposure
Our measured problem (decisions 2026-11-21): mean-log metering brightened the
forest edge until sky/cloud reached OKLCh L 0.92/0.96 vs 0.72-0.82 in the
Chlorophyll frames; a p95-bound fix (no more than 2.0 pre-exposed) exists.
I did not verify any engine's published exposure scheme this session
(Frostbite, Lumen, Decima: **unverified**). Proposal (design, not sourced):
percentile-window histogram (drop darkest ~10% and brightest ~2%) plus an
authored per-biome EV clamp for forests. Cost negligible (one-thread pass).

### 2.2 Tonemappers

| Tonemapper | Notes | Source |
|---|---|---|
| AgX (current) | Neutral, inset/outset; our measured foliage hue 145 vs 123-133 in the references, mitigated in the grade (decisions) | Sobotka / Blender 4.0, 2023 (**unverified**, not re-fetched) |
| ACES 2.0 | Released spring 2025; less aggressive tone scale and chroma handling than 1.x; OCIO 2.4 / 2.5 integration | [S17] [S18] |
| Tony McMapface | Display transform for real-time, "boring" by design (no contrast or saturation boost); accounts for Helmholtz-Kohlrausch and Bezold-Brucke; Apache/MIT; LUT sizes not verified | h3r2tic, GitHub, 2023 [S19] |
| GT7 (Polyphony) | "Physically Based Tone Mapping and Perceptual Fidelity in Gran Turismo 7", Suzuki, Yasutomi, Uchimura, SIGGRAPH 2025 PBS course; ICtCp or JzAzBz space; aims to avoid washed-out highlights and hue shifts at high intensity; code released late 2025; BeamNG adopted it | [S20] [S21] |
| Khronos PBR Neutral | Reproduces base-colour sRGB faithfully under grey lighting; product-viz oriented, so likely too flat for a landscape look; useful as a neutral A/B | Khronos ToneMapping repo [S22] |

Since the grade + AgX is baked into a 65^3 LUT, swapping the curve costs no
runtime. Evaluate GT7 and Tony offline with `bakeGradeLut` and the existing
OKLCh replay against the Chlorophyll frames (target: foliage L 0.32-0.42,
chroma 0.055-0.12, hue 123-133 without a hue rotation). Saturation of greens:
keep chroma work in OKLCh after the tonemap (current design); a curve that
works in a perceptual space (GT7) should hold green chroma at high intensity
better than AgX's inset, to be confirmed by measurement.

## Part 3. What Chlorophyll probably does (inference)

- Stated (P-grade, baseline F9): native 1440p, no upscaler, ~130 fps on an
  RTX 3080 (about 7.7 ms per frame at 130 fps: little room for a heavy AA).
  Likely TAA-class (about 80%), SMAA T2x-like (10%), MSAA-only (10%, hard with
  a GPU-driven meshlet path). No source states it.
- Soft near grass in his frames (R9) fits long history, wind, or video
  compression; cannot be separated from a YouTube capture.
- Stability at that cost likely comes from the input side (clamped blade
  geometry, coverage-correct mips, filtered voxels) rather than a clever
  resolve (about 60%, pure inference).
- Image: high-key, saturated greens suggests a custom or colour-preserving
  curve, or filmic plus a saturation boost; about 40%, unresolvable from
  compressed video.

## Part 4. Ranked recommendations

1. **Thin-feature / reactive history control in taa.comp** (FSR reactive +
   lock, TSR thin-geometry idea): detect pixels whose coverage or colour flips
   frame to frame (alpha-tested, 3x3 depth variance) and lengthen history for
   them only. Verify: walk/turn shimmer and Laplacian vs 14.8/0.529 and
   9.7/0.480.
2. **Check luminance-weighted (Karis) blend and clip** in taa.comp; hot
   sunlit glints dominate otherwise. Free if missing.
3. **Far-tier filtered opacity and blade-width/footprint clamp** (G1): removes
   input flicker. Gate: still shimmer, forest overview.
4. **Geometric specular AA audit** in mesh.frag and terrain (roughness
   widening from normal variance and `fwidth(N)`).
5. **Tonemap A/B: GT7 and Tony McMapface vs AgX**, offline via the LUT replay.
6. **Exposure: percentile-window histogram + authored forest EV clamp** (G7).
7. **Experiment only: FSR 3.1 NativeAA backend** to learn whether its lock
   beats our resolve; drop FSR 4 ML and XeSS.
8. **Optional camera-only motion blur toggle**, off in gates.

Skip: MSAA/A2C, VRS for AA.

## Sources (all fetched or returned by search this session unless marked)
- [S1] Castano, Computing Alpha Mipmaps, https://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/ (2010)
- [S2] Wyman, McGuire, Hashed Alpha Testing, I3D 2017, https://research.nvidia.com/labs/rtr/publication/wyman2017hashed
- [S3] Kaplanyan, Hill, Patney, Lefohn, Filtering Distributions of Normals for Shading Antialiasing, HPG 2016, https://diglib.eg.org/handle/10.2312/hpg20161201
- [S4] Tokuyoshi, Kaplanyan, Improved Geometric Specular Antialiasing, I3D 2019 (found via search; no direct URL fetched; DOI/ACM page **unverified**)
- [S5] Unity HDRP Geometric Specular Anti-aliasing, https://docs.unity3d.com/Packages/hdrp/manual/Geometric-Specular-Anti-Aliasing.html
- [S6] Karis, High Quality Temporal Supersampling, SIGGRAPH 2014 courses, http://advances.realtimerendering.com/s2014/ (cited by search results; page itself not fetched)
- [S7] Pedersen, Temporal Reprojection Anti-Aliasing in INSIDE, GDC 2016, https://www.gdcvault.com/play/1022970
- [S8] Jimenez, Filmic SMAA, SIGGRAPH 2016, https://research.activision.com/publications/archives/filmic-smaasharp-morphological-and-temporal-antialiasing ; [S8b] https://www.activision.com/cdn/research/Dynamic_Temporal_Antialiasing_and_Upsampling_in_Call_of_Duty_v4.pdf (2020)
- [S9] UE Temporal Super Resolution docs, https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine
- [S10] Yang, Liu, Salvi, A Survey of Temporal Antialiasing Techniques, CGF 2020, https://diglib.eg.org/handle/10.1111/cgf14018
- [S11] FSR 3.1.4 upscaler manual, https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/
- [S12] https://gpuopen.com/fsr3/ (2024)
- [S13] https://gpuopen.com/fidelityfx-superresolution-4/ (2025)
- [S14] https://www.techpowerup.com/346705/optiscaler-one-ups-amd-with-fsr-4-vulkan-support (2026; fetch 403, headline only)
- [S15] McGuire et al., A Reconstruction Filter for Plausible Motion Blur, I3D 2012, DOI 10.1145/2159616.2159639 (via Crossref in search)
- [S16] Jimenez, Next Generation Post Processing in CoD: AW, SIGGRAPH 2014, https://iryoku.com/?p=1707
- [S17] ACES 2.0 release notes, https://docs.acescentral.com/background/about-aces-2/ (2025)
- [S18] OCIO 2.4, https://opencolorio.readthedocs.io/en/latest/releases/ocio_2_4.html
- [S19] https://github.com/h3r2tic/tony-mc-mapface (2023)
- [S20] https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides.pdf (2025)
- [S21] https://www.gtplanet.net/beamng-drive-is-using-gran-turismo-7s-hdr-tone-mapping-technology-20260803/
- [S22] https://github.com/KhronosGroup/ToneMapping
