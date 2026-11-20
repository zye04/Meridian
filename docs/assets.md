# Asset import: Megascans trees, plants, ground surfaces and the Fab cliff

Only needed when importing or re-cooking these assets. The everyday
fetch and cook commands are in CLAUDE.md.

```bash
./tools/fetch-sample-assets.sh ~/Downloads       # + imports the Megascans Megaplants tree_*_usd.zip, the ground surfaces (*_4k_ue_high.zip) the Fab sandstone cliff and the grass and flower plants (*_ue_high.zip) and flower atlases (*_2k.zip) found there
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
  - rock: `rock_cliff_tg1mcgbl` (2 m), `layered_rock_vl0fdhdo` (3 m, no
    height map: luminance)
  - left out: `wild_grass_sfknaeoa` (very dark, blotches that repeat),
    `rough_sandstone_rock_xi4uccxjw` (orange, off-palette, 2x1 m),
    `layered_rock_cliff_thficeas` (its evenly spaced strata repeat as a
    brick grid on cliffs seen from 100 m)

  The zip's `Textures/` go to `_extract/<id>_4k_ue_high/`; B and N stay
  4K, ORM and H are downscaled to 2K in `_terrain/<id>/`, all four are
  cooked to `assets/terrain/<id>/{base_color,normal,arm,height}.ktx2`, and
  `albedo.txt` gets the base color's mean linear RGB. ORM packs AO,
  roughness, metal like Poly Haven's ARM. The normal maps are OpenGL
  (green up): correlated against the gradient of the H map, R with -dh/dx
  and G with +dh/dy (rows down), +0.6 to +0.95 for all, so the cook leaves
  them as they are.

## Grass and flower plants

Megascans 3D plants (Fab Standard License, downloaded by hand), tier 1
`*_ue_high.zip`: glTF in centimeters, Z up, with `Textures/T_<id>_4K_*`.
All are atlas-textured geometry cards (blades, fronds, leaves, petals
laid out on one 4K texture), not scans of a whole plant. The cook
(`meridian-cook <gltf> assets/<id>`, driven by the fetch script) keeps
LOD 0 of every variant, side by side along x with 0.1 m gaps (the
engine's cluster LOD replaces the packaged LODs, whose last one is a
camera-facing billboard quad), and writes `assets/<id>/<tier_1>.gltf`
plus `billboard/{base_color,normal_translucency}.ktx2` (the impostor
atlas, all variants stacked, for the far LOD; N-T packs the normal in RG
and the translucency in B, cooked as data).

- Textures: `B-O` is colour with opacity in alpha (the fern's zip has
  none: its opacity is ORT.R), `N` the normal map, `ORT` packs opacity (R),
  roughness (G) and translucency (B). Not AO. The nonUE glTF binds ORT as
  metallicRoughness (so translucency would read as metal); the UE glTF
  (the fern has only that one) also binds it as occlusion (opacity as AO).
  The cook rebuilds them: base color alpha = opacity, ORT's R and B set to
  255 and 0 (roughness in G, no metal), occlusion unbound, alpha MASK 0.5,
  double-sided, and `KHR_materials_diffuse_transmission` factor = the mean
  translucency over the opaque texels (0.05 to 0.43 per plant, in the cook
  log). Translucency has no texture of its own, as with the leaves.
- Normals are OpenGL (green up): over the billboards' opaque texels the
  mean G is 0.5 to 0.9 (side views, leaves face up), the same pipeline as
  the ground surfaces. The cook leaves them as they are.
- Alpha-test coverage is preserved down the mip chain (each mip's mask
  rescaled so the share of texels above 0.5 matches mip 0), for the
  plants' base color alpha and the atlases' `opacity`; masked plants don't
  thin out with distance. Billboard textures are not referenced by the
  glTF.
- The meshes carry no normals (the loader doesn't compute them: they
  render black), so the cook adds smooth area-weighted ones. One material
  each.
- What each is (the atlas is the mesh's texture), with
  LOD 0 triangles per variant (A, B, ...):
  - `grass_clumps_qmbr2`: thin green and dry brown blades, 3 variants,
    1.2k to 8.8k; `rough_meadow_grass_skfjj`: long blades with panicle
    seedheads (rough bluegrass), 5 variants, 2.4k to 6.4k;
    `wild_grass_sjliz`: fine long blades, 5 variants, 0.5k to 1.5k;
    `wild_grass_vlkhcbxia`: wide fresh and dry blades, loose tufts and a
    small violet flower, 8 variants, 1.4k to 12k
  - `common_fern_rhdso`: fern fronds (green and brown), 3 variants, 0.75k
    to 1.1k, about 1 m across
  - `clovers_rbdjz`: three-leaf clover leaves, green and yellowing, ground
    hugging, 3 variants, 0.6k to 0.9k
  - `field_poppy_vmcobd0ja`: tall poppy stems with red, pink and orange
    flowers, leaves and seed pods, 8 variants, 1.3k to 13.5k;
    `goldenrods_xg1lbfxia`: leaves and yellow flower spikes, 8 variants,
    1.9k to 9.7k except A, B, D (330k, 207k, 126k: modelled flowers, use
    sparingly); `yellow_flower_xchjef3ia`: yellow composite flowers
    (hawkbit-like) over rosette leaves and lady's-mantle-like sprays, 8
    variants, 3.3k to 39k
  - `meadow_buttercup_sf2occci2`, `assorted_flowers_pepju2`: 2K texture
    atlases of flowers without meshes (`base_color`, `opacity`,
    `translucency`, `roughness`, `normal` .ktx2 in `assets/<id>/`), card
    material for later; no other use yet.

