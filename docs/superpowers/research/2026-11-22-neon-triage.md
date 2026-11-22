# Triage: Neon deep-research report (2026-11-22)

Source: `2026-11-22-neon-deep-research.md` (answers
`2026-11-22-deep-research-briefs.md`, briefs A, B, C). Each claim was
checked against the code, the NGX SDK 310.9.16 headers and programming
guide (`build/_deps/dlss_sdk-src`), and `docs/decisions.md` before it
was trusted. Inputs to our A/Bs, never decisions on their own.

## Claims checked

| Report claim | Label given | Verdict |
|---|---|---|
| C1 #1: the cloud composite's transparency mask (1 on solid clouds) makes DLSS drop history on clouds | established | **Wrong.** `pInTransparencyMask` is "Unused/Reserved for future use" in the SR eval struct (`nvsdk_ngx_helpers_vk.h:76`); taa.comp doesn't read it either. Under DLAA SR the mask changes nothing. The reactive input is `pInBiasCurrentColorMask` (guide 3.15: problem assets only, "increased aliasing within the mask borders"); we don't pass it. |
| C2: NGX default for DLAA is preset K; we override to M | established | Right (guide 3.12; `defaultPreset` in dlss.cpp). |
| C2: K was rejected only on the forward Laplacian ratio | - | Partly. Also SSIM fwd 0.926 vs 0.993. But K's shimmer was 0.27/0.30 vs M's 0.80/0.40 and `aa` 0.75 vs 2.36 ms, and K was never run through `motion-clarity.sh` (which landed after). The stability-over-sharpness preference postdates that choice. **Re-test is justified.** |
| C4: cloud resolve weight 0.03 still / 0.1 moving, outline on quarter-res texels, composite before the upscaler | established | Right (decisions "Cloud outline and motion-adaptive resolve"). |
| B2: leaf sun/transmission dead zone where `sh.N` and the geometric normal disagree across the terminator | code | **Right** (mesh.frag 976-997: N.L > 0 with geom.L < 0, or the reverse, skips both branches). Also: back faces *are* flipped (948), so G13's "front normal" hypothesis is wrong. |
| B3: voxel shaders have no transmission | code | Right (voxel_instance.frag 72, voxel.frag 115: `NoL > 0` only). |
| C6 #1: contact shadows are a binary march re-jittered every frame | code | Right (contact_shadows.comp 67-71, IGN/PCG by frameIndex). |
| A4: grass normals blend to the terrain only inside the stream fade | code | Right (mesh.frag 952). |
| A6: 48 B vertex, ~110-125 B/triangle | code | Vertex 48 B right (mesh.h); per-triangle total not re-derived. "~1.27 GB TLAS" not found in decisions.md: unverified. |
| A2: meshopt simplification drops disjoint traced leaves (Gate 2 fails) | speculative | Already handled on `leaf-geometry` (3a3ecab "leaf meshes keep their area at coarser levels"). |
| B1: F0 0.04 is right for beech (n 1.5) | established | Consistent with our leaf F0; points G15 at roughness / unoccluded sky specular instead. |
| C5: frame 120 vs 121 sit on different Halton phases | speculative | True but constant across A/Bs, so comparisons stay fair. Real blind spot found instead: the sky rows' Laplacian is whole-frame (terrain, wall), so it hides cloud smear (see step 2 baseline). |

## Contradictions with decisions.md

1. **DLSS preset** ("DLSS DLAA is the default" + the SR preset table):
   report says K for DLAA (NVIDIA's default, cheaper, steadier). Not
   overturned yet: K's forward smear was measured. Resolved by A/B 1
   below.
2. **GTAO spec** (`specs/2026-11-21-gtao-design.md`, attachment 7):
   report argues visibility-bitmask AO and a visibility buffer first.
   Speculative; belongs to the GTAO decision the user hasn't made.
3. None of the report's other recommendations contradict a logged
   decision (no impostors, raster shadows, voxel far field all kept).

## Recommendations

| # | Change | G | Owner | Confirming measurement | Cost / risk |
|---|---|---|---|---|---|
| 1 | DLAA preset K (and L) vs M | G19, G9 | image-stability (step 2 A/B 1) | motion-clarity full table + cloud crop | 0 code; K saves 1.6 ms; risk: forward smear (0.44 ratio) |
| 2 | Cloud transparency-mask fix | G19 | - | - | **Dropped**: no effect under DLSS SR |
| 3 | Cloud outline off / off in motion | G16, G19 | image-stability (step 2); `cloudpop` terminal measures pops | cloud crop jitter/error, pop count | 0 ms; risk: softer clouds (edge width 4.1 -> 5.5 px) |
| 4 | Clouds composited after DLAA | G19 | image-stability (step 2, last) | cloud crop, summit silhouette | Moderate code; risk: aliased jittered-depth silhouettes |
| 5 | Leaf dead zone fix, tinted transmission + forward lobe, energy clamp | G13, G3 | leaf-geometry (leaf material) | backlit sunset capture, `region-luminance.py` | ~0.1 ms; risk: over-bright crowns |
| 6 | Geometric specular AA (`dFdx/dFdy(N)`) + Toksvig in cooked roughness mips | G18, G15, G19 | leaf-geometry (leaf material); terrain later | forest motion-clarity jitter, still capture of leaf sparkle | ~0.05 ms + cook change; risk: matte leaves at grazing |
| 7 | Contact shadows: deterministic (Bend-style) instead of per-frame jitter | G5, G19 | image-stability (after step 2) | ground/meadow jitter: current vs `contact=0` vs new | ~0.3 ms; risk: grass over-darkening |
| 8 | Voxel trees: transmission, crown-scale normal, filtered/larger voxels | G1 | far-field (G1, unassigned) | aerial captures vs refs, forest turn jitter | possibly -1..-2 ms; switch-band risk |
| 9 | Grass normals toward the terrain by 15-30 m | G4, G19 | image-stability (after step 2) | ground_side5 / ground_turn1 jitter | ~0 ms; flatter mid grass |
| 10 | Visibility buffer before GTAO; bitmask AO not GTAO | G2, G10 | user decision (GTAO spec) | parity route, AO halos at forest interior | weeks; large refactor |
| 11 | Quantized 16 B cluster vertices | perf/VRAM | leaf-geometry follow-up | VRAM via nvidia-smi, seam precision | days; seam cracks |
| 12 | Forest-floor ambient toward 2-5% of open sky; exposure lift cap | G7 | Phase 0 Task 4 (G7) | forest floor/open irradiance ratio (`region-luminance.py`) | small; darker forests by design |
| 13 | GI probes: hardware-trilinear probe volume, cheaper brick march | GI default | GI (deferred) | `gi probes` + scene early ms | small-medium |
| 14 | Motion metric: average jitter over 120..123; ColorVideoVDP vs SSAA clips | G19 tooling | image-stability (tooling) | - | 4x captures; VDP needs PyTorch |
| 15 | Nanite-style assemblies (branch instancing) | memory | backlog (phase 2) | - | architectural |
| 16 | Wind on trees: split cached vs dynamic cascades | - | backlog (no wind on trees yet) | - | - |

## Step 2 (cloud small-detail smear) re-planned

What the report changes: the mask fix is moot (DLSS SR ignores it), so
the cloud signal reaches DLAA with correct motion (where opacity > 0.5)
and full history; the smear is our own quarter-res history plus the
outline. The preset is a cross-cutting lever worth testing first.

Measure on a crop of small distant cumulus in the sky view
(`detail=0.35,0.50,0.95,0.63`, 1536x187+896+720 at 1440p). DLAA M
baseline, re-scored from `.superpowers/motion/baseline-dlaa/` (no GPU):

| case | crop Laplacian (ratio) | jitter | error |
|---|---|---|---|
| sky_still | 0.0295 (1.00) | 0.20 | - |
| sky_drift | 0.0298 (1.01) | 0.22 | 0.86 |
| sky_turn1 | 0.0206 (0.70) | 1.19 | 2.48 |

The whole-sky row showed turn1 at 0.96: the crop shows the 30% loss.

A/B list (proposed, not run):

0. Tool: `motion-clarity.sh` gains the `detail` crop rows (Laplacian
   on the crop, jitter, error). No GPU.
1. Preset K and L vs M, full motion-clarity table (forest, ground, sky,
   crop) + `aa` ms. Flags only.
2. `--effects cloud-outline=0`, sky rows. Flag only.
3. Shorter cloud history in motion: resolve weight at 0.5 texel/frame
   0.1 -> 0.25 (temporary constant). Sky rows + `clouds` ms.
4. Half-res march outside the layer (temporary). Sky rows + `clouds` ms.
5. Only if 1-4 leave the turn crop ratio below ~0.85: composite the
   clouds after DLAA (prototype; silhouettes against jittered depth are
   the risk).
