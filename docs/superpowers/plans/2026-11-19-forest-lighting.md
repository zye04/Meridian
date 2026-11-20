# M2b Forest Lighting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Forest light that is right and affordable: per-tile near/mid/far ray representations in a partitioned TLAS over M2a's voxel far field, sky visibility traced after a depth + normal prepass, and the first undergrowth.

**Architecture:** A prepass writes depth, motion and a normal/roughness guide; a compute pass traces one sky ray per pixel (TLAS to 20 m, then M2a's far-field march through terrain and voxel bricks) and filters it before the main pass shades each pixel once (depth EQUAL). Ray geometry lives in a partitioned TLAS whose tiles switch between full-detail BLAS, a three-quad opacity-micromap proxy and nothing (M2a's voxel far field) by camera distance, updated incrementally on the GPU.

**Tech Stack:** C++20, Vulkan 1.3, `VK_NV_partitioned_acceleration_structure`, `VK_EXT_opacity_micromap`, ray queries, GLSL, doctest.

**Spec:** `docs/superpowers/specs/2026-11-19-forest-lighting-design.md` (read it with this plan, and the M2a spec it builds on). Starts after M2a (`docs/superpowers/plans/2026-11-19-forests.md`) is complete; its recorded measurements (TLAS build/memory, shadow cost, frame vs ~7.7 ms) are inputs.

## Global Constraints

- Everything in M2a's Global Constraints (conventions, curvature, id space, capacities and drop counters, commit rules, no AI attribution, CLAUDE.md/decisions.md in the same commit, Debug build + tests + 0 validation errors per task, Release perf in the scratchpad, every game run under `flock /tmp/meridian-gpu.lock`).
- Starting radii: near 150 m, mid 2 km; hysteresis switches back at 0.9 x the boundary; partitions hold <= ~1,024 instances; sky rays: TLAS to ~20 m, far march to a few hundred meters. Starting values, replaced by measurements and recorded.
- Missing `VK_NV_partitioned_acceleration_structure` feature or `VK_EXT_opacity_micromap` is a startup error with a clear message; no silent fallback.
- Normal target: RGBA16F, world-space normal xyz + roughness w (DLSS RR packed guide layout). Motion vectors are written by the prepass. Nothing may depend on the prepass beyond depth, motion and normal (visibility-buffer swap later).
- Shadow rays: TLAS over full length; the field skips tiles held by the TLAS. Sky rays: TLAS to 20 m, then the field from 20 m with no tile skip.
- Gate: <= 16.7 ms at the forest views with everything on (no shadow cap, sky visibility on). Beating M2a's frame is a recorded goal. Levers if missed, in order: checkerboard sky rays, smaller near radius, smaller mid radius; any visible quality cost reported to the user. Target toward 1.2 ms for sun shadow + sky visibility.

## Review Focus

1. Camera teleport (`--camera` far from the start, or walk-mode respawn): every tile reaches its representation before the first shown frame; no frame with trees missing from shadows (Task 4 capture at a teleported view vs the static-TLAS reference).
2. Fast flight (50 m/s over forest): per-frame partition writes stay under the cap and pending transitions never starve far-behind tiles (Task 4 fly-through log: max update ms under cap, pending queue drains).
3. Sun near the horizon through a forest edge: canopy transmittance gives a soft partial shadow, never a hard cut or a lit forest floor (M2a Task 8 brute-force march test with a grazing ray; Task 4 sunset capture).
4. Masked ferns and impostors in the prepass: the main pass's EQUAL test leaves no holes (Task 1 clear-color check; Task 6 repeats it with ferns).
5. Disocclusion at canopy edges in motion: sky visibility neither shimmers nor leaves dark halos (Task 5 motion capture past a forest edge, Laplacian ratio and visual inspection).

---

### Task 1: Depth + normal prepass

**Files:**
- Create: `engine/shaders/prepass.frag` (variants `ALPHA_MASK`, `TERRAIN`, `IMPOSTOR`)
- Modify: `renderer_frame.cpp` (prepass early/late with the existing culling; main pass reuses the draw lists with depth EQUAL, writes off; passes `"prepass early"`, `"prepass late"`, `"scene"`), `renderer_setup.cpp` (pipelines, normal target RGBA16F), `mesh.mesh`, `terrain.mesh`, `impostor.mesh` (`invariant gl_Position`, shared position code), `mesh.frag`/`impostor.frag` (no motion output; masked uses the opaque pipeline in the main pass), `post.glsl`/`taa.comp` if motion bindings move, `CLAUDE.md` (Frame), `docs/decisions.md` (prepass rationale: canopy-edge disocclusion; slice 4 replaces it)

**Interfaces:**
- Produces: `Image normalRoughness` (RGBA16F, world normal + roughness) and the depth/motion images bound for later compute (`sky_visibility.comp` in Task 5). Impostors reproduce `gl_FragDepth` identically in both passes.

- [ ] **Step 1:** Implement; blended surfaces keep their path after the main pass.
- [ ] **Step 2: Verify.** With a debug clear color in the HDR target, no pixel keeps it at the default, forest-edge and interior views (no EQUAL holes, including masked and impostors). Mean difference vs the previous commit within run-to-run noise; motion capture Laplacian ratio unchanged; 0 dropped.
- [ ] **Step 3:** Release (under the GPU lock), median of 5 at the forest views: net cost (prepass + main) vs the single pass, recorded.
- [ ] **Step 4:** Commit ("Add a depth and normal prepass so each pixel shades once").

---

### Task 2: (moved to M2a) Far ray field

Superseded on 2026-11-20: the 2D canopy field is replaced by M2a's voxel far field (`docs/superpowers/specs/2026-11-20-far-field-design.md`; M2a Tasks 8-9), which already provides `farFieldTransmittance(worldOrigin, direction, startDistance, maxDistance, skipTlasCells)`, its CPU replica and tests. M2b only adds the per-cell TLAS flags (Task 4). The number is kept so references stay valid.

---

### Task 3: Mid proxies with opacity micromaps

Open until M2a Task 8 reports (user, 2026-11-20): the voxel far field may replace these proxies; if it does, this task goes.

**Files:**
- Create: `engine/src/render/renderer_micromap.cpp` (OMM build helper shared with Task 6), `engine/shaders/proxy_bake.frag` (orthographic coverage along three axes, reuses the bake path of M2a's impostors)
- Modify: `renderer_setup.cpp` (enable `VK_EXT_opacity_micromap`; startup error if absent), `renderer_impostor.cpp` (bake proxies next to the impostor; same cache rule), `gpu_types.h` (`PrototypeInfo`: proxy BLAS address), `tests/impostor_test.cpp`, `docs/decisions.md` (HPG 2026 proxies, Indiana Jones OMM)

**Interfaces:**
- Produces: `VkDeviceAddress PrototypeInfo.proxyBlas` (three quads, one OMM each, opaque/transparent states), coverage thinning reads the three-axis pass fractions M2a's far-field bake (`farfield_bake.comp`) already measures; no separate extinction. `MicromapBuild buildMicromap(cmd, coverage, width, height, subdivisionLevel)` in `renderer_micromap.cpp` (internal helper).
- Coverage per quad thinned to the measured pass fraction of rays through the full tree along that axis.

- [ ] **Step 1: Failing test:** coverage thinning: for a synthetic coverage image and target pass fraction p, the opaque fraction after thinning is within 1% of 1 - p.
- [ ] **Step 2:** Implement bake, thinning, OMM build, proxy BLAS.
- [ ] **Step 3: Verify.** Debug run tracing shadows against proxies only (every placed record swapped to its proxy) at the forest-edge view vs full detail: shadow density comparable (mean difference reported); inspect the proxy shadow shape. Load time added recorded.
- [ ] **Step 4:** Commit ("Bake three-quad opacity-micromap proxies for mid-range shadows").

---

### Task 4: Partitioned TLAS with near/mid/far tiles

**Files:**
- Create: `engine/shaders/tlas_update.comp`, `engine/src/render/renderer_ptlas.cpp`, `tests/representation_test.cpp`
- Modify: `renderer_setup.cpp` (enable `VK_NV_partitioned_acceleration_structure`, query `maxPartitionCount`; startup error if absent), `renderer_placement.cpp`/`renderer_frame.cpp` (replace M2a's static TLAS; pass `"tlas update"`), `gpu_types.h` (tile tables, operation buffers, capacities), `mesh.frag`/`impostor.frag`/`voxel.frag` (shadow rays: TLAS full length + `farFieldTransmittance` with `skipTlasCells = true`; M2a's receiver-distance rule and the placed-tree mask bit go), `farfield.glsl` (per-cell `cellInTlas` flags), `engine/include/meridian/farfield.h` + `tests/farfield_test.cpp` (CPU replica's `cellInTlas` argument non-null: a ray through a flagged cell ignores it), `game/src/main.cpp` (`--representation-debug` tint), `CLAUDE.md` (Frame, Conventions), `docs/decisions.md` (PTLAS numbers, radii, cap)

**Interfaces:**
- Produces in a header shared by C++ tests and the shader logic (`engine/include/meridian/representation.h`): `enum class Representation : uint8_t { Far, Mid, Near };` and `Representation chooseRepresentation(float distance, Representation current, float nearRadius, float midRadius, float hysteresis)` (switches out at the boundary, back at `hysteresis` x boundary).
- Tiles: groups of 64 m cells with <= ~1,024 instances; partition = tile; hand-placed objects in their own partition(s); global partition empty. Partition translation = tile origin. Per frame: compute appends write / update (BLAS swap near<->mid) / clear ops for at most `kMaxTileTransitionsPerFrame` tiles, nearest first; teleports run the full set before the first shown frame. Per-cell `cellInTlas` flags (every cell of a tile held near or mid) follow. Capture output adds TLAS instances per representation and partition operations this frame.
- Undergrowth layers only ever enter as Near (used in Task 6).

- [ ] **Step 1: Failing tests:** `chooseRepresentation` over a camera path oscillating +-5 m around each boundary changes state at most once per crossing direction; distances 0, 149, 151, 1999, 2001 m pick Near, Near, Mid, Mid, Far from Far start states as specified.
- [ ] **Step 2:** Implement.
- [ ] **Step 3: Verify.** Near/mid boundary view: still and motion (`--camera-velocity` across the boundary) vs the M2a static full-detail TLAS reference (keep it reachable through a debug flag until this task's commit): no pop, mean difference reported; teleported view correct on its first frame; fly-through at 50 m/s logs max `tlas update` ms under the cap and the pending queue drains; sunset capture through a forest edge (soft partial shadow); TLAS memory vs M2a recorded.
- [ ] **Step 4:** Commit ("Partition the TLAS by tile with near, mid and far representations"); remove the static-TLAS debug path in this commit.

---

### Task 5: Sky visibility

**Files:**
- Create: `engine/shaders/sky_visibility.comp` (trace), `engine/shaders/sky_visibility_filter.comp` (temporal + edge-aware spatial)
- Modify: `renderer_frame.cpp` (after the prepass: trace, filter; passes `"sky visibility"`, `"sky filter"`), `renderer_setup.cpp` (history images, sampling sequence), `mesh.frag`/`impostor.frag` (multiply diffuse sky ambient incl. ground bounce and the specular sky term with roughness-based specular occlusion), `CMakeLists.txt` (define `SKY_VISIBILITY_RAW` that skips the history, for RR later), `game/src/main.cpp` (`--sky-visibility off`), `CLAUDE.md`, `docs/decisions.md`

**Interfaces:**
- Consumes: Task 1 targets; `farFieldTransmittance(..., startDistance = 20, ..., skipTlasCells = false)` (M2a Task 8).
- Produces: filtered visibility image read by the main pass; blended surfaces use 1.

- [ ] **Step 1:** Implement: cosine ray per pixel (blue-noise/R2 by frame), TLAS to ~20 m then the far march; temporal accumulation (~0.05-0.1) with prepass motion, reset on depth/normal mismatch, extra spatial support on disoccluded pixels; edge-aware spatial filter.
- [ ] **Step 2: Verify.** Interior, forest-edge and ridge views: interiors and the ground under canopies darker, open meadow unchanged vs off (mean difference over a meadow-only crop < 0.5/255); motion capture past a forest edge at 5 m/s: Laplacian ratio within noise of the same view with `--sky-visibility off`, no shimmer or dark halos (inspect frames); 0 validation errors.
- [ ] **Step 3:** Commit ("Occlude sky light with traced and filtered sky visibility").

---

### Task 6: Undergrowth

**Files:**
- Modify: `game/src/main.cpp` (undergrowth layer: Megascans common fern, beech and rowan saplings; max draw distance ~80 m), `tools/cook/cook.cpp` (Fab `BLEND` cards cooked as `MASK` when requested: `--force-mask`), `renderer_resources.cpp` (masked BLAS geometry gets an OMM from its alpha texture via `renderer_micromap.cpp`), `CLAUDE.md`, `docs/decisions.md`

- [ ] **Step 1:** Implement.
- [ ] **Step 2: Verify.** Interior view: ferns and saplings present, cast shadows, occlude sky; Task 1's clear-color check still passes with ferns; any-hit invocations per frame drop with OMM vs without (counter or timing, reported); 0 dropped.
- [ ] **Step 3:** Commit ("Add forest undergrowth with opacity-micromapped ferns").

---

### Task 7: Milestone measurement, gate and wrap-up

**Files:**
- Modify: `docs/decisions.md`, `CLAUDE.md` (Known gaps: leaf transmission next; remaining gap to ~7.7 ms)

- [ ] **Step 1:** Release (under the GPU lock), 2560x1440, median of 5 at the four views (interior, forest edge with low sun, ridge, near/mid boundary): per-pass ms, sun shadow + sky visibility total, frame vs M2a's frame and vs ~7.7 ms.
- [ ] **Step 2:** Over 16.7 ms anywhere: apply levers in order (checkerboard sky rays; smaller near radius; smaller mid radius), re-measuring after each; report every visible quality cost to the user with before/after captures.
- [ ] **Step 3:** decisions.md: final radii, cap, OMM level, numbers, the gap left for M5. Sweep CLAUDE.md, decisions.md and specs for statements made stale (static TLAS, `terrainOccludes` boolean, unoccluded ambient, single-pass shading). Commit ("Measure M2b forest lighting and record the gap to target").
