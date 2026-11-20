# Task 2 report: Noise volumes and the weather map

Status: DONE_WITH_CONCERNS. Commit `656a033` "Generate cloud noise and the analytic weather map" (on `clouds`, base 7cede13, not pushed, no trailer).

## Implemented

- **cloud_noise.comp** (one dispatch at load, `createClouds`): base 128^3 RGBA8 (R Perlin-Worley = `remap(perlin, 0, 1, worleyFbm, 1)`, GBA Worley fBm at 4/8/16 cells), detail 32^3 RGBA8 (Worley fBm at 2/4/8), curl 128^2 RGBA16F (curl of 2D Perlin fBm, about [-1, 1]). Every lattice wraps, so the volumes tile. Base and detail get 3D mips through blits (`lod` in `cloudDensity` uses them).
- **weather_map.comp**, new pass `"weather"` placed after the barrier that waits for the previous frame's reads and before `"atmosphere"`: 512^2 RGBA8 (R coverage, G type, B precipitation) over 60 km around the camera. The corner is snapped to whole texels in double. Per texel it reads the local state through `weatherAt` (the history rule), evaluates weather noise at `xz - windOffset(3 km)` with a time coordinate (1 unit = 1800 game s), sets coverage = `remap(n, 1 - c, 1)` (c = 0 gives 0), sets type = local type plus up to ±0.16 of 32 km Perlin variation, and passes precipitation through unchanged.
- **Weather noise**: inverted Worley fBm in 4 km cells times Perlin fBm 16 km wide. It is mapped to an approximately uniform distribution with a logistic normal-CDF approximation. The mean (0.46) and single-octave deviation (0.18) were measured from dumps, and the deviation is scaled per octave count like a weighted mean of independent octaves. As a result, coverage c covers about c of the area for both the 3-octave map and the 1-octave fallback. Before this mapping, fair covered only 0.5% and the fallback band had visibly different statistics.
- **clouds.glsl** (contents named for Task 3): `remap`, tileable `gradientNoise`/`worleyNoise` and their octave versions, `weatherNoise`, `weatherFromNoise`, `vec4 weatherMapAt(vec3 world)` (map inside the texel centers, 1-octave fallback beyond it; the map's outer 64 texels blend to the fallback, so the edge texels equal it), `cloudLayer(type, coverage)`, `cloudProfile(type, h)`, and `float cloudDensity(vec3 world, float lod)`. `cloudDensity` takes world xz, and `world.y` is the altitude above sea level on the curved shell, so the march decides the curvature. It returns [0, 1] and the march scales that to extinction. The 3D volumes are read through `textures3D[]`, which aliases bindless binding 1. Include order: common.glsl, weather.glsl, clouds.glsl. It reads `pc.scene`.
- **GpuClouds / `Clouds`** in SceneData/SceneBuffer (96 B, SceneData 1072 -> 1168): layer heights (cumulus 2500/4500, stratus 1800/3000), map origin/size/resolution, wrapped wind offset, time coordinate and its period, noise periods, and bindless indices (weatherMap, baseNoise, detailNoise, curlNoise). It is filled in `setWeather` (offset, time) and `draw` (`placeWeatherMap`).
- **Ruling 1 (precision)**: all cloud noise periods (map cell 4096, base 8192, detail 1024, curl 4096) divide `kCloudNoisePeriod` = 2^18 m, which a static_assert checks. `Weather::windOffsetModulo(altitude, period)` (new, double) supplies the single uploaded offset, which keeps 1.6 cm resolution. The time coordinate wraps at 64 units. A new unit test covers this. It is recorded in decisions.md.
- **Ruling 2a**: `GpuWind::gustPhasePeriod` takes over a pad slot. wind.glsl reads `w.gustPhasePeriod`, and the GLSL constant is gone.
- **Ruling 2b**: `--weather` now goes through a `text()` helper ("missing value for --weather") and calls `weatherPreset` during parsing. `--dump-weather-map out.png` was added; it implies 120 frames when `--frames` is not given.
- **Plumbing**: `renderer_clouds.cpp` (create/destroy, place, pass, `Renderer::dumpWeatherMap`), `writeTexture` gains a layout parameter (the weather map is bindless in GENERAL), and CLAUDE.md (commands, pass list, layout, frame, descriptor sets, wind convention) plus decisions.md (new section with sources) are updated.

## Verification (all game runs under `flock /tmp/meridian-gpu.lock`, 2560x1440, Debug)

- `cmake --build build -j`: OK. The only warnings are the existing `driverLeak` unused-variable ones in untouched files.
- `ctest --test-dir build`: 100% passed (22865 assertions), including the new "wind offset modulo a period stays precise after long travel".
- Weather map dumps (`--frames 30 --weather W --dump-weather-map`), all with 0 validation errors and `weather` at 0.10-0.11 ms:
  - fair: R>0 on 35.1% of texels, G 0.85-1.0. broken: R>0 on 62.5%, G 0.45-0.76. overcast: R>0 on 100%, G 0.0-0.16. clear: R = 0 everywhere. B = 0 everywhere (no preset has precipitation).
  - Inspected (R channels side by side): clustered cumulus cells of a few km in fair, more and larger in broken, near-full in overcast. No grid seams. The outer band shows the coarser fallback blobs. The G channel (contrast-stretched) shows smooth low-frequency type variation with no seams.
- Window edge versus fallback: dumps with the camera at x = 1119 and x = 61119 (exactly one window apart) were compared column by column. |A[:,511] - B[:,0]| mean is 8.21/255 (R), while neighbouring columns differ by 8.24 and 7.96 and interior columns by 9.27. G: 0.06 against 0.05/0.06. The boundary is continuous.
- Drift: `--time-scale 0 --frames 2` (t = 0) against `--time-scale 60 --frames 60` (t = 60 s), fair, camera fixed. Phase correlation on the interior R channel measured a shift of (+3.18, +1.05) texels = (373, 123) m, |d| = 393 m. The expected value is windOffset(3 km) over 60 s = (382, 139) m, |d| = 407 m: fair's 3 m/s at 10 m becomes 6.78 m/s at 3 km, veered 20°. The direction matches within 2° and the distance within 3.5%. The remaining difference comes from the subpixel fit and the cells evolving along time. The brief's "~300 m at 5 m/s" assumes a 5 m/s wind at cloud altitude. No preset gives that, so I measured against the real wind instead of adding a wind-speed flag.
- `--weather bogus` fails in 0.06 s, before any window opens ("unknown weather preset bogus ..."). `--weather` and `--dump-weather-map` with no value report "missing value for --weather" and "missing value for --dump-weather-map".
- Ground capture (`--capture`, default view): 0 validation errors, renders normally (textures still resolve after the bindless table grew by 4 slots). GPU passes: `upload + tlas 0.04, terrain select 0.09, cull early 0.01, weather 0.10, atmosphere 0.02, scene early 1.31, ...`.

Outputs are in the session scratchpad (`wm_*.png`, `r_all.png`, `edge_*.png`, `drift_*.png`, `ground.png`).

## Files changed

New: `engine/shaders/{cloud_noise.comp, weather_map.comp, clouds.glsl}`, `engine/src/render/renderer_clouds.cpp`.
Modified: `engine/CMakeLists.txt`, `engine/include/meridian/{renderer.h, weather.h}`, `engine/shaders/{common.glsl, wind.glsl}`, `engine/src/render/{gpu_types.h, renderer_frame.cpp, renderer_impl.h, renderer_setup.cpp, renderer_terrain.cpp}`, `engine/src/weather.cpp`, `game/src/main.cpp`, `tests/weather_test.cpp`, `CLAUDE.md`, `docs/decisions.md`.

## Self-review

I read the full diff. The changes to shared files are small and additive: two lines in setup, about 10 in frame, a struct append in common.glsl and gpu_types.h, and a default parameter on `writeTexture`. The weather map's write-after-read hazard against the previous frame is covered by the existing atmosphere barrier (the map is recorded right after it), and the pass ends with a compute-write to compute|fragment sampled-read barrier. Noise tiling holds on every axis: xz wraps at 2^18 m, which every period divides; t wraps at 64, and the octave periods 64/128/256 and the /4 and /8 variants stay integral. The dump adds a compute-to-copy barrier after `vkDeviceWaitIdle`.

## Concerns

1. **`cloudDensity` and `textures3D[]` are compiled but never run.** Nothing calls them until Task 3, so the 3D descriptor aliasing on bindless binding 1 has not been checked by the validation layer at runtime, and the density constants (erosion 0.35, curl 150 m, profiles) are untuned. Task 3 should confirm both with its first capture.
2. **Overcast is not solid as specified.** `remap(n, 1 - c, 1)` at c = 1 gives R = n, which spans [0, 1] with dark Worley creases. With the HZD density threshold (`remap(shape, 1 - R, 1) * R`), overcast will show gaps where R is low. Task 3 will probably need a type- or coverage-dependent response (for example, saturating R for stratus) to get flat overcast. That is a deviation from the spec's verbatim remap, so it is for the controller or the user to decide.
3. **Task 4 needs the previous frame's offset.** For reprojection it needs the cloud wind displacement since last frame. `GpuClouds` only carries the current wrapped offset. Differencing the unbounded `wind`/`previousWind` floats loses precision late in play (about 25 cm ulp after 2300 km of travel at 3 km, against about 0.1-0.2 m of motion per frame). The fix is to upload the per-frame step computed in double, or the previous wrapped offset with a consistent wrap.
4. **Profiles are analytic instead of the spec's "small type texture"**, a builder's choice recorded in decisions.md. Type values above 1 (cumulonimbus) are clamped to 1 in the RGBA8 map.
5. The drift check used the fair preset at 6.8 m/s at cloud altitude, not 5 m/s (see Verification).

## Fix round 1 (commit 43da194, on top of 656a033)

Changes:
- **Coverage is thresholded once (review Important #1, controller ruling).** `weatherFromNoise` now sets `R = saturate((n - lo) / w)` with `lo = (1 - c)(1 + w) - w` and `w = kCoverageEdge = 0.15`. This is the ruling's `saturate(remap(n, 1 - c, 1 - c + w))`, with the threshold stretched so that c = 0 gives R = 0 exactly and c = 1 gives R = 1 everywhere. With n roughly uniform, R > 0.5 covers `c(1 + w) - w/2`. `cloudDensity` is now `coverage * cloudProfile(type, h) * shape`, eroded by detail. The `1 - R` threshold is removed and `shape` is clamped to [0, 1].
- **Erosion varies with type.** Cumulus: wispy at the base and billowy above (`billow = type * saturate(4h)`), carving up to 0.35. Stratus: wispy throughout, carving up to 0.5 (`kDetailErosion = vec2(0.5, 0.35)`, mixed by type).
- **Type clamp.** The fallback clamps type to [0, 1], the same as the RGBA8 map.
- **Layouts.** The noise volumes stay in GENERAL. The load-time transition to SHADER_READ_ONLY_OPTIMAL is replaced by a memory barrier, and the bindless descriptors are written with GENERAL, so the storage set (bindings 0-2) and the bindless entries match the real layout.
- **Shared helper.** `offsetAloft(x, z, altitude)` (anonymous namespace in weather.cpp, double) now serves both `windOffset` and `windOffsetModulo`.
- **Docs.** decisions.md now describes the density (single threshold and why, erosion by type), the map threshold formula and that it replaces the spec's remap, the type clamp at the edge, and the measured R > 0.5 fractions. CLAUDE.md now says all four cloud images stay in GENERAL.

Verification:
- `cmake --build build -j`: OK. `ctest --test-dir build`: 100% passed (weather_test included).
- `flock /tmp/meridian-gpu.lock ./build/game/game --frames 30 --weather W --dump-weather-map f1_W.png`. Every run had 0 validation errors and `weather` took 0.11 ms:

  | preset | fraction of texels with R > 0.5 | R > 0 | R min / mean | G range |
  |---|---|---|---|---|
  | clear (0) | 0.000 | 0.000 | 0 / 0 | 0.85-1.0 |
  | fair (0.35) | 0.331 | 0.400 | 0 / 0.331 | 0.85-1.0 |
  | broken (0.65) | 0.648 | 0.722 | 0 / 0.649 | 0.45-0.76 |
  | overcast (1.0) | 1.000 | 1.000 | 1.0 / 1.0 | 0.0-0.16 |

  B = 0 in all four. Inspected (R side by side): fair and broken show separate cells with crisp soft edges and no seams; overcast is solid white.
- Edge continuity (broken, cameras x = 1119 and 61119, one window apart): R across the seam differs by 5.57, against 6.41 and 5.51 between neighbouring columns; G by 0.13 against 0.14 and 0.12. The boundary is continuous.

Still open from the first report: concerns 1 (cloudDensity and textures3D are not exercised until Task 3) and 3 (Task 4 needs a precise per-frame cloud wind step). Concern 2 is resolved by this round.
