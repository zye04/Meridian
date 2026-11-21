# Materials and texture fidelity (Pillar 1: G6, G12)

Status: draft for user review
Date: 2026-11-21
Programme: `2026-11-21-chlorophyll-parity-programme-design.md` (Pillar 1)
Audit: `2026-11-21-look-gap-audit.md` (G6, G12). Issues #6, #12.

## Intent

A 4M-polygon tree must look like a photograph up close and far. Geometry
is already there; what reads as "not a photo" is the material: wrong
channels, a fixed specular, scans cooked down, and detail that turns to
flatness or sparkle with distance. Pillar 1 fixes the inputs once, so
Pillar 2 (foliage model, GTAO) and Pillar 3 (voxel albedo) are tuned on
correct materials. Estimates below are marked (est.). CE evidence: virtual
textures on its terrain (F18); BRDF Lambert + GGX, EON for some
materials, grass and canopy fitted to measured data (F20).

## 1. Import standard per asset class

Common rules: glTF metallic 0 for every organic; normals OpenGL (green
up), verified by the curl test already in assets.md; roughness in G,
occlusion in R of one packed map where the source has AO; every
non-default decision recorded in `docs/assets.md` in the commit that makes
it.

| Class | Route (current -> standard) |
|---|---|
| Trees (Unreal export) | Mask: R opacity, G roughness, B translucency. Done: base alpha, roughness, metallic 0, mean-B transmission. To add: translucency as a map (thickness for Pillar 2's model, packed with AO), per-material specular, blend reproduction (below). |
| Plants (Megaplants USD, `*_ue_high`, atlases) | Same outputs as trees; translucency map where the pack has one, else a per-prototype constant (research C). Root-to-tip AO from `Prototype` height stays shader-side. |
| Ground surfaces | B/N 4K, ARM+H 2K (current). ARM stays (AO, roughness, metal 0 forced). Height only feeds parallax. |
| Rocks / cliff | ORM from the source; metallic forced 0 unless the scan states otherwise; AO via `occlusionStrength` (already). |

**Layer blends (Megascans MA_Foliage).** The engine has one material per
glTF primitive and the cook drops `COLOR_0`. Reproduction, cheapest first:
(a) per-primitive materials already separate Trunk / Tileable / Decoration
(done, UV ranges verified); (b) the trunk-base to bark dithered blend over
1.0-1.5 m (COLOR_0.A ~225): keep COLOR_0.A as a vertex attribute for those
primitives and blend base-colour/normal toward the bark layer in
`mesh.frag` with the same stable dither as the voxel coverage; (c) the
Decoration cards (lichen, knots) are alpha cards, already masks. Option
(b) needs a second texture slot per material: decision for the user,
default is a trunk-base-only extension (`blendTexture` + `blendRange`), not
a general layer system (est. ~1 day).

**Specular factor.** Hypothesis (open): fixed F0 0.04 against MI specular
0.2-0.3 (Unreal F0 0.016-0.024, about half) gives the silvery sheen.
Add `specularFactor` to `MaterialData` (KHR_materials_specular's factor
only; default 1) scaling the dielectric F0 and the specular DFG term;
cook writes the extension from the MI parameter (inventory.json).
**A/B before committing the cook change:** same alder crown, sun-lit,
specularFactor 1 vs 0.5 (a one-line override via `--effects`-style flag
or a temporary constant), judged on the bench views and region luminance
(`tools/region-luminance.py`) on the sheen highlights. If no visible
change, drop it and record the null result in decisions.md (as the
metallic A/B).

## 2. Close-up fidelity

Current caps: leaf atlases 4096 (`cook_ready.py --leaf-size`), bark and
decoration 2048 (`--bark-size`), ground 4K colour/normal and 2K ARM+H;
sources are 8K. Sampler: trilinear, 16x anisotropy, maxLod unclamped
(`renderer_setup.cpp` ~811), so anisotropic filtering is **already on
everywhere** except the terrain's `nearestSampler` (a deliberate
height-march sampler, not a colour path). Target: keep 16x; verify on the
bench at grazing angles, add nothing.

Transcoded VRAM, BC7-class, 1 byte/texel plus mips (est.): 2K 5.6 MB,
4K 22 MB, 8K 90 MB per map. One species today (4K leaf x3 maps, ~6 bark
sets x3 maps at 2K) is about 110 MB (est.); the three species ~330 MB.
Everything at 8K/4K would be ~450 MB per species, ~1.4 GB for three.

Options:
1. **Raise caps per class**: bark 2K -> 4K (the trunk is what the camera
   meets at 1 m): +~16 MB per bark map set x ~9 sets (est. +150 MB per
   species, mitigated by sharing across variants: Tileable bark is shared
   between variants already via `.ktx2-cache`). Leaf atlas stays 4K (a
   leaf card is ~10 cm at 4K atlas of ~50 leaves, ~40 texels/cm at 1 m:
   enough).
2. **Detail textures** (tiled micro-normal + albedo variation faded in
   under ~3 m on bark and ground): ~1 MB each, fixes the "melted at 30 cm"
   look at near-zero VRAM; shader cost one extra sample per layer
   (est. <0.1 ms).
3. **Virtual texturing / streaming**: solves the 8K-everywhere case, but
   is a multi-week system in a bindless forward/visibility renderer
   (feedback pass, page table, DLSS-neutral but TAA-jitter aware). Not
   justified until a measured VRAM or quality ceiling is hit.

**Recommendation:** 1 for bark only (4K) plus 2 for bark and ground;
defer 3 (backlog row with the trigger: bench shows texel density loss at
1 m after 1+2, or VRAM over the dev GPU's budget, which the planner measures).

## 3. Distance fidelity

- **Coverage-preserving alpha mips:** implemented (`ktx2.cpp`
  `coverageMips`, scale searched to the base coverage) for leaf and card
  textures. Status: done; gate it on the bench (alpha-tested area at 100 m
  within 5% of 1 m's).
- **Normal filtering into roughness:** at cook time, per mip compute the
  mean-normal length of the finer level's texels and widen roughness
  (Toksvig: `a' = a / (n_len)`-class; LEAN/CLEAN need a second moment
  texture, not worth the memory). Needs the normal and roughness maps
  cooked together (ARM/MR packing already pairs them), and
  `m_mip_renormalize` for normals stays. Runtime cost: zero.
- **Voxel bake:** `farfield_bake.comp` and `protovoxel` read albedo and
  normals through the material path; with the import standard in place
  (correct metallic, base-colour adjustments, specular) the baked albedo
  is correct by construction. Pillar 3 owns the algorithm; Pillar 1's
  deliverable is the bench's 100 m / 1 km columns showing the voxel tier
  against the same scan.

## 4. Material test bench

One matrix: classes (bark, leaf, grass, rock, ground) x distances 1 m /
10 m / 100 m / 1 km, each beside the source scan (the scan's own
render/photo: Quixel preview or Unreal capture, git-ignored under
`assets/reference/materials/`). Captured by a script
`tools/material-bench.sh BUILD OUT` that runs fixed `--camera` views with
a placed sample (a bench placement: one prototype, flat ground, fixed sun
and exposure via `--sun`, `--clouds off`, `--placement`-free) and writes
a contact sheet. Deterministic; same capture rules as the other gates.

**Gates** (numbers set from the first run, rule: judged beside the
scan): no validation errors; mean luminance and OKLCh stats within the
resemblance-metric band (commit c23bd60) of the scan at 1 m; alpha area
ratio 100 m/1 m within 5%; no shimmer regression (`image-gates.py
shimmer` on a 1 m/s strafe at 100 m); specular sheen A/B recorded.
Budget: no more than +0.3 ms GPU at the forest edge from this pillar
(est.), VRAM +300 MB over today for three species (est.).

## Risks

- Toksvig over-darkens specular on leaves if applied to the leaf atlas:
  apply to bark/ground/rock first.
- The specular A/B may be null; the sheen may then come from the sky
  specular fallback (#7 exposure) instead, not materials.
- The blend needs vertex data the cook drops: check the vertex layout
  and cluster pool cost (+4 B/vertex on trunk-base primitives only).

## Order of work

1. Bench script and first captures (no engine change): baseline.
2. Specular A/B; then `specularFactor` + cook, if confirmed.
3. Translucency map + AO packing in `cook_ready.py`/`tree.cpp` (before
   Pillar 2's foliage model).
4. Bark 4K cap and detail textures; anisotropy check at grazing.
5. Toksvig at cook time for bark/ground/rock; re-cook.
6. Trunk-base blend; seedlings stay skipped.
7. Docs: assets.md standard, decisions.md entries, backlog row for VT.

## Left to the planner

Exact gate numbers after the first bench run; the detail-texture assets
and tile sizes; the `MaterialData` field layout and the GLSL mirror; the
glTF extension handling in `model.cpp`; the dev-GPU VRAM budget; task
split across lanes (steps 3 and 5 touch the cook, 2 and 4 the shader).
