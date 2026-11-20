# Task 4 report: temporal resolve, depth-aware composite, cloud motion vectors

Status: DONE_WITH_CONCERNS (minor; see Concerns). Commit `9cca0e4` on `clouds`, not pushed.

## Implementation

- **Wind displacement (ruling 1):** `setWeather` computes it on the CPU: `windOffsetModulo(kCloudWindAltitude, 4096)` (reduced in double, float-precise to ~0.25 mm), differenced with last frame's value and re-wrapped with `std::remainder`. That gives `GpuClouds.windDisplacement` (vec2, xz m). It replaces `pad[1..2]`, so the size stays 96, and it has `offsetof == 88` asserted. The GLSL twin is `Clouds.windDisplacement`. The wrapped 2^18 m offsets are never differenced. It is 0 on the first frame.
- **March:** writes depth `-1` where scene geometry lies in front of the layer (`tEnd > tStart && sceneDistance <= tStart`). Without this marker no pass would know the texel's air was hidden.
- **`clouds_temporal.glsl` (new):** holds the `CloudConstants` push block (scene, historyValid, inLayer) and its C++ twin in `gpu_types.h`. It also has `cloudRay(uv)` and `cloudPreviousClip(direction, d)`. That function flattens the displayed position with the current curvature drop, adds the camera delta, subtracts the wind displacement, and applies `displayedPosition` for the previous camera, then multiplies by `previousViewProjection`. When d = 0 it reprojects at infinity (the sky's rule).
- **`clouds_resolve.comp` (new):** runs at the march resolution.
  - Reprojection depth is the texel's own depth. A clear texel uses the deepest cloud in its 3x3.
  - History is sampled with a 9-tap Catmull-Rom and clipped to the RGBA box of mean ± 1σ over the 3x3 neighbours that saw the layer (geometry texels excluded).
  - Blend weight for the current frame: 0.10 outside the layer, 0.25 inside.
  - Depth is blended by opacity, with history depth taken from `textureGather`.
  - The history restarts when any of these holds:
    - history is invalid (first frame, swapchain, or a resolution change, tracked by `cloudHistoryResolution`);
    - the reprojected UV falls off-screen;
    - any gathered history texel is `-1` (disocclusion).
  - A restarted texel takes its 3x3 mean, not the single jittered sample.
- **`clouds_composite.comp` (new, replaces `clouds_composite.frag`):** full-resolution compute.
  - The bilateral rule works per 2x2 bilinear tap:
    - a geometry tap (-1) counts as clear for a geometry pixel and is skipped for a sky pixel;
    - a cloud tap deeper than the pixel's surface counts as clear;
    - if a sky pixel's 4 taps all saw geometry, it falls back to the 4x4 texels that saw the sky.
  - The composite does a premultiplied read-modify-write on HDR.
  - Where opacity > 0.5 it writes motion = previous − current UV, computed from the opacity-weighted depth.
  - Contract: reads scene depth, writes HDR and motion only. HDR and motion gained `STORAGE` usage. Clouds leave them in GENERAL, and the TAA transition in `renderer_frame.cpp` picks its old layout and stages from `cloudsEnabled`.
- **Renderer:**
  - histories `[resolution][parity]`: color RGBA16F + depth R32F;
  - one 9-binding set per resolution × parity, selected by `frameNumber % 2`;
  - three compute pipelines on one layout;
  - barriers: previous frame's compute writes → this frame's compute reads/writes, then march → resolve → composite.
- **`taa.comp`:** unchanged (ruling 4: not needed).
- **game:**
  - `--camera-yaw-rate RAD_PER_S` mirrors `--camera-velocity`: it ends at `--camera`'s yaw and needs `--frames`/`--capture`.
  - `--capture-sequence N` writes `<stem>_NNN.png` every N frames and needs `--capture`.

## Motion checks

All runs: Debug, 2560x1440, `flock /tmp/meridian-gpu.lock ./build/game/game ... --capture X.png` (120 frames). Every run reported **0 validation errors**. Laplacian is measured on the whole frame. Outputs are in `scratchpad/t4/final/`, with `results.txt` alongside.

| Check | Command (after `--camera`) | Clouds on (moving/still) | `--clouds off` | `clouds` ms (still / moving) |
|---|---|---|---|---|
| Fast turn | `1119 1355.9 -431 -1.55 0.15 --camera-yaw-rate 1.0` | 0.0678/0.0922 = **0.735** | 0.0692/0.0941 = **0.735** | 0.90 / 0.96 |
| Toward the layer | `1119 1900 -431 -1.55 0.1 --camera-velocity -60 20 0` | 0.0383/0.0506 = **0.757** | 0.0386/0.0511 = **0.755** | 1.10 / 0.90 |
| Through the base (2150→2650 m) | `1119 2650 -431 -1.55 0.1 --camera-velocity -30 250 0` | 0.0365/0.0515 = **0.709** | 0.0367/0.0517 = **0.710** | 1.55 / 1.60 (half res) |
| Silhouette, clouds drifting ~10 m/s (`--time-scale 1.5` vs `0`) | `1119 1356 -431 -1.55 0.03` | **1.000** | **1.000** | 0.82 / 0.96 |
| Silhouette + strafe 10 m/s | `... --time-scale 1.5 --camera-velocity 0 0 10` | 0.0846/0.1146 = **0.738** | 0.0863/0.1171 = **0.738** | 0.82 |
| Inside the layer, 30 m/s (extra) | `1119 3200 -431 -1.55 0 --camera-velocity -30 0 0` | 0.0470/0.0506 = 0.929 | 0.0519/0.0553 = 0.939 | 1.48 / 1.60 |

- **Run-to-run noise:** an identical rerun gives MAE 0, so the noise is effectively zero. Every clouds-on ratio is within ±0.002 of the clouds-off ratio, except inside the layer (−0.010, which is a different scene: clouds fill the view).
- **Engine baseline:** TAA keeps 48% at the 5 m/s ground view (CLAUDE.md). These views keep 71–76%.
- **Sky-only crop** (top 800 rows, clouds on): turn 0.698, toward 0.996, fly 0.959.
- **Wind drift (rated 10 m/s; `fair` at 3 km is 6.8 m/s, × time-scale 1.5):** the camera looks west into a wind from 270°, so the drift is mostly radial.
- **Cloud motion vectors A/B** (temporarily set `kMotionOpacity = 2.0`, i.e. no cloud motion; reverted, and the committed file was checked):
  - At normal speeds there is no measurable difference (sky MAE between the two ≤ 0.04%). Clouds are kilometres away, so the sky's rotation-only motion is already nearly right.
  - At a 200x time-lapse with lateral drift (`1119 1355.9 -431 0 0.15 --time-scale 200`), the sky Laplacian is 0.00707 with cloud motion and 0.00672 without (+5%). This confirms the vectors are correct in sign and magnitude.
- **GPU timing:** `clouds` ms vs Task 3 (Debug): ground 0.89 → 0.82–0.97, above 0.84 → 0.87, inside 1.37 → 1.48–1.60. The resolve and composite add about 0.1 ms. No Release median-of-5 was run for this task.
- **Inflated reading:** the sequence run printed `clouds 8.85`. That number is an artifact of a capture readback every 2 frames, not a cost: the same path without `--capture-sequence` gives 1.56 ms. This is noted in CLAUDE.md.

## Silhouette-crop inspection (full resolution, zoomed)

- **Marble wall top and corner against clouds**, strafing at 10 m/s with drifting clouds (`final/silcrops.png`, `c2.png`): the edge is sharp, with no cloud colour on the wall and no clear halo in the sky along the edge.
- **First version's disocclusion strip (fixed):** the first version reset revealed texels to the raw jittered sample. That left a blocky noise strip about 150 px wide behind the moving wall corner (`c_wall.png`, bottom). Restarting from the 3x3 mean of layer-seeing neighbours removed it: only soft, faint streaks remain.
- **Summit and ridges against drifting clouds** (`silcrops.png`, top): clean, no halo.

## Fly-through sequence across the resolution switch

- **Run:** `--camera 1119 2650 -431 -1.55 0.1 --camera-velocity -30 250 0 --capture-sequence 2` (`t4/seq/fly_000..058.png`).
- **Expected events:** the switch (2250 m) at frame 24, which is sequence index 11, and the cumulus base (2500 m) at index 41.
- **Frame-to-frame MAE (16-bit):** ~600 steady around the switch (index 10–13: 599, 585, 595, 582) and ~520 around the base (index 40–42: 535, 519, 505), with no spike anywhere. The only outlier is index 0→1, which is exposure settling at startup.
- **Visual check** (montage of 10–13 and 40–42): no pop, no black frame, no doubled clouds.

## Ruling 2 (comb striping)

- **Above-layer view** (`1119 6500 -431 -1.55 -0.25`, `final/above_mid.png`): after accumulation, mid-distance clouds show no comb. The jitter pattern is resolved.
- **Ground view, distant horizon clouds** (`t4/zoom1.png`): a faint regular texture at a 4-px period remains. It is the quarter-resolution texel grid showing through the bilinear/bilateral upsample, plus some residual per-texel variation. It is not step aliasing. It is subtle at 1:1.

## Files

- **New:** `engine/shaders/clouds_resolve.comp`, `engine/shaders/clouds_composite.comp`, `engine/shaders/clouds_temporal.glsl`
- **Deleted:** `engine/shaders/clouds_composite.frag`
- **Modified:** `engine/shaders/clouds_march.comp`, `engine/shaders/common.glsl`, `engine/src/render/gpu_types.h`, `renderer_clouds.cpp`, `renderer_frame.cpp` (two small hunks), `renderer_impl.h`, `renderer_setup.cpp` (usage flags), `engine/CMakeLists.txt`, `game/src/main.cpp`, `CLAUDE.md`, `docs/decisions.md`

## Self-review

- **Struct twin:** `GpuClouds` matches its GLSL twin. In std430 the vec2 lands at 88 after `pad0` at 84, and this is asserted. `CloudConstants` is 16 bytes, which is the same range the march's default `pc` block reads (scene only).
- **Barriers:** HDR and motion are COLOR_ATTACHMENT → GENERAL before the cloud passes and GENERAL → SHADER_READ_ONLY for TAA. When clouds are off, the old path is unchanged. The cross-frame history dependency is covered by a compute→compute barrier with SAMPLED_READ in its dst.
- **Descriptor pool:** 28 storage, 12 samplers, 5 sets, which matches 4 sets × 6 storage + 3 samplers, plus the generator set.
- **Swapchain recreate:** rewrites all 4 sets with the new HDR, motion and depth views and resets `cloudHistoryResolution`. `historyValid` is false then anyway.
- **Build:** Debug (ASan/UBSan/validation) builds clean. My one new warning (snprintf buffer) is fixed. ctest: 1/1 passed.
- **Diff:** read in full before committing.

## Concerns

1. **Static 4-px texel-grid texture on distant horizon clouds (ground view):** a quarter-res upsample artifact, not a comb. Possible levers are a smarter (bicubic) upsample in the composite or blue-noise jitter. Neither was done.
2. **Cloud motion vectors make no measurable sharpness difference at real speeds:** they only matter at extreme time-lapse (+5% at 200x). They are correct, but the motion bar was already met by TAA plus the sky's rotation-only vectors for distant clouds.
3. **Silhouette drift check is mostly radial:** with the wind from 270° and the summit view looking west, clouds drift toward the camera. The lateral drift check was done at a different yaw (0).
4. **Debug timings only:** no Release median-of-5 was run. The resolve and composite add about 0.1 ms in Debug.

---

# Fix round 1 (commit `4f0d61f`)

Status: DONE_WITH_CONCERNS. Outputs are in `scratchpad/t4/fix/`. All runs were Debug at 2560x1440, used `flock /tmp/meridian-gpu.lock ./build/game/game --camera ... --capture X.png` (120 frames), and reported 0 validation errors. Laplacian means `magick X -crop RECT -colorspace gray -morphology Convolve Laplacian:0 -format "%[fx:standard_deviation]" info:`.

## Silhouette with lateral drift (Important #1)

- **View:** `1119 1356 -431 2.5 0.12`, the summit against the cloud band. The 3 km wind blows toward +x, which is 80% across the view at yaw 2.5. I picked yaw 2.5 over 0 or 3.14 because at those yaws no silhouette meets clouds at this location.
- **Crop:** `1000x340+900+560`, covering the summit, the ridgeline and the clouds above them.

| Run | Crop Laplacian | Ratio vs frozen |
|---|---|---|
| `--time-scale 0` (frozen) | 0.0376837 | — |
| `--time-scale 1.5` (~10 m/s) | 0.0377381 | **1.001** |
| `--time-scale 15` (~100 m/s) | 0.0377236 | **1.001** |
| `--clouds off`, ts 0 / 1.5 | 0.038082 / 0.038082 | 1.000 (still camera: true by construction, not evidence) |

- **Inspection** (`lat_crops.png`: frozen, 1.5x, 15x, zoomed 1.6x): the ridgeline is sharp, with no smear on the clouds and no halo or cloud colour on the summit.
- **Wall-corner strafe**, rerun on the new build (`wall.png`): unchanged and clean. Whole-frame ratio is 0.738 with clouds on and 0.738 off.
- **decisions.md:** the 1.000 sentence is replaced by these numbers. It now states that clouds-off is 1 by construction for a still camera.

## Crop-based on vs off ratios (moving / still)

| Case | Crop | Clouds on | Clouds off |
|---|---|---|---|
| Fast turn (`1119 1355.9 -431 -1.55 0.15 --camera-yaw-rate 1.0`) | sky `2560x800+0+0` | 0.698 | 0.685 |
| Fast turn | horizon band `2560x400+0+600` (low clouds, ridges, wall top) | **0.728** | **0.733** |
| Toward the layer (`1119 1900 -431 -1.55 0.1 --camera-velocity -60 20 0`) | sky `2560x800+0+0` | 0.996 | 0.995 |
| Toward the layer | band `2560x400+0+600` | 0.909 | 0.908 |

- Deterministic reruns match with MAE 0, so run-to-run noise is about 0. By that standard, the fast-turn horizon band is 0.005 below clouds-off (0.7% relative): measurable but small.
- My reading is that the 4-px upsample grid on the low clouds in that band loses a little more detail under motion than plain sky does. I did not redesign anything for it, per the instructions.
- Every other crop has clouds on at or above clouds off.

## Disocclusion inside the layer (implemented, small change)

- **March:** `cut = depth > 0 && tEnd > tStart && sceneDistance < tEnd`. It catches geometry in front of the layer or inside the marched segment. A cut texel stores its depth as `-1 - d`. Geometry in front of the layer gives d = 0, i.e. -1, the old encoding. Clouds in front of the surface keep their colour.
- **Resolve:**
  - decodes the stored depth;
  - builds neighbourhood stats from texels with the same cut state;
  - rejects history touching cut texels only when the current texel is not cut;
  - re-encodes the resolved depth.
- **Composite:** skips a cut tap for sky pixels and decodes it for geometry pixels.
- **Effect on existing views:** MAE vs round 0 is ~5/65535 (strafe, inside layer, above), with 0 validation errors.
- **Coverage gap:** the inside-layer path can't be exercised with current content. Terrain tops out at 1.41 km and the lowest layer starts at 1.8 km. The in-front path is the same code with d = 0, and it is exercised by the wall strafe.

## Docs

- **CLAUDE.md Known gaps:**
  - the pre-exposed cloud history is not rescaled on exposure change;
  - static 4-px quarter-res grid;
  - step-aliasing bands.
  The levers live in decisions.md: bicubic or 4x4 bilateral upsample, blue-noise jitter, smaller mid-distance steps.
- The CLAUDE.md views and frame description are updated for the lateral silhouette view, its crop, and the `-1 - d` encoding.

## Concerns

1. **Fast turn, horizon band:** 0.728 with clouds on vs 0.733 off. A 0.7% relative drop, measurable given deterministic runs.
2. **Static comb remains after accumulation** (ruling 2, capture `fix/comb.png` from `lat_ts0.png`, crop `450x200+1330+560`): a mid-distance cumulus seen from below shows soft vertical banding plus the fine 4-px horizontal texel grid on a frozen 120-frame accumulation. This is step aliasing and upsample structure, not jitter noise. It is recorded as a known gap with its levers; the march was not redesigned.
3. **Inside-layer cut path:** untested, because no current geometry reaches the cloud layer.

---

# Fix round 2 (commit `2a924dd`): static bands and 4-px grid

Status: DONE. Outputs are in `scratchpad/t4/r2/`. All runs were Debug at 2560x1440 under `flock /tmp/meridian-gpu.lock` and reported 0 validation errors. The comb view is `1119 1356 -431 2.5 0.12 --time-scale 0`, crop `450x200+1330+560` (as `fix/comb.png`).

## Diagnosis (before changing anything)

- **Static, not residual noise:** between frames 120 and 121 the crop MAE is 9/65535 (`exp.sh base`).
- **Not the resolve:** with variance clipping off (gamma 1000), and with clipping off plus 2% current weight over 400 frames, the bands are unchanged (`cmp2.png`).
- **The jitter is involved:** with the march jitter fixed at 0.5, the same bands get stronger (`cmp1.png`).
- **Cause:** the march jittered only its start, `t0 = tStart + u * step0`, and each later step depended on `t`. Where the step rule is `(tEnd - t) / (128 - i)` (long, low rays through the 1.8–4.5 km envelope, i.e. mid-distance clouds seen from below), the start offset shrinks to `u * step * (128 - i) / 128`. Later samples therefore barely move between frames, accumulation can't cover their intervals, and the bands stay. This is the "jitter applied only at the start, later steps snap to a grid" case.
- **4-px grid:** I measured it as the periodic part of the per-row mean |2nd derivative| by row phase mod 4 (`grid.py`: max − min over mean).

| Image | Row value | Note |
|---|---|---|
| base | 0.384 | |
| converged reference | 0.322 | so it's deterministic |
| flat sky / terrain | 0.02–0.07 / 0.009 | reference levels |

  I attribute it to bilinear upsampling: its slope breaks at texel centres show up on smooth vertical cloud gradients.

## Changes

1. **March (`clouds_march.comp`):** step boundaries no longer depend on the jitter. Each sample sits at `t + u * dt`, the same per-frame IGN fraction of its own step. The last step is clamped to end exactly at `tEnd`. Mean depth uses the sample position.
2. **Composite:** where all 4x4 low-res texels agree with the pixel's surface (none skipped or read as clear), it uses separable Catmull-Rom weights, with the result clamped to rgb ≥ 0 and T in [0, 1]. Otherwise it falls back to the old bilateral 2x2 bilinear path, so nothing rings across silhouettes. The depth used for motion takes only the positive weights.
3. **Resolve:** a restarted texel now uses a 1-2-1 tent mean instead of a box mean.
   - Why: with per-step jitter, single-frame noise alternates between neighbouring columns (IGN's x step), and the 3x3 box mean left vertical stripes in the strafe disocclusion strip.
   - Result: the tent cancels the alternation exactly, and the strip is clean (`wall_tent.png` vs `wall.png`).
- **Ruled out:** jittering the light march as well made no difference (`cmp5.png`), so I reverted it.

## Before / after

- **Comb crop:** `comb_before_after.png` (top: round 1, bottom: final). The horizontal bands and grid are gone.
- **Contrast-stretched 4x zoom:** `cmp7.png` shows the cloud body is smooth. Grid metric (rows): 0.384 → 0.305 (per-step jitter) → 0.158 (Catmull-Rom). The remaining value is curvature kinks of a C1 interpolant, not visible.
- **Fair ground view** (`gcmp.png`, crop `1100x500+1300+550`): the hatched/comb texture on the distant clouds is gone.
- **What remains:** thin dark ribbons on the cloud's underside. They are also in the converged reference and don't change when the light march is jittered, so they come from the density field (shape cut / profile), not sampling. That is out of this task's scope; it's noted in decisions.md.

## Round-1 checks still hold

| Check | Round 1 | Final |
|---|---|---|
| Silhouette crop `1000x340+900+560`, drifting / frozen | 1.001 | 1.000 (0.99971) |
| Fast turn, sky `2560x800+0+0` | 0.698 | 0.701 |
| Fast turn, band `2560x400+0+600` | 0.728 | 0.728 |

(Clouds-off: 0.685 sky / 0.733 band, unchanged.)

- **Fly-through sequence** (`r2/seq`): frame-to-frame MAE around the switch (indices 8–14) is 567–602, and around the base (39–43) 503–534. No spike.
- **Wall strafe crop:** clean (`wall_tent.png`).

## Cost (`clouds` ms, Debug, median of 3)

| View | Before | After |
|---|---|---|
| Ground | 0.91 | 1.08 |
| Above the layer | 0.85 | 1.05 |
| Inside the layer | 1.49 | 1.67 |

- **Breakdown:** the new march with the old composite measured ~0.99 at the ground, so the cost splits roughly half to the exact last step and half to the 16-tap upsample. Run-to-run spread is about ±0.07.
- **Not yet profiled:** these are Debug numbers; no Release run was done.

## Docs

- **decisions.md:**
  - the march entry now describes per-step jitter;
  - the composite entry describes Catmull-Rom with the bilinear fallback;
  - the round-1 "known gaps" bullet is replaced by the diagnosis, numbers, cost and the remaining underside ribbons;
  - the restart text says tent mean.
- **CLAUDE.md Known gaps:** only the pre-exposed cloud history remains; the grid/band entry is dropped.
