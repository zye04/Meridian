# Look-gap audit: Meridian vs the Chlorophyll Engine (2026-11-21)

Status: open. This is the source list for the next lighting/look work. Each
gap below gets a spec or a lane when it is picked up; mark it there and
here. Nothing on this list is to be dropped silently: resolve it, or move it
to `docs/backlog.md` with a reason.

## Target

The Chlorophyll Engine (Toni Macaroni): all raster, native 1440p, no
upscaler, ~130 fps average on an RTX 3080. Look target = the frames in
`assets/reference/chlorophyll/*.png` and `assets/reference/target-meadow.png`
(git-ignored; re-create from the video). The user's verdict on 2026-11-21
after playing the current build: "the Chlorophyll Engine is doing something
noticeably better in lighting, clarity, voxelization, texture shadowing and
lighting ... the look we are going for is the photorealism and quality that
the Chlorophyll Engine managed under raster".

Evidence images are in `docs/images/look-gap-2026-11-21/` (ours only; the
reference frames are not committed). `user-*` are the user's in-game
screenshots of the `ue-trees` worktree build (main + the first Black Alder
import), `lane-*` are lane captures from the same day.

## Gaps, ranked by visual impact

### G1. Distant forests read as grey speckle, not tree masses
- Evidence: `user-03-sunset-ridge-voxel-speckle.jpg`,
  `user-05-valley-voxel-speckle-flat-terrain.jpg`.
- Seen: past the voxel switch (2 px, `--voxel-trees on`) every hillside
  forest is a salt-and-pepper field of grey/dark dots; no crown shapes, no
  shading across a crown, no depth between crowns. His aerials show soft,
  shaded crown masses with dark gaps.
- Diagnosis: `voxel_instance.frag` draws each voxel as a flat-shaded
  camera-facing quad with binary coverage (stable hash vs opacity). At 1-2
  px per voxel that is unfiltered binary noise TAA can't integrate; flat
  shading gives no crown-scale form. The far-field bricks (`voxel.frag`)
  march to opacity 0.5, also binary. Part of the grey is G6 (metallic
  leaves baked into the voxel albedo).
- Direction: prefiltered coverage (alpha / opacity accumulated per
  pixel instead of a binary hash, or alpha-to-coverage), a crown-scale
  normal (density gradient or the prototype's crown ellipsoid) and depth
  darkening inside the crown (the crown sky visibility already in
  mesh.mesh), voxel albedo from the fixed leaf materials. His engine
  generates the voxels inside the meshoptimizer LOD build: read the
  decisions.md entry before re-deciding.
- Gates: aerial frames (forest overview `700 1450 -400 -1.55 -0.5 --trees`,
  high overview) beside `aerial-*.png`; shimmer and Laplacian in motion;
  the far tier's GPU ms.

### G2. Flat "default raster" ambient: no ambient occlusion at the mid scale
- Evidence: `user-04-meadow-bland-grass-flat-ambient.jpg`,
  `user-05-...jpg`, `user-02-...jpg`.
- Seen: ground, grass mat, rocks and trunks all receive the same sky light;
  no darkening in creases, under objects, at grass bases across the mat,
  between trees. His frames have rich, layered occlusion.
- Diagnosis: Meridian has micro shadows (per-plant base darkening), contact
  shadows (sun only), the crown sky visibility (tree crowns only) and GI
  probes (off by default, coarse). There is no screen-space AO.
- Direction: GTAO (ground-truth ambient occlusion) with bent normals and a
  multi-bounce fit, plus specular occlusion; temporally stable under native
  TAA. Research 2026 practice (XeGTAO, its successors) and log it.
  Budget ~0.5-1 ms at 1440p.
- Gates: the meadow start view and the forest edge beside
  `target-meadow.png`; still and motion shimmer.

### G3. Foliage doesn't glow when backlit (no foliage subsurface)
- Evidence: `user-03-...jpg`, `user-04-...jpg` (grass against a low sun).
- Seen: leaves and grass facing away from the sun stay dull; his grass and
  crowns light up against the sun.
- Diagnosis: `diffuseTransmission` (lighting.glsl) is a weak, untinted
  transmission factor; no thickness map, no view-dependent forward
  scatter. The Megascans masks carry a translucency/thickness channel we
  don't use yet (backlog "Leaf translucency map").
- Direction: a two-sided foliage shading model: transmission tinted by
  albedo and scaled by the thickness/translucency map, a forward-scatter
  lobe toward the sun, wrap diffuse for thin geometry.
- Gates: backlit views (sun ahead and low: the sunset view, a meadow view
  toward the sun) beside the references; cost in mesh.frag.

### G4. Grass looks bland
- Evidence: `user-04-...jpg`.
- Seen: uniform blades and colour, little variation per clump, no depth
  darkening inside the mat, washed-out pale tufts in patches.
- Direction: mostly G2 + G3 (AO in the mat, translucency), then shading:
  per-clump colour/brightness variation, specular sheen on blades, the mat's
  own bottom-up occlusion. Grass assets are phase 2 content; the shading is
  engine work now.

### G5. Contact shadows and micro shadows are weak
- Evidence: user report (2026-11-21); `lane-raster-vs-rt-leaf-shadow.jpg`
  (raster leaf shadow on the ground, top, vs rt, bottom).
- Diagnosis: contact shadows are a binary 16-step march with per-pixel
  jitter (backlog "Contact shadows: soft penumbra, Bend-style wavefront
  march"); the jitter is ~1/4 of the still meadow shimmer (native lane).
  Raster sun shadows are soft and blobby under trees (cascade 1, ~10 cm
  texels), where a real 10 m crown gives a crisper dapple.
- Direction: Bend-style wavefront screen-space shadows; contact-hardening
  (PCSS-style) or a finer cascade 1 for the cascade shadows.

### G6. Imported Megascans tree materials are wrong (Black Alder, first import)
- Evidence: `user-01-alder-bark-metallic.jpg` (chrome/gold bark, wobbly
  relief), `user-02-alder-crown-grey-white-trunk-bases.jpg` (grey leaves,
  white trunk bases), `lane-alder-forest-edge-first-import.jpg`.
- Hypotheses (lane `ue-trees`, fixing now): the Megascans Mask is used as
  the glTF metallicRoughness texture, so its B channel lands in metallic;
  MA_Foliage blends Trunk / Tileable / Decoration by vertex colour and we
  ignore it (white bases); normal strength / green flip / normal-map
  encoding (melted relief). MA_Foliage also boosts base colour
  ("LightInfluence BaseColor Strength" 3) and adds colour variation.
- Gates: close-up trunk and sunlit crown, before/after.

### G7. Overall image is grey and washed out in forests and at low sun
- Evidence: `user-02-...jpg` (whole frame grey-white), `user-03..05`.
- To check first: exposure metering inside forests, the haze change of
  2026-11-21 (fd02fe2), GI off/on, the grade file the user has saved
  (`~/.config/meridian/grade.json`, overrides the new starting grade in
  interactive runs). Not yet diagnosed.

### G8. Terrain colour: beige/grey/pink patches
- Evidence: `user-05-...jpg`, `lane-aerial-haze-old-new-clean.jpg`.
- Placeholder terrain material (phase 2, Houdini). It still dominates wide
  shots; his clearings are bright lime meadow. Engine-side candidate:
  macro colour variation and vegetation-driven ground tint, if phase 2 is
  far off.

### G9. Motion shimmer still above DLSS
- Native TAA, current main: walk 15.9, turn 10.4 /255 vs DLSS 10.1, 6.1
  (`meridian-native/.superpowers/native/next.md`). The 3x3 reconstruction
  WIP (branch `native`, 36c37a8) cuts 25% but softens 20%.

### G10. Forest-edge frame cost
- 13.2 ms with `--trees` vs a 10 ms target; ~3 ms is shading sub-pixel tree
  triangles (visibility buffer = VG slice 4), 1.5 ms the voxel brick
  listing.

### G11. Tree shadows missing at distance with raster shadows
- Forest overview shows no tree shadows on the ground; fix WIP on branch
  `far-m-shadow2` (needs Debug, docs and the raster lane's agreement).

### G12. Material fidelity up close and at distance
- Evidence: `user-01-alder-bark-metallic.jpg` (bark relief melted, low
  detail up close); user (2026-11-21): a 4M-polygon tree "has to look
  almost like an actual photorealistic picture".
- Seen: materials lose their photographic quality up close (resolution,
  relief) and at distance (detail turns into flatness or sparkle).
- Diagnosis: scans cooked down from 8K to 2-4K; no detail textures or
  texture streaming; normal-map detail is not filtered into roughness at
  distance; no per-asset material test bench.
- Direction: programme Pillar 1 (`2026-11-21-chlorophyll-parity-programme-design.md`):
  import standard, close-up resolution (detail textures / virtual
  texturing), anisotropic filtering, Toksvig/LEAN-class normal filtering,
  coverage-preserving mips, a test bench at 1 m / 10 m / 100 m / 1 km.

### G13. Leaf back faces render black
- Evidence: `user-06-lone-tree-giant-cards-black-backfaces.jpg`.
- Seen: leaves seen from behind (sun on the other side) are near black,
  front faces olive-yellow; the crown reads as a black-and-yellow mosaic.
- Likely: two-sided leaves shade the back face with the front normal (no
  flip), and transmission is too weak to light it (G3). Engine bug, high
  impact: fix with the foliage shading model (G3) or before it.

### G14. Some Megascans tree variants show giant leaf cards
- Evidence: `user-06-...jpg` (lone tree), left tree in
  `user-08-trees-mid-distance.jpg`.
- Seen: single leaves ~0.3-0.5 m on one tree kind (lone/Field variants of
  beech or hornbeam), unlike the small leaves of the others.
- Likely: wrong material/atlas region on those variants (a single-leaf
  texture where a cluster card belongs), or a scale/UV issue in
  `cook_ready.py`. Pipeline (#6 / G6).

### G15. Leaf textures posterized and glossy up close
- Evidence: `user-07-leaf-closeup-posterized-glossy.jpg`.
- Seen: blotchy banded colour and blue sky reflections on leaves at 0.5 m,
  a wet-plastic sheen.
- Likely: roughness read from the wrong channel or inverted (gloss vs
  roughness), the fixed F0 0.04 (materials spec A/B), and texture
  compression quality (KTX2 encode settings). Pillar 1 (#12).

### G16. Clouds blocky / pixel-stepped at distance
- Evidence: `user-09-blocky-clouds-cliff-speckle.jpg`,
  `user-11-aerial-terrain-banding-blocky-clouds.jpg`.
- Seen: distant cumulus with stair-stepped edges (quarter-res texels
  visible); near clouds fine (`user-10-...jpg`).
- Likely: the cloud outline effect (a5d0c4a, default strength 1)
  sharpening quarter-resolution texels at distance, or the bilateral
  upsample. Check `--effects cloud-outline=0` first.

### G17. Terrain slopes show banding / terracing in aerial views
- Evidence: `user-11-...jpg` (diagonal dark stripes and terraces on lit
  slopes).
- Likely: the terrain shadow-height map (aaab45b, 4 m texels) banding,
  or heightfield quantization in the normals. A/B against
  `-DMERIDIAN_TERRAIN_MARCH_BRUTE_FORCE=ON` / a build before aaab45b.

Also confirmed by these screenshots: G1 (far forest blue-grey speckle,
`user-09`), G7 (washed distant terrain and pale sky, `user-11`), the
known cirrus straight-line streaks (`user-10`, backlog row "Cloud look
gaps").

## Order proposed for the next session

1. G6 (in flight) and G7's diagnosis: cheap, they distort every judgment.
2. G2 GTAO: contained, biggest single lift.
3. G3 + G4 foliage shading (shared shading model).
4. G1 voxel crowns (+ G11 shadows).
5. G5 shadows (wavefront contact shadows, contact hardening).
6. G9, G10 as the perf/clarity track.

Every item: research current practice first (decisions.md), side-by-side
with the reference frames, ms at 2560x1440, motion gates.

## What landed on 2026-11-21 (context)

Native 1440p TAA default (DLSS A/B only); raster sun shadows default;
motion-adaptive sharpen, still-weight 1/16, Keys cubic history; crown sky
visibility (leaves, bark); exposure highlight cap; starting grade; haze as
a valley boundary layer (clean summer); clouds stable (p99 flicker 0.46 /
0.49 vs 1.33 / 1.86) + outline effect; masked depth prepass; terrain
shadow-height map; grass mat twice as dense; voxel trees (rasterized
prototype voxels, 2 px switch, coverage-keeping mips, far cells in the
coarsest cascade); GI radiance cache + forest fixes (still off by
default); textured beech from scans; Fab + Bridge plugins for Linux UE
5.9 and a headless export path for Megascans Trees (`ue-trees` lane).
