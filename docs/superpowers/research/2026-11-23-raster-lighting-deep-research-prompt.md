You are doing deep technical research for a real-time rendering engineer. Output a sourced report, not marketing.

## Goal
Find the techniques, with primary sources, that make a PURE RASTER (no hardware ray tracing in the shipped look) open-world forest renderer look photoreal in daylight: lighting, shadows, ambient/specular occlusion, foliage light transport, exposure and tonemapping. Target look: the "Chlorophyll Engine" indie devlog (YouTube VC_NUxbeA0E, author Toni Macaroni, @ToniMacaroniy on X).

## What is known about the target engine (author's own statements, Oct-Nov 2026)
- All raster, not path traced. No hardware ray tracing anywhere; min spec is mesh-shader GPUs (RTX 2060 / RX 6400).
- GPU-driven, cluster LOD (meshlets, modified meshoptimizer DAG); small triangles become voxels built during the LOD build; no impostors; far tiles are per-tile voxel aggregates without per-instance data.
- Visibility-buffer deferred path, shading in compute.
- GI: hybrid screen-space + voxel GI. A per-asset voxel opacity volume (built for GI) gives the optical depth of light through each tree crown plus light scattered between leaves.
- Shadows: semi-static, virtual-shadow-map-like cache plus a small dynamic map around the player for animated trees; far animated trees get contact shadows only; grass and small ground cover are left out of the shadow map and rely on screen-space contact shadows.
- BRDF: Lambert + GGX, EON (energy-preserving Oren-Nayar) for some materials; models fitted to measured data for grass and canopy. Tonemap: AgX with a contrast control. Exposure scheme: unknown.
- 1440p on an RTX 3080: ~130 fps average, ~80 in dense forest. 2.5D art-directed clouds.

## Our engine (context, do not research it)
Vulkan 1.3, mesh shaders, cluster LOD DAG, voxel far field, cascaded shadow maps (cached), screen-space contact shadows, a probe GI clipmap traced against a heightfield and far-field voxels, sky-view LUT + sky capture ambient, AgX + 3D LUT grade, histogram auto-exposure, DLSS DLAA. Measured problems: under a forest canopy the ground gets far too much sky ambient (10-14% of open light vs 1.5-5% measured in real beech forests) unless probe GI is on; mean-to-grey metering lifts forests back to mid-grey; with a physically correct 3 EV exposure cap, tree trunks read very dark; backlit leaves go near-black with GI on; contact and micro shadows look too weak.

## Questions (answer each with techniques, numbers and sources)
1. Local exposure / local tonemapping for games (2023-2026): Unreal 5 Local Exposure (bilateral grid), exposure fusion, Lottes, others. Cost at 1440p, artefacts (halos, motion), how shipped titles tune them. How do they keep forest shade readable without lifting global exposure?
2. Auto-exposure metering rules used in shipped open-world games: percentile windows, center weighting, sky exclusion, incident-light (illuminance-based) metering, per-biome or authored EV clamps. Any published numbers for forest interiors.
3. Canopy occlusion of sky light WITHOUT ray tracing: for the ground (forest floor) and for trunks and leaves inside crowns. Voxel/opacity-volume sky visibility, horizon-based or cone-traced occlusion, low-frequency SH/probe occlusion, "bent cone" methods, canopy density maps. What did Horizon Forbidden West, Ghost of Tsushima, Red Dead Redemption 2, Alan Wake 2, Avatar: Frontiers of Pandora, Unreal (Lumen software path, distance-field AO) do for forests?
4. Foliage light transport: leaf transmission (diffuse/specular), two-sided BRDF, multiple scattering inside a crown, measured leaf data (PROSPECT, LOPEX), how to avoid black backlit leaves, how ambient should reach leaf back faces.
5. Screen-space + voxel GI hybrids at real-time budgets (2-3 ms at 1440p): SSGI, visibility bitmask AO/GI (Therrien et al. 2023), GTAO, radiance cascades in screen space, voxel cone tracing variants. Which give stable, non-noisy results under foliage motion?
6. Contact shadows and micro-occlusion for vegetation: screen-space shadow length/fade, thickness heuristics, Days Gone / Bend screen-space shadows, specular occlusion from AO, grass base darkening.
7. Shadow maps for forests: virtual shadow maps vs cached cascades, filtering for leaf shadows (PCSS, moment shadow maps), leaf shadow softness vs sun angular size.
8. Calibration data: measured luminance or irradiance ratios (forest floor vs open, sunfleck vs shade, sky vs sunlit foliage), real photographs' OKLab/CIE L distributions for forests, anything usable as a numeric target.

## Rules
- Prefer primary sources: papers, SIGGRAPH/GDC talks with slides, engine docs, shipped code. Give a link for every claim.
- Mark each claim: VERIFIED (you read the source), SECONDARY (press/forum), or INFERRED (your reasoning).
- Give costs in ms with the GPU and resolution when sources state them.
- Say plainly when something is unknown or disputed. Do not invent numbers.
- End with a ranked list: technique, which question it answers, expected visual gain, cost, risk, sources.
