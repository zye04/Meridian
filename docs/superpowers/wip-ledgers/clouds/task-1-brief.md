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

