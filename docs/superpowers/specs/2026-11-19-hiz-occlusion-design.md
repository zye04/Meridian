# Two-phase Hi-Z occlusion culling — design

## Intent

GPU-driven phase 2: skip drawing instances hidden behind others (behind a
hill, inside a building). It's the biggest culling win in dense open
worlds and the base of phase 3 (cluster culling uses the same pyramid).

## Goals

- Instances fully hidden by depth already drawn this frame are not drawn.
- Never cull something visible: the image with occlusion culling on matches
  the image with it off (mean difference within capture noise).
- No popping when objects come into view (disocclusion) or when the camera
  moves.
- A test scene dense enough to measure the win, on the real ground.

## Non-goals

- Meshlet / cluster culling (phase 3), LOD.
- Occlusion culling for blended instances (few, CPU-sorted) or for shadows
  (RT shadows go through the TLAS, which keeps every instance).
- Fixing TAA motion smear (user deferred it).

## Approaches

1. **Two-phase (chosen; Niagara, Nanite).** Early pass draws instances
   visible last frame (frustum test only), builds a depth pyramid from that
   depth, late pass tests every instance against it, draws the newly
   visible ones and updates per-instance visibility. Conservative: no
   visible instance is skipped, so nothing pops.
2. **Single pass against last frame's pyramid (reprojected).** Cheaper (no
   second pass), but newly disoccluded objects are missing for a frame:
   popping in motion, against the project's priorities.
3. **Depth prepass + occlusion queries.** CPU readback latency or
   conditional rendering per object; doesn't fit the GPU-driven pipeline.

## Design

- **Depth pyramid:** R32F image sized to the screen rounded *down* to a
  power of two (so each texel covers whole pixel blocks), full mip chain.
  Built in compute from the stored depth image, one dispatch per mip,
  sampling with a `VK_SAMPLER_REDUCTION_MODE_MIN` sampler (reverse-Z:
  farthest = min). Recreated with the swapchain.
- **Test:** project each instance's bounding sphere to a screen rectangle
  (Mara & McGuire 2013, as in zeux's approximate projected bounds), pick
  the mip where the rectangle spans at most one texel, sample the min
  sampler over it, and compare with the sphere's nearest depth
  (`znear / (viewDepth - radius)` for the infinite reverse-Z projection).
  Spheres crossing the near plane are visible.
- **Visibility:** one uint per instance, persistent. Early cull draws
  `visible && inFrustum`; late cull tests `inFrustum && notOccluded`, draws
  those not drawn early, and writes the new visibility for every instance.
  New instances start invisible (late pass catches them).
- **Frame:** cull early -> scene pass 1 (clear; sky + early draws; depth
  stored) -> pyramid build -> cull late -> scene pass 2 (load; late draws +
  blended) -> TAA... Draw lists are per pass (two sets of bucket counts).
- **Scene data:** the view matrix and projection terms the test needs are
  added to `SceneData` (must-match with GLSL).
- **Controls and stats:** `Renderer::setOcclusionCulling(bool)` (game flag
  `--no-occlusion`, for A/B) and `Renderer::drawnInstanceCount()` (GPU
  counter, read back a frame later), printed by capture runs.
- **Test scene:** `--scatter N` places N random Poly Haven models
  (rocks, stumps, trunk, branches, roots, fern, nettle, shrub, dandelion,
  saplings) on the ground with random yaw and scale, within a fixed
  radius of the start. Ground height comes from a new
  `terrainHeight(settings, x, z)` matching the mesh. The default start and
  the sample models move to eye height on the ground.

## Verification

- Unit test: `terrainHeight` matches mesh vertex heights.
- A/B captures with `--scatter`: occlusion on vs off at eye height and in
  a view behind a hill; images match within noise; drawn-instance count
  drops; GPU ms (median of 5, same size) not worse in an open view and
  better in an occluded one.
- Motion capture (`--camera-velocity`) on vs off: no popping (images
  match within noise).
- 0 validation errors.

## Left to the builder

Pyramid build workgroup layout, scatter radius and scale ranges, the
exact counter readback mechanism.

## Sources

- Kapoulkine, niagara (two-phase culling, depth pyramid):
  https://github.com/zeux/niagara
- Kapoulkine, Approximate projected bounds:
  https://zeux.io/2023/01/12/approximate-projected-bounds/
- Mara & McGuire, 2D Polyhedral Bounds of a Clipped, Perspective-Projected
  3D Sphere, JCGT 2013
- vkguide compute culling (min-reduction sampler):
  https://vkguide.dev/docs/gpudriven/compute_culling/
