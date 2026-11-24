# Geo Channels and Causal Placement Implementation Plan

Status: done (2026-11-24).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Placement layers read cook-time geo channels (wetness, flow, two-scale topographic position, sun exposure, talus) and earlier layers' density, so objects sit where they would grow or fall; the forest is scored against real canopy data.

**Architecture:** `meridian-cook --geo` computes 8-bit channels from any heightfield into a `.geo` file beside the `.height`. The engine loads it as generic data (`GeoChannels`), placement rules gain integer channel ramps and an optional parent-layer ramp, mirrored bit-exactly in placement.glsl; the terrain material's forest floor evaluates the forest rule through the same GLSL. The game rewrites `placeScenery` on the channels and scores the forest with `--score-forest`.

**Tech Stack:** C++20, Vulkan 1.3 compute (GLSL), doctest, bash + magick + python3/numpy (tools).

**Spec:** `docs/superpowers/specs/2026-11-23-geo-channels-causal-placement-design.md`

## Global Constraints

- Placement keep/reject stays integer-only; `placement check` and `stream check` print 0 mismatches.
- The engine never names a channel: semantics live in `tools/cook/geo_channels.h` (cook and game).
- A channel texel covers exactly r x r heightfield samples: heightfield width = r x channel width (8000 = 4 x 2000, 7500 = 2 x 3750); cook and game refuse anything else.
- A rule's noise (mask, cluster) may vary density inside a channel-defined extent, never define where forests are.
- Each causal rule carries a one-line physical reason.
- Release startup within ~1 s of today's (`--placement` on, Napf); parity route frame times unchanged within run-to-run noise.
- Commits: no AI attribution; one feature per commit; CLAUDE.md and decisions.md current in the same commit as the change.
- Every game run through `flock /tmp/meridian-gpu.lock`; captures at 2560x1440.

## Review Focus

1. A point at the crop's last sample row/column: its channel texel index is sample / r and must stay < channel width (Task 4 test `channel index at the crop edge`).
2. A heightfield with no `.geo` beside it (procedural terrain, an old cook): the game must still start; channel-dependent layers are skipped with one printed warning (Task 8 verification run).
3. Flat ground (a plateau, a lake fill): fill, flow and wetness give finite values, no NaN or division by zero (Task 2 test `flat plane gives finite wetness`).
4. A parent that is streamed and a child that is static (or the reverse): the child's density depends only on the parent's rule, never on streaming state (Task 4 test `parent density ignores streaming`).
5. Extreme latitude (the atmosphere is parametric; other worlds): sun exposure at latitude 89 or -60 gives finite bytes (Task 3 test `sun exposure is finite at any latitude`).

---

### Task 1: GeoChannels file format (engine)

**Files:**
- Create: `engine/include/meridian/geo.h`, `engine/src/geo.cpp`
- Modify: `engine/CMakeLists.txt` (add geo.cpp)
- Test: `tests/geo_test.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Produces:
  ```cpp
  constexpr uint32_t kGeoChannelCount = 8;
  struct GeoChannels {
    uint32_t width = 0, height = 0;   // channel texels
    uint32_t ratio = 0;               // heightfield samples per texel side
    uint32_t fieldWidth = 0, fieldHeight = 0; float fieldSpacing = 0;  // the heightfield it was cooked from
    std::vector<uint8_t> texels;      // kGeoChannelCount per texel, row 0 = north
    uint8_t at(uint32_t i, uint32_t j, uint32_t channel) const;
  };
  void saveGeoChannels(const GeoChannels &, const std::filesystem::path &);
  GeoChannels loadGeoChannels(const std::filesystem::path &);   // throws on bad magic/size
  bool geoMatches(const GeoChannels &, const Heightfield &);     // sizes, spacing, ratio rule
  std::filesystem::path geoPathFor(const std::filesystem::path &height); // x.height -> x.geo
  ```
  File: header {magic "MGC1", version 1, width, height, ratio, fieldWidth, fieldHeight, fieldSpacing, channelCount} then texels.

- [ ] **Step 1: Write failing tests** `geo round trip` (save, load, equal texels and header), `geo load rejects a truncated file` (CHECK_THROWS), `geoMatches requires width = ratio x channel width` (true for 8000/4/2000 at 0.5 m, false for 7999 or spacing 1.0).
- [ ] **Step 2: Run** `cmake --build build && ./build/tests/tests -tc='geo*'` — expected: compile failure (no geo.h).
- [ ] **Step 3: Implement** geo.h/geo.cpp as above, following `saveHeightfield`/`loadHeightfield` (terrain.cpp) style.
- [ ] **Step 4: Run** the same command — expected: PASS.
- [ ] **Step 5: Commit** "Geo channels: file format".

### Task 2: Cook: depression fill, flow, wetness

**Files:**
- Create: `tools/cook/geo_channels.h` (header-only semantics: channel enum and byte encodings), `tools/cook/geo.h`, `tools/cook/geo.cpp`
- Modify: `tools/cook/CMakeLists.txt` (geo.cpp into meridian-cook-lib)
- Test: `tests/geo_cook_test.cpp`

**Interfaces:**
- Produces (geo_channels.h, used by cook and game):
  ```cpp
  enum GeoChannel : uint32_t { kWetness = 0, kFlow = 1, kLocalPosition = 2, kLandformPosition = 3,
                               kSunExposure = 4, kTalus = 5 };
  // byte encodings (the scales the spec leaves to the builder, fixed here):
  uint8_t encodeWetness(float twi);          // clamp((twi - 2) / 18 * 255)
  uint8_t encodeFlow(float sca);             // clamp(log2(max(sca, 1)) * 12.75), sca in m^2/m
  uint8_t encodeLocalPosition(float m);      // clamp(128 + m * 63.5), +-2 m
  uint8_t encodeLandformPosition(float m);   // clamp(128 + m * 7.94), +-16 m
  uint8_t encodeSunExposure(float relative); // clamp(255 * relative / 1.5), relative to a flat unshaded surface
  // talus: 0 = out of reach, 1..255 = closeness to the source's foot
  ```
  and the matching `decode*` for the game's rule constants.
- Produces (geo.h, cook lib):
  ```cpp
  struct Grid { uint32_t width, height; float spacing; std::vector<float> v; };  // row-major, row 0 north
  Grid fillDepressions(const Grid &heights);          // Priority-Flood+epsilon (Barnes et al. 2014): every cell drains to the edge
  Grid smoothGrid(const Grid &, float radiusMeters);  // box via summed-area table
  Grid specificCatchmentArea(const Grid &filled);     // FD8, Freeman 1991, exponent 1.1; m^2/m
  Grid wetnessIndex(const Grid &sca, const Grid &heights);  // ln(sca / (tan b + tan(0.001 deg)))
  ```

- [ ] **Step 1: Write failing tests:** `fill leaves a monotone slope unchanged`; `fill raises a pit to its spill height` (pit cell >= rim minimum); `flow converges in a V-valley` (thalweg SCA > 10x the side cells'); `flow spreads on a cone` (SCA at equal radius equal within 5% across 8 directions: no D8 stripes); `flat plane gives finite wetness` (all values finite).
- [ ] **Step 2: Run** `./build/tests/tests -tc='fill*,flow*,flat*'` — expected: FAIL (undefined).
- [ ] **Step 3: Implement** in geo.cpp; flow processes cells in descending filled height (sort indices), distributes to lower neighbours by (tan slope)^1.1 x contour weight (0.5 orthogonal, 0.354 diagonal), SCA = area / contour width.
- [ ] **Step 4: Run** the tests — expected: PASS.
- [ ] **Step 5: Commit** "Cook: depression fill, FD8 flow and wetness".

### Task 3: Cook: topographic position, sun exposure, talus, `--geo`

**Files:**
- Modify: `tools/cook/geo.h`, `tools/cook/geo.cpp`, `tools/cook/cook.cpp` (CLI)
- Test: `tests/geo_cook_test.cpp`

**Interfaces:**
- Consumes: Task 1 `GeoChannels`, Task 2 `Grid`, encoders.
- Produces:
  ```cpp
  Grid topographicPosition(const Grid &heights, float radiusMeters);   // h - mean within radius (disk or box; box ok)
  Grid sunExposure(const Grid &heights, float latitudeDegrees);        // annual mean direct cos-incidence x horizon visibility, relative to a flat unshaded surface
  Grid talus(const Grid &heights);   // sources: slope > 40 deg; reach while the line from the source top stays >= 32 deg below horizontal (Fahrboschung); value = closeness 0..1
  GeoChannels cookGeo(const Heightfield &field, uint32_t ratio, float latitudeDegrees);
  ```
  CLI: `meridian-cook --geo in.height out.geo RATIO [LATITUDE]` (default 47); refuses `width % RATIO != 0`; prints per-channel min/mean/max bytes and the time.
  `cookGeo`: computes on the full heightfield except sun exposure and talus at channel resolution (block-mean heights); fill/flow on heights smoothed to ~4 m (spec); block reduction: mean for positions/wetness, max for flow and talus.
  Sun: horizon angle per texel in 16 azimuths (march to 2 km); sun path every hour of the 21st of each month; atmosphere ignored (cos incidence only).

- [ ] **Step 1: Write failing tests:** `topographic position is positive on a ridge and negative in a bowl`; `south slope gets more sun than north at 47 N` (z = +south in world: north = -z, row 0 north); `sun exposure is finite at any latitude` (89, -60); `talus lies below a cliff step and not above it`; `cookGeo refuses a ratio that does not divide the width`.
- [ ] **Step 2: Run** `./build/tests/tests -tc='topographic*,south*,sun*,talus*,cookGeo*'` — expected: FAIL.
- [ ] **Step 3: Implement** the functions and the CLI branch (pattern: the `--heightfield` branch in cook.cpp).
- [ ] **Step 4: Run** tests — PASS; then `./build/tools/cook/meridian-cook --geo assets/terrain/relief-0.5.height assets/terrain/relief-0.5.geo 4` — expected: per-channel stats with nonzero spread on every channel 0-5, under ~60 s.
- [ ] **Step 5: Look** at channels 0, 3, 4, 5 as greyscale PNGs (a throwaway python dump into `.superpowers/geo/`): ravines wet and low, south slopes bright, talus under the Nagelfluh walls. Fix before going on if any channel looks wrong.
- [ ] **Step 6: Commit** "Cook: topographic position, sun exposure, talus; --geo".

### Task 4: Placement rules read channels and a parent (CPU reference)

**Files:**
- Modify: `engine/include/meridian/placement.h`, `engine/src/placement.cpp`, every `evaluatePoint`/`densityAt` caller (`renderer_placement.cpp` Debug checks; tests)
- Test: `tests/placement_test.cpp`

**Interfaces:**
- Consumes: Task 1 `GeoChannels`.
- Produces:
  ```cpp
  struct ChannelRamp { int32_t channel = -1; float lo = 0, hi = 0; }; // bytes 0..255; lo > hi inverts (as the mask); -1 = off
  // DensityRule gains: std::array<ChannelRamp, 4> channels{};
  struct ParentRamp { uint32_t layerId; DensityRule rule; float lo, hi; }; // density 0..1 of the parent's raw rule
  // PlacementLayer gains: std::optional<ParentRamp> parent;  (one level deep by type)
  // PlacementRule (integer) gains: int32_t channelRamp[4][3]; // channel (-1 off), lo, hi in byte units
  // Signatures gain `const GeoChannels *geo` after the Heightfield:
  uint16_t densityAt(const Heightfield &, const GeoChannels *, const PlacementLayer &, uint32_t seed, int64_t x, int64_t z);
  std::optional<PlacedPoint> evaluatePoint(const Heightfield &, const GeoChannels *, const PlacementLayer &, const DitherPattern &, uint32_t seed, int32_t cellX, int32_t cellZ, uint32_t point);
  uint32_t geoTexelIndex(uint32_t sample, uint32_t ratio, uint32_t channelSize); // min(sample / ratio, size - 1)
  ```
  Channel factor: `smooth16(ramp16(byte, lo, hi))` (inverted form as the mask), multiplied in after the cluster factor. Parent factor: the parent rule's density (its own layerSeed from `parent.layerId`) ramped by lo/hi in 0..65535, multiplied in last. A rule with an active channel ramp and `geo == nullptr` throws.

- [ ] **Step 1: Write failing tests:** `densityAt values are pinned` still passes unchanged with `geo = nullptr` and no ramps (existing anchors 39852, 7760, 52583); `a channel ramp keeps only texels inside it`; `channel index at the crop edge` (sample width-1 maps to channel width-1 for ratio 4 and 2); `a parent ramp follows the parent's density`; `parent density ignores streaming` (same density whether the parent layer has `streamFullDensity` 0 or 30); `a channel rule without geo throws`.
- [ ] **Step 2: Run** `./build/tests/tests -tc='*density*,*channel*,*parent*'` — expected: FAIL (compile).
- [ ] **Step 3: Implement**; callers pass `nullptr` until Task 6.
- [ ] **Step 4: Run** the full suite `./build/tests/tests` — expected: all pass.
- [ ] **Step 5: Commit** "Placement: channel ramps and parent layers (CPU reference)".

### Task 5: Forest truth and the score

**Files:**
- Modify: `tools/fetch-relief-patch.sh` (swissSURFACE3D Raster 0.5 m, URL pattern `ch.swisstopo.swisssurface3d-raster/swisssurface3d-raster_<year>_<e>-<n>/..._0.5_2056_5728.tif`, year fallback as for ALTI3D; then `--geo` for both relief heightfields, ratio 4 and 1)
- Create: `tools/canopy-truth.py` (numpy; surface minus ground at 0.5 m; per 8 m cell forest iff canopy > 3 m covers >= 20%; writes `assets/terrain/relief-forest-truth.bin`: header u32 width, height, cell size in samples, then u8 per cell, row 0 north, centred like the heightfield)
- Modify: `game/src/main.cpp` (`forestRule(...)` shared by `placeScenery` and the score; `--score-forest FILE`)

**Interfaces:**
- Consumes: Task 4 `densityAt(..., geo, ...)`.
- Produces: `--score-forest FILE` prints `Forest score: precision P, recall R, F1 F (cells N, truth forest T%)` and exits 0 (no window, no GPU). A cell is forest iff the mean `densityAt` over a 4x4 grid of points inside it >= 32768.

- [ ] **Step 1:** Run the fetch script; check the truth file: forest fraction of the patch printed by canopy-truth.py is plausible (Napf: roughly 30-60%), and a PNG dump of it shows the forested ravines.
- [ ] **Step 2:** Implement `forestRule` (today's rule, unchanged) and `--score-forest`.
- [ ] **Step 3:** Run `./build-release/game/game --terrain assets/terrain/relief-0.5.height --score-forest assets/terrain/relief-forest-truth.bin` — record the **baseline** line in the spec's success criteria section.
- [ ] **Step 4: Commit** "Forest truth from swissSURFACE3D and --score-forest; baseline F1".

### Task 6: GPU placement reads channels and parents

**Files:**
- Modify: `engine/shaders/placement.glsl`, `placement.comp` (all variants incl. `PLACEMENT_STREAM`), `engine/src/render/gpu_types.h` (`PlacementRule` mirror, `GpuPlacementLayer` parent fields, `PlacementConstants.geo`), `renderer_placement.cpp`, `renderer.h` (`void setGeoChannels(const GeoChannels &)`; call before `setPlacement`; kept on the CPU for the Debug checks), `game/src/main.cpp` (load `geoPathFor(heightPath)` when present and matching)

**Interfaces:**
- Consumes: Task 4 integer rule layout; Task 1 loader.
- Produces: the geo texels in a device buffer (`uvec2` per texel, 8 bytes) reachable from `PlacementConstants` and (for Task 7) `SceneData`; `GpuPlacementLayer` carries the parent's `PlacementRule`, its layer seed, lo, hi and a has-parent flag.

- [ ] **Step 1:** Mirror `ChannelRamp`/parent evaluation in placement.glsl operation for operation; static_asserts for the new sizes.
- [ ] **Step 2:** Temporary check layer in the game (one rule with a wetness ramp and the forest as parent) — Debug run on the patch: `flock /tmp/meridian-gpu.lock ./build/game/game --terrain assets/terrain/relief-0.5.height --trees --capture .superpowers/geo/check.png` — expected `placement check: ... 0 mismatches`, `stream check: ... 0 mismatches`, 0 validation errors. Remove the temporary layer.
- [ ] **Step 3: Commit** "Placement on the GPU: channel ramps and parent layers".

### Task 7: Forest floor follows the forest layer

**Files:**
- Modify: `engine/shaders/surface.glsl` (replace `forestMask` with `forestDensity`), `common.glsl`/`gpu_types.h` (`SceneData`/`Terrain`: geo buffer, forest rule + parent, replacing `forestMaskPeriod/Lo/Hi/Seed`), the renderer setter that feeds them (`Renderer::setForestRule(const PlacementLayer &)` or via `setPlacement` by layer id 1: builder's call, documented in renderer.h)

**Interfaces:**
- Consumes: Task 6 shared GLSL rule evaluation and the geo buffer.
- Produces: `float forestDensity(...)`: the forest layer's density at the four surrounding channel texel centres, blended bilinearly in float (material only); 0 outside the crop; today's floor ramp bands applied to it.

- [ ] **Step 1:** Implement; remove the mask fields and their game plumbing (`kMaskFrequency` etc.) if nothing else uses them (grep).
- [ ] **Step 2:** Capture the forest overview on the patch with `--trees` (camera `200 1450 -900 -1.55 -0.5`, adjust to the patch) — expected: floor material under every forest, no 2-4 m blocks at the edge view (eye height at a forest edge).
- [ ] **Step 3: Commit** "Terrain forest floor follows the forest layer".

### Task 8: Causal layers

**Files:**
- Modify: `game/src/main.cpp` (`placeScenery`, `forestRule`)

**Interfaces:**
- Consumes: Tasks 2-7; `decode*` from geo_channels.h for readable constants.

- [ ] **Step 1:** Rewrite the layers per spec goal 4 (forest; lone trees with the forest as inverted parent; saplings and ferns with the forest as parent; rocks on talus + steep; meadow plants by wetness and sun), one-line physical reason per rule. Without a matching `.geo`, skip channel-dependent layers and print `No geo channels for <file>: causal layers skipped`.
- [ ] **Step 2:** Score round: `--score-forest` on the patch; tune; repeat. Stop when two rounds each gain < 0.02 F1. Record every round's line in the plan's notes below.
- [ ] **Step 3:** Captures, causal vs today's rules (today's from a Release build of the commit before Task 8, in a worktree beside the repo) — views: forest overview, eye-height forest edge, a ravine wall, a scree foot, a wet hollow (picked from the channel dumps of Task 3). Side-by-side sheet in `docs/images/causal-placement-2026-11-23/` for the user.
- [ ] **Step 4:** Run without a `.geo` (`--terrain` on a copy of relief-2.height without its .geo) — expected: starts, prints the skip warning, 0 validation errors.
- [ ] **Step 5: Commit** "Causal placement layers on the geo channels" (message carries baseline and final F1).

### Task 9: Napf channels, budgets, docs

**Files:**
- Modify: `tools/fetch-sample-assets.sh` (cook `napf.geo`, ratio 2), `CLAUDE.md` (cook `--geo`, `--score-forest`, the relief patch's truth, new placement hashes, `.geo` beside `.height`, Layout entries for geo.h/geo.cpp/geo_channels.h), `docs/decisions.md` (entry: channels, algorithms and sources, the score, the calls), `docs/backlog.md` (rows: instance-level dependence; wet/scree materials from the channels; 0.5 m clipmap), the spec's and plan's Status lines, `docs/superpowers/resume.md`, the triage note (steps 2-3 done)

- [ ] **Step 1:** Cook `napf.geo`; Debug run on the Napf with `--trees` — 0 mismatches; record the placement hashes with and without `--trees`.
- [ ] **Step 2:** Release startup on the Napf, before (commit before Task 1) and after — within ~1 s; parity route `--benchmark 60 out.csv --route tools/routes/parity.route --trees` before/after through `tools/bench-route.py` — no segment worse beyond noise.
- [ ] **Step 3:** Docs sweep: grep the repo for `forestMask`, `kMaskFrequency`, `mask complement`, old placement hashes, "nothing stored below" and fix stale claims.
- [ ] **Step 4: Commit** "Napf geo channels; docs" and push.

## Notes (filled during execution)

- Baseline F1 (Task 5): `Forest score: precision 0.546, recall 0.320, F1 0.403 (cells 250000, truth forest 69.4%)` (relief-0.5, today's mask rule; precision under the 69.4% base rate: the noise forest is no better than chance)
- Score metric: kappa added (Task 8 ruling); baseline kappa -0.225.
- Tuning rounds (Task 8), forest rule on relief-0.5 (kappa / F1 / ours vs truth 69.4%):
  1. slope 18-38, cluster 120 m 0.3, sun cleared above 1.06: 0.385 / 0.740 / 49.4%
  2. slope 14-34, no cluster: 0.461 / 0.829 / 67.2% (also tried: 14-34 + cluster 0.429; 18-38 no cluster 0.438; 10-30 0.437)
  3. 14-34 + cluster 0.15: 0.449; + dry ramp (TWI 6-9): 0.438 (no gain)
  4. 12-36: 0.461; 16-32: 0.456 (no gain) -> stop; final = round 2
