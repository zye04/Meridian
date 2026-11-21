# A. Far vegetation as voxels/aggregates (G1, G10, G11)

Status: research, 2026-11-21. No code changed. Verification (rechecked
online): "[V]" = page fetched and read; "[S]" = title/authors/year
confirmed by search results only (publisher page blocked or not fetched);
"unverified" = not confirmed, do not rely on it. Cost figures: those
attributed to decisions.md are measured locally; all others marked
"estimate" are mine, not sourced.

## 1. Sources

**UE 5.7 Nanite Foliage / Voxel (2025) [V].** Solves sub-pixel foliage
triangles (cost, aliasing, lost leaves). The Nanite builder voxelizes
simplified clusters into at most 128 4x4x4 voxel bricks; it switches
triangles -> voxels when voxels give lower error than triangles. Opacity =
fraction of random rays that hit ([S], search snippet only). Each voxel stores a
*distribution of normals* (in vertex-colour alpha); at GBuffer write one
normal is chosen stochastically per pixel, and Epic plans to fold the
distribution into shading like roughness. Voxel clusters rasterize front
to back for early-Z. No ms figures; "generally better than triangles in
the distance". Shipping in 5.7; the Witcher 4 tech-demo use and its PS5 60 fps claim are
from press snippets, unverified. Docs say nothing about voxel shadow handling (VSM only).
https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage

**Nanite Assemblies (5.7; the fetched page is the 5.9 docs) [V].** Instanced
parts (branches, fronds) so a high-poly tree is small on disk/memory;
hierarchical LOD simplifies far instances into single clusters. Doc
figures (Witcher 4 demo): one tree 3.5 GB -> 29 MB on disk, streaming
36 MB -> 2.7 MB. Solves memory/streaming, not the far look. https://dev.epicgames.com/documentation/unreal-engine/nanite-assemblies

**NVIDIA RTX Mega Geometry on Witcher 4 forest (2025), unverified.** Only
a search snippet ("~1M trees", voxel LOD, PS5 60 fps RT); the PC Gamer
page body could not be read. RT-focused; not used below.

**Deep Appearance Prefiltering (Bako, Sen, Kaplanyan, ACM TOG 2022) [V].** Learned
prefiltered appearance of complex assets across scales; shows the target
(filtered look, no noise) but is neural and per-asset; research only.
https://arxiv.org/pdf/2211.05932

**Loubet & Neyret, Hybrid mesh-volume LoDs for all-scale pre-filtering of complex 3D assets (Computer Graphics Forum 36(2), 2017) [S]** (publisher pages returned 403).
Per LOD level: macroscopic surfaces stay a simplified mesh with prefiltered
normal distribution and albedo; sub-resolution detail becomes a
heterogeneous microflake volume, voxelized with an artifact-free
algorithm that preserves local occlusion. Tested on trees. Closest
academic match to Nanite voxels and to "voxels in the LOD build".
Research-grade, no shipped use known. https://diglib.eg.org/handle/10.1111/cgf13138 (not fetchable here; found via search)

**SGGX microflake distribution (Heitz, Dupuy, Crassin, Dachsbacher,
SIGGRAPH 2015) [V].** Stores a microflake distribution as a
symmetric 3x3 (6 floats, or 3 quantized) per voxel; linearly
interpolable and prefilterable, analytic. Gives per-voxel
anisotropic normal spread + projected area per view direction: exactly
the missing "normal distribution + view-dependent opacity".
Mature as a representation; real-time uses are offline-to-game bakes.
https://research.nvidia.com/labs/rtr/publication/heitz2015sggx

**Heitz & Neyret, SVO prefiltering (HPG 2012, pp. 125-134, best paper) [S].** "Representing
appearance and pre-filtering subpixel data in sparse voxel octrees":
per-voxel normal distribution + colour + opacity, with correct
alpha/colour filtering through mips. Foundation of the above; ray-march
oriented. https://diglib.eg.org/handle/10.2312/EGGH.HPG12.125-134
(GigaVoxels, Crassin 2009: not checked, omitted.) Its attributes are
SDF + Gaussian microdetail descriptors, which I paraphrased above as
"normal distribution + colour + opacity": approximate.

**Aggregate G-Buffer AA (Crassin, McGuire, Fatahalian, Lefohn, I3D 2015)
[V].** Collapses sub-pixel foliage/thin geometry into 2
aggregate surfaces per pixel (mean normal, spread, coverage), shaded once.
~8x MSAA quality at 30% less memory, up to 1.3x faster. Useful idea:
shade the aggregate with the normal *spread* (shorten the mean normal /
raise roughness) instead of sampling one normal. Not adopted widely.
https://research.nvidia.com/labs/rtr/publication/crassin2015agaa

**Hashed alpha testing (Wyman & McGuire, I3D 2017) [S].** Stable
object-space hash thresholds for alpha-tested/stochastic transparency
that stays attached to the surface and converges under TAA. Our
voxel coverage hash is this idea; it is stable but, at 1-2 px per voxel,
variance is too high for TAA to integrate (G1 diagnosis). Works where
each hashed element covers many pixels. Mature.
https://i3dsymposium.github.io/2017/ (proceedings listing: pp. 7:1-7:9, [S]; the paper itself was not fetched, so "converges under TAA" is my reading)

**Horizon Forbidden West / Decima, "Adventures with Deferred Texturing"
(James McLaren, Guerrilla, 2022) [V].** Visibility-buffer deferred
texturing for foliage and alpha-tested geometry, compute shading, software
VRS. The fetched page says nothing about far trees. The claim that distant
trees become a height map is **unverified** (one search snippet only);
it is not used as precedent. https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west

**Ghost of Tsushima, "Procedural Grass" (Eric Wohllaib, Sucker Punch, GDC
2021) [V].** GPU-generated individual blades. The session page gives no
numbers and no far-LOD statement (a snippet gave 83k blades / 2.5 ms:
unverified, dropped). Grass only. https://gdcvault.com/play/1027214/contactUs

**Far Cry 5, AC series, MSFS 2020/2024, Witcher 3, Unity HDRP.**
Searches returned no vegetation-LOD talk with usable detail. Convention
in all: mesh LODs -> billboard/octahedral impostors, SpeedTree-style;
X-Plane does per-tree GPU LOD with a camera-facing billboard as last
LOD [V] (https://developer.x-plane.com/2021/08/next-generation-trees-and-opengl/,
2021). Meridian already rejected impostors (decisions.md).

**Far shadows.** No reputable far-vegetation shadow talk found beyond
the general approaches: cached/clipmap cascades over the view (ours),
Beer-transmittance shadow maps: Deep Shadow Maps (Lokovic & Veach,
SIGGRAPH 2000, https://graphics.pixar.com/library/DeepShadows) and Opacity
Shadow Maps (Kim & Neumann, EG Rendering Workshop 2001,
https://diglib.eg.org/items/86a53d3f-cb12-4e56-8fd6-391ae1b45cc5), both
[S]; our cloud shadow map is this idea, and
height/horizon maps for terrain (ours). Nanite/VSM covers foliage by
virtual shadow maps, with voxels as casters unspecified.

## 2. What Chlorophyll likely does (inference)

Update 2026-11-21: F3 is now first-hand (his YouTube reply: a modified
meshoptimizer and a custom DAG builder emit voxels in the LOD build).
New stated facts: triangles swap to voxels by screen error (F22); tile
proxies past ~1 km drop instance data and DAG traversal (F22); distant
grass uses the same voxel path (F23); the per-asset voxel opacity
volume gives crown optical depth and inter-leaf scattering (F21), which
bears on the crown-darkness bullet below (50% before, now stated for
lighting; the far-tier use is optional per his reply). The bullets
below are otherwise unchanged.

- Voxels are produced in the cluster-simplification step (as Nanite
  does) and carry *filtered* attributes: opacity, albedo, and normal or
  normal spread. Confidence 70% (F3 is second-hand; the frames show
  smooth crown shading that binary coverage cannot give).
- Voxels are drawn with continuous coverage at a larger voxel size than
  ours (a switch on error, not a fixed 1-2 px), likely 3-8 px, so each
  voxel is a visible shaded patch and the crown reads as a mass.
  Confidence 55%.
- Far tier: per-tile voxel hierarchy "traversed as one aggregate" (F4):
  probably raster of tile boxes + short front-to-back traversal, or a
  compute march, to accumulated opacity, not a 0.5 threshold. 50%.
- Crown darkness (R1/R3) comes from the voxel grid he already has for GI
  (F5): per-voxel density/AO and a sun-visibility term, not per-pixel
  rays. 50%.
- Far tree shadows (R2): either the same voxel structure traced toward
  the sun, or a shadow map with voxel casters. Unknown; 35% each.

## 3. Recommendations for Meridian (ranked)

All ms below marked "estimate" are my extrapolation, not measurements, at
2560x1440 on the 4080. Baselines are measured values read from decisions.md (voxel trees 3.7 ms and far field 2.0-2.7 ms at
the forest overview; far-shadow rebuild ~0.3 ms/frame while the sun
moves). Each needs a capture pass to confirm.

1. **Shade the voxel as a crown volume, deterministically (G1).** At
   bake time store per voxel a normal from the 3x3x3 opacity gradient plus
   a spread (length of the mean normal, Toksvig/SGGX-lite: 1 float), and
   drive the lighting with: ambient cube (already baked as 6-axis optical
   depths), sun visibility from the baked depth toward the sun (already),
   wrap/transmission using spread. Cost: bake time and 1 byte/voxel; shading
   +0.1-0.2 ms. Rejected: Nanite's per-pixel stochastic normal pick; noisy
   and our TAA is the limiter (decisions.md already says deterministic).
   This is the cheapest change with the biggest crown-form gain.
2. **Replace binary hash coverage by filtered coverage with larger
   voxels (G1, G10).** Raise `kVoxelMipPixels` from 2 to 3-4 px for the
   mid tier (voxel count falls 2-4x; the pass is count-bound, so this
   likely nets -1 to -2 ms) and composite with continuous opacity: depth-
   bucketed front-to-back (Nanite) into an RGBA16F accumulation target,
   resolved against scene depth, or at minimum blue-noise dither with
   per-pixel (not per-voxel) thresholds so TAA integrates it. Accumulation
   +0.3-0.6 ms. Risk: edge/depth correctness against the cluster
   pass and the 10% switch band dither; check popping with the motion
   gates. Hashed-alpha logic (Wyman 2017) says per-voxel hash is only
   valid when elements cover many pixels, which is why raising voxel size
   is the lever.
3. **Far field: accumulate, do not threshold (G1).** Replace "first
   0.5-opacity hit" in voxel.frag by 2-4 front-to-back samples at L1/L2
   with accumulated alpha and the same shading as (1); keep the 0.5 hit
   only for the depth write. +0.3-1.0 ms of the current 2-2.7 ms
   (ridge 0.94 ms). Gives smooth far crown masses with dark gaps
   because partial opacity composites.
4. **Far canopy transmittance shadow map (G11).** The depth-of-first-
   0.5-hit far shadow map is binary and its bias (normal offset, 2 m +
   slope) plausibly erases ground shadows of 15-25 m trees at an 8 m
   texel; decisions.md notes no tree shadows show on the overview ground
   with any caster. Build the map as Beer transmittance (sum of
   extinction along the sun through the L1 bricks), blurred 5x5 like
   the cloud shadow map, and multiply at receivers beyond the last
   cascade that carries trees; sample 1 tap. Rebuild cost ~0.3 ms/frame
   while the sun moves, ~0 static; 15 MiB (16-bit). First debug the
   receiver path (are the ground fragments reading the far map at all at
   1-4 km?) before changing the format. Alternative: add canopy height
   to the terrain shadow-height pyramid (a height-map approach; Horizon precedent unverified):
   cheapest, hard edges, no translucency.
5. **Bake voxels from the leaf materials, not the palette (G1/G6).** Part
   of the grey is wrong albedo (audit); fix with the G6 import before
   judging 1-3.
6. **Do not** pursue: impostors/billboards (rejected), per-pixel
   stochastic normals, neural prefiltering (Bako 2022), full SGGX volume
   with microflake tracing (RT-oriented; the 6-float SGGX is overkill
   for 4 m voxels, but its projected-area idea is already in the
   `1 - exp(-sigma v / S)` quad).

Order: 5 -> 1 -> 2 -> 3 -> 4 (4's debug can run first since it is
independent). Net expected effect on G10: items 1-3 add ~0.5-1.5 ms but 2's
bigger voxels remove 1-2 ms; moving the switch outward (voxels instead
of sub-pixel clusters: the ~3 ms shading cost in G10) is the main lever,
and it is only safe once 1-2 make voxels look right.

## 4. Open questions
- Does the far map's depth compare fail on ground receivers (bias, sample
  position, or fade) or is nothing cast? A one-frame debug view settles it.
- Nanite's voxel brick LOD error rule vs our fixed pixel rule: worth
  checking Epic's source (5.7 `NaniteBuilder`) for the exact switch.
- Horizon's far-tree height map: confirm at the source before using it
  as precedent.
