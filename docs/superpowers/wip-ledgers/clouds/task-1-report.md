# Task 1 report: Weather state, wind and history

Status: DONE_WITH_CONCERNS (concerns are minor, see below)
Commit: 7cede13 Add the weather state and the shared global wind

## Implemented
- `engine/include/meridian/weather.h`, `engine/src/weather.cpp`: WindState, WeatherState,
  weatherPreset (clear 0 / fair 0.35 / broken 0.65 type 0.6 / overcast 1.0 type 0; winds 3/3/6/8 m/s;
  unknown name throws std::invalid_argument), windVelocityAt, Weather (all interface names/signatures
  from the brief), kFrontLineUpwind = 60000, kWeatherSubstep = 0.25.
  Extra public items: `double Weather::time() const` (needed for the gust phase), constants
  kWeatherHistoryInterval = 1000 m, kWeatherHistoryCapacity = 256, kWindShearExponent = 1/7,
  kWindVeer = 0.35 rad, kWindVeerAltitude = 1000 m. `WindState::direction` defaults to {1,0,0}
  (the brief's `Vec3 direction;` would default to zero, violating "unit").
- Blend: smoothstep in absolute game time (C1); direction by angle along the shorter arc.
- update(absolute game seconds): fixed 0.25 s substeps (midpoint rule) for offset/travel/history,
  then extrapolation from the last substep to the exact time with the same midpoint velocity
  (continuous offsets at any frame rate; bitwise identical at substep times).
- History: one sample per 1 km of travel, 256 samples (~255 km), fixed-capacity vector kept
  oldest-first (erase front when full) so `history()` is a contiguous ordered span. stateAt follows
  the controller's rule; speed 0 never divides (spans guarded).
- WindState is the 10 m wind: power law (altitude clamped >= 1 m) and veer linear from 0 at 10 m to
  0.35 rad at 1 km (clockwise from above). Profile is time-invariant, so one 10 m offset integral
  serves every altitude.
- GPU: GpuWind (48 B), GpuWeather (32 B) in SceneData (944 -> 1072 B), `Wind`, `WeatherHistorySample`,
  `WeatherHistoryBuffer`, `Weather` in common.glsl; per-frame host-visible history buffer
  (renderer_setup.cpp); Renderer::setWeather + upload in draw (previousWind kept like the previous
  camera). History travel uploaded relative to the current travel (<= 0) for float precision.
  wind.glsl: windVelocity, windOffset, gust (40 m value-noise cells carried by the 10 m offset,
  evolving along gustPhase, periodic every 256 cells; amplitude = gustiness).
  weather.glsl: WeatherSample weatherAt(SceneBuffer, vec3) with binary search, same rule.
- game: --weather (default fair), --time-scale, --weather-front SECONDS (start -> overcast);
  non-interactive runs print the weather line.
- CLAUDE.md (Commands, Layout, Conventions: the wind contract) and docs/decisions.md (new section,
  spec sources plus power-law / Ekman references).

## Tests (tests/weather_test.cpp, 8 cases)
presets; wind profile; C1 blend fair->overcast over 100 s (FD h = 0.01 s, 1e-4 on value and slope at
both ends, coverage/type/speed); rate independence (1, 4, 60 per s and 10 game s per update = 600x at
60 fps; two setTargets incl. a wind turn; current, travel within 1e-5, offset and history size equal);
offset = integral across 2 -> 12 m/s (per-frame increments within [2, 12]*dt, total 1150 m, aloft
scaled/veered); front arrival at D/speed within one interval for D = 80 and 150 km; calm 600 s
(travel frozen, stateAt finite and = last arrived state) then 180 deg reversal over 60 s (finite,
unit direction, bounded coverage); history clamping (upwind -> current, 400 km downwind -> oldest,
capacity reached).

## TDD evidence
RED: `cmake --build build -j` with header + tests, no weather.cpp:
  83 x `undefined reference to meridian::...` e.g.
  `weather_test.cpp:(.text+0x261): undefined reference to meridian::weatherPreset(std::basic_string_view<char>)`
  `collect2: error: ld returned 1 exit status`
GREEN: `./build/tests/tests -tc=...` -> `test cases: 8 | 8 passed | 0 failed`, 13646 assertions.
Mutation check (then reverted): linear blend + start-of-step extrapolation -> 2 cases fail
(C1 blend, offset integral; 17 assertions).
Full suite: `ctest --test-dir build` 100% passed; `./build/tests/tests` 37/37 cases, 22857 assertions.

## Capture verification
`flock /tmp/meridian-gpu.lock ./build/game/game --camera 1119 1355.9 -431 -1.55 -0.1 --capture <scratch>/t1-new.png`
  Rendered 120 frames, GPU 1.70 ms, ..., 0 validation errors
  Weather at the camera: coverage 0.35, type 1.00, wind 3.0 m/s from 270 deg, gustiness 0.30 (game time 2 s)
Baseline: same command at 3e99e05 (temporary WIP commit, checkout, incremental build, then back and
soft reset) -> t1-base.png. `magick compare -metric MAE t1-base.png t1-new.png null:` -> `0 (0)`.
Front flags: `--weather clear --weather-front 600 --time-scale 300 --frames 120` -> coverage 0.00
at the camera at game time 600 s with wind 8.0 m/s (front still ~61 km upwind, as designed);
`--weather stormy` -> error message.
GLSL: nothing includes wind.glsl/weather.glsl yet, so they were compiled via a scratch compute shader
(glslc -O, vulkan1.3) calling weatherAt/windVelocity/windOffset/gust; SPIR-V offsets of SceneBuffer
wind/previousWind/weather = 944/992/1040, matching C++.

## Self-review
- Diff read in full; shared files got small additive hunks only.
- WeatherState wind in stateAt is the global current wind (spec: one global wind).
- A setTarget during a blend restarts from the current value (value-continuous, slope not); spec
  only requires C1 for a single transition.

## Concerns
- No CPU/GPU parity test for weatherAt / wind.glsl (no GPU test harness); verified by compile + layout
  only. First consumer (weather map task) should sanity-check against Weather::stateAt.
- Wind offset grows without bound (float on the GPU): ~0.06 m precision after 864 km of travel
  (1 day at 10 m/s); fine for M4, but long sessions may want a rebased offset later.
- "Altitude" in the wind profile is above ground for vegetation and above sea level for clouds
  (documented), so clouds over high terrain get slightly less shear than strictly physical.
- Wikipedia references added in decisions.md for the 1/7 power law and Ekman veer (not in the spec's
  source list).
