# GI probes: foliage radiance lit where the ray meets it

Status: implemented (2026-11-23), decisions.md "GI probes: foliage lit where the ray meets it"

## Why

The reference path tracer measured the forest edge's shaded trunk at 2.1x
the reference's luminance and too green (B/G 0.61 vs 0.84) under the GI
probes. A term-ablation spike (artifacts in `.superpowers/spike-gi-canopy/`)
attributed it:

- Foliage radiance zeroed in the probes: trunk 0.87x the reference, so the
  probes' occlusion of sky and ground is about right.
- The foliage radiance itself (`gi_probes.comp`: `albedo (1 + t) / pi *
  (sun 0.5 leafSun sun.y + 0.5 sky)`) is ~10x too bright: it lights every
  leaf a probe ray meets as an outer, sunlit, sky-exposed leaf, while the
  leaves seen from below are the shaded underside layers. Its sky half
  alone is 4.6x too bright, so a better `leafSun` alone cannot fix it.
- Lighting each foliage hit by its own sun and sky transmittance (two
  marches in the world far-field voxels from the ray's 0.5-opacity point)
  took every front-view grid cell from 0.70-1.05 to 0.90-1.14 of the
  reference (trunk 0.47 -> 1.08, B/G 0.96). Albedo split and a 1 m ray
  start changed nothing. Cost ~+0.6 ms `gi probes` (one capture).

The backlit canopy's 1.8x darkness is not the probes' (same ratio with
`--gi off`); it is the direct path's and gets its own design.

## Goal, non-goals

- Goal: probe foliage radiance from the light that actually reaches the
  hit, so the front, backlit and interior views match the reference.
- Non-goals: the direct-path backlit error; the bitmask pass (stays next);
  per-tree prototype voxels for the probes (the 4 m world voxels already
  land within ~10%).
- Anti-goals: constants tuned to this beech or this view (engine stays
  asset-agnostic); a GI cost that eats the 180 fps budget.

## Approaches (user picked A)

- **A. Per-hit lighting in the probe trace** (picked): march sun and sky
  transmittance from the foliage hit, directional leaf phase, multiple
  scattering from the local escape. Accurate, contained in one shader plus
  the far-field march; cost is two marches per foliage ray.
- B. Sun from the far shadow map: one lookup, but 8 m texels and a binary
  0.5-opacity depth; translucent crown edges go lit/unlit. Fallback for
  the sun term only if A's cost does not trim.
- C. Lighting volume baked over the voxels on sun change: cheapest per
  frame, most code and memory, stalls on sun motion. Overkill now.

## Design (A)

1. **Hit point from the albedo march.** `farFieldTransmittanceAlbedo`
   also returns the absorption-weighted mean distance (same accumulation
   as the albedo, constant-folded out elsewhere), replacing the spike's
   separate `farFieldShadowDistance` march.
2. **Sun and sky transmittance at the hit:** `farFieldTransmittance`
   toward the sun and straight up from the hit, as in the spike. Only for
   rays that met foliage (`1 - T` above a small threshold).
3. **Leaf phase instead of the flat-surface weights.** Bi-Lambertian
   leaves with a spherical leaf-angle distribution (Shultis & Myneni
   1988): `Gamma(beta) = omega / (3 pi) (sin b - b cos b) + tau / pi cos
   b`, `G = 1/2`, beta between the sun's travel direction and the
   direction from the hit to the probe. Sun source `E_sun T_sun Gamma / G`
   (no `sun.y` factor: a volume, not a ground plane); looking toward the
   sun shows transmission, away shows reflection. The palette already
   holds reflected (rho) and transmitted (tau) rgb separately. Sky:
   isotropic, `omega / (2 pi) E_sky T_up` (what the spike used).
4. **Multiple scattering, spectral invariant (p-theory, Knyazikhin /
   Huang 2007):** the in-crown source scales by `1 / (1 - p omega)` per
   channel, recollision probability `p = 1 - T_up` (the hit's measured
   escape) clamped below 1; it adds the green the spike lacks. Kept only
   if it moves the reference ratios toward 1; otherwise left out and
   logged.
5. Remove `leafSun`, `canopySun` (and its per-probe march).

## Checks

Release, 2560x1440, `--trees --effects pom=0 --bloom 0 0 --streamed off`,
reference 1024 spp, `tools/reference-compare.py`:

- Forest edge front: every 4x4 cell within 0.9-1.1 of the reference;
  trunk rect 885 850 915 1150 luminance within 10%, B/G within 0.08.
- Forest edge backlit (`--sun -0.9 0.4 0.15`): GI-dominated cells
  (columns 2-3) closer to 1 than today; sunward cells unchanged (direct
  path).
- Forest interior `85 1158.6 -53 -1.55 0.25`: grid within 0.9-1.1, floor
  shade still 1.5-5% of the sunflecks.
- Cost: `gi probes` median over 5 runs (`GPU_RUNS=5`) at most +0.4 ms on
  the edge view; else B for the sun term.
- Debug build: 0 validation errors, existing tests pass.
- decisions.md entry, backlog rows (GI over-bright row closed; backlit row
  reworded to the direct path), CLAUDE.md GI paragraph, resume.md.
