# Terrain Material Implementation Plan

**Status:** done on main (c3302f5..c4aa310).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Texture the terrain with Poly Haven ground scans (hex-tiled, height-blended, triplanar rock), and add a motion-capture option that proves nothing smears in motion.

**Architecture:** A scripted-capture option moves the camera at a fixed velocity so motion blur can be measured. `meridian-cook --texture` cooks loose JPEGs to KTX2. The renderer gets one `TerrainMaterial` (three layers of four textures) stored inline in `SceneData`; `mesh.frag` samples it for materials flagged `terrain`, and falls back to the procedural colors when no terrain material is set.

**Tech Stack:** C++20, Vulkan 1.3, GLSL (bindless textures, buffer device address), Basis Universal, doctest, ImageMagick (`magick`) for image metrics.

**Spec:** `docs/superpowers/archive/specs/2026-11-19-terrain-material-design.md`

## Global Constraints

- Commits: one feature per commit, message explains what and why, **no AI attribution trailer of any kind**. Push to `main` after each commit.
- Style: 2-space indent, clang-format default-ish; comments only for a non-obvious why.
- Any struct in `gpu_types.h` marked "must match" changes together with its GLSL mirror; update the `static_assert` sizes.
- Every capture run must report `0 validation errors`.
- Keep `CLAUDE.md` and `docs/decisions.md` current in the same commit as the change they describe.
- GPU ms are comparable only at the same window size (Hyprland tiling varies it); note the size with each number.
- Two captures of one view never match exactly; compare mean difference (~0.02/255), not exact pixels.
- Mip LOD bias stays 0. Anisotropy stays 16x.
- Target: terrain material costs < 0.5 ms extra GPU time.

## Review Focus

1. Fresh clone, terrain textures not cooked: the game still runs (exit 0) with the procedural terrain and prints which file is missing. (Task 3, Step 4.)
2. Only some terrain files present: same fallback for the whole terrain, never a half-textured terrain or a bindless index pointing at nothing. (Task 3, Step 4.)
3. Mip seams: hex-cell borders and triplanar edges must not show lines at grazing angles; every sample uses `textureGrad` with derivatives of the unrotated UV. (Task 5, Step 3, close-up grazing view.)
4. Normal map orientation: bumps lit from the sun's side on all three layers, including rotated hex tiles and each triplanar axis. (Task 4, Step 4 and Task 5, Step 3, sunset view.)
5. `--camera-velocity` without `--capture`/`--frames`: rejected with an error, not silently ignored in an interactive run. (Task 1, Step 3.)

---

### Task 1: Motion capture option

**Files:**
- Modify: `game/src/main.cpp` (`Options`, `parseOptions`, main loop)
- Modify: `CLAUDE.md` (automated visual check section)

**Interfaces:**
- Produces: CLI `--camera-velocity X Y Z` (m/s, world space).

- [ ] **Step 1: Add the option.** `Options` gets `std::optional<meridian::Vec3> cameraVelocity`. In scripted runs each frame advances a fixed `1/60 s` (not wall time, so runs are deterministic): `camera.position += velocity / 60`. The camera starts at `--camera position - velocity * frames / 60`, so the captured (last) frame is exactly at the `--camera` position and compares directly with a still capture of the same arguments.
- [ ] **Step 2: Error text.** Extend the unknown-option message with `--camera-velocity x y z`.
- [ ] **Step 3: Reject interactive use.** If `cameraVelocity` is set and `frames == 0` after the capture default is applied, throw `std::runtime_error("--camera-velocity needs --capture or --frames")`. Verify: `./build/game/game --camera-velocity 1 0 0` exits non-zero with that message.
- [ ] **Step 4: Build and baseline.** Build, then on the current (procedural) terrain capture the sample-model view still and moving:
  ```bash
  V="--camera -1.6 299.2 700 -0.35 -0.12"
  ./build/game/game $V --capture still.png
  ./build/game/game $V --camera-velocity 0 0 -5 --capture moving.png
  ```
  Expected: both report `0 validation errors`. Measure sharpness (Laplacian standard deviation) of each:
  ```bash
  magick IMG -colorspace gray -morphology Convolve Laplacian:0 -format "%[fx:standard_deviation]\n" info:
  ```
  Record both numbers (baseline for Task 5). A moving value well below the still value means TAA blurs in motion today; report it, don't fix it here.
- [ ] **Step 5: Docs.** Add the option and the sharpness command to CLAUDE.md's automated visual check section, one line each.
- [ ] **Step 6: Commit** `game/src/main.cpp CLAUDE.md`: "Add --camera-velocity for scripted motion captures".

### Task 2: Cook loose textures; fetch and cook terrain layers

**Files:**
- Modify: `tools/cook/cook.cpp` (`main`, new `cookTexture`)
- Modify: `tools/fetch-sample-assets.sh`
- Modify: `CLAUDE.md` (Commands, assets gotcha)

**Interfaces:**
- Consumes: existing `decode(bytes)`, `encodeKtx2(image, Usage, jobPool)`, `readFile(path)` in cook.cpp.
- Produces: `meridian-cook --texture <in.jpg|png> <out.ktx2> <color|data|normal>`; cooked layers at `assets/terrain/<id>/{base_color,normal,arm,height}.ktx2` for `forest_ground_04`, `forest_leaves_04`, `rock_face_03`.

- [ ] **Step 1: `void cookTexture(const std::filesystem::path &input, const std::filesystem::path &output, Usage usage)`** in cook.cpp. `main` accepts both forms; usage message lists both.
- [ ] **Step 2: Fetch.** Texture list becomes `forest_ground_04 forest_leaves_04 rock_face_03 brown_mud_leaves_01 rocky_trail dirt_floor mossy_rock`; each also downloads `${texture}_disp_2k.jpg` (resolve the URL from `.Displacement["2k"].jpg.url`; it may live in a different directory from the diffuse map, so don't derive it).
- [ ] **Step 3: Cook in the script.** If `build/tools/cook/meridian-cook` exists, the script cooks every Poly Haven model (`assets/source/<id>/<id>_2k.gltf` -> `assets/<id>`) and the three terrain layers (diff -> `base_color.ktx2` color, `nor_gl` -> `normal.ktx2` normal, `arm` -> `arm.ktx2` data, `disp` -> `height.ktx2` data), skipping outputs that already exist. Otherwise it prints that the build is needed for cooking.
- [ ] **Step 4: Verify.** `./tools/fetch-sample-assets.sh` completes; `ls assets/terrain/*/` shows 4 `.ktx2` files for each of the 3 layers. Re-running does no downloads or cooks.
- [ ] **Step 5: Docs.** CLAUDE.md: add the `--texture` form under Commands; the assets gotcha now says the fetch script cooks everything once the build exists.
- [ ] **Step 6: Commit**: "Cook loose textures and fetch terrain layers with height maps".

### Task 3: Terrain material plumbing (no visual change)

**Files:**
- Modify: `engine/include/meridian/renderer.h`
- Modify: `engine/src/render/gpu_types.h`, `engine/shaders/common.glsl`
- Modify: `engine/src/render/renderer_impl.h`, `renderer_resources.cpp` or `renderer_frame.cpp` (wherever `SceneData` is filled)
- Modify: `engine/shaders/mesh.frag` (flag rename only)
- Modify: `game/src/main.cpp`

**Interfaces:**
- Produces (public, renderer.h):
  ```cpp
  struct TerrainLayer {
    TextureId baseColor, normal, arm, height;
    float tileSize = 2.0f; // meters per texture repeat
  };
  struct TerrainMaterial {
    TerrainLayer soil, needles, rock;
  };
  void Renderer::setTerrainMaterial(const TerrainMaterial &material);
  ```
- Produces (GPU, must match): `GpuTerrainLayer { uint baseColor, normal, arm, height; float tileSize; float pad[3]; }` (32 B); `GpuTerrain { GpuTerrainLayer layers[3]; uint enabled; uint pad[3]; }` (112 B), appended to `SceneData` after `atmosphere` as `terrain`; layer order soil, needles, rock. `SceneData` static_assert 528 -> 640.
- Renames: `MaterialData::proceduralTerrain` -> `terrain` (comment: "Shaded with the renderer's TerrainMaterial; procedural colors until one is set"), `kMaterialProceduralTerrain` -> `kMaterialTerrain` in both C++ and GLSL.

- [ ] **Step 1: Implement the struct, setter and renames.** `enabled = 0` until `setTerrainMaterial` is called.
- [ ] **Step 2: Game.** `main.cpp` loads the 12 files from `assets/terrain/<id>/` with `loadKtx2` (base color `ColorSpace::Srgb`, others `Linear`), uploads them and calls `setTerrainMaterial` with tile sizes from the textures' real-world size (Poly Haven `dimensions`, in mm, via the API; expect ~2-4 m). If any of the 12 files is missing, print `Missing <path>; terrain stays procedural (see tools/fetch-sample-assets.sh)` once and skip the call.
- [ ] **Step 3: Build, run tests** (`ctest --test-dir build`): pass.
- [ ] **Step 4: Fallback check.** Default-view capture with assets present, and again with `assets/terrain` renamed away. Expected: both `0 validation errors`, exit 0, the second prints the missing-file line, and both match the pre-change image (mean diff ~0.02/255, since the shader doesn't read the terrain data yet). Restore the directory.
- [ ] **Step 5: Reference sweep.** `grep -rn 'proceduralTerrain\|ProceduralTerrain\|procedural terrain' --exclude-dir=build --exclude-dir=assets .` Expected: only the GLSL fallback function `proceduralTerrain()` and accurate mentions remain; fix stale comments/docs (CLAUDE.md layout notes, decisions.md).
- [ ] **Step 6: Commit**: "Add TerrainMaterial: three texture layers for the terrain".

### Task 4: Layered terrain shading

**Files:**
- Modify: `engine/shaders/mesh.frag` (`proceduralTerrain` stays as fallback; new `terrainSurface`)
- Modify: `docs/decisions.md` (Materials and assets section)

**Interfaces:**
- Consumes: `pc.scene.terrain` (Task 3).

- [ ] **Step 1: `Surface terrainSurface(vec3 normal)`**, used when `kMaterialTerrain` is set and `scene.terrain.enabled != 0`.
  - Flats (soil, needles): world-XZ projection, `uv = vec2(x, -z) / tileSize` (with this sign, tangent +X, bitangent -Z, cross = +Y). Normal maps are OpenGL convention.
  - Rock: triplanar, weights from `pow(abs(normal), 4)` normalized; per-axis normals combined with the Whiteout blend (Ben Golus, "Normal Mapping for a Triplanar Shader").
  - Soil vs needles: low-frequency world-space value noise (period ~40 m), then height-sharpened: `w = clamp((heightA - heightB) * k + mask, 0, 1)` style (Unity/MicroSplat height blend); pick `k` so transitions are a few cm wide, not meters.
  - Flats vs rock: slope `smoothstep(0.75, 0.9, normal.y)` (current thresholds), height-sharpened the same way.
  - Snow: keep current procedural snow on top.
  - ARM: occlusion = r, roughness = g, metallic forced 0.
  - All samples through `textureGrad(textures[nonuniformEXT(i)], uv, dFdx(uv), dFdy(uv))`, derivatives taken once outside any branch; skip a layer's samples when its weight is 0.
- [ ] **Step 2: Build.** Shader compiles in all variants.
- [ ] **Step 3: Decision log.** Add a "Terrain material" entry to `docs/decisions.md`: layers, projection, height blending, Whiteout triplanar, textureGrad, mip bias 0 (with the Epic mip-bias source), sources from the spec.
- [ ] **Step 4: Captures.** Default view, sample-model view, sunset (`--sun 0.9 0.05 -0.4`, yaw 1.15). Expected: `0 validation errors`; textured ground, rock on slopes, no stretching on cliffs, bumps lit from the sun side. Tiling may still show at distance (fixed in Task 5).
- [ ] **Step 5: Commit**: "Shade the terrain with scanned ground and rock layers".

### Task 5: Hex-tiling and final verification

**Files:**
- Modify: `engine/shaders/mesh.frag`
- Modify: `docs/decisions.md`, `CLAUDE.md` (remove "terrain is procedural"-style claims if any; update Known gaps if the motion baseline found TAA blur)

**Interfaces:**
- Consumes: `terrainSurface` (Task 4).

- [ ] **Step 1: Hex-tile the flat layers.** Port Mikkelsen's hex-tiling (JCGT 2022, reference code github.com/mmikk/hextile-demo, `hextiling.h`): three samples per map on a hex grid with per-tile random rotation and offset; luminance-weighted blend with the paper's contrast falloff (defaults: rotation strength 1, fall-off contrast 0.6, exponent 7); normals rotated with their tile; gradients rotated with the UV. Height samples follow the same weights so height blending still works. Rock stays plain triplanar.
- [ ] **Step 2: Build.**
- [ ] **Step 3: Captures.** Same three views plus a high, wide view (`--camera 0 450 900 0 -0.5`) for repetition, and a grazing close-up (`--camera -1.6 298.6 700 -0.35 -0.05`) for mip seams. Expected: `0 validation errors`; no visible grid at distance; no hex-cell lines; detail contrast similar to Task 4 (not washed out).
- [ ] **Step 4: Motion check.** Repeat Task 1 Step 4 (same view, still and `--camera-velocity 0 0 -5`). Expected: moving/still sharpness ratio no worse than the Task 1 baseline ratio.
- [ ] **Step 5: GPU cost.** Note GPU ms (with window size) from the default-view capture against the Task 3 capture; expected < 0.5 ms more at a comparable size.
- [ ] **Step 6: Docs.** Hex-tiling entry in `docs/decisions.md` (Mikkelsen source, parameters). If the Task 1 baseline showed TAA blur in motion, add it to CLAUDE.md Known gaps.
- [ ] **Step 7: Commit**: "Hex-tile terrain layers to remove visible repetition".
