# Reference Path Tracer Implementation Plan

Status: done (2026-11-23)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `--reference SPP` capture mode that path traces the `--camera` view through the TLAS and the heightfield, with the raster path's own material code, as ground truth for the raster approximations.

**Architecture:** mesh.frag's material and terrain functions move to `surface.glsl` and read a `SurfaceInput` global instead of varyings; mesh.frag fills it from varyings, `reference.comp` from ray hits. A reference run warms up normally, freezes exposure, then each `draw` dispatches one sample per pixel into an accumulation image that replaces the upscaler's output, until SPP samples are in; the normal capture path writes it.

**Tech Stack:** C++20, Vulkan 1.3, GLSL compute with `GL_EXT_ray_query`, Python 3 + numpy for the comparison tool.

**Spec:** `docs/superpowers/specs/2026-11-23-reference-path-tracer-design.md`

## Global Constraints

- Every game run goes through `flock /tmp/meridian-gpu.lock`; captures at 2560x1440 (fullscreen); GPU ms compared only with identical `Render WxH` lines.
- Commits carry no AI attribution trailer; one feature per commit; push to `main`.
- New CLI flags documented in CLAUDE.md, the choice logged in `docs/decisions.md`, in the commit that adds them.
- Raster output must not change: refactor acceptance is mean difference <= 0.5/255 and OKLab mean L identical to 3 decimals on the three gate views below.
- Per-dispatch GPU time of the reference pass stays under 100 ms (driver timeout headroom).
- Furnace and analytic checks pass within 1% before any raster result is judged against the reference.
- Debug run once before the final commit (validation clean, placement / far-field checks 0 mismatches).

Gate views (all `--trees`, Release): edge `600 1205.6 -400 -1.55 0.02`, overview `700 1450 -400 -1.55 -0.5`, meadow `1119 1355.9 -431 -1.55 -0.25`.

## Review Focus

1. Alpha-tested plants (ferns, poppies, masked cards) in the TLAS: rays must pass through cut-out texels for primary hits too, not only shadow rays. Pinned by Task 4's albedo comparison on the meadow view.
2. Rays leaving a surface hit themselves (normal-offset too small on 28M-triangle crowns, or on terrain at grazing angles): the result shows as black speckle or NaN. Pinned by Task 3's NaN/Inf count and Task 5's furnace (any self-shadowing breaks 1%).
3. Low or set sun (transmittance ~0, terrain shadows across valleys): must give finite, dark output, not NaN. Pinned by Task 5's sunset run.
4. Single-sided geometry hit from behind (rock, bark back faces): the shading normal must face the ray as mesh.frag's `gl_FrontFacing` flip does. Pinned by Task 4's albedo comparison (rock and trunk regions).
5. Trees beyond 4 km are in the TLAS under `kFarFieldCasterMask` only: primary and bounce rays must include both masks, or distant forest vanishes. Pinned by Task 4's overview albedo comparison (far region).

---

### Task 1: Shared surface evaluation (`surface.glsl`)

**Files:**
- Create: `engine/shaders/surface.glsl`
- Modify: `engine/shaders/mesh.frag` (lines 121-925 move out; `main` fills the input), `engine/CMakeLists.txt` (re-configure for the new include)

**Interfaces:**
- Produces (GLSL, `surface.glsl`):
  - `struct SurfaceInput { vec3 relativePosition; vec3 dpdx; vec3 dpdy; vec2 uv; vec4 tangent; vec3 tint; float height; float streamFade; float crownVisibility; };`
  - global `SurfaceInput surfaceIn;` read by every moved function in place of `inRelativePosition`, `inUv`, `inTangent`, `inTint`, `inHeight`, `inStreamFade`, `inCrownVisibility` and of `dFdx/dFdy(inRelativePosition)`.
  - `vec4 sampleTexture(uint index, vec2 uv)`: `texture(..., pc.scene.mipBias)` under `#ifdef SURFACE_FRAGMENT`, `textureLod(..., 0.0)` otherwise.
  - Unchanged names and signatures: `Surface terrainSurface(vec3 normal)`, `Surface proceduralTerrain(vec3 normal)`, `Surface materialSurface(Material m, vec3 normal)`, `vec3 terrainNormal()`, `float plantOcclusion(Material m)`, `float crownOcclusion()`.
- mesh.frag defines `SURFACE_FRAGMENT`, includes `surface.glsl`, and at the top of `main` (non-depth variants) sets `surfaceIn` from its varyings and `dFdx/dFdy(inRelativePosition)`.

- [ ] **Step 1: Baseline captures before touching code**

Run (Release, each gate view): `.superpowers/khr32/cap.sh build-release .superpowers/refpt/base` (or the same three `--capture X.png --frames 120` runs into `.superpowers/refpt/base/`).
Expected: three PNGs and logs, no validation errors.

- [ ] **Step 2: Move lines 121-925 of mesh.frag into `surface.glsl` and replace the varying reads with `surfaceIn` fields**

Derivatives the moved code takes (`dFdx(inRelativePosition)` in `terrainSurface`, the hex tiling's `dx/dy`) come from `surfaceIn.dpdx/dpdy`; derivatives of other quantities computed inside the moved code stay `dFdx` only under `SURFACE_FRAGMENT` and are zero vectors otherwise.

- [ ] **Step 3: Build Release and Debug**

Run: `cmake -S . -B build-release && cmake --build build-release && cmake --build build`
Expected: no shader or C++ errors.

- [ ] **Step 4: Capture the gate views and compare with the baseline**

Run per view: `python3 -I tools/image-gates.py diff base/V.png after/V.png` and the OKLab stats script.
Expected: diff <= 0.5/255, mean L equal to 3 decimals, GPU total within 0.3 ms.

- [ ] **Step 5: Commit** — "Shared surface evaluation for raster and ray hits"

---

### Task 2: `--streamed off`

**Files:**
- Modify: `game/src/main.cpp` (option parse, the placement layer list near line 446), `CLAUDE.md`

**Interfaces:**
- Produces: CLI `--streamed on|off` (default on). Off drops every layer whose `streamFullDensity > 0` before `setPlacement`; the static layers and the placement hash of the static table are unchanged.

- [ ] **Step 1: Capture the meadow view with `--streamed off`**

Run: `./build-release/game/game --camera 1119 1355.9 -431 -1.55 -0.25 --streamed off --capture .superpowers/refpt/s.png --frames 30`
Expected now: exit with the unknown-option usage text.

- [ ] **Step 2: Implement the option**

- [ ] **Step 3: Re-run**

Expected: `streamed: 0 layers` in the output, no validation errors, and (Debug) `placement hash 0x341d51799d224d3c` without `--trees`.

- [ ] **Step 4: Commit** with the CLAUDE.md line — "--streamed off: skip the streamed meadow layers"

---

### Task 3: Comparison tool

**Files:**
- Create: `tools/reference-compare.py`

**Interfaces:**
- Produces: `python3 -I tools/reference-compare.py RASTER.pfm REFERENCE.pfm [--ev RASTER_EV REFERENCE_EV] [--regions FILE --view NAME] [X0 Y0 X1 Y1 ...]`
  - Both files are pre-exposed with their run's exposure; exposure is proportional to 2^-EV100, so the raster is multiplied by `2^(EV_raster - EV_ref)` to put it on the reference's scale (`--ev` defaults to `0 0`).
  - Per region (default 4x4 grid): `L_raster L_ref ratio  rgb/G raster  rgb/G ref`, then `frame: rel. error E, NaN/Inf N`.
  - Exit code 1 when either file has a NaN or Inf pixel.
  - `--regions` reads the same JSON shape as `tools/albedo-regions.json`.

- [ ] **Step 1: Self-test with synthetic files**

Write two small `.pfm`s from Python (8x8, raster constant 1.0, reference constant 0.5) and run the tool with `--ev 0 1` (raster x 2^(0-1) = 0.5).
Expected: every ratio `1.000`, exit 0. Then one file with a NaN pixel: exit 1, `NaN/Inf 1`.

- [ ] **Step 2: Implement; reuse the PFM reader of `tools/region-luminance.py` (copy its reader function, same orientation: top-left origin)**

- [ ] **Step 3: Re-run the self-test.** Expected: as in Step 1.

- [ ] **Step 4: Commit** — "Reference comparison tool"

---

### Task 4: Reference mode: accumulation, primary hits, albedo output

**Files:**
- Create: `engine/shaders/reference.comp`, `engine/shaders/ray_hit.glsl`, `engine/src/render/renderer_reference.cpp`
- Modify: `engine/include/meridian/renderer.h`, `engine/src/render/renderer_impl.h`, `engine/src/render/renderer_frame.cpp` (branch before the scene passes; skip upscaler, bloom and exposure adaptation while referencing), `engine/src/render/gpu_types.h` (`ReferenceConstants`, must match), `engine/CMakeLists.txt` (`add_shader(reference.comp reference)`), `game/src/main.cpp`, `CLAUDE.md`

**Interfaces:**
- Consumes: `surface.glsl` (Task 1).
- Produces:
  - `void Renderer::setReference(uint32_t samplesPerPixel)` (0 = off), `void Renderer::beginReference()` (freezes exposure, clears accumulation), `uint32_t Renderer::referenceSamples() const`.
  - `ray_hit.glsl`: `struct RayHit { bool hit; bool terrain; float t; vec3 position; vec3 geometricNormal; vec3 normal; vec2 uv; vec4 tangent; uint materialIndex; vec3 tint; float height; };` and `RayHit traceClosest(vec3 origin, vec3 direction, float tMax)`: TLAS query with `kShadowCasterMask | kFarFieldCasterMask`, alpha-tested candidates via `alphaTestCandidate`, against `terrainRayDistance`; normals flipped toward `-direction`, as `gl_FrontFacing` does. The instance's tint and height (vertex y over the mesh top) as mesh.mesh computes them (move that computation into a shared function `instanceTint` in common.glsl if it is not one).
  - `ReferenceConstants { uint64_t scene; uint32_t frame; uint32_t mode; }` (mode 0 radiance, 1 albedo, 2-4 furnace kinds, Task 5).
  - Accumulation: RGBA32F sum and RGBA32F sum of squares (luminance in .r of the second, for noise), display-sized, created with the swapchain targets.
  - In a reference run, `output` (the upscaler's image) receives sum / count, so `--capture` (PNG and PFM) and the albedo capture work unchanged.
  - CLI: `--reference SPP` (requires `--capture`; rejected with `--benchmark`, `--capture-sequence`, camera velocities); `--frames` frames warm up first, then `beginReference`, then the loop draws until `referenceSamples() == SPP`. `--capture-albedo FILE` in a reference run writes the reference's mean primary-hit diffuse albedo.
  - While referencing, the game stops advancing game time (wind, clouds, weather frozen at the warm-up's end), and blended materials are skipped by `traceClosest` (treated as absent; the TLAS records of blended meshes are ignored by mask or material check).
  - Startup/exit line: `Reference: N spp, M ms per sample pass (max X), total S s, noise P%` (noise: median over 16x16 tiles of the relative standard error of the mean luminance).

- [ ] **Step 1: Raster albedo of the gate views (`--effects pom=0 --bloom 0 0`)**

Run per view: `game --camera ... --trees --effects pom=0 --bloom 0 0 --capture r.png --capture-albedo r_albedo.pfm --frames 120`.

- [ ] **Step 2: Implement accumulation, the primary ray and mode 1 (albedo: the first hit's reflected diffuse albedo `baseColor x (1 - metallic) x (1 - f)`, what mesh.frag writes to the albedo guide; sky 0.5 as the raster albedo writes)**

- [ ] **Step 3: Reference albedo of the same views**

Run: `game --camera ... --trees --effects pom=0 --bloom 0 0 --reference 64 --capture f.png --capture-albedo f_albedo.pfm --frames 120`
Expected: `Reference:` line, no validation errors, per-dispatch max under 100 ms.

- [ ] **Step 4: Compare albedos**

Run: `python3 -I tools/reference-compare.py r_albedo.pfm f_albedo.pfm --ev 0 0`
Expected: whole-frame mean ratio within 3% (mip 0 vs mipped filtering), every grid cell within 10%, rock / trunk / fern / far-forest cells included, NaN/Inf 0. A cell far off points at hit attributes (UV, tangent, alpha, mask, back faces): fix before Task 5.

- [ ] **Step 5: Noise falls with samples:** the noise readout at `--reference 16` vs `--reference 64` on the edge view differs by a factor 2 +- 0.4.

- [ ] **Step 6: Commit** — "Reference mode: accumulation, primary hits, albedo"

---

### Task 5: Path integrator and furnace checks

**Files:**
- Modify: `engine/shaders/reference.comp`, `engine/shaders/lighting.glsl` (only if a sampling helper belongs beside the BRDF), `game/src/main.cpp`, `CLAUDE.md`

**Interfaces:**
- Consumes: `traceClosest`, `materialSurface`/`terrainSurface`, `transmittanceToSun`, `cloudShadow`, `skyLuminance`, `skyCaptureAt`, `applyAerialPerspective`, `terrainOccludes`/`terrainShadowVisibility`, `pcg3d`.
- Produces: mode 0 radiance as the spec's path (camera jitter box filter; NEE sun on the disk with a TLAS + terrain shadow ray, `transmittanceToSun` x `cloudShadow`; front BRDF = Lambert x (1 - f) + GGX with `energyCompensation`, back = f x transmission colour Lambert; BSDF sampling: cosine, GGX VNDF, back hemisphere, picked by albedo weights; Russian roulette from bounce 3, max 16; misses: `skyLuminance` x capture transmittance + capture clouds, sun disk excluded; aerial perspective on the camera segment only); occlusion terms of `Surface` (micro, crown, plant, stream fade) set to 1.
- CLI `--reference-furnace white|leaf|sun` (with `--reference`):
  - `white`: sky radiance 1 everywhere, sun off, haze off, every material base colour 1, metallic 0, specular off, transmission 0; output raw radiance (no sunIlluminance, no exposure).
  - `leaf`: as white, but every material that has transmission takes f = 0.5 with transmission colour 1.
  - `sun`: sky black, sun on, haze off, objects off (pair with `--placement off`), base colour 1, one bounce; writes the NEE estimate divided by the closed form `E cos(theta) / pi` (E the sun's transmitted irradiance) where the closed form > 0, else 0.

- [ ] **Step 1: Furnace runs (expected to fail before Step 2):** `game --camera <edge> --trees --reference 256 --reference-furnace white --capture w.pfm --frames 10` then `reference-compare.py --ev 0 0` against a constant-1 PFM the tool writes with `--constant 1` (add that flag in this task).
Expected after Step 2: every cell 1.00 +- 0.01; same for `leaf`.

- [ ] **Step 2: Implement the integrator and the furnace modes**

- [ ] **Step 3: Sun check:** `game --camera 1119 1420 -431 -1.55 -0.6 --placement off --sun 0.2 0.95 0.2 --reference 256 --reference-furnace sun --capture s.pfm --frames 10`. Expected: mean over pixels with value > 0.5 is 1.00 +- 0.01.

- [ ] **Step 4: Sunset robustness:** `game --camera 1119 1500 -431 3.14 -0.15 --sun 0.9 0.05 -0.4 --trees --reference 16 --capture ss.pfm --frames 60`; the compare tool against itself reports NaN/Inf 0.

- [ ] **Step 5: First radiance runs of the gate views (`--reference 1024`, `--effects pom=0 --bloom 0 0`):** noise under 1%, per-dispatch max under 100 ms; record total seconds.

- [ ] **Step 6: Commit** — "Reference path integrator with furnace checks"

---

### Task 6: First comparison, docs, sweep

**Files:**
- Modify: `docs/decisions.md`, `docs/backlog.md`, `docs/superpowers/resume.md`, `CLAUDE.md`, this plan's and the spec's Status lines

- [ ] **Step 1: Forest edge, raster vs reference, default sun and backlit (`--sun -0.9 0.4 0.15`):** raster `--effects pom=0 --bloom 0 0 --capture r.pfm`, reference same + `--reference 1024`; compare with `--regions` covering the central trunk (885 850 915 1150), the right trunk (2185 700 2205 1000), canopy shade and the floor. Record ratios and B/G for both.

- [ ] **Step 2: Sweep:** `grep -rn 'mesh.frag' CLAUDE.md docs/*.md` for statements about where material/terrain code lives (now `surface.glsl`), the Layout section's shader list, and every flag list (`--reference`, `--reference-furnace`, `--streamed`); fix stale lines.

- [ ] **Step 3: Docs:** decisions.md entry (the approach, the checks and their numbers, the first comparison's result and what it says about the trunk tint); backlog rows: streamed layers not in the TLAS, blended materials ignored by the reference, sky/clouds not path traced; resume.md next steps.

- [ ] **Step 4: Debug run once** on the edge view with `--reference 4`: validation clean, checks 0 mismatches.

- [ ] **Step 5: Commit** — "Reference path tracer: first comparison and docs"
