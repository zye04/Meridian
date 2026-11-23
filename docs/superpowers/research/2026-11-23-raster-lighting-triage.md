# Triage: pure-raster forest lighting deep research (2026-11-23)

Report: `2026-11-23-raster-lighting-deep-research.md` (Gemini deep research,
prompt beside it). Checked against our code and measurements; the report's
own VERIFIED / SECONDARY / INFERRED labels were not re-fetched here, so
every number below is a lead until its source is read.

| Report rank | Claim | Our state | Verdict | Goes to |
|---|---|---|---|---|
| 1a | Evaluate leaf back-face ambient with probe GI along -N | Already done: mesh.frag samples `giSample(-N)` for `backIrradiance` | Not the cause of black backlit leaves under GI; the probes inside a crown really are dark | G7 sub-issue (leaves black under GI), G13 |
| 1b | Crown multiple scattering: sun through the crown's optical depth even where the shadow map says shadowed | Crown visibility is ambient-only; sun transmission stops at the shadow map | **New, worth an A/B**: CE states it does this with its per-asset voxel volume (baseline F21); our prototype voxels hold the optical depth | G7 sub-issue, G15 leaf material |
| 2 | Canopy sky-visibility map from the voxel far field for terrain and trunks | Backlog "Sky occlusion" row proposed exactly this; the 2026-11-21 try failed partly because exposure rose 1.2 EV, which the 3 EV floor now prevents | **Do**: confirms our plan | G7 sub-issue (floor ambient without GI) |
| 3 | Local exposure (exposure fusion, or bilateral + Gaussian as in Ghost of Tsushima / UE5) | None | **Do**: research the two, pick by halos and motion stability | G7 sub-issue (local exposure) |
| 3 | UE "Low Percent 70-80%" metering | Unverified; UE's own defaults are 10 / 90 as far as we know | Ignore until read in the source | - |
| 4 | Bend-style deterministic screen-space shadows, depth-scaled thickness | contact_shadows.comp jitters the march per frame (IGN / PCG3D) | Relevant to G5 (weak, unstable contact shadows) | G5 |
| 4 | Grass base darkening with min(screenAO, baseAO) | Plant micro occlusion exists (height gradient) | Tune in G5, not new | G5 |
| 5 | Visibility bitmask AO + SSGI instead of GTAO | GTAO spec waiting for user decisions; neon triage #10 already leaned bitmask | Confirms the lean | #2 GTAO spec |
| 5 | Lagarde specular occlusion | Already in lighting.glsl (`specularAmbientOcclusion`) | Done | - |
| 6 | Contact-hardening sun shadows (PCSS in cascades 0-1) | 3x3 tent PCF, fixed width | New candidate after G5; leaf-shadow look is the leaf-geometry lane's first | backlog |
| 7 | EON diffuse on bark, soil, canopy | Lambert; CE uses EON on some materials (F20) | Cheap A/B, low priority | backlog |
| 8 | Sunfleck / shade 10-100x; closed beech floor 1.5-5% | Ours: ~7-10x without GI, ~36x with GI | Calibration targets, consistent with #7 | G7 doc |
