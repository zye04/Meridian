# Task 7 report: cloud look development against reference photos

Status: DONE_WITH_CONCERNS. Commit 782dd03. Artifacts are under `.superpowers/sdd/2026-11-19-clouds-weather/artifacts/t7/`: `iter-NN/` for each iteration (captures, `out.txt` with the metrics, `<view>-<photo>.png` side-by-sides, `sheet.png`), `final/` for the deliverables, `band/` and `probe/` for the experiments, `regress-before/` and `regress-after/` for Step 3.

## Tooling (Step 1)

- `tools/cloud-compare.sh BUILD OUT [game args]` runs three views under the GPU lock, holding it only for each run, with `--placement off`:
  - **ground:** `--camera 1119 1356 -431 -1.55 0.5 --sun 0.45 0.85 0.25`, compared with cloud1 and cloud5.
  - **wide:** the same at pitch 0.1, compared with cloud3 and cloud4.
  - **sunset:** `--camera 1119 5500 -431 -1.55 -0.2 --sun -0.998 0.05 0.03 --weather broken`, compared with cloud2. From iteration 17 on this view uses broken weather: cloud2 shows a dense field of heaps, and fair weather gave sparse puffs.
- Each view is captured three times: a `.png`, a `.pfm` with clouds on, and a `.pfm` with `--clouds off`. The script deletes the pfms after measuring them. It writes `<view>-<photo>.png` (capture beside photo, both scaled to 720 px high) and `sheet.png`.
- `tools/cloud-metrics.py` produces the numbers. This second file was added next to the script, following the existing `image-gates.py` / `motion-gates.sh` pattern.
  - **capture mode:** luminance in cd/m² (exposure divided out: pfm × 1.2 × 2^EV100). The cloud mask is the sky pixels (blue > red in the clouds-off image) where clouds-on differs from clouds-off by more than 5%. It reports:
    - `top`: 95th-percentile cloud luminance over the clear sky behind it, per pixel.
    - `base`: the median over bottom-edge pixels, taken 3 px inside each cloud.
    - `spread`: the bottom edge's 10–90% row range divided by cloud height, median over clouds of at least 0.05% of the frame. Clouds are found with a union-find over vertical runs (scipy isn't installed).
    - `mid`: median cloud luminance over the tops (shading contrast).
    - `cover`.
  - **photo mode:** colour segmentation on sRGB (clouds: saturation < 0.3 and value > 0.45; sky: blue). Values are linearized. It runs on the reference photos and also on the capture's PNG, so the comparison with the photos is like for like (both display-referred).
- Reference measurements (photo mode):

| Photo | top | mid | base | spread | cover |
|---|---|---|---|---|---|
| cloud1 | 11.1 | 0.39 | 2.49 | 0.29 | 0.32 |
| cloud5 | 11.4 | 0.59 | 2.75 | 0.39 | 0.23 |
| cloud3 | 6.25 | 0.63 | 2.10 | 0.35 | 0.44 |
| cloud4 | 4.68 | 0.63 | 2.03 | 0.34 | 0.35 |
| cloud2 | 12.1 | 0.64 | n/a | 0.58 | n/a |

  - The photos contradict two of the user's numeric targets. Their bases are 2–2.8× brighter than the sky just below them (target: < 1), and their tops are 4.7–11× the sky (target: 1.5–3×, and the photo whites clip, so the true ratio is higher). See Concerns.

## Iteration log

Release build. Iterations 00-14 ran at 1920x1080 (fallback output) and 15 onward at 2560x1440, because the monitor came on mid-task. Numbers are the capture-mode `top / base / spread` for ground and wide, plus cover from iteration 07 on. Each iteration's comparison images are in `iter-NN/`.

| # | Change | Ground | Wide | Notes | Kept |
|---|---|---|---|---|---|
| 00 | baseline | 3.89 / 1.08 / .33 | 3.40 / 1.08 / .34 | grey, smooth smoke-puff blobs; sunset beige and too bright | – |
| 01 | albedo 0.99, no edge darkening, phase -0.3 | 4.67 / 1.08 / .35 | 4.14 / 1.09 / .35 | slightly whiter | yes |
| 02 | octaves 4 × 0.5/0.5/0.5 (brief) | 3.59 / 1.05 / .35 | 3.22 / .92 / .36 | darker; brief value | yes |
| 03 | density gain 3 | 3.54 / .92 / .44 | 3.24 / .91 / .31 | crisper edges | yes |
| 04 | ground bounce in the ambient | 4.43 / .93 / .38 | 4.13 / .93 / .38 | undersides go from dark grey to light grey; clearly whiter | yes |
| 05 | detail tile 1024 → 512 m | 4.39 / .92 / .38 | 4.10 / .94 / .36 | textured edges | yes |
| 06 | base tile 8192 → 4096 m | 4.15 / .91 / .37 | 3.30 / .92 / .43 | smaller clouds, horizon field like cloud4; cover too high | yes |
| 07 | cumulus cut 0.35 → 0.5 | 4.68 / 1.06 / .39, cover .61 | 3.46 / 1.06 / .39 | | yes |
| 08 | fallback optical depth beyond the map | unchanged | .94 base | sunset tops 3078 → 2135 cd/m²; sunlit rims and dark bodies | yes |
| – | banding experiments (`band/`) | | | noise mip forced to 0: no change; `--render-scale 0.5`: only blurs | – |
| 09 | base ramp after the cut (flat bases) | 4.65 / .94 / .38 | 3.42 / .95 / .37 | | yes |
| 10 | shape frozen vertically below 25% | 5.33 / 1.09 / .49 | | straight-walled columns | reverted |
| 11 | same below 10% | 4.77 / 1.07 / .34 | 3.43 / 1.08 / .38 | barely visible | reverted (no gain for the code) |
| 12 | octave contribution 0.75 | 7.13 / 1.12 / .38 | 5.18 / 1.18 / .33 | display barely changes (auto-exposure); breaks Frostbite's energy rule | reverted |
| 13 | cumulus erosion 0.35 → 0.5 | 4.76 / 1.09 / .37, cover .54 | 3.56 / 1.08 / .34 | cauliflower edges | yes |
| 14 | base noise squashed ×2 vertically | 4.28 / 1.22 / .44, cover .50 | 3.31 / 1.21 / .46 | flat loaves, closer to cloud4 | yes |
| 15 | density gain 3 → 6 (now 1440p) | 3.98 / 1.16 / .39 | 3.17 / 1.15 / .42 | crisper, lobes visible | yes |
| 16 | composite clamp to the central 2×2 range | MAE 1.4e-4 | | no visible ring removed | reverted |
| 17 | cumulus cut 0.5 → 0.6 | 4.29 / 1.24 / .42, cover .34 | 3.16 / 1.22 / .42, cover .42 | cover now within the photos' range | yes |
| – | sunset view uses `--weather broken` | | | denser field of heaps, as in cloud2 | yes |

Two cleanups after the loop: the march now calls `cloudShadow()` instead of duplicating its map/fallback blend (equivalent; Debug MAE against the final ground capture is 7e-5, the same level as run-to-run noise), and a stale comment was fixed.

## Final numbers (`artifacts/t7/final/targets.txt`, Debug, 2560x1440)

- **1 White:** albedo 0.99, extinction 0.06/m at density 1 (cores saturate), HG 0.8/-0.3 with w 0.3, 4 octaves at 0.5/0.5/0.5, no powder or edge darkening. The method follows the brief exactly. Tops are 10.7k cd/m² against 2.8k sky. The display still reads light grey rather than white. **PARTIAL**: auto-exposure and the pale summer sky are not cloud parameters (backlog row).
- **2 Shape:** flat bases (base spread .42, photos .29-.39), billowy erosion at the top and wispy at the base, curl distortion, sharp edges (gain 6). The upsample is unchanged and no softer: the overshoot clamp was tested and had no effect. **MOSTLY**: the clouds are smoother and less cauliflowered than cloud1/cloud5, and the quarter-res stair-step shows on small distant clouds (backlog).
- **3 Scale:** 0.3-1.5 km loaves with real but modest vertical development; cover .34/.42 against the photos' .23-.44; clustered by the 4 km weather cells. **PASS-ish**: no single large 2 km cloud like cloud1/cloud5 at fair coverage. Sizes are fairly uniform.
- **4 Colour:** per-sample sun transmittance (already in place), sky ambient above and ground bounce below (new), aerial perspective (already in place). The sunset clouds have warm rims, but the bodies are slate blue and the atmosphere is not orange, unlike cloud2. **PARTIAL**.
- **5 Contrast:** top/sky 4.3 (ground) and 3.2 (wide) against the 1.5-3 target. Base/sky 1.24 / 1.22 against < 1. **FAIL against the numbers, but the photos measure the same way** (tops 4.7-11, bases 2.0-2.8 display-referred). The targets conflict with the references. The display-referred base (1.47 / 1.27) is still below the photos.

## Regression checks (Step 3, Debug, 2560x1440, same clock state, `regress-before/` = HEAD, `regress-after/`)

- **Validation:** 0 errors on every run (before, after, and the final compare). `ctest`: 1/1 passed.
- **Fast turn** (`1119 1355.9 -431 -1.55 0.15 --camera-yaw-rate 1.0`), moving/still Laplacian ratio:
  - Sky crop `2560x800+0+0`: on 0.852 → **0.794**, off 0.855.
  - Band crop `2560x400+0+600`: on 0.733 → 0.722, off 0.730.
  - The absolute moving Laplacian *rose* (0.0195 → 0.0202) and the still rose more (0.0228 → 0.0254), because there is more cloud detail. The ratio drop is detail the motion loses, not a new smear. Flagged nonetheless.
- **Silhouette with drifting clouds** (`1119 1356 -431 2.5 0.12`, crop `1000x340+900+560`), ts 1.5 / ts 0: 1.000 → 0.998.
- **Clear vs `--clouds off`:** MAE 1.4e-5 → 7.4e-5 normalized (0.019/255). 0.46 px over 1% fuzz before vs 0.32 after; the diff ×20 shows only terrain noise. Within noise.
- **`clouds` ms** (median of 3; `scene early` identical in both, so same clock state):

| View | Before | After |
|---|---|---|
| ground | 1.08 | 0.89 |
| wide | 1.05 | 0.81 |
| sunset (fair) | 1.31 | 0.86 |
| `weather` pass | 0.39 | 0.36 |

- **Underside ribbons:** gone (`regress-after/underside-before-after.png`).

## Carried issues

- **Contour banding:** the cause was the shape (profile × stretched shape gave stacked shelves). It was not jitter or TAA phases: mip 0 changed nothing, and render scale 0.5 only blurs. It is gone. A quarter-res stair-step remains (backlog).
- **Underside ribbons:** gone.
- **Far/dusk self-shadow:** fixed. The march adds `cloudShadow` (map blended into the fallback) beyond its 1.2 km. The fallback's k fit predates the new shape (backlog: refit).
- **Map-edge band bias:** the fallback function was not changed, so I did not recheck it with another view. The decisions.md wording was corrected to "in the one view measured per side".
- **Catmull-Rom overshoot:** not visible (clamp tested, MAE 1.4e-4), left as is.
- **Stratus near-uniform / blue overcast** and **erosion as a second threshold at low coverage:** not in the brief's views. Backlog rows were added (overcast also lacks diffuse transmitted sunlight).

## Self-review

- All values are named constants with why-comments. No debug flags, views or stray files are committed. The tools are `cloud-compare.sh` and `cloud-metrics.py`, both documented in CLAUDE.md.
- The spec (`2026-11-19-clouds-weather-design.md`) still describes edge darkening by lowered albedo. The brief's target 1 replaces it with octaves and I did not edit the spec. The roadmap is untouched.
- decisions.md: the Task 2-6 entries are updated in place (noise tiles, density, lighting, shape, ribbons, shadow-map use, band wording), and a new "Cloud look development" entry has the sources.

## Honest read per comparison

- **ground vs cloud1/cloud5:** the same family (fair cumulus, gaps, flat-ish grey undersides). Ours is a field of many small, smoother clouds that read light grey under a paler sky. The photos have bright white crisp cauliflower clouds, including one big 2 km cloud. Moderately close.
- **wide vs cloud3/cloud4:** the closest pairing. The field of flat-based loaves receding to the horizon matches cloud4's structure. Whiteness and edge crispness are still short.
- **sunset vs cloud2:** the same geometry (a field of heaps seen from above, sun ahead, lit rims), but dark blue-grey bodies under a non-orange sky. Not the same mood: the atmosphere/haze colour and the photo's processing dominate.

## Fix round 1 (interrupted)
- **Debug edits that must be reverted before any commit:** clouds_composite.comp, mesh.frag and sky.frag carry the calibration hack (`python3 -I .../t7/fix1/hack.py off` or `git checkout` of those three restores them). build-release is built with the hack.
- **Done, uncommitted:** I1 (metrics rewritten as asked; corrected photo bases are still 2.1-3.4x and tops 5.9-13x, so the conflict holds), the compare script now stops on missing photos or a failed game run, and the ground-bounce coverage moved to the CPU-set `clouds.groundCoverage` (the old pad1). I3: the cause is diagnosed but nothing is fixed or written up. The cloud temporal resolve loses the detail (ratio 0.899 with no history, Release). It is a trade with density gain: gain 1/3/6 gives 0.836/0.810/0.794, and the moving Laplacian is 0.0196/0.0199/0.0202 (`fix1/turn/`).
- **Partial:** I2 code is in place but not yet fitted. The fallback now uses the local layer, and the march passes `marched=true` so the regional mean covers only the rest of the path. Calibration r0 (`fix1/calib-r0.txt`, fit not yet changed) is off: fallback minus map is fair -0.05 to -0.20 and broken +0.05 to +0.17. `kCloudShadowFallbackFit` and L still need a refit.
- **Untouched:** the docs (CLAUDE.md sunset `--weather broken`, decisions qualifier and numbers, backlog row for the motion trade), the final regeneration, the Step 3 re-run and the commit.
