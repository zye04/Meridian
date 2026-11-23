# Triage: foliage light transport in probe + screen-space GI (2026-11-23)

Report: `2026-11-23-gi-foliage-deep-research.md` (Neon deep research,
prompt beside it). Checked against our code and measurements; the
report's labels were not re-fetched, so its numbers are leads until a
source is read.

| Report item | Claim | Our state (checked) | Verdict | Goes to |
|---|---|---|---|---|
| 1 | Leaf single-scattering albedo omega = rho + tau; tau >= rho in the visible (beech PAR rho 0.124, tau 0.247, Pilarski 2008) | GI now uses albedo x (1 + diffuseTransmission) (89bc219); beech diffuseTransmission 0.217 (0.35 x 0.62, cook mapping of Unreal's translucency), i.e. tau/rho 0.22 against a measured ~2 | **Real gap, two parts.** (a) Engine: mesh.frag lights a leaf's front with the full base colour and its back with base colour x factor, while `KHR_materials_diffuse_transmission` defines the factor as the transmitted fraction of diffuse light: front (1 - f) base colour, back f x transmission colour. Engine-stands-alone: follow the standard. (b) Cook: Unreal's translucency mapping is not a measured transmission; calibrate it in the cook against the measured tau/rho | (a) new issue, engine; (b) materials spec (G15 leaf material) |
| 1 | Recollision multi-scatter omega / (1 - p omega), p ~ 1 - hemispherical escape | gi_probes.comp: single scatter with a 0.5-weight heuristic | Do after the per-crown sun term (needs T_hemi) | #30 |
| 1, 3 | Per-asset opacity volume: intra-crown sun optical depth + sky escape | Prototype voxels exist (protovoxel.h, the voxel trees' fine grids) but the probes march only the world far field | **Do**: replaces `leafSun`, the #30 lead | #30 |
| 2 | Crown visibility x probes double counts one band | Fixed (3b6b900): crown term only with GI off | Done | - |
| 2 | Jimenez multi-bounce AO fit | `multiBounceOcclusion` (lighting.glsl) is exactly this fit; Lagarde specular occlusion too | Done | - |
| 2, 3 | Screen AO/GI radius below the finest probe spacing; sector-masked probe fill (each direction gets probe or screen light, never both) | Bitmask pass designed (gi-design spec), not built | **Adopt in the plan**: radius <= 1.5 m, 4-sector probe masking, depth-scaled or material-aware thickness | bitmask plan |
| 3 | Spatial-first filtering, little temporal history under DLAA with swaying leaves | Matches the user's stability-over-sharpness rule | Adopt | bitmask plan |
| 3 | Cost ~1.1 ms full-res 1080p RTX 2080 (paper); 1440p 4080 half-res 0.4-1.5 ms estimated | - | Budget ~1 ms, measure | bitmask plan |
| 4 | Backface zeroing kills foliage probes | We have no backface rule | N/A | - |
| 4 | Slice 0 inside the heightfield on slopes | `giSliceHeight(level, 0) = 0`: slice 0 sits exactly on the terrain under its column centre | Check on a slope view (rays from below the surface) | backlog |
| 4 | Probe inside its own foliage voxel attenuates every ray from t = 0 | Probe rays and canopySun start at distance 0 | Plausible darkening at the edge; A/B a 0.5-1 m start | backlog / #30 |
| 4 | SH L1 negative ringing | `giIrradianceChannel` clamps at 0 | Done (clamp); ringing reduction not needed yet | - |
| 5 | Shade colour targets (Chiao 2000): closed forest shade ~(0.83, 1, 0.62) linear RGB normalized to G, woodland shade (sky through gaps) ~(0.64, 1, 1.65) | Edge trunk with GI now 0.78 1.06 1.06 rgb/Y, i.e. (0.74, 1, 1.0) normalized: between the two, consistent with an edge view | **Use as the colour gate** for GI views (interior should lean toward forest shade, edge toward woodland shade) | GI look checks |
| 5 | Closed beech floor 1-3% of open PAR; sunflecks 10-100x shade | Interior floor with GI 1.5-2.7% of sunflecks | Inside the target | - |
| 5 | Beech leaf atlas (Y 0.041) 2-3x darker than measured leaf rho (PAR 0.124) | Texture path verified exact; the atlas is the asset | Not an engine bug. Part is definition: a scan's diffuse albedo excludes the ~0.04-0.05 cuticle specular an integrating sphere includes, so diffuse rho ~0.07-0.08; still ~1.7x. Flag in the audit ranges, decide in the materials lane (cook-side correction vs keep the asset) | materials spec, audit ranges |
| 5 | Beech bark PAR 0.10-0.15 (Pilarski) | Our tiling bark 0.117 | Inside | - |
| 6 | Validator: false colour outside ranges, albedo override, lighting-only view | Albedo capture + audit script; no in-engine view | Later: lighting-only (neutral albedo) and material-ID views as debug modes | backlog |

Audit ranges (tools/albedo-audit.py) updated from the report's PAR values:
leaf 0.03-0.15 stays (diffuse, no cuticle), bark 0.05-0.30 stays, grass
diffuse 0.05-0.18 (Lagarde's 0.25 is broadband with specular).

Order unchanged: GI default (done today), bitmask pass plan (now with the
radius, sector masking and filtering above), then G5. The KHR transmission
fix is small and engine-side; it goes before the bitmask pass because it
changes leaf brightness everywhere (front energy) and the bitmask tuning
would otherwise be done on the wrong leaves.
