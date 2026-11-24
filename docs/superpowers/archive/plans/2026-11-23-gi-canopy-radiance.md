# GI canopy radiance Implementation Plan

Status: done (2026-11-23)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** GI probe rays that meet foliage return radiance lit by the sun and sky that reach the hit, with a leaf phase function and multiple scattering, matching the reference path tracer.

**Architecture:** The far-field march's albedo mode accumulates reflected and transmitted palette rgb separately plus the absorption-weighted hit distance; `gi_probes.comp` marches sun and sky transmittance from that hit and evaluates a bi-Lambertian spherical-leaf phase (CPU twin in `gi.h`, tested), replacing `leafSun`/`canopySun`.

**Tech Stack:** GLSL (Vulkan compute), C++20, doctest.

**Spec:** `docs/superpowers/archive/specs/2026-11-23-gi-canopy-radiance-design.md`

## Global Constraints

- No asset-specific constants (engine stays asset-agnostic); reflectance and transmittance come from the far-field palette.
- `gi probes` GPU cost at most +0.4 ms over today on the edge view (median, `GPU_RUNS=5`); otherwise the sun term falls back to the far shadow map (spec approach B).
- Gates use Release, fullscreen 2560x1440, `--trees --effects pom=0 --bloom 0 0 --streamed off`, reference 1024 spp, `tools/reference-compare.py`.
- Commits: no AI attribution trailer, one feature per commit; decisions.md, CLAUDE.md, backlog.md updated in the same commit.

## Review Focus

- Sun below the horizon (`sun.y <= 0`): no sun term, no march toward it, sky term only.
- A ray that crosses foliage too thin to scatter (`1 - T` tiny): no hit lighting, no division blow-up in the mean distance or albedo.
- Probe inside a voxel: hit distance near 0 still gives a valid hit point.
- Debug build: 0 validation errors, existing GI tests pass.
- Interior view (deep canopy): floor shade stays 1.5-5% of the sunflecks.

---

### Task 1: Leaf phase function, CPU reference

**Files:**
- Modify: `engine/include/meridian/gi.h`, `engine/src/gi.cpp`
- Test: `tests/gi_test.cpp`

**Interfaces:**
- Produces: `float leafPhase(float cosBeta, float reflectance, float transmittance)` (gi.h): the bi-Lambertian, spherical-leaf area scattering phase normalized by pi G (G = 1/2), so its integral over the sphere is `reflectance + transmittance`. `cosBeta`: cosine between the incident light's travel direction and the scattered direction. Formula: `Gamma = w / (3 pi) (sin b - b cos b) + t / pi cos b`, `w = r + t`, result `Gamma / (pi * 0.5)` (Shultis & Myneni 1988).

- [ ] **Step 1: Failing tests** in `tests/gi_test.cpp`:
  - `TEST_CASE("leafPhase integrates to the scattering albedo")`: for (r, t) in {(0.1, 0.1), (0.05, 0.2), (0.3, 0)}, 2 pi times the integral of `leafPhase(cos b, r, t) sin b` over b in [0, pi] (midpoint, 4096 steps) is `r + t` within 1e-3.
  - `TEST_CASE("leafPhase forward is transmission, backward is reflection")`: `leafPhase(1, 0, 0.2) > 0`, `leafPhase(1, 0.2, 0) == approx(0)`; `leafPhase(-1, 0.2, 0) > leafPhase(-1, 0, 0.2)`.
- [ ] **Step 2:** `cmake --build build && ./build/tests/tests -tc='leafPhase*'`: fails (undeclared).
- [ ] **Step 3:** Implement `leafPhase` in gi.cpp (clamp cosBeta to [-1, 1], b = acos).
- [ ] **Step 4:** Same command passes; full `./build/tests/tests` passes.

### Task 2: Per-hit foliage lighting in the probe trace

**Files:**
- Modify: `engine/shaders/farfield.glsl` (farMarchCellImpl, farFieldMarch, farFieldTransmittanceAlbedo), `engine/shaders/gi.glsl` (leafPhase twin), `engine/shaders/gi_probes.comp`

**Interfaces:**
- Consumes: `leafPhase` (Task 1), mirrored in gi.glsl with the same signature.
- Produces: `float farFieldTransmittanceScatter(FarFieldBuffer farField, vec3 worldOrigin, vec3 direction, float startDistance, float maxDistance, out vec3 reflected, out vec3 transmitted, out float distance)`, replacing `farFieldTransmittanceAlbedo` (its only caller is gi_probes.comp): absorption-weighted means of the palette's reflected rgb (`palette.albedo[entry].rgb`), transmitted rgb (`palette.albedo[paletteSize + entry].rgb`) and segment-midpoint distance, each sum divided by `max(1 - T, 1e-6)`. The march's `wantAlbedo` accumulator becomes these three sums (still constant-folded out of the plain march).

- [ ] **Step 1:** Change the accumulator and wrapper as above; update the farfield.glsl comments that describe the albedo mode.
- [ ] **Step 2:** In gi_probes.comp, for a foliage ray with `1 - T > 0.01`: hit `q = origin + distance * direction`; `sunT` = `farFieldTransmittance(q, sun, 0, kGiTreeDistance)` if `sun.y > 0` else 0; `upT` = `farFieldTransmittance(q, (0,1,0), 0, kGiTreeDistance)`. Per channel with `w = r + t`:
  `single = sunAtProbe * sunT * leafPhase(dot(sun, direction), r, t) + w / (2 pi) * upT * skyIrradiance`;
  `leaf = single / (1 - p w)`, `p = min(1 - upT, 0.95)`. `radiance = mix(leaf, radiance, T)`. Remove `leafSun`, `canopySun` and its shared variable and march.
- [ ] **Step 3:** `cmake --build build-release` and the Debug `build`; Debug capture of the edge view: 0 validation errors.
- [ ] **Step 4:** Release captures (edge front, edge backlit, interior) and references for the interior view (edge references exist in `.superpowers/spike-gi-canopy/`). Gates: edge front every 4x4 cell 0.9-1.1, trunk rect 885 850 915 1150 within 10% and B/G within 0.08 of the reference; backlit columns 2-3 closer to 1 than base; interior grid 0.9-1.1.
- [ ] **Step 5:** A/B the multiple scattering (`p = 0`): keep it only if the edge-front and interior cell ratios are closer to 1 with it.
- [ ] **Step 6:** Cost: `GPU_RUNS=5`-style median of `gi probes` over 5 edge captures, before (main) and after; at most +0.4 ms, else the sun term from the far shadow map and re-measure.

### Task 3: Docs and commit

**Files:**
- Modify: `docs/decisions.md` (new entry: why, ablation numbers, formula and sources, gates, cost), `CLAUDE.md` (GI paragraph: foliage along a ray lit at its hit), `docs/backlog.md` (close the GI over-bright row; reword the backlit row to the direct path; G3 row's trunk-tint note), the spec Status line, `docs/superpowers/resume.md`.

- [ ] **Step 1:** Downstream sweep: `grep -rn 'leafSun\|canopySun\|farFieldTransmittanceAlbedo\|half the crown' engine docs CLAUDE.md` returns only history in decisions.md.
- [ ] **Step 2:** Read the full diff; commit `GI probes: light foliage where the ray meets it`, push to main.
