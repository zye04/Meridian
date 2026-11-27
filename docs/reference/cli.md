# Meridian: game CLI, controls and views

Reference for the game binary: every flag, the overlays, capture and
benchmark modes, and the named views. Read the part you need; CLAUDE.md
keeps the build/test/capture essentials. Every game run goes through
`flock /tmp/meridian-gpu.lock` (examples omit it).

## Controls, overlays, benchmark

Controls: WASD, mouse, Esc. Fly mode: Space/Ctrl up/down, 30 m/s,
Shift x5. `F` toggles walk mode: eye 1.7 m above the ground, WASD horizontal at 1.4 m/s (Shift 5 m/s), no vertical keys.
`G` opens the grading panel (Dear ImGui overlay): every `Grade` field,
sun azimuth/elevation, haze, bloom (intensity, scatter) and an Effects
section (`Effects`, renderer.h, `Renderer::setEffects`: parallax on/off + depth x0..4,
parallax self-shadow, terrain detail on/off (the 0.3 m noise on the two finest levels; off by default since the relief test), micro shadows on/off + strength (also darkens plant bases: architecture.md "Plant micro occlusion"), contact shadows on/off
(off skips the dispatch) + length x0..3, bloom on/off, cloud outline strength 0..3, cut-out mip bias -2..2, stable TAA on/off), with Save, Load, Reset and a "show built-in
grade" A/B box. It frees the cursor and freezes the camera while open;
changes apply at once. Save writes the grade file,
`$XDG_CONFIG_HOME/meridian/grade.json` (default
`~/.config/meridian/grade.json`, shared by every checkout; flat JSON, field
names as in `grade.h` plus `sunAzimuthDegrees`, `sunElevationDegrees`,
`haze`, `fovDegrees`, `bloomIntensity`, `bloomScatter`, `effect*` keys; missing = defaults), which **interactive runs load at
startup** (explicit `--sun`, `--haze` and `--bloom` still win); capture runs ignore it so gates stay deterministic.
`--grade FILE` loads a grade file in any run (A/B captures of a tuned
look); `--overlay grade|perf` opens a panel at startup (it is in captures;
repeat the flag for both).
`F3` toggles the performance overlay (passive, top right, the cursor stays
captured): FPS, frame / CPU / GPU ms, per-pass GPU ms, a 5 s frame-time
graph, AVG / 0.1% / 1% / 5% low / MAX FPS over the last 10 s. x% low =
1000 / mean of the slowest ceil(x% n) frames (`frame_stats.h`). `F4` (or its
Record button) records every frame to `meridian-<timestamp>.csv` in the
working directory (`frame,frame_ms,cpu_ms,gpu_ms,<one column per GPU
pass>`; the path is relative to the cwd; frame_ms is present to present, cpu_ms is frame_ms minus the time
blocked on fence and acquire, GPU columns are the newest finished frame,
2 frames behind) and prints the summary on stop; a recording still
running at exit is written too, and on a write error the run keeps it
in memory and reports. `--benchmark SECONDS FILE.csv` (SECONDS > 0; not
with `--capture` or the camera velocities) is a timed run with real
frame times, static camera and no camera input (Esc still quits; the
partial recording is written): FILE.csv is checked writable up front, 60
warm-up frames, then SECONDS recorded; writes the CSV, prints the
summary, exits. `--route FILE` (only with `--benchmark`; camera_route.h,
`TIME X Y Z YAW PITCH [LABEL]` per line) flies the camera along the keys
(t = 0 in warm-up, then real seconds since recording started; a label
starts a segment). `python3 -I tools/bench-route.py bench.csv
tools/routes/parity.route` prints per-segment avg / 1% low fps, avg ms
and the 4 heaviest passes, flagging BELOW 180 (route) and BELOW 105
(segment). The parity route (~60 s, run it with `--trees`): meadow,
grass, forest-edge, forest-interior, aerial.

## Debug views and the report

`F5` cycles the debug views (none, base-color, lod, stretch, sun,
ambient, specular, luminance); `--debug-view <name>` starts with one
(captures too). A view replaces the
tonemapped frame with render-sized data read before the upscaler (no
DLSS/TAA smear): `base-color` the diffuse albedo the shading used (base
colour x (1 - metallic); voxels their mean leaf colour); `lod` triangle
clusters green (DAG level 0, full detail) to red (level 7+), voxel
clusters blue to violet by level, far cells cyan, terrain grey, sky
black, each cluster a slightly different brightness (0.7-1.0 of its
colour, a per-cluster hash, so borders show). Legend at full brightness
(sRGB; `lodViewLegend`, renderer_debug.cpp, from debug_view.frag's
formulas; every report also writes it as `legend.txt`):

| kind | level 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7+ |
|---|---|---|---|---|---|---|---|---|
| triangles | #72ff6c | #abff6c | #d2ff6c | #f2ff6c | #fff06c | #ffd06c | #ffa86c | #ff6c6c |
| voxels | #7caeff | #7c95ff | #827cff | #9f7cff | #b67cff | #ca7cff | #dc7cff | #ec7cff |
| far cells | #61cbcb | #69dada | #6fe7e7 | #76f3f3 | | | | |

Terrain #898989, sky #000000. Voxel clusters reach DAG level 11 at
~5-9 km (all drawn as 7+).

`stretch`: the base colour texture on the surface, hue by the ratio of its
texel density along its two principal directions (green 1:1, yellow 2:1,
red 4:1 and over; per triangle, since UVs are linear on one) over an
8 x 8 checker of the texture (dark squares 0.6); grey where there is no
texture (terrain, voxels, untextured), black on the sky. `sun`,
`ambient`, `specular`: one part of the light, exposed, before contact
shadows, bloom and the grade, through a neutral tonemap (colour / (1 +
luminance), so hue and the ratios between the three stay): sun = sun
diffuse + specular (leaf transmission included), ambient = sky, ground
and probes (diffuse + specular), specular = sun + ambient specular.
Blended surfaces and the sky write none (black). `luminance`: the HDR
frame (after contact shadows and clouds, before the grade) in EV from
18% grey after exposure, rounded, one colour each (`luminanceViewLegend`,
also in `legend.txt`):

| EV | <=-5 | -4 | -3 | -2 | -1 | 0 | +1 | +2 | +3 | +4 | >=+5 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| sRGB | #2a0050 | #3a20b0 | #2060e0 | #20a8c8 | #30a040 | #808080 | #c8c830 | #f0d000 | #f08800 | #e02010 | #ffffff |

`F7` freezes the game clock (wind, clouds, weather; `--freeze-time` starts
frozen); the camera, exposure and TAA keep running. `F8` cycles solo/hide
of what the crosshair (the render's centre) shows: solo-instance,
solo-material, hide-instance, hide-material, off; it picks on the first
step (one hitch: the debug set) and keeps that target for the cycle, and
prints `Filter: <step> <id> (<name>)`. `--debug-filter <step> N` does the
same after frame N of a capture run. The debug mesh shaders drop the
clusters (depth pre-pass too, so what is behind shows), the terrain (solo)
and the voxel quads, far cells discard; shadows, GI probes and blended
surfaces still see everything, and voxels have no material (solo-material
removes them). With a view or a filter on, the forest edge `--trees`
costs GPU 32.53 ms (scene early 16.45) against 24.25 (10.89) off. A magenta crosshair
marks the render's centre pixel while a view is on (the mouse stays
captured). `F6` (or `--report-at N`: at frame N of a capture run) writes a
bundle to `~/meridian-reports/<YYYY-MM-DD_HH-MM-SS>/` and prints `Report:
<dir>`: `shot.png` (that frame without the view or crosshair, lossless),
`views/<view>.png` (every view of the same frame), `legend.txt` (the lod
and luminance views' colours), `pixel.json` (the
centre render pixel: kind sky/triangles/voxels/far cell/terrain, instance
id and placed, mesh and material index and glTF names (`mesh_name` is
`<file>/<glTF mesh name>#<primitive>`), cluster index and DAG level or
far cell and level, distance in m, the views' values: base colour, lod,
stretch (`texels_per_m`, `ratio`), `light_cd_m2` (sun and ambient, each
diffuse and specular, haze transmittance applied: their sum plus haze
inscatter and emissive is the HDR value), `luminance` (`hdr_cd_m2`,
`ev_from_grey`, the `exposure` it was shaded with); `filter` and
`time_frozen` in effect), `look.json` (the
look in effect, grade file format) and `repro.txt`: a command line that
reproduces the view with `--capture <dir>/repro.png` (every original flag
except pose, sun, look and run control, then `--camera`, `--sun`, `--grade
<dir>/look.json`, `--frames` = the report frame in a capture run, 120
interactive, and `--game-time S` when the game time differs from what
those frames step to; `--freeze-time --game-time S` when frozen). Its
second line is the same shot as a `--capture-views` line named after the
bundle (the launch's view options, e.g. `--weather`, move onto it rather
than the first line's kept flags), so `cat ~/meridian-reports/*/repro.txt`
is a view list. The
filter is in the shot but not in repro.txt. `--game-time S` starts the
game clock (wind, clouds, weather) at S seconds. While a view or the report is on, the
scene pipelines are swapped for their debug set (one hitch); off, nothing
of it exists.

## Captures

Automated visual check (deterministic, ~4 s, exit code 1 on any Vulkan
validation error):

```bash
./build/game/game --camera X Y Z YAW PITCH [--sun X Y Z] [--scatter N] [--trees] [--lod-error PX] --capture out.png [--frames N]
```

`--capture-views FILE DIR` renders a list of views in one launch (one
load, one `flock` for the whole list), each to `DIR/<name>.png`. A line of
FILE is a view name, then view options; blank lines, `#` comments and lines
starting with `flock` (a repro.txt's command line) are skipped. View
options, which change without a reload: `--camera`, `--sun`, `--grade`,
`--haze`, `--fov`, `--bloom`, `--effects`, `--game-time`,
`--freeze-time`, `--time-scale`, `--weather`, `--weather-front`,
`--debug-view`, `--debug-filter` (its frame counts from the view's
start), `--frames`. Every other flag needs a reload and is the launch's
only (`--trees`, `--placement`, `--voxel-lod`, `--far-field`, `--gi`,
`--shadows`, `--aa`, `--lod-error`, `--clouds`, ...); a view line that
gives one is an error. The launch's own view options are every view's
defaults; a line overrides them. Words split on whitespace (no quoting).
Not with `--capture`, `--benchmark`, `--reference` or camera velocities.
Each view starts with `Renderer::cut()`, which leaves the renderer as a
fresh launch's first frame finds it (temporal histories: the DLSS reset
flag, TAA, clouds, motion; GI probes and ground cache, cleared; sun cascades, terrain and far shadow
maps; depth pyramid; exposure back to its initial EV; jitter and noise
frame index back to 0), then renders `--frames` (120) frames like a single
capture and prints `View i/N: name` and the lines a single capture prints
at its end (`Rendered ...` with that view's validation errors and drops,
`GPU passes:`, cull list need), the run's most-in-a-frame figures restarted
per view. `tools/capture-sheet.py DIR [OUT.jpg] [--columns N] [--width PX]`
tiles the folder's PNGs in the order written into one labelled JPG
(default `DIR/sheet.jpg`).

## Reference path tracer

`--reference SPP` (with `--capture`; builds the TLAS and BLAS; not with `--benchmark`,
`--capture-sequence`, `--capture-albedo` or camera velocities) is the
reference path tracer (spec `docs/superpowers/archive/specs/2026-11-23-reference-path-tracer-design.md`,
renderer_reference.cpp, reference.comp): after the `--frames` warm-up the
exposure and game time freeze and every frame adds one path traced sample
per pixel (TLAS + heightfield, materials through surface.glsl as the raster
path) to an accumulation that replaces the upscaler's output, bloom off;
after SPP samples the capture saves it and the run prints `Reference: N
spp, total S s, noise P%` (median over 16x16 tiles of the relative standard
error of the tile mean); `reference` in `GPU passes:` is one sample's ms.
`--reference-mode radiance|albedo|white|leaf|sun` (default radiance):
`albedo` accumulates the primary hit's diffuse albedo (compare with a raster
`--capture-albedo`); `white`, `leaf`, `sun` are the integrator's furnace
checks. Compare against a raster capture with `--effects pom=0 --bloom 0 0`
(and `--streamed off` where meadow shows) via `tools/reference-compare.py`.
Known differences: the reference's terrain is the bilinear heightfield
(no near detail displacement), blended materials trace as opaque, distant
trees are their full geometry (the raster's voxels are what is judged).
`--capture-albedo FILE.pfm` (with `--capture`) also saves the linear
diffuse albedo the shading used on the same frame (scene attachment 3,
kept alive for the run by `Renderer::enableAlbedoCapture`; the sky reads
0.5); `python3 -I tools/albedo-audit.py FILE.pfm tools/albedo-regions.json
VIEW` prints per-region mean albedo against approximate physical ranges
and, for single-material rects, a source texture (materials spec D6).
A `.pfm` capture path writes the linear HDR image the tonemap reads
(the upscaler's output, pre-exposed) instead, for luminance ratios.

## Motion gates

Motion sharpness: add `--camera-velocity X Y Z` (m/s) and/or
`--camera-angular-velocity YAW PITCH` (rad/s; `--camera-yaw-rate YAW` is
short for `YAW 0`), fixed 1/60 s steps ending at
`--camera`, and compare with the still capture of the same view:
`python3 -I tools/image-gates.py laplacian motion.png` (Laplacian std:
the gate is the motion frame's absolute sharpness, minimums anchored to
TAA v2; `ratio` is the informational motion/still column; also
`shimmer`, `ssim`, `diff`). The whole motion gate set runs with `tools/motion-gates.sh BUILD OUT [game args]`
(`GPU_RUNS=5` adds per-pass GPU medians). `--capture-sequence N` also
saves `<capture stem>_NNN.png` every N frames (fly-throughs,
time-lapses); per-pass GPU ms of such runs are inflated by the readbacks.

## Field of view, haze, anti-aliasing

`--fov DEGREES` (horizontal, 30 < x < 150, default 70, matched to the Chlorophyll
frames; `verticalFov` in camera.h derives the vertical angle from the window
aspect every frame) sets the field of view. Interactive runs also read
`fovDegrees` from the grade file (explicit `--fov` wins); the G panel's "Field
of view" slider (50..110) saves, loads and resets with the Look. Captures
taken before this change used ~101 degrees horizontal (fixed fovY 1.2 rad), so
old views and gate images are not comparable.

`--haze clean|summer|summer-hazy` picks the aerosol (default summer:
a clean summer day's boundary layer, AOD 0.03 from the valley floor (5th
percentile of the terrain) to 1.2 km above it with a smooth top, plus
a free-troposphere background (H 2 km) with 0.01 above the top, ~0.05
in all over the floor; startup prints `Haze layer:`; summer-hazy:
haze through the ~2 km boundary layer, ~45 km visibility on the Napf ridges, grey sky;
clean = Bruneton's very clean defaults, for A/B).

`--aa dlss|taa` picks the anti-aliasing: DLSS (default; DLAA at
native 1440p unless `--dlss-mode` says otherwise; falls back to TAA with
the log line `DLSS unavailable: <reason>; using TAA` when NGX reports no
Super Resolution, e.g. non-NVIDIA GPUs or the `-DMERIDIAN_DLSS=OFF` build), or our
own TAA at native resolution (taa.comp, the fallback, an A/B). `--rr on|off` (default off: Super
Resolution DLAA, ~2.3 ms at 1440p against RR's ~4.7, decisions.md) picks
Ray Reconstruction under DLSS, an A/B
(preset F, `kRayReconstructionPreset` in dlss.cpp; falls back to Super
Resolution DLAA with `DLSS Ray Reconstruction unavailable; using Super
Resolution`). `--dlss-mode dlaa|quality|balanced|performance` (default
dlaa) and `--dlss-preset E|F|J|K|L|M` (Super
Resolution only; default: M for DLAA, Quality and Balanced, NGX's own
for Performance, `defaultPreset` in dlss.cpp; A/B override) set DLSS. `--render-scale S` (0 < S <= 1, default 1) renders the scene
at the display size times S for TAA (`--aa taa`); DLSS picks its own render size.
Startup prints `Render WxH, display WxH, jitter phases N, mip bias B,
DLSS|DLSS Ray Reconstruction|TAA` and, with the DLSS module, `DLSS: SDK
310.9.16, Super Resolution ..., Ray Reconstruction ... (preset F)`.

## Weather and clouds

Weather: `--weather clear|fair|broken|overcast` (default `fair`),
`--time-scale X` (game seconds per real second), `--weather-front
SECONDS` (blend from the start preset to overcast over that many game
seconds; the change enters at the front line 60 km upwind and travels
with the wind). Capture runs print the exposure (EV100), the sky
irradiance with clouds and clear (x sun), the open-ground illuminance (lux) that bounds the exposure, and the weather at the camera (coverage,
type, wind speed and the direction it comes from, gustiness, game time).
`--clouds off` skips the cloud march, composite, cloud shadows and sky
capture (A/B). Ambient light (diffuse irradiance, the specular sky
fallback) includes the clouds through the sky capture, taken at the
camera. Front time-lapse: `--weather fair --weather-front 600
--time-scale 600 --capture-sequence 20 --frames 1200` from the ground or
above-layer view (the overcast deck reaches the camera after ~800 frames).
Surfaces (mesh.frag, terrain too) multiply the sun by `cloudShadow` at
their flat world position: the shadow map within ~13.8 km of the camera
(horizontally and across the sun), blended over 1.5 km into the
coverage-based fallback beyond (`1 - k R` per crossing, darkened by the
number of clouds a low sun's ray crosses; calibrated against the map);
the map fades out as the sun sets (6 to 3 degrees).
`--dump-weather-map out.png` saves the weather map after the last frame
(R coverage, G type, B precipitation; row 0 = north) and prints its
window; two dumps at `--time-scale 0` and `--time-scale 60 --frames 60`
show one game minute of drift.

## LOD, placement, far field, GI, shadows

`--lod-error PX` sets the LOD threshold (default 1 pixel; 0 = full
detail, for A/B); `--bloom INTENSITY SCATTER` sets the glare (default
0.015 0.7; `--bloom 0 0` = off, for A/B); `--effects pom=0,pom-depth=3,pom-shadow=0,terrain-detail=1,micro=0,micro-strength=.5,contact=0,contact-length=2,bloom=0,cloud-outline=1,cutout-bias=0,taa-stable=0`
sets the live effect toggles (defaults = normal rendering; for A/B); `--no-occlusion` draws everything in the frustum
(occlusion culling A/B). Capture runs print drawn instances, clusters,
triangles and entries dropped by full cull lists (nonzero = missing
geometry or shadow; the most in any frame of the run, main view plus the
sun shadow cascades rendered that frame, every frame drawn included: the
run waits for the frames still in flight before it reports), per list: chunks, work items,
clusters, terrain nodes, bvh nodes (the voxel lists' drops are in the total
only); a nonzero total also ends the run with a `WARNING: up to N entries
dropped by full cull lists in a frame` line on stderr. Two `cull list need,
main view|sun shadow cascades, most in a frame:` lines give each list's
largest phase, queue or bucket over the run (the main view's also terrain
nodes and retest clusters) (the need while nothing drops,
a lower bound once something does; the caps in gpu_types.h), then one
`cull list need, cascade N:` line per sun shadow cascade. A GPU-only
buffer placed outside device memory prints `warning: a N MiB GPU buffer is
outside device memory` when it is created.
They also print drawn terrain nodes (2048 triangles each), `cells N`
(placement cells visible), `far cells N` (cells drawn as far-field
voxels), `voxel trees N (bricks B, voxels V)` (voxel clusters drawn,
their bricks and voxel quads, both phases; when any draw, a `voxel quads N:
area A px = D x the render's pixels, S% under 1 px` line follows: the quads'
screen area, depth complexity and sub-pixel share), `bvh nodes N` (cluster group BVH nodes the
traversal visited), `placed N`
(placed instances), and `GPU passes:`
with GPU ms per pass (upload + tlas, terrain shadow (the shadow-height map, 0 unless the sun turned or the camera moved 1 km), sun shadows, terrain select, stream (only with a streamed layer), cull cells, cull early, traverse, cull clusters, far shadow (the map slice, 0 unless the sun moved), atmosphere, weather, gi probes + gi ground (unless `--gi off`), scene early, voxel trees, far field, pyramid, cull late, scene late, voxel trees late, blended, clouds, gi albedo (unless `--gi off`), contact shadows, aa (the upscaler), bloom, exposure,
tonemap, overlay when a panel is open; they sum to the total). A
new pass is one `endPass("name")` call in `renderer_frame.cpp`.

## Views, trees, scatter, placement layers, far field, GI and shadow modes

Useful views (Napf terrain; ground 1354.2 m at the start): the default
start is eye height on a ridge top at (1119, -431), 1.5 km east of the
Napf summit (-359, -465, 1406.7 m), looking at it past the sample models
(`1119 1355.9 -431 -1.55 -0.1`); open view toward the summit
`1119 1356 -431 -1.55 -0.02`; full-ground view `1119 1355.9 -431 -1.55
-0.25`; LOD transitions from above `1119 1420 -431 -1.55 -0.6`; high
overview (3000 m above the ground) `1119 4354 -431 -1.55 -0.35`; across
the crop's east edge into the mirrored terrain `6500 3500 0 1.57 -0.6`;
sunset, long ridge shadows across the valleys, `1119 1500 -431 3.14 -0.15
--sun 0.9 0.05 -0.4`; far horizon from the summit `-359 1408.7 -465 1.57
0`; float precision far out, at the crop's corner (ground 1286.5 m),
`7000 1288.2 7000 -1.55 -0.25` (compare its `--camera-velocity 5 0 0`
Laplacian with the still's). Placed forests (seed 1, all of these need `--trees`): forest edge
`600 1205.6 -400 -1.55 0.02`; forest interior `85 1158.6 -53 -1.55
0.25`; forest overview `700 1450 -400 -1.55 -0.5`;
canopy from 40 m above `85 1200 -53 -1.55 -1.2`; a
placed moss rock close up `1944.3 953.4 -1923.9 -1.55 -0.4`; trees on
the summit ridge at ~1.5 km: the open view toward the summit.
`--trees` brings the trees back; by default they are hidden (no tree
prototypes loaded, no forest or lone-tree layers, no forest floor in the
terrain material, no tree models in `--scatter`), until they are textured
and keep their coverage (`docs/backlog.md`). With `--trees` the Megascans
Trees cooked from Unreal (`assets/source/megascans/<species>/SM_*`,
docs/assets.md) are used when present: beech, hornbeam and alder forests and
lone trees, saplings under the canopy (layer 12), every cooked
variant; startup prints `Megascans Trees: F forest, L lone, S sapling
prototypes`. Without them, the Megaplants.
`--tree-palette NAME` loads those trees from
`assets/source/megascans/<species>-NAME/` instead: the same trees cooked
with a retinted leaf atlas (`tools/unreal/cook_palette.sh`, docs/assets.md).
`--scenery broadleaf` places the "Forest European Broadleaf" pack's small
and medium pieces (main.cpp `ForestFloorScatter`, `ClearingStones`): stones
and roots under the canopy (layer 14), flat ground rocks on open knolls
(15), the pack's medium rocks on the steep-ground layer (3) and its small
and clustered rocks as scree (13), without the sandstone cliff. Cliffs,
ledges, embankments and large rocks are not placed. With `--trees` the
forest layer then takes only the open-crowned variants (`BroadleafForest`:
beech Field_01 and Forest_03, hornbeam Field_01 and Forest_02, alder
Field_02 and Field_01) at 9 m spacing. The terrain's grass and forest-floor colour
targets move to `BroadleafGround` (main.cpp): grass toward the streamed
meadow as rendered, the forest floor darker. Without the flag the placement is
unchanged.
Clouds (layer 1.7-4.8 km above sea level, cirrus sheet at 9 km): beside
the Chlorophyll frames: fair from 1500 m `1119 1500 -431 -1.55 0.05`, cirrus `1119 1356 -431 0
0.5`, stratocumulus the fair view with `--weather broken`, overcast
`1119 1356 -431 -1.55 0.25 --weather overcast`; fair cumulus from the
ground `1119 1355.9 -431 -1.55 0.15`; above the layer `1119 6500 -431
-1.55 -0.25` (pitch -0.1 looks across the weather map's 30 km edge);
inside the layer (half-resolution march) `1119 3200 -431 -1.55 0`; the
summit silhouette against clouds `1119 1356 -431 -1.55 0.03`; look
development (`tools/cloud-compare.sh`, each beside its photo in
`assets/reference/clouds/`, git-ignored): midday from the ground looking
up `1119 1356 -431 -1.55 0.5 --sun 0.45 0.85 0.25` (cloud1, cloud5), the
same toward the horizon at pitch 0.1 (cloud3, cloud4), sunset from above
the layer `1119 5500 -431 -1.55 -0.2 --sun -0.998 0.05 0.03 --weather
broken` (cloud2);
the script also captures `.pfm`s with clouds on and off and prints the
sunlit tops and the bases over the clear sky behind them, the cover, the
spread of the bases and the shading contrast, and the same measured on
the photos (display-referred, so compare the capture's image line); dusk
clouds `1119 1355.9 -431 -1.55 0.15 --sun -0.7 0.035 0.7` (sun 2 deg up;
`-0.05` for just below the horizon). Cloud motion checks (Laplacian vs
the still, and vs `--clouds off`): fast turn, the fair view with
`--camera-yaw-rate 1`; toward the layer `1119 1900 -431 -1.55 0.1
--camera-velocity -60 20 0`; across the base (quarter -> half switch at
2550 m, base 2800 m) `1119 2950 -431 -1.55 0.1 --camera-velocity -30 250
0` (with `--capture-sequence 2` for the sequence); summit silhouette
with clouds drifting across the view `1119 1356 -431 2.5 0.12
--time-scale 1.5` (~10 m/s, 80% lateral; still: `--time-scale 0`;
Laplacian on the crop `1000x340+900+560`, since whole frames are
dominated by terrain); strafing past the marble wall's top
`1119 1356 -431 -1.55 0.03 --time-scale 1.5 --camera-velocity 0 0 10`.
`--scatter N` places N random Poly Haven models and imported Megaplants
trees within 150 m of the start (fixed seed; loading 21 models, 14M
triangles, makes Debug startup ~2.5 min); over the scatter:
`1119 1362 -431 -1.55 -0.25`; a 27 m beech (with `--scatter 100`):
`1115.47 1366.85 -429.72 1.223 0`. Missing models drop out of the pick,
so without the Megaplants trees the same seed places different content
and these views differ.
The game places rocks over the whole Napf at load (`placeScenery` in
main.cpp; 21281 on steep ground, 124776 scree along rockfall runouts),
poppies, goldenrod and ferns (~200k together; ferns ~46k under the forest
with `--trees`), streamed meadow layers around the camera (a short grass mat,
seed-head tufts in patches, hawkbit drifts (grasses and hawkbit graded
by `Prototype::tint`), clover: ring ~6.8M slots,
~2.2M live; building their dense dither patterns adds ~10 s to Release
and ~59 s to Debug startup on the first run only: the patterns are cached,
see CLAUDE.md Gotchas), plus forests, lone trees and saplings with `--trees`
(~3.1M instances; all of these are causal layers on the `.geo` channels), which loads the rocks (and with `--trees` the
trees) as prototypes: Debug startup
~95 s, Release ~18 s. `--terrain FILE` loads another heightfield instead of `assets/terrain/napf.height` (e.g. the relief patch). `--placement off` skips it (Release ~11 s:
heightfield pyramid, normal map and transcoding the terrain's 4K
textures, ~8.7 s of it); use it for captures that
don't need placement. `--streamed off` drops only the streamed meadow layers
(grass mat, tufts, hawkbit, clover; not in the TLAS), so a raster capture
shows what `--reference` traces; the static table and its hash are unchanged. Debug runs print `placement check:` (GPU table vs
the CPU reference, must be 0 mismatches), `stream check:` (every
regenerated streamed cell vs the CPU reference, must be 0 mismatches) and
`placement hash 0x...` of the static table (0x07e033157372f778 for seed 1 without
trees, 0x5353fab653ec154d with `--trees`, both with `napf.geo` cooked, and the 30 traced Megascans Trees variants (alder, beech, hornbeam) cooked (docs/assets.md), every asset present). Capture runs print `streamed: L layers,
window WxW, ring C slots, N live`. Loading
placement also prints `TLAS:` (records, sizes, build ms, prototype BLAS
before and after compaction, cell margin, then VMA's blocks on the
device-local heaps and, in brackets, on all heaps including system
memory, now and at the peak while models loaded); without `--shadows rt` or
`--reference` nothing is ray traced, no TLAS or BLAS is built and it
prints `TLAS: off (no ray-traced mode)` with the cell margin and the same
memory figures. The far field's line gives the same pair at its peak.
Every run prints `Loaded: resident N MiB device memory allocated (all heaps
M MiB)` once at the first draw, after everything loaded: the VRAM gate's
resident figure. `--far-field off` draws
every tree as clusters instead of distant forest cells as voxels (A/B).
`--voxel-lod on|off|near` (default on): the voxel levels of tree models' cluster
DAGs (spec `2026-11-24-dag-voxel-foliage-design.md`; models with a foliage
mesh over 4 m tall) replace their triangles where the LOD cut picks them,
rasterized as ~1 px quads (architecture.md, Frame); off keeps the triangles
at every distance in every view, cascades included (A/B; slower: the
traversal visits every level, and the cull lists overflow at most views, so
off drops entries: not a clean reference). near is the reference for the
voxels' look: `--voxel-near 150`, so the light is the default's and only
the main view's geometry changes (0 dropped at the voxel-look stops).
`--voxel-near M` (default 0): with voxel LOD on, the main view keeps the
triangles of every instance whose origin is within M meters of the camera
(`Renderer::setVoxelNear`, common.glsl `voxelLodAt`; CPU reference
`voxelsAt` and `selectClustersBvh(..., voxels)`); beyond M the LOD cut picks
voxels by projected size as before; the cascades keep their voxel casters.
`--voxel-coverage hash|area-pixel-phase` (default area-pixel-phase; spec
`2026-11-26-voxel-leaf-coverage-design.md`): how the main view draws a voxel
of coverage f (voxel_shape.h `VoxelCoverage`, voxel_lod.glsl).
area-pixel-phase: a leaf voxel's quad shrinks to area f but never under one
pixel (a = 1 / q for a full quad of q pixels, at most 1) and draws on
floor(P p) of the P jitter phases (p = f / a), one more with chance the
fractional part of P p by its hash, placed at a stable-hash point inside
the full quad; wood draws whole quads by the hash (chance f, always at
f >= 0.5). hash (A/B, main's rule): every voxel draws its whole quad with
chance f, wood also always at f >= 0.5. The cascades always take hash.
`--far-shadows tlas|voxel` (default voxel): sun shadow rays from receivers
beyond `kFarShadowDistance` (4 km, lighting.glsl) skip the far-field trees
in the TLAS and read the far shadow map of the bricks instead
(`farfield_shadow.comp`, farfield.h: 8 m texels, 16-bit depth of the first
0.5-opacity point along the sun, read with a normal offset and a
slope-scaled bias, built on frame 1 and rebuilt a slice per
frame into a second map when the sun turns more than 0.1 degrees; a sun more
than 1 degree off the built map falls back to the per-ray brick march);
`tlas` turns the far shadow map off: with `--shadows rt` every receiver
traces every tree in the TLAS (legacy A/B; also what `--far-field off`
does), in raster mode receivers past 4 km take objects from the cascades
(up to cascade 4's 5.12 km). Raster mode with the far shadow map takes only
the far field past 4 km, no TLAS ray. Debug prints `far shadow map check:` (GPU map vs the CPU
replica) and the first build's ms.
`--gi off|probes` (default probes since 2026-11-23; off is the A/B and
the fallback for weaker GPUs, where crown sky visibility is the only canopy
occlusion of ambient): `probes`
lights the ambient term from global-illumination probes (spec
`docs/superpowers/specs/2026-11-20-gi-design.md`): a camera-centred
clipmap (5 levels, 2-32 m columns, 48^2 columns x 8 slices above the
terrain under each column), traced in compute against the heightfield
pyramid (`terrainRayDistance`) and, with `--trees`, the far-field voxels;
ground hits read a ground radiance cache (4 levels of 256^2 texels at 1-8
m, albedo from the rendered image, relit by the sun with terrain, tree and
cloud shadow plus the probes' own irradiance), misses the sky-view LUT + sky
capture. No ray tracing.
`--shadows raster|rt` (default raster; rt is an A/B, decisions.md): the
sun's visibility of objects
(trees, rocks, plants, models) from the cascaded shadow maps (lighting.glsl
`cascadeVisibility`: 3x3 tent PCF from 4 hardware depth-compare taps
(`shadowSampler`, the cascades' `sampler2DShadow` view of the bindless
table), finest cascade within its radius,
blended into the next over the outer 15%), or `rt` from one TLAS ray per
pixel toward a point on the sun disk (`rt` builds the TLAS and BLAS, which
the raster default doesn't). Either way the terrain march,
cloud shadow, contact shadows and the far path stay. Capture runs print
`sun shadows: N cascade renders, max X ms` (the slowest shadow pass of
the run: the re-render hitch). `--shadow-refresh` re-renders all five
cascades every frame (normally only when the camera or sun moves), to
measure their cost and cull list need in the averaged frames.
