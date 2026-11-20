# M4 Clouds and Weather Implementation Plan

**Status:** done, merged to main 2026-11-20 (Tasks 1-8; measurements in decisions.md "Clouds and weather measured").

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A game-driven weather state with a shared global wind, and volumetric clouds that read it: drifting with the wind, fronts arriving from upwind, lit from clear to overcast, dimming the sun through a cloud shadow map and the ambient through a sky + cloud capture, without smear.

**Architecture:** CPU `Weather` integrates state, wind and a travel-distance history in fixed game-time substeps and uploads `Wind`/`WeatherGpu` blocks (current and previous) into the scene buffer. Per frame on the GPU: an analytic weather map around the camera, a Beer cloud shadow map, a time-sliced sky + cloud capture feeding the sky irradiance; after the scene passes a quarter-resolution (half inside the layer) cloud march, a temporal resolve and a depth-aware composite that writes HDR color and cloud motion vectors only.

**Tech Stack:** C++20, Vulkan 1.3, GLSL compute, doctest.

**Spec:** `docs/superpowers/specs/2026-11-19-clouds-weather-design.md` (read it with this plan).

## Global Constraints

- Branch `clouds` in its own worktree (created from main by superpowers:using-git-worktrees); its own `build/` directory; `assets/` is a symlink to the main checkout's `assets/` (git-ignored, includes the Napf heightfield). Never touch the main checkout's `build/` or `assets/`. Release perf builds in the session scratchpad.
- **GPU lock (user, 2026-11-19):** every run of the game binary (captures and perf runs, Debug or Release) goes through `flock /tmp/meridian-gpu.lock <game binary> ...`. Hold it only for the run itself, never for builds.
- Conventions (CLAUDE.md): physical units, pre-exposure, camera-relative positions, reverse-Z, motion vectors previous - current in UV, atmosphere in km. Every struct marked "must match" changes with its GLSL twin (static_assert sizes).
- The cloud composite reads scene depth and writes HDR color and motion only (independent of M2b's prepass).
- Shared files (`common.glsl`, `mesh.frag`, `renderer_frame.cpp`, `gpu_types.h`, `renderer_setup.cpp`) get small, additive hunks; new code goes in new files (`renderer_clouds.cpp`, `clouds*.comp`, `weather.glsl`, `wind.glsl`).
- Defaults: fair-weather cumulus; cumulus base 2.5 km, tops up to 4.5 km above sea level; stratus/overcast 1.8-3.0 km; weather map 512² over ~60 km around the camera, texel-snapped; front line 60 km upwind of the world origin; history covers ~250 km of air travel; base noise 128³, detail 32³; shadow map 1024² over ~30 km.
- Budget: ~1 ms for clouds + shadow map + sky capture at 2560x1440 at the ground and above-layer views (Release, median of 5); inside the layer reported separately, target <= 2 ms. Over budget: report the numbers with the levers (step counts, resolution inside the layer, shadow-map and capture slicing) to the user before cutting anything.
- No new third-party dependencies.
- Each task is one commit with a what-and-why message, **no AI attribution / Co-Authored-By trailer**; CLAUDE.md and `docs/decisions.md` (with sources) updated in the same commit as the change they describe.
- Every task: `cmake --build build` (Debug: ASan+UBSan+validation) succeeds, `ctest --test-dir build` passes, every capture reports 0 validation errors.

## Review Focus

1. Camera crossing the cloud base or top while flying (resolution switch quarter <-> half, history weight change): no pop, no one-frame black or doubled clouds (Task 4 fly-through capture sequence across the base).
2. Sun at or below the horizon (dusk) and sun straight overhead: shadow map projection stays valid (no NaN from a degenerate basis), clouds go dark at night instead of glowing (Task 5 test for the sun-aligned basis at elevation 90° and -5°; Task 3 dusk capture).
3. Wind speed 0 and wind direction reversing: weather stays defined (no division by zero in front travel), clouds still evolve through the time coordinate (Task 1 tests with speed 0 and a 180° change).
4. Thin mountain silhouettes and terrain edges in front of moving clouds: no cloud history bleeding onto the terrain or halo (Task 4 silhouette motion capture, crops inspected).
5. Extreme time scale (time-lapse 600x) at 60 fps: fixed substeps keep weather and wind deterministic and stable (Task 1 test: 1 vs 60 substeps per second give the same state within 1e-5).

---

### Task 1: Weather state, wind and history (CPU + scene upload)

**Files:**
- Create: `engine/include/meridian/weather.h`, `engine/src/weather.cpp`, `engine/shaders/wind.glsl`, `engine/shaders/weather.glsl` (GPU structs + history lookup), `tests/weather_test.cpp`
- Modify: `engine/include/meridian/renderer.h` (`setWeather`), `engine/src/render/gpu_types.h` + `engine/shaders/common.glsl` (`GpuWind`, `GpuWeather` in `SceneData`: current and previous wind; address of the history buffer), `engine/src/render/renderer_frame.cpp` (upload), `renderer_setup.cpp` (history buffer), `engine/CMakeLists.txt`, `tests/CMakeLists.txt`, `game/src/main.cpp` (`--weather clear|fair|broken|overcast`, `--time-scale X`, `--weather-front SECONDS`; capture output prints coverage/type/wind at the camera), `CLAUDE.md` (Commands, Conventions: the wind contract), `docs/decisions.md` (weather/wind design, sources)

**Interfaces:**
- Produces in `weather.h`:
  - `struct WindState { Vec3 direction; /* unit, y = 0 */ float speed10m = 3.0f; float gustiness = 0.3f; };`
  - `struct WeatherState { float coverage = 0.35f; float type = 1.0f; float precipitation = 0.0f; WindState wind; };` plus `WeatherState weatherPreset(std::string_view name)` for `clear`, `fair`, `broken`, `overcast` (unknown name throws).
  - `class Weather { public: explicit Weather(uint32_t seed, const WeatherState &start); void setTarget(const WeatherState &target, float seconds); void update(double gameSeconds); const WeatherState &current() const; WeatherState stateAt(Vec3 worldPosition) const; Vec3 windOffset(float altitude) const; /* xz used, y = 0 */ double travel() const; uint32_t seed() const; std::span<const WeatherHistorySample> history() const; };` with `struct WeatherHistorySample { float travel; float coverage, type, precipitation; };` (history interval and length: builder's choice within the spec's ~250 km rule).
  - `Vec3 windVelocityAt(const WindState &, float altitude)` (xz, y = 0; the engine has no Vec2) (power-law speed from 10 m, veer with altitude).
  - Constants: `kFrontLineUpwind = 60000.0f` (m from the world origin), `kWeatherSubstep = 0.25` (game s).
- Produces: `void Renderer::setWeather(const Weather &weather);` (call every frame before `draw`; the renderer keeps last frame's wind as `previousWind`).
- `wind.glsl`: `vec2 windVelocity(Wind w, float altitude)`, `vec2 windOffset(Wind w, float altitude)`, `float gust(Wind w, vec2 worldXZ)`. `weather.glsl`: `WeatherSample weatherAt(SceneBuffer s, vec3 world)` reading the history (same rule as `Weather::stateAt`).

- [ ] **Step 1: Failing tests** in `tests/weather_test.cpp`:
  - blend from `fair` to `overcast` over 100 s: value and first derivative continuous at both ends (finite differences, 1e-4);
  - `update` with 1, 4 and 60 calls per game second for 3600 s gives the same `current()` and `travel()` within 1e-5 (time scale 600 included);
  - `windOffset` equals the integral of velocity across a speed change 2 -> 12 m/s (no jump);
  - a state change at t0 reaches a point D m downwind of the front line at t0 + D / speed (within one history interval);
  - speed 0 for 600 s: `stateAt` defined (no NaN) and equal to the last arrived state; a 180° direction change keeps `stateAt` finite;
  - `stateAt` upwind of the front line returns `current()`; beyond the history returns the oldest sample.
- [ ] **Step 2:** Implement; `setWeather` uploads; `main.cpp` drives a `Weather` from the presets and flags (default `fair`).
- [ ] **Step 3: Verify.** Tests pass; capture (under the GPU lock) prints the weather line; image unchanged vs the previous commit (mean difference within run-to-run noise).
- [ ] **Step 4:** Commit ("Add the weather state and the shared global wind").

---

### Task 2: Noise volumes and the weather map

**Files:**
- Create: `engine/src/render/renderer_clouds.cpp`, `engine/shaders/cloud_noise.comp` (base 128³ Worley-Perlin RGBA8, detail 32³ Worley RGBA8, 2D curl noise; at load), `engine/shaders/weather_map.comp`, `engine/shaders/clouds.glsl` (shared cloud functions: remap, profiles, density)
- Modify: `renderer_impl.h`, `renderer_setup.cpp` (images, pipelines), `renderer_frame.cpp` (pass `"weather"` before `"atmosphere"`), `gpu_types.h` + `common.glsl` (`GpuClouds`: layer heights per type, map window origin/size, image indices), `engine/CMakeLists.txt`, `game/src/main.cpp` (`--dump-weather-map out.png`, debug), `docs/decisions.md`

**Interfaces:**
- Consumes: Task 1 `WeatherGpu`/history, `windOffset`.
- Produces: weather map image (RGBA8: R coverage, G type, B precipitation), window texel-snapped around the camera; `clouds.glsl`: `float cloudDensity(vec3 world, float lod)` (weather x vertical profile x base noise eroded by curl-distorted detail), `vec4 weatherMapAt(vec3 world)` with the coarse analytic fallback outside the window.

- [ ] **Step 1:** Implement noise generation (once at load) and the per-frame map (noise at `xz - windOffset(cloud altitude)`, time coordinate, `remap(n, 1 - coverage, 1)` from `weatherAt`).
- [ ] **Step 2: Verify.** `--dump-weather-map` for `fair` and `overcast`: plausible cell structure, no grid seams, no window-edge discontinuity with the fallback; two dumps 1 game minute apart at 5 m/s show cells moved downwind ~300 m (texel measurement); `"weather"` ms reported.
- [ ] **Step 3:** Commit ("Generate cloud noise and the analytic weather map").

---

### Task 3: Cloud march, lighting and haze (first visible clouds)

**Files:**
- Create: `engine/shaders/clouds_march.comp`
- Modify: `renderer_clouds.cpp`, `renderer_frame.cpp` (pass `"clouds"` after `"scene late"`, before `"taa"`), `renderer_setup.cpp` (quarter- and half-resolution targets), `clouds.glsl`, `game/src/main.cpp` (`--clouds off`), `CLAUDE.md` (Frame; Useful views: fair cumulus ground view, above-layer ~6500 m, inside-layer, mountain silhouette), `docs/decisions.md`

**Interfaces:**
- Produces: march output per pixel: RGBA16F (in-scattered luminance pre-exposed, transmittance) + R32F transmittance-weighted mean depth (m); `bool cameraInCloudLayer` decision (inside or within a margin) selects quarter vs half resolution. A simple bilinear composite (replaced in Task 4) writes HDR only.
- Lighting: short sun march through the layer; sun color/illuminance from the atmosphere transmittance at the sample; dual-lobe HG phase; Wrenninge multiple-scattering octaves (2-3); edge darkening by lowered albedo; ambient from the sky irradiance with a bottom-to-top gradient; haze `L = L_haze(0..d) + T_haze(0..d) * L_cloud` (aerial-perspective LUT within its range, sky-view/transmittance beyond). The march stops at scene depth.

- [ ] **Step 1:** Implement (blue-noise jittered start, distance-growing steps).
- [ ] **Step 2: Verify.** Stills at the ground view for `clear` (mean difference vs Task 2 within noise: no clouds), `fair`, `broken`, `overcast`; the above-layer view; the inside-layer view; a dusk view (`--sun` 2° above the horizon) where clouds redden and darken, and a sun-below-horizon run with no glowing clouds. Inspect each.
- [ ] **Step 3:** Commit ("Ray-march volumetric clouds from the weather map").

---

### Task 4: Temporal resolve, depth-aware composite and cloud motion vectors

**Files:**
- Create: `engine/shaders/clouds_resolve.comp`, `engine/shaders/clouds_composite.comp`
- Modify: `renderer_clouds.cpp` (history ping-pong per resolution), `renderer_frame.cpp`, `renderer_setup.cpp`, `game/src/main.cpp` (`--capture-sequence N` writes `out_NNN.png` every N frames, for time-lapse and fly-through sequences), `CLAUDE.md`, `docs/decisions.md`

**Interfaces:**
- Consumes: Task 3 outputs; prepass-independent contract (reads depth; writes HDR color and motion only).
- Resolve: reproject by weighted cloud depth + wind displacement since last frame (`windOffset` of `wind` vs `previousWind` at the cloud altitude), variance clipping, disocclusion reset, lower history weight inside the layer; history reset when switching resolution.
- Composite: depth-aware (bilateral on full-resolution depth) upsample; blend by transmittance; where cloud opacity > 0.5, write cloud motion (weighted depth with the current and previous camera, plus the wind displacement) into the motion target.

- [ ] **Step 1:** Implement.
- [ ] **Step 2: Verify (user's motion checks), all under the GPU lock:**
  - fast turn (yaw rate equivalent via `--camera-velocity` circling, or a scripted turn flag if needed, builder's choice), flying toward the layer, flying through it (sequence across the base), and the summit silhouette in front of clouds drifting at 10 m/s: Laplacian ratio vs the still of the same view for each, reported, and each no worse than the same view with `--clouds off` beyond run-to-run noise;
  - silhouette crops at full resolution: no cloud history bleeding onto terrain, no halo (inspect);
  - fly-through sequence: no pop at the resolution switch.
- [ ] **Step 3:** Commit ("Resolve clouds temporally with their own motion vectors").

---

### Task 5: Cloud shadow map

**Files:**
- Create: `engine/shaders/cloud_shadow.comp`, `engine/shaders/cloud_shadow_blur.comp`
- Modify: `renderer_clouds.cpp`, `renderer_frame.cpp` (in `"weather"`), `gpu_types.h` + `common.glsl` (`GpuClouds`: shadow map basis, origin, extent, image index), `engine/shaders/mesh.frag` (sun illuminance x cloud shadow transmittance; beyond the map, mean transmittance from the fallback coverage), `clouds_march.comp` (light march beyond the layer uses the map), `tests/weather_test.cpp` (or `tests/clouds_test.cpp`), `docs/decisions.md`

**Interfaces:**
- Produces: `float cloudShadow(vec3 world)` in `clouds.glsl`; CPU `struct SunBasis { Vec3 right, up, forward; }; SunBasis cloudShadowBasis(Vec3 sunDirection);` (in `weather.h`, used to build the projection; stable for any sun direction).
- Map: 1024², ~30 km, texel-snapped, RGBA16F or R11G11B10F (front depth, mean extinction, max optical depth); `T = exp(-min(maxOD, meanExt * max(0, d - front)))`; bilateral blur on extinction and optical depth only.

- [ ] **Step 1: Failing tests:** `cloudShadowBasis` orthonormal for sun elevations 90°, 45°, 2°, -5°; a CPU Beer-shadow evaluation over a synthetic density column matches brute-force optical depth within 5% at 10 depths.
- [ ] **Step 2:** Implement.
- [ ] **Step 3: Verify.** `broken` ground view: cloud shadows on the terrain match the clouds' positions against the sun (inspect a high oblique view); `clear` unchanged vs Task 4 within noise; `overcast`: terrain sun term strongly dimmed.
- [ ] **Step 4:** Commit ("Dim the sun with a Beer cloud shadow map").

---

### Task 6: Sky + cloud capture, ambient and the time-lapse

**Files:**
- Create: `engine/shaders/sky_capture.comp`
- Modify: `engine/shaders/sky_irradiance.comp` (integrates the capture instead of the sky-view LUT alone), `renderer_clouds.cpp` (capture image, slicing), `renderer_frame.cpp`, `mesh.frag` (specular sky fallback reads the capture where it read the sky-view LUT, if cheap; else unchanged), `CLAUDE.md`, `docs/decisions.md` (UE sky light recapture, Frostbite)

**Interfaces:**
- Produces: capture image (octahedral ~64² or 6 x 32², builder's choice) of sky-view LUT + coarse cloud march, updated over a few frames; `skyIrradiance` keeps its buffer and meaning (and adds SH-L1 terms only if cheap).

- [ ] **Step 1:** Implement.
- [ ] **Step 2: Verify.**
  - `clear` with coverage 0: image within run-to-run noise of Task 5;
  - `fair`, `broken`, `overcast` ground and above-layer stills: overcast flat, dim and diffuse after exposure adapts (exposure value printed), clear unchanged;
  - time-lapse: `--weather fair --weather-front 600 --time-scale 600 --capture-sequence 60` over enough frames for the front to cross the crop from the above-layer view and from the ground view: the front arrives from upwind, coverage builds without popping between frames, lighting dims smoothly (inspect the sequence).
- [ ] **Step 3:** Commit ("Light the scene from a sky and cloud capture").

---

### Task 7: Cloud look development (user, 2026-11-19)

The clouds work but look wrong against real photos: grey instead of white, smoke-puff shapes, uniform small cells, flat color, low contrast. This task makes them match the references through a measured comparison loop.

**Files:**
- Modify: `engine/shaders/clouds.glsl`, `clouds_march.comp`, `weather_map.comp`, `cloud_noise.comp` (as the loop requires), `game/src/main.cpp` (only if a capture flag is missing), `CLAUDE.md` (look-dev views), `docs/decisions.md` (every look choice with sources)
- Create: `tools/cloud-compare.sh` (builds the side-by-side comparison images and prints the measured targets)

**Inputs:** reference photos in `assets/reference/clouds/` (git-ignored: `cloud1.jpg`..`cloud6.jpg`, `images.jpg`); source notes (HZD 2015, Nubis 2017, Frostbite 2016, Wrenninge et al. 2015, Revision 2013 course notes, the GameDev.net HZD thread, Sebastian Lague's Clouds).

**Targets (user):**
1. White, not grey: albedo ~0.99 (scattering ≈ extinction), cumulus extinction ~0.04-0.1 /m; dual-lobe Henyey-Greenstein (forward ~0.8 blended with back ~-0.3) for silver linings; Wrenninge multiple-scattering octaves (3-4; extinction, scattering and eccentricity each attenuated ~0.5 per octave) instead of the powder hack.
2. Shape: flat sharp bases, billowing cauliflower tops, crisp edges: per-type height profile (sharp base, rounded top); base shape eroded by high-frequency Worley detail (inverted toward the base for wisps, billowy toward the top); curl-noise distortion at edges; the temporal upsample must not soften edges.
3. Scale: fair-weather cumulus 0.5-2 km wide with real vertical development; weather-map cells sized to match, clustered with gaps rather than uniformly spaced.
4. Color: sun light at each sample tinted by the atmosphere transmittance at that sample (pink/orange tops at sunset); ambient from skylight above (blue) vs ground bounce below by height in the cloud; aerial perspective on distant clouds.
5. Contrast: shaded bases darker than the adjacent sky, sunlit tops near-white.

- [ ] **Step 1:** `tools/cloud-compare.sh`: captures (under the GPU lock) matched to the references — midday ground view looking up (vs `cloud1.jpg`, `cloud5.jpg`), a scattered-cumulus wide view toward the horizon (vs `cloud3.jpg`, `cloud4.jpg`), sunset from above the layer (vs `cloud2.jpg`) — each placed beside its reference in one image; prints measured targets: sunlit-top luminance / adjacent clear sky (target 1.5-3x at midday), base luminance / adjacent sky (target < 1), base flatness (vertical spread of base heights in the frame).
- [ ] **Step 2: Loop**, one change at a time: change, rebuild, run the script, record the numbers and the comparison image path in the report, keep or revert. Until all five targets hold and the comparisons read as the same kind of cloud.
- [ ] **Step 3: Verify** nothing regressed: Task 4's motion checks (silhouette crop, turn sky crop), clear vs `--clouds off` within noise, 0 validation errors; "clouds" ms before/after (Debug).
- [ ] **Step 4:** Publish the final comparison images to the user (artifact page or files) and get their OK on the look before Task 8. Then commit ("Develop the cloud look against reference photos").

---

### Task 8: Measurement and wrap-up

**Files:**
- Modify: `docs/decisions.md`, `docs/backlog.md` (rows: precipitation, wet surfaces, lightning, fog banks/valley mist, light shafts, cumulonimbus are later milestones; no local cloud occlusion of ambient until GI)

- [ ] **Step 1:** Release build in the scratchpad; under the GPU lock, median of 5 at 2560x1440: `"weather"` + `"clouds"` ms at the ground view (`fair`, `overcast`), the above-layer view, and the inside-layer view; frame totals. Over ~1 ms (or 2 ms inside): stop and report numbers and levers to the user.
- [ ] **Step 2:** Sweep CLAUDE.md, decisions.md and the specs for statements this milestone made stale (sky irradiance from the sky-view LUT alone, "no clouds", the frame order, the roadmap's M4 description where it conflicts with the approved spec: report roadmap conflicts to the user instead of editing it).
- [ ] **Step 3:** Commit ("Measure clouds and weather").

---

### Rebase and merge (after Lane A reports M1 complete)

- [ ] Rebase `clouds` onto main; resolve overlaps (M1 Task 6 curvature warp, Task 7 terrain UVs, walk mode) keeping both sides' intent; rebuild; tests; re-run the Task 4 motion captures and the Task 6 overcast and clear stills under the GPU lock; compare with the pre-rebase numbers.
- [ ] Final whole-branch review (opus), then fast-forward main to `clouds` and push; tell the user so M2a can start on top.
