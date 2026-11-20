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
  hand, a login is needed): `uncut_grass_oilpt20` (2 m), `wild_grass_pjwgw0`
  (1 m), `forest_floor_xdfddec` (2 m), `soil_mud_olsetpp0` (0.88 m),
  `gravel_on_soil_rmsigwp0` (0.39 m), `rock_cliff_tg1mcgbl` (2 m); sizes are
  each scan's real-world size from its JSON. The zip's `Textures/` go to
  `_extract/<id>_4k_ue_high/`; B, N, ORM and H are downscaled from 4K to 2K
  in `_terrain/<id>/` and cooked to `assets/terrain/<id>/{base_color,
  normal,arm,height}.ktx2`. ORM packs AO, roughness, metal like Poly
  Haven's ARM. The normal maps are OpenGL (green up): correlated against
  the gradient of the H map, R with -dh/dx and G with +dh/dy (rows down),
  +0.6 to +0.94 for all six, so the cook leaves them as they are.
