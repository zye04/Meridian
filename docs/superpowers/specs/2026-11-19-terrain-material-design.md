# Terrain material — design

## Intent

Replace the flat slope/height colors in `proceduralTerrain()` (mesh.frag)
with photoscanned Poly Haven ground textures, so the terrain reads as real
ground before phase 2 (Hi-Z occlusion) scatters assets over it. Hard
requirement from the user: textures stay razor sharp in motion, with no
smearing.

## Goals

- Flat ground: pine-needle litter and forest soil, mixed by a large-scale
  noise mask so there's no uniform carpet.
- Steep slopes: rock cliff texture, projected so cliffs don't stretch.
- Snow at altitude stays procedural (no snow texture yet).
- No visible tiling grid at any distance on the current 2 km terrain.
- Sharp: full-resolution detail up close, crisp layer transitions, no added
  blur when the camera moves.

## Non-goals

- Splat maps, painted layers, or terrain-editing tools.
- Terrain LOD, streaming, or the 15 km world (separate project).
- GPU grass, decals, parallax/displacement mapping.
- Mip bias tuning for upscaling (comes with DLSS).

## Anti-goals

- Any blur in motion that the old flat colors didn't have.
- Visible repetition or hex-cell seams.
- A large cost: target under 0.5 ms extra at 1080p-ish capture sizes.

## Approaches considered

1. **Hex-tiling (Mikkelsen 2022) + height blending (chosen).** Three
   samples per map, luminance-weighted blend with a contrast parameter that
   keeps detail sharp instead of averaging it to mush; works with normal
   maps (rotated per tile). Height maps make layer borders follow the
   texture (needles fill gaps between soil stones) instead of soft
   cross-fades.
2. **Heitz–Neyret histogram-preserving stochastic tiling.** Best contrast
   preservation, but needs a precomputed Gaussianization LUT per texture;
   more pipeline for little gain over 1.
3. **Two-scale blend + macro color variation only.** Cheapest, but the
   blend of two scales is itself a blur, and repetition still shows on
   large flat areas. Conflicts with the sharpness requirement.

## Design

- **Layers:** `forest_ground_04` (soil), `forest_leaves_04` (needles) on
  flats; `rock_face_03` on slopes; snow procedural. Each layer has base
  color (sRGB), OpenGL normal, ARM (AO/roughness/metal), and height.
- **Projection:** world XZ for the flat layers (hex-tiled); triplanar for
  rock with Whiteout normal blending (Ben Golus). Layers with zero weight
  are not sampled.
- **Mip selection:** all hex-tile and triplanar samples use `textureGrad`
  with derivatives of the unrotated world UV (rotated with the tile), so
  there are no mip seams and anisotropic filtering (already 16x) stays
  effective.
- **Blending:** flats mix by low-frequency world noise, then sharpened by
  height; flat vs rock by slope, also height-sharpened.
- **Textures:** `meridian-cook --texture in.jpg out.ktx2 [--srgb|--normal]`
  cooks a single image (same UASTC/zstd/mips path as the glTF cook).
  `fetch-sample-assets.sh` also downloads `rock_face_03` and
  `brown_mud_leaves_01` (spare variant) and each layer's `_disp_2k.jpg`.
- **Engine side:** the terrain material gets its layer texture indices
  through `MaterialData` (or a terrain-specific struct reached from the
  scene buffer); the game loads the KTX2 files and falls back to the
  current procedural colors if they're missing.
- **Sharpness:** mip bias stays 0 (native-resolution TAA; negative bias
  shimmers without an upscaler). TAA already uses Catmull-Rom history and
  variance clipping.

## Verification

- Captures: default view, sample-model view (close-up ground), sunset.
  Check no tiling grid, no hex seams, crisp transitions.
- Motion check: new game option `--camera-velocity X Y Z` (m/s) moves the
  camera during a capture run's frames. Compare a moving capture with a
  still one of the same final view, and with the old terrain, for added
  blur. Reusable for every later "no smear" check.
- GPU ms against the previous capture at the same window size.
- 0 validation errors.

## Left to the builder

Tile sizes per layer (start from texture real-world size, ~2-4 m), noise
frequency, blend contrast values, whether height goes in its own KTX2 or
packs into the ARM metal channel (metal is 0 for terrain).

## Sources

- Mikkelsen, Practical Real-Time Hex-Tiling, JCGT 2022:
  https://jcgt.org/published/0011/03/05/paper.pdf
- Heitz & Neyret / Deliot, stochastic texturing:
  https://medium.com/@jasonbooth_86226/stochastic-texturing-3c2e58d76a14
