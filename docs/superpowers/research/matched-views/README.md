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
