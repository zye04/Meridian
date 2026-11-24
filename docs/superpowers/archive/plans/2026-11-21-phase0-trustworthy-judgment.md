# Phase 0: Trustworthy Judgment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Status: done (2026-11-24 review): #14 benchmark route and #15 resemblance metric closed, #7 (G7 grey image) landed 2026-11-23 (decisions.md "Exposure: shade lift floor").

**Goal:** Make every later look and performance judgment trustworthy: a fixed camera route benchmarked with averages, 1% lows and per-pass ms per segment; a resemblance metric per matched view; the grey-image (G7) cause found.

**Architecture:** A camera route (keyframes, interpolated in the engine's camera header, unit-tested) drives the existing `--benchmark` recorder; a Python summary splits the CSV into the route's segments and flags the 180 fps / 105 fps lines and the passes that dominate. A Python metric compares a matched capture with its reference frame over labelled regions. G7 is an investigation with captures, ending in a fix or a follow-up spec.

**Tech Stack:** C++20 (game, engine header), doctest, Python 3 + numpy (tools, run with `python3 -I`), ImageMagick (existing tools), Vulkan game binary.

**Spec:** `docs/superpowers/specs/2026-11-21-chlorophyll-parity-programme-design.md` (Phase 0 section; Calls made 1, 2, 7).

## Global Constraints

- Native 2560x1440, `--aa taa`, raster shadows (the defaults); every game run through `flock /tmp/meridian-gpu.lock`; compare ms only between identical `Render 2560x1440` lines.
- Performance lines: 180 fps average (5.5 ms) and 105 fps (9.5 ms) in the heaviest segment.
- Commits: one feature each, message explains what and why, no AI attribution trailer of any kind.
- CLAUDE.md and decisions.md updated in the same commit as a new flag or tool.
- Python tools: no third-party deps beyond numpy; ImageMagick via subprocess like `tools/image-gates.py`.
- The reference frames (`assets/reference/...`) are git-ignored and never committed.

## Review Focus

1. A route time before the first key or after the last: the camera holds the first / last key (no extrapolation, no NaN).
2. Yaw keys across the ±π wrap (e.g. 3.0 -> -3.0): the camera turns the short way, not a full revolution.
3. A malformed route file (missing fields, non-increasing times, comments, blank lines): a clear error naming the line, the game exits non-zero before startup.
4. A benchmark shorter or longer than the route: segments the CSV doesn't reach are reported as "not reached", not as zero fps.
5. Reference frames with black side bars and the channel avatar: the metric's regions are in the frame's own normalized coordinates and never include the bars.

---

### Task 1: Camera route (engine header + tests)

**Files:**
- Create: `engine/include/meridian/camera_route.h`
- Test: `tests/camera_route_test.cpp` (add to `tests/CMakeLists.txt`'s `add_executable(tests ...)`)

**Interfaces:**
- Produces:
  - `struct CameraKey { float time; Vec3 position; float yaw; float pitch; std::string label; };`
  - `struct CameraPose { Vec3 position; float yaw; float pitch; };`
  - `std::vector<CameraKey> parseRoute(std::istream &in);` — one key per line `TIME X Y Z YAW PITCH [LABEL]`, `#` comments and blank lines skipped; throws `std::runtime_error("route line N: ...")` on a short line or a time not greater than the previous key's; at least two keys.
  - `CameraPose routePose(const std::vector<CameraKey> &keys, float time);` — linear position and pitch, yaw along the shortest arc, clamped to the first/last key outside the range.
  - `float routeDuration(const std::vector<CameraKey> &keys);` — last key time minus first.
  - A key's `label` names the segment that starts at it (empty = continues the previous segment).

- [ ] **Step 1: Write the failing tests** in `tests/camera_route_test.cpp`:
  - `"routePose interpolates position and pitch linearly"`: keys (0, (0,0,0), 0, 0) and (2, (10,0,-4), 0, 0.4); at t=1: position (5,0,-2), pitch 0.2.
  - `"routePose clamps outside the route"`: t=-1 gives key 0's pose, t=5 gives the last key's pose.
  - `"routePose turns yaw the short way across the wrap"`: keys yaw 3.0 and -3.0 over 0..1; at t=0.5 `std::abs(std::remainder(yaw - 3.14159265f, 6.2831853f)) < 1e-4`.
  - `"parseRoute skips comments and keeps labels"`: input `"# r\n0 1 2 3 0.5 -0.1 meadow\n\n4 1 2 9 0.5 -0.1\n"` → 2 keys, key 0 label `"meadow"`, key 1 label `""`, key 1 position.z 9.
  - `"parseRoute rejects short lines and non-increasing times"`: `CHECK_THROWS_WITH_AS(... "0 1 2 3 0.5\n1 ...", "route line 1: expected TIME X Y Z YAW PITCH [LABEL]", std::runtime_error)`; times `0`, `0` → throws with `"route line 2: time must increase"`.
- [ ] **Step 2: Run** `cmake --build build && ./build/tests/tests -tc="*route*"` — Expected: compile failure (header missing).
- [ ] **Step 3: Implement** the header (inline functions, namespace `meridian`, includes `math/vec3.h`, `<istream>`, `<sstream>`, `<string>`, `<vector>`, `<stdexcept>`, `<cmath>`). Yaw: `start + remainder(end - start, 2π) * f`.
- [ ] **Step 4: Run** `cmake --build build && ./build/tests/tests -tc="*route*"` — Expected: all route tests pass; then `ctest --test-dir build` all pass.
- [ ] **Step 5: Commit** `git add engine/include/meridian/camera_route.h tests/camera_route_test.cpp tests/CMakeLists.txt && git commit -m "Camera route: keyframed camera path for benchmark fly-throughs"` (message body: why — #14).

### Task 2: `--route` in the game, the parity route, the summary tool

**Files:**
- Modify: `game/src/main.cpp` (Options, argument parsing, usage text, validation, the frame loop's camera update)
- Create: `tools/routes/parity.route`
- Create: `tools/bench-route.py`
- Modify: `CLAUDE.md` (flag + tool + route), `docs/decisions.md` (entry: benchmark route and ledger, why route averages over single shots)

**Interfaces:**
- Consumes: `parseRoute`, `routePose`, `routeDuration` (Task 1).
- Produces:
  - Game flag `--route FILE`: only with `--benchmark` (else error `"--route needs --benchmark"`); the camera follows `routePose(keys, t)` with t = 0 during warm-up and t = seconds since recording started afterwards (real time, so frame times are real); if `--benchmark`'s SECONDS is shorter than the route the route is cut, longer holds the last key. Route errors exit non-zero before the renderer starts.
  - `tools/bench-route.py CSV ROUTE` prints one row per labelled segment: `segment  frames  avg_fps  low1_fps  avg_ms  top passes (ms)`, the whole-route row last, and flags `BELOW 180` on the route row and `BELOW 105` on any segment; segments past the CSV's end print `not reached`. Segment boundaries come from cumulative `frame_ms`. 1% low = 1000 / mean of the slowest ceil(1% n) frame_ms (as `frame_stats.h`). Top passes: the 4 largest per-pass column means.

- [ ] **Step 1: Write the route** `tools/routes/parity.route` (~60 s, `--trees` assumed), labelled segments in this order, each 10-15 s, using views from CLAUDE.md: `meadow` (start view `1119 1355.9 -431 -1.55 -0.1`, walking west at eye height), `grass` (walk height looking slightly down near the start), `forest-edge` (`600 1205.6 -400 -1.55 0.02`), `forest-interior` (`85 1158.6 -53 -1.55 0.25`), `aerial` (climb to the forest overview `700 1450 -400 -1.55 -0.5`, then the high overview `1119 4354 -431 -1.55 -0.35`). Keep keys >= 2 s apart; no key inside terrain (check `y` against the listed ground heights).
- [ ] **Step 2: Implement `--route`** in main.cpp (parse at argument time into `options.route`, `std::vector<meridian::CameraKey>`; apply in the loop where `--camera-velocity` is applied; add to the usage string and the CLAUDE.md `--benchmark` paragraph).
- [ ] **Step 3: Verify the flag errors:** `./build-release/game/game --route tools/routes/parity.route` → exits 1 with `--route needs --benchmark`; a copy of the route with one line cut to 5 fields → exits 1 naming the line.
- [ ] **Step 4: Implement `tools/bench-route.py`** (numpy; reads the CSV header for pass columns).
- [ ] **Step 5: Run the benchmark:** `cmake --build build-release && flock /tmp/meridian-gpu.lock ./build-release/game/game --trees --benchmark 60 /tmp/parity.csv --route tools/routes/parity.route && python3 -I tools/bench-route.py /tmp/parity.csv tools/routes/parity.route` — Expected: 5 segment rows + a route row, `Render 2560x1440` in the log, 0 validation errors; a second run's route avg_fps within ~3% of the first.
- [ ] **Step 6: Record the baseline** (both runs' tables) in the decisions.md entry and in issue #14 (`gh issue comment 14 --body-file ...`).
- [ ] **Step 7: Commit** `git add game/src/main.cpp tools/routes/parity.route tools/bench-route.py CLAUDE.md docs/decisions.md && git commit -m "Benchmark route: --route fly-through, per-segment fps, 1% lows and pass ledger"`.

### Task 3: Resemblance metric

**Files:**
- Create: `tools/resemblance.py`
- Create: `docs/superpowers/research/matched-views/regions.json` (after the matched-views lane lands; if it hasn't, create it with the frames it has)
- Modify: `CLAUDE.md` (tool line), `docs/superpowers/research/matched-views/README.md` (how to run it)

**Interfaces:**
- Consumes: matched-view captures (ours) and the reference frames (git-ignored paths).
- Produces: `tools/resemblance.py OURS.png REF.png --regions regions.json --view NAME` printing, per region present for that view (`sky`, `foliage-lit`, `foliage-shadow`, `grass`, `far`): OKLCh mean L, mean C, mean h and L p5/p95 for ours and ref and the delta; plus whole-frame rows `contrast` (std of L), `sharpness` (Laplacian std on L, as image-gates' laplacian but in OKLab L), `shadow/sunlit` (mean L foliage-shadow / mean L foliage-lit). Regions in `regions.json`: `{view: {"ours": {region: [x0,y0,x1,y1]}, "ref": {...}}}` in normalized [0,1] coordinates of each image (the ref's excluding its black bars).

- [ ] **Step 1: Implement** `tools/resemblance.py` (sRGB -> linear -> OKLab -> OKLCh per Björn Ottosson's published matrices; hue mean as a circular mean).
- [ ] **Step 2: Verify identity:** `python3 -I tools/resemblance.py X.png X.png --regions regions.json --view target-meadow` on any capture → every delta prints `0.000`.
- [ ] **Step 3: Write `regions.json`** for each matched view (open each side-by-side, pick rects that sit fully inside one material in both images).
- [ ] **Step 4: Run on every matched view** and paste the table into issue #15 and the matched-views README.
- [ ] **Step 5: Commit** `git add tools/resemblance.py docs/superpowers/research/matched-views/regions.json docs/superpowers/research/matched-views/README.md CLAUDE.md && git commit -m "Resemblance metric: OKLCh region stats against the matched reference frames"`.

### Task 4: G7 diagnosis (grey, washed-out image)

**Files:**
- Create: `docs/superpowers/research/2026-11-2x-G7-grey-image.md` (findings, with capture paths)
- Modify (only if the cause is engine-side and the fix is a few lines): the owning source file; otherwise a spec for the fix.

**Interfaces:**
- Consumes: the user's screenshots `docs/images/look-gap-2026-11-21/user-02-*.jpg`, `user-05-*.jpg` (views under the alder canopy and over the valley); the capture log's exposure (EV100) and sky irradiance lines.

- [ ] **Step 1: Reproduce** both views as captures (`--trees`, the default build; find the camera by matching the screenshots, starting from the forest-interior and high-overview views). Record EV100 and the image's mean OKLab L.
- [ ] **Step 2: A/B one factor at a time** on both views, capture each: `--haze clean` vs default; `--gi probes` vs off; `--grade ~/.config/meridian/grade.json` vs none (interactive runs load it; captures don't); `--effects micro=0` and crown visibility on/off if a toggle exists; the Megaplants vs the alder build (G6 grey leaves). Expected output: a table `factor | EV100 | mean L | p5 L | sky/ground ratio`.
- [ ] **Step 3: Decide the cause:** the factor(s) whose A/B moves mean L / p5 toward the matched references (Task 3's numbers for the closest view). Write the doc with the table and side-by-sides.
- [ ] **Step 4: Fix** if it is a constant or a metering rule (with before/after captures and the motion gates unchanged); otherwise write the follow-up as a sub-issue of #7 with the evidence.
- [ ] **Step 5: Commit** the findings doc (and the fix if any) with a message stating the cause; comment the result on #7.
