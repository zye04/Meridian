# Global constraints (verbatim from the plan) — binding for every task


- Branch `clouds` in its own worktree (created from main by superpowers:using-git-worktrees); its own `build/` directory; `assets/` is a symlink to the main checkout's `assets/` (git-ignored, includes the Napf heightfield). Never touch the main checkout's `build/` or `assets/`. Release perf builds in the session scratchpad.
- **GPU lock (user, 2026-11-19):** every run of the game binary (captures and perf runs, Debug or Release) goes through `flock /tmp/meridian-gpu.lock <game binary> ...`. Hold it only for the run itself, never for builds.
- Conventions (CLAUDE.md): physical units, pre-exposure, camera-relative positions, reverse-Z, motion vectors previous - current in UV, atmosphere in km. Every struct marked "must match" changes with its GLSL twin (static_assert sizes).
- The cloud composite reads scene depth and writes HDR color and motion only (independent of M2b's prepass).
- Shared files (`common.glsl`, `mesh.frag`, `renderer_frame.cpp`, `gpu_types.h`, `renderer_setup.cpp`) get small, additive hunks; new code goes in new files (`renderer_clouds.cpp`, `clouds*.comp`, `weather.glsl`, `wind.glsl`).
- Defaults: fair-weather cumulus; cumulus base 2.5 km, tops up to 4.5 km above sea level; stratus/overcast 1.8-3.0 km; weather map 512² over ~60 km around the camera, texel-snapped; front line 60 km upwind of the world origin; history covers ~250 km of air travel; base noise 128³, detail 32³; shadow map 1024² over ~30 km.
- Budget: ~1 ms for clouds + shadow map + sky capture at 2560x1440 at the ground and above-layer views (Release, median of 5); inside the layer reported separately, target <= 2 ms. Over budget: report the numbers with the levers (step counts, resolution inside the layer, shadow-map and capture slicing) to the user before cutting anything.
- No new third-party dependencies.
- Each task is one commit with a what-and-why message, **no AI attribution / Co-Authored-By trailer**; CLAUDE.md and `docs/decisions.md` (with sources) updated in the same commit as the change they describe.
- Every task: `cmake --build build` (Debug: ASan+UBSan+validation) succeeds, `ctest --test-dir build` passes, every capture reports 0 validation errors.


## Review Focus (from the plan)

1. Camera crossing the cloud base or top while flying (resolution switch quarter <-> half, history weight change): no pop, no one-frame black or doubled clouds (Task 4 fly-through capture sequence across the base).
2. Sun at or below the horizon (dusk) and sun straight overhead: shadow map projection stays valid (no NaN from a degenerate basis), clouds go dark at night instead of glowing (Task 5 test for the sun-aligned basis at elevation 90° and -5°; Task 3 dusk capture).
3. Wind speed 0 and wind direction reversing: weather stays defined (no division by zero in front travel), clouds still evolve through the time coordinate (Task 1 tests with speed 0 and a 180° change).
4. Thin mountain silhouettes and terrain edges in front of moving clouds: no cloud history bleeding onto the terrain or halo (Task 4 silhouette motion capture, crops inspected).
5. Extreme time scale (time-lapse 600x) at 60 fps: fixed substeps keep weather and wind deterministic and stable (Task 1 test: 1 vs 60 substeps per second give the same state within 1e-5).


Spec: docs/superpowers/specs/2026-11-19-clouds-weather-design.md (the binding authority). Repo conventions: CLAUDE.md, docs/decisions.md.
