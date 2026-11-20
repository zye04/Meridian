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

