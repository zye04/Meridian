# Backlit canopy: direct sun on leaves (short design)

Status: approved 2026-11-23 (user: "go with your recommendations").

## Why

The reference path tracer puts the backlit forest edge (`600 1205.6 -400
-1.55 0.02 --sun -0.9 0.4 0.15`) at 1.4-2.5x the raster on the sunward side.
An ablation (worktree `canopy-backlit`, `.superpowers/spike-backlit/`) split it:

- Ambient is right: the reference's indirect light (full minus one-bounce)
  matches the raster's ambient within ~15% per cell. The error is direct sun,
  90-100% of it in 3-9% of the pixels (bright backlit leaves).
- Normal mismatch (top rows): where the shading normal and the geometric
  normal disagree about the sun's side, the reference evaluates the front
  lobe for light arriving behind the face (a leak) and the raster gives no
  sun at all. Gating the reference like the raster: cells 1,0 / 1,1 from
  1.90 / 2.41 to 1.17 / 1.57.
- Plant base occlusion on crowns (lower rows): `plantOcclusion`, meant for
  tufts (mesh.mesh says so), applies to every leaf material, crowns included:
  ambient down to 0.25x and, through `microOcclusion`, a microShadow on the
  low sun. With micro and contact shadows off, 80-100% of the lower rows'
  gap closes.
- Not the cause: sun cascades (`--shadows rt` is darker), voxel trees, LOD.

## Approaches

- A1 (chosen): the light's side comes from the geometric normal; the cosine
  from the shading normal, as an absolute value for the two Lambert lobes of
  a thin (diffuse-transmission) surface; GGX only when both normals face the
  sun. Same rule in mesh.frag and reference.comp (pbrt's rule: reflection
  vs transmission by the geometric normal, BSDF in the shading frame).
- A2: gate only the reference to the raster's rule. Cheapest, but the raster
  keeps unlit leaves where normals disagree.
- A3: light leaves by the geometric normal only. Loses the shading normals'
  variation.
- B1 (chosen): no tuft base darkening on crowns (leaf meshes of half height
  >= `kCrownMinHalfHeight`, the crown visibility's own test): mesh.mesh
  sends height 1 for them.
- B2: also take the plant occlusion out of the sun's micro shadow for every
  plant. Not needed for this view; tufts keep their look.

## Design

- mesh.frag, leaf materials only (non-leaves unchanged):
  - geometric normal faces the sun, shading normal does not: front diffuse
    with |N.L| (no specular);
  - geometric normal faces away: transmission with |N.L| (today it needs
    N.L < 0 too, so mismatched pixels got nothing).
- reference.comp: front lobe only when N.L > 0 and Ng.L > 0 (all surfaces);
  thin surfaces: diffuse with |N.L| when Ng.L > 0 >= N.L, transmission with
  |N.L| when Ng.L < 0.
- mesh.mesh: `outHeight = 1` for crown leaf meshes.
- Contact shadows on leaves: re-measured after A1 + B1, decided then.

## Check

New references (front, backlit; 1024 spp) since the reference changes;
`tools/reference-compare.py` per cell before/after, edge front and backlit.
Success: backlit sunward cells within ~1.3x, front not worse than today's
mean |ln ratio| by more than noise.
