# Asset import: Megascans trees and the Fab cliff

Only needed when importing or re-cooking these assets. The everyday
fetch and cook commands are in CLAUDE.md.

```bash
./tools/fetch-sample-assets.sh ~/Downloads       # + imports the Megascans Megaplants tree_*_usd.zip and the Fab sandstone cliff (*_ue_high.zip) found there
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
