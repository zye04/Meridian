# Asset import: Megascans trees, ground surfaces and the Fab cliff

Only needed when importing or re-cooking these assets. The everyday
fetch and cook commands are in CLAUDE.md.

```bash
./tools/fetch-sample-assets.sh ~/Downloads       # + imports the Megascans Megaplants tree_*_usd.zip, the ground surfaces (*_4k_ue_high.zip) and the Fab sandstone cliff (*_ue_high.zip) found there
tools/usd2gltf/setup.sh && tools/usd2gltf/.venv/bin/python -I tools/usd2gltf/usd2gltf.py in.usd out.gltf   # USD tree -> unflattened glTF
./build/tools/cook/meridian-cook --tree in.gltf outDir --triangles N [--bark-texture dir]   # flatten to <= N tris
```

- Megaplants trees: `assets/source/megascans/_extract/<zip>/` (untrusted
  archive contents, one fresh directory each), `_usd/<tree>.gltf`
  (usd2gltf output), `<tree>/<tree>.gltf` (cooked, 4M triangles, saplings
  1M, Bark012 bark).
- Their USD has no normals on most meshes (the cook computes them),
  leftHanded winding (usd2gltf flips it), and leaf materials named
  `TwoSided_Material` (usd2gltf marks them double-sided; the cook treats
  double-sided materials as leaves).
- The Fab sandstone cliff lands in
  `massive_sandstone_cliff_vmmdcj2/vmmdcj2_tier_1.gltf` there (10.6 m
  tall, origin at its base).
- Ground surfaces (Megascans on Fab, Fab Standard License; downloaded by
  hand, a login is needed), by what each one shows (contact sheet in the
  G1 lane's worktree, `.superpowers/ground-assets/contact-sheet.png`),
  with each scan's real-world size from its JSON:
  - grass: `uncut_grass_oilpt20` (lush long grass, 2 m),
    `wild_grass_pjwgw0` (grass with clover, 1 m), `lawn_grass_sbykrmp0`
    (dry mown lawn, colour-matched, 0.41 m); clover: `clover_vlzlbjon`
    (1.5 m)
  - forest floor: `forest_floor_sfjmafua` (moss, short grass, litter),
    `forest_floor_xdfddec` (red-brown soil, leaves, twigs); path:
    `forest_path_ugsnfawlw` (wood chips, bark); all 2 m
  - soil: `soil_dirt_pjusf0` (dry tan dirt), `soil_dirt_peioh0` (dark
    soil, pale pebbles), both 1 m; mud: `soil_mud_olsetpp0` (0.88 m)
  - gravel: `gravel_on_soil_rmsigwp0` (fine grey gravel, channels);
    scree: `gravel_on_soil_rmrk3sp0` (angular stones); both 0.39 m
  - rock: `rock_cliff_tg1mcgbl` (2 m), `layered_rock_cliff_thficeas`
    (2 m), `layered_rock_vl0fdhdo` (3 m, no height map: luminance)
  - left out: `wild_grass_sfknaeoa` (very dark, blotches that repeat),
    `rough_sandstone_rock_xi4uccxjw` (orange, off-palette, 2x1 m)

  The zip's `Textures/` go to `_extract/<id>_4k_ue_high/`; B and N stay
  4K, ORM and H are downscaled to 2K in `_terrain/<id>/`, all four are
  cooked to `assets/terrain/<id>/{base_color,normal,arm,height}.ktx2`, and
  `albedo.txt` gets the base color's mean linear RGB. ORM packs AO,
  roughness, metal like Poly Haven's ARM. The normal maps are OpenGL
  (green up): correlated against the gradient of the H map, R with -dh/dx
  and G with +dh/dy (rows down), +0.6 to +0.95 for all, so the cook leaves
  them as they are.
