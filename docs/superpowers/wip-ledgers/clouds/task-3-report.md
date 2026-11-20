# Task 3 report: Cloud march, lighting and haze

Status: DONE_WITH_CONCERNS. Commit `3941e8c` "Ray-march volumetric clouds from the weather map" (branch `clouds`, base 43da194, not pushed, no trailer).

## What I implemented

- **`clouds_march.comp`** (new pass `"clouds"`, runs after `"scene late"` and before `"taa"`):
  - Resolution: quarter resolution, or half when `cameraInCloudLayer`. That function checks whether the camera is inside the local layer or within 250 m of it. The local layer is computed from `Weather::stateAt` at last frame's camera, using the tallest cumulus top.
  - Outputs: RGBA16F (pre-exposed in-scattered luminance with the haze in front, plus transmittance) and R32F (transmittance-weighted mean cloud distance along the ray in m, 0 = no cloud).
  - Geometry: the ray is unjittered and built like the AP LUT's. The layer is the type envelope (1.8-4.5 km), placed as a sphere shell around the planet center below the camera, which matches M1's curvature warp. Shell intersections use a stable quadratic with `c = (h-a)(2R+h+a)`.
  - Steps: 64 across the segment, at least 60 m, growing as 1.2% of distance, at most 128. The start is jittered by IGN, animated per frame (the engine has no blue-noise texture). The march stops at scene depth, the sea-level sphere, 300 km, or T < 0.01.
  - Lighting: a 5-step sun march (40 m, doubling each step). Sun color comes from the transmittance LUT, with an explicit planet-shadow test. Without that test the LUT returns the transmittance to the ground, and clouds would glow after sunset. Phase is dual-lobe HG (0.8 / -0.5, 30% back). Multiple scattering uses 3 Wrenninge octaves (a 0.25, b 0.7, c 0.5). Edge darkening lowers albedo (0.98 down to 0.5 below density 0.15). Ambient is sky irradiance / 2π with a 0.35 to 1 gradient from bottom to top. Each step uses Frostbite's energy-conserving integration.
  - Haze: `L = L_haze(0..d)(1 - T) + T_haze L_cloud` at the mean depth. Within 128 km it reads the AP LUT. Beyond that it extends the LUT's last value using a Bruneton transmittance ratio and the sky-view luminance.
- **`clouds_composite.frag`**: a bilinear fullscreen composite with premultiplied blend into HDR (`L + T·dst`), using the existing `alphaBlend` pipeline option. It skips pixels whose full-res surface is nearer than the clouds' mean depth. It writes HDR only, no motion.
- **`clouds.glsl`**: `cloudDensity(world, lod, out heightFraction)` is added as an overload, and the old signature is kept as a wrapper. The shape changes are described under Concerns 1.
- **Plumbing**:
  - `renderer_clouds.cpp`: march set layout (depth, storage x2, sampled x2), with one set per resolution. Pipeline layout is {atmosphere, bindless, march set}. Also adds `createCloudTargets` / `destroyCloudTargets` (called from `createSwapchain` / `destroySwapchain`, so a resize recreates them) and `drawClouds` (barriers and depth transition included).
  - `renderer_frame.cpp`: the late pass now stores depth instead of DONT_CARE. The next frame's depth transition also waits on FRAGMENT reads. Adds the pass call and `setCloudsEnabled`.
  - `--clouds on|off` in `main.cpp`.
- **Docs**:
  - CLAUDE.md: `--clouds`, the pass list, the Frame section, the march descriptor layout, the Layout section, and new useful views (ground, above, inside, silhouette, dusk).
  - decisions.md: a new section "Cloud march, lighting and haze" with sources (Frostbite 2016, Wrenninge 2013, Nubis 2022, Bruneton, Jimenez IGN), and a note on the shape cut added to the density bullet.

## Captures

All runs were Debug at 2560x1440 under `flock /tmp/meridian-gpu.lock ./build/game/game --camera ... --capture out.png` (120 frames). Every run reported **0 validation errors**. These were the first executions of `cloudDensity` and the `textures3D[]` lookups, and they were clean. The swapchain resize at startup (637x695 to 2560x1440) exercised target recreation, also with no errors. Outputs are in the scratchpad `t3/`. Final constants (`f_*`):

| Capture | Args | GPU total | clouds ms | What I saw |
|---|---|---|---|---|
| clear ground | `1119 1355.9 -431 -1.55 0.15 --weather clear` | 2.00 | 0.33 | No clouds. MAE vs `--clouds off` is 3.1/65535 (0.012/255). MAE vs the Task 2 baseline is 4.06/65535. Run-to-run noise between two identical `--clouds off` runs measured 5.8/65535, so this is within noise. |
| fair ground | same, default fair | 1.98 | 0.49 | Separate cumulus with flat grey bases and domed tops, roughly 30% of the sky. Rerun MAE vs first run 2.07/65535 (deterministic). |
| broken ground | `--weather broken` | 1.90 | 0.41 | Larger and more numerous clouds, plus big overhead masses with gaps. |
| overcast ground | `--weather overcast` | 1.85 | 0.20 | A continuous dark-grey sheet with no holes once the stratus shape floor was in (before it, blue holes showed). The terrain is still sunlit; that is Task 5. |
| clear above | `1119 6500 -431 -1.55 -0.25 --weather clear` | 3.39 | 0.33 | No clouds. |
| fair above | same, fair | 3.67 | 0.61 | Cumulus field over terrain to the horizon, white sunlit tops, haze increasing with distance, curving with the horizon. |
| broken above | `--weather broken` | 3.76 | 0.70 | Dense field. |
| overcast above | `--weather overcast` | 3.43 | 0.39 | Flat grey sheet top covering the ground. |
| inside (half res) | `1119 3200 -431 -1.55 0` | 3.10 | 0.84 | The camera sits between cumulus. Clouds above and below the eye line are soft (half res plus TAA). |
| window edge | `1119 6500 -431 -1.55 -0.1` | 3.37 | 0.60 | The center row is about 30 km out (map edge). Inspected a full-res crop of rows 640-1140: the cloud field is continuous, with no seam, density step or size change. |
| dusk (sun +2°) | `... 0.15 --sun -0.7 0.035 0.7` | 2.07 | 0.45 | Clouds turn orange/salmon with darker cores. The sky reddens. |
| sun below horizon (-2.9°) | `--sun -0.7 -0.05 0.7` | 2.35 | 0.47 | Clouds are dark blue-grey silhouettes against the twilight sky. They do not glow. |
| silhouette | `1119 1356 -431 -1.55 0.03` | 2.02 | 0.43 | Clean sample-wall edge against clouds (inspected a full-res crop). The composite's depth test keeps clouds off geometry. |

Tuning iterations I looked at:
1. With the plain product, clouds were flat-bottomed slabs with vertical walls and no gaps.
2. A debug pass printed base-noise threshold fractions, which gave the measured shape range.
3. After the range stretch and the cut, the result is proper cumulus.
4. Overcast showed holes, so I added the stratus floor.
5. Raising extinction from 0.03 to 0.06 and Wrenninge b from 0.5 to 0.7 brightened the tops, which had been grey.

Tests: `cmake --build build -j` OK (only the existing driverLeak warnings). `ctest --test-dir build`: 100% passed.

## Cost

These are Debug numbers for the GPU pass, one run each; Release median-of-5 is Task 7. The `clouds` pass costs 0.49 ms on the ground and 0.61 ms above the layer for fair weather, and 0.84 ms inside the layer at half resolution. **Clear costs 0.33 ms.** That is the floor of marching an empty layer, because every step does a weather lookup, and beyond the 30 km window that lookup is the analytic fallback (history binary search plus Worley). With M4's ~1 ms still having to cover the resolve, shadow map and capture, this is tight. Levers, which I have not applied:
- coarser steps, or one weather lookup per several steps, beyond the window;
- skipping the march when coverage is 0 everywhere (global state and history);
- fewer light steps or lower kSteps.

## Files changed

New: `engine/shaders/clouds_march.comp`, `engine/shaders/clouds_composite.frag`.
Modified: `engine/CMakeLists.txt`, `engine/include/meridian/renderer.h`, `engine/shaders/clouds.glsl`, `engine/src/render/{renderer_clouds.cpp, renderer_frame.cpp, renderer_impl.h, renderer_setup.cpp}`, `game/src/main.cpp`, `CLAUDE.md`, `docs/decisions.md`.

## Self-review

I read the full diff. The shared files got small hunks: setup +2 lines; frame +18 (setWeather layer, setter, depth store, depth src stage, pass call); impl.h members. The new code lives in renderer_clouds.cpp and the new shaders.

Barriers:
- Depth goes from late-test write to SHADER_READ_ONLY for compute and fragment.
- A fragment-to-compute WAR barrier covers the march targets, which both frames in flight share.
- A compute write to fragment sampled-read barrier precedes the composite.
- An HDR color-attachment write to read/write barrier precedes the blend.
- The next frame's depth transition now includes FRAGMENT in its source stages.

Push constants are COMPUTE|FRAGMENT on the march layout, pushed for both bind points. With clouds off, depth stays in DEPTH_ATTACHMENT layout, and the next frame transitions from UNDEFINED anyway. The no-attribution check on the commit message returned 0.

## Concerns

1. **The density deviates from Task 2's "plain product" ruling.** The base Perlin-Worley shape turned out to span only about 0.78-0.92, so the plain product rendered flat slabs with no gaps. I now stretch the shape over that measured range and cut the product `coverage × profile × shape` at a fixed type-dependent level (stratus 0.1, cumulus 0.35). There is still no second coverage threshold; coverage stays thresholded once, in the map. I also added the controller's stratus shape floor (`mix(shape, 1, 0.6·coverage·(1-type))`). All of this is recorded in decisions.md. The measured range is a hard-coded constant, like Task 2's noise mean and deviation; regenerating the noise with more contrast would be the cleaner fix.
2. **Visible structured dither** on distant clouds, as comb or vertical streaks at 20+ km and on the far ground field. It comes from the IGN-jittered coarse steps at quarter resolution and the bilinear upsample, and TAA clamping keeps it. Task 4's temporal resolve should remove it. If it doesn't, the step growth (1.2%) is the lever.
3. **Cost floor of 0.33 ms in clear weather** comes from the fallback weather lookups on every empty step (see Cost).
4. The inside-layer decision uses last frame's camera position for `stateAt`; the 250 m margin covers the one-frame lag. The AP LUT and the march share the unjittered ray; the depth sample is the full-res depth at the quarter-res pixel center (point sample).
5. Overcast lighting is dim and blue-tinted, because ambient is the clear-sky irradiance. Task 6's capture will fix this. Cloud shadows on terrain are Task 5.
6. I report Debug GPU timings only, since the GPU shader code is identical in Release. The Release median of 5 is left to Task 7.

## Fix round 1 (commit 5fc9df2, on top of 3941e8c)

### Changes
- **Important #1 (coverage is a plain multiplier):** `cloudDensity` now computes `density = coverage * saturate(remap(profile * shape, cut, 1))` and then erodes it, using the controller's formula. The stratus shape floor now depends on type only (`mix(shape, 1, 0.6 (1 - type))`), so coverage appears nowhere except as that multiplier. decisions.md now describes the code exactly:
  - The Density bullet says coverage is a plain multiplier on the cut of profile x shape, and the stale "plain product" and "the march then cuts" text is gone.
  - The Shape bullet gives the exact formula.
  - clouds.glsl comments are updated to match.
- **Steps:** `dt = max(max(60 m, 0.012 t), (tEnd - t) / (128 - i))`. This follows the distance rule and widens only where the remaining steps couldn't reach the segment's end. The old `segment/64` base step is gone, and the jitter offset uses the same rule at `tStart`.
- **Light march:** it is skipped when `sunLight` is zero (night or planet shadow) and breaks once the light sample is above the layer envelope top.
- **Depth transition:** the source stage is now EARLY|LATE fragment tests.

### Verification
- `cmake --build build -j`: OK. `ctest --test-dir build`: 100% passed.
- Captures were Debug, 2560x1440, `flock /tmp/meridian-gpu.lock ./build/game/game --camera ... --capture` (scratchpad `t3/r1_*`). All had **0 validation errors**.

| Capture | Args | clouds ms (before) | What I saw |
|---|---|---|---|
| fair ground | `1119 1355.9 -431 -1.55 0.15` | 0.89 (0.49); rerun 0.81 | Same cumulus field, with gaps and domed tops kept and no slab walls at cell edges. The before/after crop of mid-distance clouds (`ab.png`, rows 580-830, x 1300-2300) shows the comb/vertical striping clearly reduced and more detail inside the clouds. Some fine dither remains for Task 4. Rerun after the comment-only cleanup: MAE 0.036/255, near run-to-run noise. |
| overcast ground | `... --weather overcast` | 0.29 (0.20) | Continuous grey sheet, no holes. |
| above layer | `1119 6500 -431 -1.55 -0.25` | 0.84 (0.61) | Same field. The comb pattern is still visible on mid-distance clouds seen from above: a 1-2 km slab gets only ~8 steps at 1.2% of 20 km. Left for Task 4. |
| 30 km edge | `1119 6500 -431 -1.55 -0.1` | 0.71 (0.60) | Inspected a full-res crop of rows 640-1140: continuous cloud field, no seam. |
| clear ground | `... --weather clear` | 0.46 (0.33) | No clouds. MAE vs `--clouds off` is 0.027/255, near the 0.022/255 run-to-run noise. |
| inside (half res) | `1119 3200 -431 -1.55 0` | 1.37 (0.84) | Under the 2 ms inside-layer target, up 63%. |
| sun below horizon | `... --sun -0.7 -0.05 0.7` | 0.47 | Against fair ground's 0.81-0.89, this shows the light-march skip working. |

### Concern (cost)
Following the distance rule costs time:
- ground fair 0.49 to 0.81-0.89 ms;
- above 0.61 to 0.84 ms;
- clear 0.33 to 0.46 ms;
- inside 0.84 to 1.37 ms.

Clouds alone now exceed the ~1 ms M4 budget at the ground and above views once the shadow map, capture and resolve are added. Per the constraints I am reporting rather than cutting. Levers:
- kStepGrowth (1.2%; 2% would roughly halve the long-ray steps);
- kMaxSteps (128);
- a cheaper or coarser weather lookup beyond the 30 km window, which is also the clear-weather floor;
- fewer light steps;
- Task 4's temporal resolve, which may allow coarser steps with less visible striping.
