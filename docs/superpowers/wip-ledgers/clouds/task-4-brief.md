### Task 4: Temporal resolve, depth-aware composite and cloud motion vectors

**Files:**
- Create: `engine/shaders/clouds_resolve.comp`, `engine/shaders/clouds_composite.comp`
- Modify: `renderer_clouds.cpp` (history ping-pong per resolution), `renderer_frame.cpp`, `renderer_setup.cpp`, `game/src/main.cpp` (`--capture-sequence N` writes `out_NNN.png` every N frames, for time-lapse and fly-through sequences), `CLAUDE.md`, `docs/decisions.md`

**Interfaces:**
- Consumes: Task 3 outputs; prepass-independent contract (reads depth; writes HDR color and motion only).
- Resolve: reproject by weighted cloud depth + wind displacement since last frame (`windOffset` of `wind` vs `previousWind` at the cloud altitude), variance clipping, disocclusion reset, lower history weight inside the layer; history reset when switching resolution.
- Composite: depth-aware (bilateral on full-resolution depth) upsample; blend by transmittance; where cloud opacity > 0.5, write cloud motion (weighted depth with the current and previous camera, plus the wind displacement) into the motion target.

- [ ] **Step 1:** Implement.
- [ ] **Step 2: Verify (user's motion checks), all under the GPU lock:**
  - fast turn (yaw rate equivalent via `--camera-velocity` circling, or a scripted turn flag if needed, builder's choice), flying toward the layer, flying through it (sequence across the base), and the summit silhouette in front of clouds drifting at 10 m/s: Laplacian ratio vs the still of the same view for each, reported, and each no worse than the same view with `--clouds off` beyond run-to-run noise;
  - silhouette crops at full resolution: no cloud history bleeding onto terrain, no halo (inspect);
  - fly-through sequence: no pop at the resolution switch.
- [ ] **Step 3:** Commit ("Resolve clouds temporally with their own motion vectors").

---

