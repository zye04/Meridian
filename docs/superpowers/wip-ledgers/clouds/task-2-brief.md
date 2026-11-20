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

