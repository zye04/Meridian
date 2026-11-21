# Research B: ambient occlusion and mid-scale indirect shadowing (2026-11-21)

Scope: gap G2 (and G4's "mat's own bottom-up occlusion") in `specs/2026-11-21-look-gap-audit.md`; fact F5 / requirement R3 in the Chlorophyll baseline.
Method note (verification status): fetched and checked this session: XeGTAO README, the GTAO memo (full text), the bitmask paper abstract, the GDC Vault grass session page. Not fetchable (timeouts, 10 MB limit, no slides online): Lagarde 2014 PDF text, Lumen talk, Ghost of Tsushima slides. Every figure not from a fetched source is marked **unverified** with its origin (author memory).

## 1. Sources, newest first

**Visibility-bitmask AO/SSIL: Therrien, Levesque, Gilet, "Screen Space Indirect Lighting with Visibility Bitmask", arXiv 2301.11376 (Jan 2023; published in The Visual Computer) [verified: authors, year, abstract]**
- Solves: GTAO's horizon model treats every occluder as an infinitely thick wall, so thin things (leaves, grass) over-occlude. Also gives directional AO and one-bounce screen-space indirect.
- How: replaces the two horizon angles per slice with a bit field of sectors around the hemisphere; samples with an assumed thickness clear sector bits; visibility = free bits, giving more accurate ambient than a bent normal. The abstract claims lower noise and "low performance overhead" vs previous methods; the paper's benchmark hardware is RTX 2080 at 1080p (search snippet; ms figures not extracted, unverified). Cost is linear in samples.
- Maturity: reference code (Drin-Matane, cdrinmatane.github.io, 2023) and community Unreal ports; not a documented AAA default (unverified). Thickness is a global constant, the weak point.
- https://arxiv.org/abs/2301.11376 ; https://cdrinmatane.github.io/posts/ssaovb-code

**UE5 Lumen screen traces and short-range AO (Epic, SIGGRAPH 2022 "Lumen" talk, docs 5.x) [unverified: not fetched; from memory]**
- Solves: small-scale indirect shadowing that the coarse GI misses. Lumen's final gather is probe-based at the screen; screen-space traces and a "short range AO" (a bent-normal-style AO from depth, ~0.5 m) fill the contact scale; the long range comes from surface cache / distance fields.
- Cost: short-range AO is ~0.2-0.5 ms at 1080p console-class GPUs (Epic figures from memory, **unverified**). Maturity: shipping. Lesson: AO is split by scale, short from depth, long from a world structure.
- https://advances.realtimerendering.com/s2022/ (Lumen talk, 2022); https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine

**XeGTAO (Intel; Filip Strugar and Steve McCalla; GitHub, MIT licence, archived read-only by Intel 22 Apr 2024) [verified from README]**
- Solves: production GTAO. Features: pre-filtered depth MIP chain, Hilbert-curve + R2 sampling, a 5x5 depth-aware spatial denoise designed to work with TAA, optional bent normals, thin-occluder heuristics, full-resolution operation with configurable slice/step counts.
- Cost (README): High preset 1920x1080 RTX 2060 ~0.56 ms; 3840x2160 RTX 3070 ~1.4 ms; 1080p Intel i7-1195G7 iGPU ~2.39 ms. Bent normals add ~25%. 1440p on a 4080 is not published: interpolating (pixels x1.8 vs 1080p, GPU ~2-3x faster than a 2060) gives roughly 0.3-0.4 ms (my estimate, unverified).
- Maturity: widely ported; archived, so expect to own the code.
- https://github.com/GameTechDev/XeGTAO

**GTAO: Jimenez, Wu, Pesce, Jarabo, "Practical Realtime Strategies for Accurate Indirect Occlusion", Activision Technical Memo ATVI-TR-16-01 (SIGGRAPH 2016 course) [verified, full text read]**
- Defines GTAO: analytic cosine-weighted visibility integral over horizon angles per slice, with a thickness heuristic for thin occluders (their Fig. 4). Multi-bounce fit from albedo rho: `G(A, rho) = a*A^3 - b*A^2 + c*A`, a = 2.0404 rho - 0.3324, b = 4.7951 rho - 0.6417, c = 2.7552 rho + 0.6903. GTSO: specular occlusion from the visibility cone (bent normal + AO) intersected with the specular lobe, via a precomputed lookup.
- Cost: "0.5 ms on a PS4 at 1080p" [verified, paper abstract/Fig. 1]. Maturity: de-facto standard.
- https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf (also on activision.com/cdn/research)

**Specular occlusion: Lagarde and de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0", SIGGRAPH 2014 course [PDF exists at the URL below; text not read here (size limit)]**
- Cheap empirical form `SO = saturate(pow(NoV + AO, exp2(-16*roughness - 1)) - 1 + AO)`. The formula was verified only as it appears in Unity's SRP, credited to Lagarde, derived from Oat and Sander 2008 cone intersection (search result); I did not confirm it inside the Frostbite notes. GTSO (above) is the better-grounded successor. Maturity: ubiquitous.
- https://seblagarde.wordpress.com/wp-content/uploads/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf

**Grass/foliage AO: Wohllaib, "Procedural Grass in Ghost of Tsushima", GDC 2021 Advanced Graphics Summit [verified: session page and speaker; ~83,000 blades on screen at ~2.5 ms per press coverage; slides not obtained]**
- What the session covers (GDC Vault abstract): GPU blade generation, rendering/animation, and making thousands of blades read as a natural field. That the talk does AO as a model-space height darkening and blends blades toward the terrain is **unverified**: only a secondary search snippet says "AO is simulated by darkening blades vertically based on model-space height" (likely about a community reimplementation, not Sucker Punch). Treat the practice (bottom-up gradient, no screen-space AO on blades) as a widespread convention, not a confirmed Sucker Punch claim. Horizon (Guerrilla) analogous: unverified, from memory.
- https://gdcvault.com/play/1027214 (2021)

**HBAO+ (NVIDIA, 2013) / ASSAO (Intel, 2016) [unverified, from memory]**: predecessors, superseded in accuracy by GTAO; deinterleave/denoise ideas only. https://github.com/NVIDIAGameWorks/HBAOPlus ; https://github.com/GameTechDev/ASSAO

**Large-scale occlusion combined with SSAO (industry practice, no single paper)**
- Unreal DFAO (2014-) then Lumen; Frostbite multiplies SSAO with baked AO; open-world titles use heightfield/voxel/baked canopy visibility. Rule: SSAO owns scale below ~1-2 m, a world structure owns larger scale, overlap avoided by an SSAO radius falloff.
- Maturity: this is how every open-world engine does it.

**Temporal stability of screen-space AO under TAA (practice, unverified as a survey; XeGTAO's README confirms the noise sequence and TAA-aware denoise)**
- One slice pair per pixel per frame with a per-frame-advancing sequence, temporal accumulation (reprojection with depth rejection) and a depth-aware spatial filter; half-res (cost /3-4, loses thin detail) is a common alternative. Letting TAA alone integrate the noise fights its neighbourhood clamp.

## 2. Best inference of what Chlorophyll does

Inferences, not facts. Baseline F5 states "hybrid screen-space + voxel GI, no ray tracing"; R3 lists the observed strong mid-scale occlusion.

1. A screen-space AO/SSGI term at contact scale (GTAO-class or a bitmask variant, since his scene is mostly thin foliage), temporally accumulated. Confidence: medium-high (~75%): "screen-space" in F5 almost certainly means this, and his frames show creases and base darkening.
2. A world-space voxel structure (the same voxels as his far tier) that supplies mid/large-scale occlusion and indirect light: darker forest floor, dark gaps between crowns. Confidence: medium (~65%).
3. Grass bottom-up darkening and clump variation in the blade material, not from SSAO. Confidence: medium (~60%), by analogy with Ghost of Tsushima/Horizon and because screen-space AO cannot reproduce occlusion inside a 20 cm dense mat at distance.
4. Specular occlusion present: low confidence (~35%), cannot be seen in the frames.

## 3. Recommendation for Meridian, ranked

Current ambient (from CLAUDE.md): sky irradiance LUT + cloud capture; plant micro occlusion (height gradient, `s.occlusion`, scaled by the Effects micro strength); crown sky visibility per vertex (ambient only); GI probes (off by default, coarse). The depth buffer is complete after the late scene pass; normals (attachment 5) exist only under Ray Reconstruction.

**R1 (do first): XeGTAO-style GTAO, half-res-capable, with bent normals and thin-occluder handling, multi-bounce fit, and GTSO.**
- Placement: after the late scene pass and the clouds composite, immediately before contact shadows (both read depth; contact shadows subtract sun radiance, AO must not touch it). Passes: depth prefilter mip chain (the existing depth pyramid is min-reduced R32F, reversed-Z; XeGTAO wants viewspace-linear depth mips with a weighted average, so a small separate prefilter is cleaner than reusing it), AO main, spatial denoise, apply.
- Normals: write the world normal (+ roughness) in attachment 5 always. Today it is null without RR and the writes are discarded. The cost is an RGBA16F write per pixel in the opaque lists (~0.1-0.2 ms bandwidth at 1440p, estimate) and the VkImage; alternative: reconstruct normals from depth (XeGTAO supports it) which is cheaper but wrong on alpha-tested foliage and at silhouettes. Recommend writing attachment 5 always; also gives DLSS RR parity.
- The apply problem (the main architectural risk): the engine shades in one forward pass, so ambient is already summed into HDR. Options: (a) write AO as a separate screen-space multiplier and apply to HDR in a compute pass, scaling only the ambient part. That needs ambient separated from sun in the colour, which it is not (contact shadows already use a separate "sun light" attachment 6 to subtract the sun; the same trick works: have mesh.frag write an "ambient radiance" attachment, then HDR -= ambient*(1-AO)). Costs one more RGBA16F target and its write. (b) Compute AO in a pre-pass from the early-pass depth, and sample it in the late-pass mesh.frag. Early depth is partial (late pass adds newly visible), so use last frame's AO reprojected, i.e. one-frame lag with TAA already integrating it. (a) is deterministic and mirrors the contact-shadow design; (b) saves the extra target but lags. Recommend (a), reusing the sun-light attachment pattern.
- Interaction: AO scales ambient diffuse and, via GTSO, sky specular. Micro occlusion and crown visibility already darken plants/crowns: reduce their strength or apply screen AO only as min() there, A/B with the micro slider. Radius 0.5-2 m so it does not fight the voxel/GI scale.
- Exposure: AO is a dimensionless multiplier on pre-exposed ambient, so no interaction; apply before the upscaler so TAA sees it; the meter (after bloom) sees the darker result.
- Thin grass: use the bitmask thickness idea or XeGTAO's thin-occluder compensation; exclude (or lower the strength for) masked/alpha-tested grass pixels, since the mat's occlusion comes from R3 below. Mark with a stencil bit or the material id in attachment 5 alpha.
- Cost at 2560x1440, RTX 4080: ~0.3-0.5 ms full-res High + denoise (+25% with bent normals), ~0.15-0.25 ms half-res; extrapolated from the XeGTAO README (unverified for this GPU); measure with the pass counter (`endPass("gtao")`).
- Temporal: 1 slice pair per pixel per frame with the Hilbert/R2 noise advancing per frame, then temporal accumulation (motion vectors exist, depth) and a 3x3 depth-aware blur. Gate with the existing shimmer/Laplacian tools on the meadow views.

**R2: bottom-up grass occlusion in the material, plus per-clump variation.** Cheapest, biggest effect for the mat (G4). The micro-occlusion term already has a height gradient; strengthen the response curve, add a clump-scale low-frequency darkening (world-position noise, wind-independent) and a terrain-contact term (blend toward ground AO). ~0 ms. This is also where thin-geometry AO belongs (Ghost of Tsushima practice).

**R3: world-space mid-scale occlusion: canopy/forest sky visibility on the ground.** Darkens the floor under and between trees, forest-edge bases: the thing SSAO cannot reach. Cheapest source: the forest mask (already shared by placement and material) turned into a low-res canopy-density map, sampled in the terrain shader for sky visibility; or, with GI probes on, the voxel march (already present in `gi_probes.comp`) is the principled version. Priority is above R1 for the "dark under trees" look in forest frames, below it for creases and rocks. Cost: ~0 ms (a texture read) for the density-map version.

**R4 (later):** visibility-bitmask variant if GTAO over-darkens thin foliage; same passes, different main shader.

Suggested order: always-write normals; R1 with apply path (a); R2 in the same commit series; R3 after; log decisions.md entry per CLAUDE.md before implementing (one entry: GTAO choice, apply path, thin-foliage policy, double-darkening rule).

## 4. Caveat

Unverified items are marked inline; replace with 4080 measurements before logging in decisions.md. Watch double-darkening with micro/crown terms and sub-pixel blade noise vs TAA clamp (shimmer gate).

## Sources

URLs: arxiv.org/abs/2301.11376 (fetched); github.com/GameTechDev/XeGTAO (fetched); iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf (fetched); seblagarde.wordpress.com/wp-content/uploads/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf (resolves via redirect, not read); gdcvault.com/play/1027214 (fetched); cdrinmatane.github.io/posts/ssaovb-code (search result); advances.realtimerendering.com/s2022/ (Lumen, not fetched); github.com/NVIDIAGameWorks/HBAOPlus, github.com/GameTechDev/ASSAO (not fetched).
