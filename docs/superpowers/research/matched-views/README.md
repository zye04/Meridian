# Matched views: Chlorophyll reference frames vs Meridian

Built 2026-11-21 against main (Release, `--trees`, 2560x1440 fullscreen, `--frames 30`, 0 validation errors).
Each `*-sbs.jpg` is his frame (black bars cropped) left, ours right, both scaled to 720 px high.
Our captures are not committed (size); re-create them with the commands below. His frames are not stored here.

Prefix every command with `flock /tmp/meridian-gpu.lock ./build-release/game/game --trees`.

## Conventions
- `--sun X Y Z` is the direction towards the sun (y up). Yaw 0 looks -Z, -1.55 looks -X, 3.14 looks +Z; azimuth below is relative to the view, positive = right of the view direction.
- Meridian FOV is fixed: `Camera::fovY = 1.2 rad` (68.8 deg vertical), 16:9 = about 101 deg horizontal. His FOV is unknown; from the framing it looks narrower (roughly 80-95 deg horizontal), so our views show more scene and everything looks smaller/farther. No way to match without a FOV option.
- Camera Y = ground height from `napf.height` + the height above ground in the table.
- Sun estimates are from shading and shadow directions only (no ground truth); error is probably +-30 deg azimuth, +-10 deg elevation.

## Table

| Frame | Est. sun (az / el) and `--sun` | Camera (x y z yaw pitch), above ground | Our command (after the prefix) | What cannot match |
|---|---|---|---|---|
| meadow-cliff-forested-hills | ahead-left 30 / 55 deg: `-0.503 0.819 0.276` (his cliff face is dark = backlit, foreground lit, trees lit from the left) | 1119 1224.5 -700 -1.55 0.12, 1.7 m | `--camera 1119 1224.5 -700 -1.55 0.12 --sun -0.503 0.819 0.276` | Weakest match. His is a gentle meadow with a vertical cliff across a valley; ours is a steep slope (in shade with this sun) facing an eroded cliff with a forested rim. His foreground is lit and the cliff is much farther; no matching foreground trees. |
| flower-meadow-cumulus | right-ahead 50 / 50 deg: `-0.492 0.766 0.414` (bright translucent grass, brighter sky on the right) | 1400 1292.0 -431 3.14 0.1, 1.5 m | `--camera 1400 1292.0 -431 3.14 0.1 --sun -0.492 0.766 0.414` | His is a smooth flower-covered field sloping up to a low forest line; ours has a meadow only at the bottom edge, then a terraced valley. No forest band at his distance; weather preset differs from his flat cumulus (we use default `fair`). |
| aerial-forest-slope-river | right-behind 100 / 50 deg: `0.333 0.766 -0.550` (right-facing slopes lit, tree shadows fall left/towards camera, left slope dark) | 400 1427.3 -300 -1.2 -0.35, 250 m | `--camera 400 1427.3 -300 -1.2 -0.35 --sun 0.333 0.766 -0.550` | No river or water (backlog: later), no valley floor, placeholder Megaplants crowns (sparse, bright yellow-green, no crown shading), exposed bare-soil patches on slopes where his slopes are meadow. Our ridge is a single big forested spur, not a valley slope. |
| aerial-river-valley-clearings | right 90 / 50 deg: `0.013 0.766 -0.643` (right hill lit, shadows left of trees) | 600 1404.5 -400 -1.55 -0.3, 200 m | `--camera 600 1404.5 -400 -1.55 -0.3 --sun 0.013 0.766 -0.643` | No river. Clearings are brown soil patches, not green meadow; his hill is a rounded meadow-topped ridge, ours a forested summit. Our horizon is higher (pitch) and hazier. |
| target-meadow (his overlay: 60 fps, GPU 7.67 ms) | left 70 / 55 deg: `-0.207 0.819 0.535` (tree shadow falls right and slightly towards camera) | 1400 1292.0 -431 -1.55 0.1, 1.5 m | `--camera 1400 1292.0 -431 -1.55 0.1 --sun -0.207 0.819 0.535` | His is a meadow rising to a forested peak with exposed rock; ours is a grassy ridge with sparse trees and no forest on the slope (forest mask is noise-based, none at this spot). Single hero tree absent. |

## Notes
- Searched ~20 candidates (forest edge/interior, ridge views in 4 directions, aerial at 200-250 m); the other candidates are not kept. Closest compositions found: aerial = forested spur at (400..600, -300..-400) looking -x; meadow = ridge east of the start at 1400,-431.
- Keep the start's sample models (marble wall) out of frame: stay >150 m from (1119, -431).
- Frame-time of the aerial views is higher with trees everywhere (`GPU passes` in the capture log); compare GPU ms only at the same resolution.
- Not matched in any frame: his grading/contrast (his sky is deeper blue, greens more saturated), cloud shapes (ours are the weather system's cumulus), and the FOV.

Our full captures are not committed (size); re-create them with the commands above.

## Resemblance metric (2026-11-21)

`python3 -I tools/resemblance.py OURS.png REF.png --regions docs/superpowers/research/matched-views/regions.json --view NAME`
prints OKLCh mean L / C / h and L p5 / p95 per region (`sky`, `foliage-lit`, `foliage-shadow`, `grass`, `far`) for ours and the reference, the delta (ours - ref; hue circular, degrees), and whole-frame `contrast` (std of L), `sharpness` (Laplacian std of L, reference resized to our height first) and `shadow/sunlit`. Regions are normalized rects in each image (the reference without its black side bars) in `regions.json`, picked on the side-by-sides to sit inside one material; a region is left out of a view when ours has no comparable material (no lit grass, no lit foliage...). Where the two sides show different light (e.g. our shaded slope vs his lit meadow) the delta says so; it is not a bug of the metric. Caveats: regions are hand-picked and a few contain cloud edges (sky) or shadowed vs lit mismatches; the fps overlay and avatar of target-meadow are only in the whole-frame rows. Identity check (same image, same rects on both sides): every delta 0.000.

Captures used: `.superpowers/matched-views/<view>-ours.png` (git-ignored, 1600x900).

meadow-cliff-forested-hills
```
metric                         ours      ref    delta
sky L                         0.623    0.764   -0.142
sky C                         0.036    0.050   -0.014
sky h                       245.307  255.510  -10.203
sky L p5                      0.185    0.702   -0.517
sky L p95                     0.939    0.845    0.094
grass L                       0.209    0.396   -0.187
grass C                       0.027    0.082   -0.055
grass h                     206.570  126.859   79.711
grass L p5                    0.138    0.235   -0.098
grass L p95                   0.283    0.568   -0.284
far L                         0.298    0.494   -0.195
far C                         0.041    0.039    0.002
far h                       244.293  237.754    6.539
far L p5                      0.197    0.356   -0.159
far L p95                     0.818    0.796    0.023
contrast (std L)              0.276    0.219    0.057
sharpness (lap std L)         0.068    0.061    0.007
```

flower-meadow-cumulus
```
metric                         ours      ref    delta
sky L                         0.528    0.575   -0.046
sky C                         0.084    0.084   -0.000
sky h                       250.488  253.167   -2.679
sky L p5                      0.506    0.528   -0.022
sky L p95                     0.553    0.637   -0.083
grass L                       0.271    0.312   -0.041
grass C                       0.065    0.060    0.005
grass h                     125.960  123.359    2.602
grass L p5                    0.136    0.199   -0.063
grass L p95                   0.497    0.473    0.025
far L                         0.567    0.614   -0.047
far C                         0.038    0.027    0.011
far h                       237.110  250.570  -13.460
far L p5                      0.433    0.351    0.082
far L p95                     0.639    0.835   -0.196
contrast (std L)              0.182    0.203   -0.021
sharpness (lap std L)         0.080    0.056    0.025
```

aerial-forest-slope-river
```
metric                         ours      ref    delta
sky L                         0.764    0.803   -0.039
sky C                         0.046    0.051   -0.005
sky h                       245.303  251.514   -6.211
sky L p5                      0.730    0.784   -0.054
sky L p95                     0.824    0.836   -0.012
foliage-lit L                 0.302    0.350   -0.047
foliage-lit C                 0.052    0.039    0.013
foliage-lit h               136.383  148.628  -12.246
foliage-lit L p5              0.145    0.182   -0.037
foliage-lit L p95             0.508    0.648   -0.140
foliage-shadow L              0.225    0.142    0.083
foliage-shadow C              0.041    0.011    0.030
foliage-shadow h            241.910  188.256   53.654
foliage-shadow L p5           0.167    0.100    0.067
foliage-shadow L p95          0.290    0.365   -0.075
far L                         0.590    0.507    0.083
far C                         0.039    0.039   -0.000
far h                       223.262  216.583    6.679
far L p5                      0.432    0.360    0.072
far L p95                     0.707    0.683    0.024
shadow/sunlit                 0.744    0.405    0.340
contrast (std L)              0.227    0.248   -0.021
sharpness (lap std L)         0.154    0.126    0.029
```

aerial-river-valley-clearings
```
metric                         ours      ref    delta
sky L                         0.773    0.670    0.103
sky C                         0.032    0.081   -0.049
sky h                       248.354  252.714   -4.360
sky L p5                      0.721    0.653    0.068
sky L p95                     0.826    0.692    0.134
foliage-lit L                 0.308    0.482   -0.174
foliage-lit C                 0.048    0.075   -0.027
foliage-lit h               137.509  127.822    9.688
foliage-lit L p5              0.134    0.317   -0.182
foliage-lit L p95             0.531    0.693   -0.162
foliage-shadow L              0.349    0.385   -0.036
foliage-shadow C              0.043    0.063   -0.021
foliage-shadow h            187.463  130.564   56.899
foliage-shadow L p5           0.196    0.186    0.010
foliage-shadow L p95          0.540    0.556   -0.015
far L                         0.608    0.640   -0.032
far C                         0.044    0.042    0.001
far h                       235.954  239.880   -3.926
far L p5                      0.368    0.498   -0.129
far L p95                     0.708    0.827   -0.119
shadow/sunlit                 1.133    0.799    0.334
contrast (std L)              0.214    0.177    0.037
sharpness (lap std L)         0.170    0.125    0.045
```

target-meadow
```
metric                         ours      ref    delta
sky L                         0.709    0.506    0.203
sky C                         0.063    0.092   -0.029
sky h                       248.916  252.787   -3.871
sky L p5                      0.692    0.480    0.212
sky L p95                     0.722    0.535    0.187
grass L                       0.584    0.489    0.095
grass C                       0.070    0.098   -0.028
grass h                     113.837  121.173   -7.337
grass L p5                    0.514    0.299    0.215
grass L p95                   0.633    0.634   -0.001
contrast (std L)              0.254    0.151    0.103
sharpness (lap std L)         0.126    0.110    0.016
```
