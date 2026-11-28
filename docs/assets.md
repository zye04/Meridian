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
- Textured trees: the USD has no textures, so they come from Unreal
  ("Textured Megaplants trees from Unreal" below). The cook keeps UVs and maps for any material with a base
  colour texture; untextured ones get the tint palette and Bark012.

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

## Megaplants textured from Megascans scans (no Unreal)

```bash
python3 -I tools/usd2gltf/texture_tree.py _usd/<tree>.gltf _textured/<tree>_tex.gltf \
  --leaf-atlas _extract/<atlas>_4k --bark _extract/<bark>_4k_ue_high/Textures [--atlas-leaves 0,1]
./build/tools/cook/meridian-cook --tree _textured/<tree>_tex.gltf <tree>_tex --triangles 4000000
```

(paths under `assets/source/megascans/`). The fetch script does this for
the beech when `unidentified_tree_leaves_pecjv2_4k.zip` and
`beech_bark_thspcaes_4k_ue_high.zip` are in the downloads directory; the
game loads `<tree>_tex` instead of `<tree>` whenever that directory
exists. Each leaf piece is fitted onto one leaf of the atlas (petiole
down, `--atlas-leaves` picks atlas leaves, 0 = largest), UVs welded per
position; the wood gets the bark's B/N/ORM. Atlases on hand (Megascans
`*_4k.zip`): `unidentified_tree_leaves_pecjv2` (ovate, beech/hornbeam),
`assorted_leaves_ojioj2` (0, 1: palmate maple), `oak_leaves_qgkpd2`
(small serrated), `oak_leaves_rgijqka2` (lobed oak sprigs, drying),
`oak_leaves_sedlecvh2` (spotted, dead), `assorted_leaves_ojiok2`; barks
`beech_bark_thspcaes` (olive-grey), `wmohcgfn` (smooth, light),
`vmhkcdsg` (fissured). Maple and oak are not textured: see decisions.md
"Textured beech from Megascans scans".

## Textured Megaplants trees from Unreal

The Megaplants' textures (leaf atlases with opacity, normal, roughness,
translucency, bark) exist only in their Unreal assets, which are presets
of the Procedural Vegetation Editor (PVE, Experimental). One-time setup
in Unreal Engine 5.7+ (Linux editor):

1. Create an empty project. Edit > Plugins: enable "Procedural Vegetation
   Editor" (restart). Project Settings: enable "Nanite Foliage
   (Experimental)" (restart).
2. On fab.com, add the Megaplants (beech first) to your Library; in the
   editor, open the Fab window and add them to the project.
3. Open the beech preset in the PVE and generate/bake a variant (static or
   skeletal output). Optionally drag one tree into the open level: if the
   tree is an assembly of placed branch meshes, the script reads their
   placements from the level.
4. Diagnostics first (writes only `inventory.json` and the log), in the
   editor's Output Log, Python mode:
   `py /home/zye/Projects/dev/meridian/tools/unreal/export_trees.py --folder /Game/<pack folder> --species beech --inventory-only`
5. Then the export, the same command without `--inventory-only`:
   `~/Downloads/unreal-export/beech/` gets `<asset>.gltf` (+ .bin, PNG
   textures), `textures/*.png` (every texture the materials reference),
   `manifest.json` (meshes, triangle counts, material slots, textures by
   role: base colour, normal, ORM/roughness, translucency, opacity),
   `placements.json` (only for placed instanced meshes) and
   `export_log.txt`. Headless:
   `UnrealEditor <project>.uproject -run=pythonscript -script="<path>/export_trees.py --folder /Game/<pack folder> --species beech"`.

For the Megascans Trees packs (static meshes, card leaves) the script has
run end to end; see "Megascans Trees from Unreal" below. Wind (Dynamic
Wind: skeletal bones and weights) is listed in the inventory, not
exported.

Cook (the glTF is meters, Y up; one mesh or an unflattened assembly):

```bash
./build/tools/cook/meridian-cook --tree ~/Downloads/unreal-export/beech/<asset>.gltf assets/source/megascans/<name> --triangles 4000000
```

`--bark-texture` is only needed when the wood material has no texture of
its own. Then view it with `--trees` (and `--scatter N`, after adding the
name to the Megaplants list in `scatterModels`, main.cpp), or, without
touching the game, through a copy of `assets/` in which
`source/megascans/<an existing tree>` links to the new cook (the game
reads the assets directory at compile time: `MERIDIAN_ASSET_DIR`).

If glTF export fails, `usd2gltf.py` also reads a USD export with
textures (UsdPreviewSurface inputs connected to UsdUVTexture files); FBX
would need a new dependency (assimp via FetchContent), not added.

## Megascans Trees from Unreal (Black Alder, European Beech, European Hornbeam)

The Megascans Trees packs on Fab (Black Alder, European Beech, Hornbeam,
...) are Unreal-only: Nanite static meshes with card leaves on an 8K
atlas, a tiling bark, a unique trunk-base texture and "decoration" cards
(lichen, knots), all on `MA_Foliage` (`/Game/MSPresets`). Per pack:
`Geometry/SimpleWind/SM_<Pack>_{Field,Forest,Sapling,Seedling}_NN` (use
these; `PivotPainter/*_PP` are the same meshes with wind pivots),
`Materials/SimpleWind/MI_*`, `Textures/{TwoSided/Summer,Tileable,Trunk/Trunk_NN,Decoration}`.

1. In the editor, add the pack from Fab to the project (it lands in
   `/Game/<Pack>`; `ls "~/Documents/Unreal Projects/<project>/Content"`).
   Nothing in the project is modified or saved by the export, and it runs
   headless next to an open editor (~1 min startup, ~10 s per 8K texture):

   ```bash
   ~/Apps/UnrealEngine/Engine/Binaries/Linux/UnrealEditor-Cmd "<project>.uproject" -run=pythonscript \
     -script="$PWD/tools/unreal/export_trees.py --folder /Game/<Pack>/Geometry/SimpleWind --species <species>" \
     -unattended -nosplash -nop4 -RenderOffscreen
   ```

   `<species>` is the lower-case pack name (`blackalder`,
   `europeanbeech`, `europeanhornbeam`): the game looks for
   `assets/source/megascans/<species>/SM_<Pack>_<Kind>_NN/`. Output in
   `~/Downloads/unreal-export/<species>/`: one glTF per mesh (geometry,
   meters, Y up; material baking does nothing headless, so it is off),
   `textures/*.png` (the Summer set of the pack's own textures: the PNG
   exporter crashes the editor on engine float textures such as
   PivotPainter's, so `/Engine` textures, Winter parameters and the
   Impostor slot are skipped), `manifest.json` (slots per mesh with their
   textures), `inventory.json` (material parameters, texture settings
   including Unreal's colour adjustments).
2. Cook-ready glTFs, then the tree cook (texture encodings are cached in
   `assets/source/megascans/<species>/.ktx2-cache`, so a pack's variants
   share them; the first tree takes ~90 s, the rest ~20 s):

   ```bash
   python3 -I tools/unreal/cook_ready.py ~/Downloads/unreal-export/<species> assets/source/megascans/_unreal/<species> [SM_... ...]
   ./build/tools/cook/meridian-cook --tree assets/source/megascans/_unreal/<species>/SM_<Pack>_Forest_01.gltf \
     assets/source/megascans/<species>/SM_<Pack>_Forest_01 --triangles 1000000
   ```

   (one cook per kept variant; seedlings are skipped). The cook traces
   the leaf cards' alpha into opaque leaf geometry (`Traced leaf
   material` lines; decisions.md "Tree leaves as opaque geometry"); the
   decoration cards transmit nothing and stay alpha-tested cards (glTF
   MASK at the source cutoff; decisions.md "Trace only cards that
   transmit"). Kept variants and budgets (`--triangles`), 30 trees, 28.7M
   triangles: Forest 1.2M (beech 1.5M), Field 1.2M (beech Field_01 4M,
   alder Field_02 2M), Sapling 0.3M; alder Forest 01 03 04 06 08, Field
   01 02 04, Sapling 02 05 06; beech Forest 01 03 05 06 08, Field 01,
   Sapling 01 02 04; hornbeam Forest 01 02 05 08 10, Field 01 03, Sapling
   01 03 05. `meridian-cook --coverage tree.gltf` prints its crown coverage
   across the cluster LOD cut (>= 0.95 up to the voxel switch).
3. `--trees` picks every cooked variant up (main.cpp `addUnrealTrees`,
   weights in decisions.md); a new species needs a line there. Hornbeam:
   `--folder /Game/EuropeanHornbeam/Geometry/SimpleWind --species
   europeanhornbeam`.
4. Leaf palette (optional, beside the originals, which stay untouched):
   `tools/unreal/cook_palette.sh tools/unreal/leaf-palette.json` retints
   each species' leaf atlas toward the palette's OKLCh target
   (`retint_leaves.py`: the atlas's mean moves, its variation stays; the
   leaf transmission colour is the base colour times a factor, so it
   follows) and cooks every variant present in `<species>/` into
   `<species>-<palette name>/` with the same budgets, sharing
   `.ktx2-cache`. The game loads them with `--tree-palette <name>`. A
   palette with `picks` retints only those variants and links the rest
   to `<species>-<others>/` (cook that palette first):
   `leaf-palette-light1.json` after `leaf-palette.json` (chlorophyll1).
   The game loads light1 by default: a fresh checkout gets it from the
   Unreal exports (`_unreal/<species>`, steps 1-3) and then
   `tools/unreal/cook_palette.sh tools/unreal/leaf-palette.json` followed
   by `tools/unreal/cook_palette.sh tools/unreal/leaf-palette-light1.json`
   (CPU, minutes per variant); `tools/fetch-sample-assets.sh` does not cook
   palettes. Without them the game prints one `Tree palette light1: ...
   not cooked` warning and loads the original cooks (the old, yellower
   leaf colours); `--tree-palette none` does so silently.

What the export holds (verified on Black Alder and European Beech):

- **glTF materials are the mesh's slots in order, without the Impostor
  slot** (it only exists on the impostor LOD). Checked by UV ranges and
  heights: tiling UVs (v from -5 to 3) on the Tileable bark, 0..1 on the
  trunk base (0-1.5 m) and decoration, an atlas sub-range on TwoSided.
- **Mask** (linear, `TC_MASKS`): R opacity (leaf and decoration cards; the
  bark's R is not used), G roughness (leaves ~0.61), B translucency
  (leaves ~0.42, twigs ~0), A unused. The Albedo's alpha is solid.
  `cook_ready.py` writes the base colour with R as alpha, a
  metallicRoughness map with G as roughness and metallic 0, and
  `diffuseTransmissionFactor` = 1.1 on leaves (mature beech leaves'
  transmittance over reflectance, decisions.md #32; the Mask's B is an
  artist weight and not used), decoration 0. That source factor means "the back transmits t times the
  base colour", not KHR's share: `meridian-cook --tree` translates it to
  KHR (`khrLeafTransmission`, tree.h: base colour scaled by 1 + t, reused
  as the transmission colour texture, factor t / (1 + t)).
- **Normal maps are already OpenGL (green up)**, as glTF wants, although
  Unreal itself is DirectX: measured by the curl of the map
  (corr(dNx/drow, dNy/dcol) = -0.16 to -0.30, the sign of the known-GL
  Bark012 map's -0.37; a DirectX map gives a positive value). Flipping
  green made bark look melted and lit from below.
- **Albedo adjustments**: Unreal applies a texture's `adjust_*` settings
  at build time and the PNG exporter writes the unadjusted source; the
  beech's leaf atlas has brightness 0.9 and hue 355, which
  `cook_ready.py` applies (in linear HSV, as Unreal does). The alder's
  are identity.
- `COLOR_0` is wind and blend data (leaves: R phase, G, A per-leaf; trunk
  base: A drives `MA_Foliage`'s dithered trunk-to-bark blend), not AO.
  `cook_ready.py` writes the Trunk slot MASK (cutoff 0.5), and the tree
  cook keeps COLOR_0 alpha only on a masked single-sided material whose
  base colour has no alpha (`isFadeMaterial`, tree.cpp): the trunk base
  is cut where base alpha x vertex alpha < 0.5 (a hard edge, no dither).
  Leaves, decoration and the tiling bark drop it (the alder Tileable
  carries alpha < 1 on 4-7% of its vertices: blend data, not a fade).
  The plant cook drops a Megascans plant's COLOR_0 too (wind data).
  `TEXCOORD_1` is constant per
  primitive (dropped too).
- MI parameters (`inventory.json`) not reproduced: Albedo Tint with
  "Tint BaseColor" (identity with "Split Albedo Controls", whose leaf and
  branch tints are white), "LightInfluence BaseColor Strength" 3,
  specular 0.2-0.3 (Unreal F0 0.016-0.024 against our fixed 0.04),
  seasons and per-instance colour variation, wind. The master's graph was
  not readable from Python (`get_material_property_input_node` returns
  nothing for its attributes).

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
  double-sided, and `KHR_materials_diffuse_transmission` from the mean
  translucency t over the opaque texels (0.05 to 0.43 per plant, in the cook
  log): as for the trees, base colour x (1 + t), also the transmission
  colour texture, factor t / (1 + t). The translucency map itself is
  averaged (the engine has no transmission factor texture).
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

