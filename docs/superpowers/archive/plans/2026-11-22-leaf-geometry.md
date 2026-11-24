# Tree Leaves as Opaque Geometry Implementation Plan

Status: done 2026-11-23 (merged; decisions.md "Tree leaves as opaque geometry").

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `meridian-cook --tree` turns the Megascans Trees' alpha-tested leaf and decoration cards into opaque leaf triangles traced from the atlas, so trees render without alpha testing.

**Architecture:** A new cook module `tools/cook/leaf_trace.{h,cpp}` traces a material's base-colour alpha once into a simplified, filled atlas-space mesh (`traceAtlas`), then clips that mesh onto every card triangle through the card's UVs (`clipToCards`). `tree.cpp` runs both before `splitLeafPieces` for every textured leaf material with alpha, writes the material opaque, and simplifies leaves to 2 mm (wood keeps 1 cm). A second module `tools/cook/coverage.{h,cpp}` (`meridian-cook --coverage`) measures crown coverage across the cluster LOD cut, the gate for crowns thinning out.

**Tech Stack:** C++20, meshoptimizer v1.0 (already a dependency), fastgltf, basisu, doctest.

**Spec:** `docs/superpowers/specs/2026-11-21-materials-fidelity-design.md` section 2 (D1, D2).

## Global Constraints

- Leaves and decoration of the Megascans Trees: opaque, double-sided, no alpha test; masked plants (grass, flowers, ferns, streamed meadow) unchanged.
- Leaf outline simplification error ~2 mm (`kLeafSimplifyError = 0.002f`); wood keeps 1 cm (`kWoodSimplifyError = 0.01f`).
- Trace input: the cooked base colour's alpha (4K leaf atlas, 2K decoration), cutoff = the material's (0.5).
- Total tree triangles <= ~30M over all cooked prototypes; variants: Forest 5 per species, Field 6 (alder 3, hornbeam 2, beech 1), Sapling 3 per species.
- Budgets: ~1.2M per Forest/Field variant (beech up to 1.5M), ~0.3M per sapling; if 2 mm outlines don't fit, cut variants before coarsening outlines.
- Gates vs today's cards (same build and AA): motion jitter lower (`tools/motion-clarity.sh` forest rows), crown coverage within 5% from 30 m to the voxel switch, forest edge GPU ms not above today's, tree memory under the cap, close-up look, no validation errors, Debug checks pass.
- Deviation from the spec, decided here: no Douglas-Peucker + earcut. A filled marching-squares mesh simplified by meshoptimizer (planar, absolute error) gives the same outline tolerance, handles holes and nesting for free, and adds no dependency. Record this in decisions.md (Task 5).
- Every game run through `flock /tmp/meridian-gpu.lock`. Commits: no AI attribution, one feature per commit.
- The main checkout's `assets/` is shared with other sessions: cook only into this worktree's own species directories until the merge (Task 0).

## Review Focus

1. Card UVs outside [0,1] (the alder's strip cards at u ~ -0.02, some v < 0): leaves must appear where the wrapped atlas has them, not vanish. Test in Task 2.
2. A leaf crossing a card's diagonal must stay one piece: positions on a shared card edge bit-identical in both card triangles. Test in Task 2.
3. Bent cards (non-coplanar triangles): every output vertex lies on its card triangle. Test in Task 2.
4. Specks and single texels in the alpha (noise, mip bleed): dropped below the minimum area, never a flood of tiny pieces. Test in Task 1.
5. Materials that must not be traced: untextured leaves (Megaplants palette), opaque-alpha textures, wood. Test in Task 3.

---

### Task 0: Worktree assets and card baselines

**Files:** none in git (asset layout + measurements under `.superpowers/sdd/leaf-geometry/artifacts/`).

- [ ] **Step 1: Asset layout.** In the worktree, `assets/` is a real directory: a symlink per entry of the main checkout's `assets/` except `source`; `assets/source/` the same except `megascans`; `assets/source/megascans/` the same except `blackalder`, `europeanbeech`, `europeanhornbeam`, which are symlinks to the main checkout's card cooks for the baseline (replaced by real directories in Task 4).
- [ ] **Step 2: Builds.** `cmake -S . -B build && cmake --build build` and Release `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`. Expected: both build.
- [ ] **Step 3: Card baselines** (Release, fullscreen 1440p, default AA = DLSS DLAA; also `--aa taa` for GPU ms): GPU ms and drawn triangles at the forest edge `600 1205.6 -400 -1.55 0.02 --trees`, forest interior `85 1158.6 -53 -1.55 0.25 --trees`, open view toward the summit `1119 1356 -431 -1.55 -0.02 --trees`; `tools/motion-clarity.sh build-release <out>/cards-motion`; startup lines `TLAS:`, `Prototype voxels:`, `Far shadow map:` and Release startup time; on-disk size of the three species' cooked trees. Write the table to `artifacts/baseline-cards.md`.

### Task 1: Atlas trace

**Files:**
- Create: `tools/cook/leaf_trace.h`, `tools/cook/leaf_trace.cpp`
- Modify: `tools/cook/CMakeLists.txt` (add `leaf_trace.cpp` to `meridian-cook-lib`)
- Test: `tests/leaf_trace_test.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Produces:
  ```cpp
  namespace meridian::cook {
  // Atlas-space triangles: positions are texture coordinates (glTF: u right,
  // v down, 0..1 over the image).
  struct AtlasMesh {
    std::vector<std::array<float, 2>> uv;
    std::vector<uint32_t> indices;
  };
  // alpha: width * height bytes, row 0 = v 0. Fills where alpha >= cutoff * 255
  // (marching squares, edges interpolated between texel centres), simplifies
  // to `toleranceTexels`, drops connected parts under `minAreaTexels`.
  AtlasMesh traceAtlas(const std::vector<uint8_t> &alpha, uint32_t width, uint32_t height,
                       float cutoff, float toleranceTexels = 1.0f, float minAreaTexels = 16.0f);
  double atlasArea(const AtlasMesh &mesh); // in UV units^2
  }
  ```

- [ ] **Step 1: Failing tests** in `tests/leaf_trace_test.cpp`:
  - `traceAtlas fills an opaque disk`: 256x256, alpha 255 inside radius 80 texels around (128,128), 0 outside; `atlasArea * 256^2 == Approx(pi*80^2).epsilon(0.02)`; `indices.size()/3 < 400`; every triangle has positive area in UV.
  - `traceAtlas keeps holes`: ring 40..90 texels; area within 2% of `pi*(90^2-40^2)`; no triangle contains the centre (point-in-triangle test on (0.5,0.5)).
  - `traceAtlas drops specks`: the disk plus a 3x3-texel island at (10,10); result area within 2% of the disk alone and no vertex within 6 texels of (10,10).
  - `traceAtlas of transparent alpha is empty`: all zero -> `indices.empty()`.
- [ ] **Step 2: Run** `cmake --build build && ./build/tests/tests -tc="traceAtlas*"`. Expected: FAIL (unresolved `traceAtlas`).
- [ ] **Step 3: Implement** in `leaf_trace.cpp`. Marching squares over the (width-1)x(height-1) cells between texel centres (texel (x,y) centre = ((x+0.5)/w, (y+0.5)/h)); each cell emits the filled polygon of its case (16 cases; saddles 5/10 resolved by the mean of the four corners), edge points at the linear interpolation of alpha to `cutoff*255`; fan-triangulate each cell polygon. Weld (`meshopt_generateVertexRemap` on exact positions: shared cell edges compute their crossing from the same two texels in a fixed order so neighbours match bit for bit). Simplify with `meshopt_simplify` on (u, v, 0) positions, target index count 0, error `toleranceTexels / max(width,height)` with `meshopt_SimplifyErrorAbsolute`. Then union-find over shared indices and drop components whose area < `minAreaTexels / (width*height)`. Compact the vertices.
- [ ] **Step 4: Run** the same command. Expected: 4 test cases PASS.
- [ ] **Step 5: Commit** `Cook: trace an atlas alpha into a filled, simplified mesh`.

### Task 2: Clip the atlas mesh onto cards

**Files:**
- Modify: `tools/cook/leaf_trace.h`, `tools/cook/leaf_trace.cpp`
- Test: `tests/leaf_trace_test.cpp`

**Interfaces:**
- Consumes: `AtlasMesh` (Task 1).
- Produces:
  ```cpp
  struct CardVertex {
    std::array<float, 3> position;
    std::array<float, 3> normal;
    float uv[2];
  };
  // Every card triangle (indices into `vertices`) keeps the atlas triangles
  // under its UV triangle (in every integer UV tile it touches), clipped to it
  // and mapped by barycentrics to its position and normal. Output vertices on
  // a card edge are computed from that edge's endpoints in a fixed order, so
  // cards sharing an edge produce bit-identical positions there.
  void clipToCards(const AtlasMesh &atlas, const std::vector<CardVertex> &vertices,
                   const std::vector<uint32_t> &indices, std::vector<CardVertex> &outVertices,
                   std::vector<uint32_t> &outIndices);
  ```
  `tree.cpp`'s `Vertex` becomes `using Vertex = CardVertex;` (same layout: position, normal, uv).

- [ ] **Step 1: Failing tests:**
  - `clipToCards maps the atlas onto a flat card`: atlas = Task 1's disk; card = unit square in xz (2 triangles sharing the diagonal, UVs 0..1, normals +y); output area (3D) == `atlasArea` within 1e-4 relative; all output vertices have y == 0 and normal (0,1,0).
  - `clipToCards keeps a leaf across the card's diagonal in one piece`: same card; union-find over output triangles by exact position bytes gives exactly 1 component (the disk crosses the diagonal).
  - `clipToCards follows a bent card`: the square's second triangle rotated 30 degrees about the diagonal; every output vertex lies within 1e-5 m of the plane of the card triangle that produced it; component count still 1.
  - `clipToCards wraps UVs outside [0,1]`: card UVs shifted by (-1, 0); output area equals the unshifted case within 1e-4 relative, output u in [-1, 0].
  - `clipToCards clips to the card`: a card covering only the UV quarter [0,0.5]^2; output area equals the atlas area inside that quarter (computed by clipping the atlas triangles to the quarter in UV, scaled by the card's area per UV area) within 1e-4 relative.
- [ ] **Step 2: Run** `./build/tests/tests -tc="clipToCards*"`. Expected: FAIL.
- [ ] **Step 3: Implement.** Uniform grid (256x256 over [0,1]^2) of atlas triangle ids by UV bounds. Per card triangle: for each integer tile (i,j) its UV bounds touch, query the grid with the card's UVs minus (i,j); clip each atlas triangle to the card's UV triangle (Sutherland-Hodgman, card triangle as three half-planes); an output point carries which source it came from (atlas vertex inside the card: barycentric map; on card edge k: the parameter t along that edge's endpoints taken in canonical order (lower vertex index first), position and normal lerped between those endpoints; a card corner: the card vertex itself); fan-triangulate; uv = atlas uv + (i,j); normals renormalized. Skip zero-area polygons. Degenerate card triangles (UV area ~0, the alder's 2-triangle strips can be thin, not zero) are skipped with a count.
- [ ] **Step 4: Run.** Expected: 5 test cases PASS; `./build/tests/tests -tc="traceAtlas*"` still PASS.
- [ ] **Step 5: Commit** `Cook: clip the traced atlas onto leaf cards`.

### Task 3: Traced leaves in the tree cook

**Files:**
- Modify: `tools/cook/tree.cpp` (constants, `loadPrototype`, `simplify`, `cookTexturedMaterial`, `cookTree`), `tools/cook/tree.h` (doc comment of `cookTree`)
- Test: `tests/tree_cook_test.cpp`

**Interfaces:**
- Consumes: `traceAtlas`, `clipToCards`, `CardVertex` (Tasks 1-2).
- Produces: cooked leaf materials with `alphaMode` OPAQUE, double-sided, `diffuseTransmission` kept; stdout line per traced material `Traced leaf material <name>: atlas <A> triangles, <C> cards -> <T> triangles`.

- [ ] **Step 1: Tests.**
  - Modify `cookTree keeps a textured leaf material's UVs through simplification and pruning`: its texture alpha becomes 255 everywhere (so it is not traced and the test keeps covering UV-preserving pruning); its expectation `alphaMode == Mask` becomes `alphaMode == Opaque`.
  - New `cookTree traces alpha-tested cards into opaque leaves`: source = the test file's `tube()` + 50 single-quad cards (UVs 0..1 each) with a 64x64 texture whose alpha is a disk of radius 24 texels, `alphaMode MASK`, double-sided; budget large (no pruning). Expect: leaf material `alphaMode == Opaque`, `doubleSided`, `diffuseTransmission` present; `result.leafAreaBefore == Approx(50 * cardArea * pi*24^2/64^2).epsilon(0.03)`; leaf pieces (union-find on exact positions) == 50; every leaf vertex's UV inside [0,1].
  - New `cookTree leaves untextured and opaque-alpha leaves untraced`: the existing untextured test source cooks with the same triangle count as before this task (record it from the existing test's numbers: `result.sourceTriangles` unchanged).
- [ ] **Step 2: Run** `./build/tests/tests -tc="cookTree*"`. Expected: the new trace test FAILS (material still MASK).
- [ ] **Step 3: Implement.**
  - Constants: `kSimplifyError` -> `kWoodSimplifyError = 0.01f` and `kLeafSimplifyError = 0.002f`; `simplify` passes `p.leaf ? kLeafSimplifyError : kWoodSimplifyError`.
  - `bool tracedMaterial(const fastgltf::Material &, bool leaf, const basisu::image &color)`: leaf && textured && `color.has_alpha()` && `alphaMode != Opaque`: the predicate `cookTexturedMaterial` uses today for `masked`; both call sites use it.
  - `cookTree`: move the `sourceImage` decode lambda above prototype loading; for each traced material, build its `AtlasMesh` once (alpha from the decoded base colour, cutoff as today); `loadPrototype(asset, primitive, const AtlasMesh *atlas)` replaces the primitive's vertices/indices with `clipToCards` output before `splitLeafPieces` and prints the traced line.
  - `cookTexturedMaterial`: for a traced material, base colour cooked with cutoff -1 (no coverage mips), material left OPAQUE; the transmission average still uses the alpha as today; print `traced` instead of `alpha-tested`.
- [ ] **Step 4: Run** `./build/tests/tests -tc="cookTree*"` then the whole suite `./build/tests/tests`. Expected: all PASS.
- [ ] **Step 5: Commit** `Tree cook: trace alpha-tested leaf cards into opaque geometry, 2 mm leaf outlines`.

### Task 4: Crown coverage tool

**Files:**
- Create: `tools/cook/coverage.h`, `tools/cook/coverage.cpp`
- Modify: `tools/cook/CMakeLists.txt` (`coverage.cpp` into `meridian-cook-lib`, which then links `meridian` for `buildClusterLod`/`selectClustersBvh`), `tools/cook/cook.cpp` (`--coverage <tree.gltf>`)
- Test: `tests/leaf_trace_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Fraction of a w x h grid over [x0,x1]x[y0,y1] covered by the triangles
  // projected onto xy (orthographic), sampled at cell centres.
  double projectedCoverage(const std::vector<std::array<float, 3>> &positions,
                           const std::vector<uint32_t> &indices, float x0, float y0, float x1,
                           float y1, uint32_t resolution);
  struct CoverageSample { float distance; double coverage; double ratio; size_t triangles; };
  // Side view of a cooked tree along -z: per distance, the clusters the LOD cut
  // draws at 1 px (2560x1440, 70 degrees horizontal), projected at 1024^2 over
  // the tree's xy bounds; ratio = coverage / full-detail coverage.
  std::vector<CoverageSample> crownCoverage(const std::filesystem::path &gltf,
                                            const std::vector<float> &distances);
  ```
  CLI: `meridian-cook --coverage tree.gltf` prints a table for 10, 30, 60, 120 m and the voxel switch distance (finest voxel = tree height / 128 projecting to 2 px).

- [ ] **Step 1: Failing test** `projectedCoverage of half a square`: a triangle pair covering x in [0,0.5] of the unit square, resolution 256: `== Approx(0.5).epsilon(0.01)`; a triangle covering the lower-left half: `Approx(0.5).epsilon(0.02)`.
- [ ] **Step 2: Run** `./build/tests/tests -tc="projectedCoverage*"`. Expected: FAIL.
- [ ] **Step 3: Implement.** `crownCoverage` reads every primitive (fastgltf, as `loadPrototype` does) into a `MeshData`, `buildClusterLod` per primitive, camera at (centre.x, centre.y, max.z + d) on the tree's local axes, `selectClustersBvh(lod, camera, 1, projY, 0.1, 1440, 1)` with projY from the vertical fov of 70 degrees horizontal at 16:9; projects the selected clusters' triangles; full detail = the original indices.
- [ ] **Step 4: Run** the test, PASS; then `./build/tools/cook/meridian-cook --coverage assets/source/megascans/blackalder/SM_BlackAlder_Field_01/SM_BlackAlder_Field_01.gltf` (cards). Expected: a table; record it with the card baselines (`artifacts/baseline-cards.md`).
- [ ] **Step 5: Commit** `Cook: crown coverage across the cluster LOD cut (--coverage)`.

### Task 5: Re-cook, variant cut, gates, docs

**Files:**
- Modify: `docs/assets.md` ("Megascans Trees from Unreal": traced leaves, cooked variants and budgets), `docs/decisions.md` (the 2026-11-22 entry: implemented, measured, trace method deviation), `CLAUDE.md` (placement hash with `--trees`, any changed numbers), `docs/superpowers/specs/2026-11-21-materials-fidelity-design.md` (Status, D1/D2 implemented), `docs/backlog.md` (rows resolved or added)

- [ ] **Step 1: Pick variants.** Contact sheet of every Forest/Field/Sapling variant per species (one capture each, or the Unreal export's thumbnails if present); keep the most distinct (Forest 5, Field alder 3 / hornbeam 2 / beech 1, Sapling 3 per species). Record the list in assets.md.
- [ ] **Step 2: Re-cook** the kept variants into the worktree's real species directories (copy the main checkout's `.ktx2-cache` first): `meridian-cook --tree ... --triangles` 1200000 (Forest/Field; beech 1500000), 300000 (Sapling). Expected: `Traced leaf material` lines; record cooked triangles per variant and the total (<= 30M). If over: drop variants (one Forest per species at a time) before touching the error.
- [ ] **Step 3: Gates** (same commands and AA as Task 0, plus `--coverage` on the traced Field_01 per species): table cards vs traced in `artifacts/gates.md`. Pass: motion-clarity forest rows' jitter lower and error not higher; coverage ratio >= 0.95 at 30/60/120 m and the switch (or no worse than cards where cards are below); forest edge GPU ms not above cards; close-up captures (1 m and 5 m of one tree per species, e.g. walk-mode views near the forest edge) show outlines without background fringes; a Debug `--trees` capture with 0 validation errors, `placement check` and `stream check` 0 mismatches, far-field and far-shadow checks passing.
- [ ] **Step 4: If coverage fails:** stop and report (the spec's fallbacks are engine work: area-preserving pruning per DAG level, then an earlier voxel switch).
- [ ] **Step 5: Docs** as listed under Files, with the measured numbers; commit `Trees: traced opaque leaves, 30 cooked variants` and merge to main after the gates; then re-cook the same variants into the main checkout's `assets/source/megascans/` (the shared assets) and remove the dropped variants' cooked directories there.

## Resume notes (2026-11-22, paused)

**Done and committed** (tests pass, `./build/tests/tests`): Tasks 1-4 (atlas
trace, clip onto cards, traced leaves in the tree cook with 2 mm outlines,
`meridian-cook --coverage`), plus two additions approved mid-run:
- `3a3ecab` Cluster LOD keeps leaf area per DAG level (group-uniform scale,
  max 4x, added vertices in the cluster cache v4; opaque double-sided leaf
  meshes with transmission only). Fixed the coverage gate's first failure
  (alder Forest_01 0.93/0.85/0.72/0.62 -> 0.98/0.97/0.96/0.96 at
  30/60/120 m/switch).
- `714ba50` Tree cook coarsens leaf outlines 2 -> 5 mm before pruning
  (biggest trees pruned to 19% of leaves, survivors x2.3, before).

**Cooked** (worktree only, `assets/source/megascans/{blackalder,europeanbeech,
europeanhornbeam}` are real dirs with the traced cooks; everything else in
`assets/` is a per-file symlink farm of the main checkout's assets, so
cluster caches (v4) are written here, never into main's): 30 variants
(Task 5 list, `staging/cook-all.sh`), 28.7M triangles (cap 30M); beech
Field_01 4M, alder Field_02 2M budgets; every tree's survivors scaled
<= 1.22. Main's assets are untouched.

**Open problem (Task 5 gate 2):** alder and hornbeam pass coverage >= 0.94
everywhere; the beech does not: Field_01 0.86 at 120 m, 0.38 at its 169 m
switch; Forest_01 0.29 at its 345 m switch (its cards also drop: 0.75 at
250 m, 0.31 at 346 m). Spikes in `.superpowers/sdd/2026-11-22-leaf-geometry/
artifacts/`: `spike-cluster-perpiece.cpp` (per-piece area attribution; with
clamp 64 the beech passes, 0.93 / 1.01, but remnants land up to 21 m outside
the tree; capping moves at k x level error bounds that to ~k px but the beech
thins again at k <= 16; ~0.5 GB of added vertices over 30 trees),
`spike-cluster-error-raise.cpp` (area loss counted as LOD error, cumulative
vs full detail: no gain). Next to try: per-piece attribution + move cap
k ~ 8 for levels drawn before the switch only, or a finer prototype voxel
grid for tall trees (beech switches at 169-345 m because its voxels are
height/128); decide with the GPU gates.

**Not run yet:** Task 5 GPU gates on the traced trees (motion-clarity,
GPU ms, overview captures, Debug checks); docs (assets.md, decisions.md,
CLAUDE.md placement hash), merge, re-cook in main's assets. Card baselines
are in `artifacts/cards/` (forest edge DLSS GPU 25.79 ms, 52.0M triangles;
motion jitter still 1.24 / side5 7.88 / side30 9.92 / turn1 4.79). Ledger:
`.superpowers/sdd/2026-11-22-leaf-geometry/progress.md`.

**Lane notes from the Neon triage** (main,
`docs/superpowers/research/2026-11-22-neon-triage.md`): the leaf dead-zone
bug (mesh.frag 976-997, G13 #16), geometric specular AA (G18) and the
F0-is-right note (G15) are on this lane.
