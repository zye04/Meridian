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

