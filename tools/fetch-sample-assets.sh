#!/usr/bin/env bash
# Downloads test assets into assets/ (not tracked by git).
# From Khronos glTF-Sample-Assets:
# - FlightHelmet (CC0-1.0), KTX2 (Basis Universal) variant.
# - AlphaBlendModeTest (CC-BY-4.0, Microsoft), PNG/JPEG source, to cook with
#   meridian-cook. Tests opaque, masked and blended materials.
# From Poly Haven (CC0), 2K, resolved through its public API:
# - Forest rocks, litter and small plants as glTF, to cook with meridian-cook.
# - One film-weight tree (island_tree_01, 1.6M triangles) for virtualized
#   geometry.
# From swisstopo swissALTI3D (open government data, free use with the
# attribution "© swisstopo"): the 2 m elevation tiles of the Napf region,
# E 2631..2645, N 1198..1212 (LV95 km, 15 x 15 tiles), mosaicked and cooked
# to assets/terrain/napf.height. Needs magick.
# From ambientCG (CC0): a bark texture set (Bark012, 2K) for imported trees.
# Megascans Megaplants trees (Fab, need a login, so not downloaded): pass the
# directory holding the tree_*_usd.zip downloads as the first argument to
# import variant A of each through tools/usd2gltf and meridian-cook --tree.
# The same directory may hold Megascans rock downloads (*_ue_high.zip, Fab):
# the massive sandstone cliff is cooked for placement; and the ground
# surfaces (*_4k_ue_high.zip) for the terrain material; and the grass and
# flower plants (*_ue_high.zip, *_2k.zip atlases); and the "Forest European
# Broadleaf" pack (forest_european_broadleaf_*: *_gltf_high.zip meshes,
# *_4k_ue_high.zip surfaces).
# Once the project is built, everything is also cooked to KTX2.
set -euo pipefail
megaplants="${1:-}"

for tool in curl jq magick unzip; do
  command -v "$tool" >/dev/null || { echo "$tool is required" >&2; exit 1; }
done

root="$(cd "$(dirname "$0")/.." && pwd)"
models="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"

download() {
  local base="$1" dest="$2"
  shift 2
  mkdir -p "$dest"
  for file in "$@"; do
    if [[ ! -f "$dest/$file" ]]; then
      echo "Downloading $file"
      mkdir -p "$(dirname "$dest/$file")"
      curl -fsSL "$base/$file" -o "$dest/$file"
    fi
  done
}

files=(FlightHelmet.gltf FlightHelmet.bin)
for material in GlassPlasticMat LeatherPartsMat LensesMat MetalPartsMat RubberWoodMat; do
  for map in BaseColor Normal OcclusionRoughMetal; do
    files+=("FlightHelmet_Materials_${material}_${map}.ktx2")
  done
done
download "$models/FlightHelmet/glTF-KTX-BasisU" "$root/assets/FlightHelmet" "${files[@]}"

download "$models/AlphaBlendModeTest/glTF" "$root/assets/source/AlphaBlendModeTest" \
  AlphaBlendModeTest.gltf AlphaBlendModeTest.bin AlphaBlendLabels.png \
  MatBed_baseColor.jpg MatBed_normal.jpg MatBed_occlusionRoughnessMetallic.jpg

polyhaven_files() {
  curl -fsSL -A meridian-fetch "https://api.polyhaven.com/files/$1"
}

for model in rock_moss_set_01 rock_moss_set_02 boulder_01 namaqualand_boulder_04 \
    tree_stump_01 tree_stump_02 dead_tree_trunk_02 dry_branches_medium_01 \
    pine_roots fern_02 nettle_plant shrub_02 dandelion_01 fir_sapling \
    pine_sapling_small island_tree_01; do
  dest="$root/assets/source/$model"
  # Each file has its own URL (the .bin is shared across resolutions).
  while read -r file url; do
    download "${url%/*}" "$(dirname "$dest/$file")" "${url##*/}"
  done < <(polyhaven_files "$model" | jq -r '.gltf["2k"].gltf |
      "\(.url | split("/")[-1]) \(.url)",
      (.include | to_entries[] | "\(.key) \(.value.url)")')
done

bark="$root/assets/source/textures/Bark012"
if [[ ! -f "$bark/Bark012_2K-JPG_Color.jpg" ]]; then
  echo "Downloading Bark012"
  mkdir -p "$bark"
  curl -fsSL -A meridian-fetch "https://ambientcg.com/get?file=Bark012_2K-JPG.zip" \
    -o "$bark/Bark012.zip"
  unzip -q -o "$bark/Bark012.zip" -d "$bark" Bark012_2K-JPG_Color.jpg \
    Bark012_2K-JPG_NormalGL.jpg Bark012_2K-JPG_Roughness.jpg \
    Bark012_2K-JPG_AmbientOcclusion.jpg
  rm "$bark/Bark012.zip"
fi

# Latest year available per tile; the survey is flown in rotation, so a tile
# can be missing from the newest year.
napf_tiles="$root/assets/source/napf"
napf_url="https://data.geo.admin.ch/ch.swisstopo.swissalti3d"
if [[ ! -f "$root/assets/terrain/napf.height" ]]; then
  mkdir -p "$napf_tiles"
  for n in $(seq 1212 -1 1198); do
    for e in $(seq 2631 2645); do
      tile="$napf_tiles/${e}-${n}.tif"
      [[ -f "$tile" ]] && continue
      for year in 2025 2024 2023 2022 2021 2020 2019; do
        name="swissalti3d_${year}_${e}-${n}"
        status=$(curl -sS -w '%{http_code}' "$napf_url/$name/${name}_2_2056_5728.tif" \
          -o "$tile.part") || status=curl-failed
        if [[ "$status" == 200 ]]; then
          mv "$tile.part" "$tile"
          break
        fi
        rm -f "$tile.part"
        if [[ "$status" != 404 ]]; then
          echo "Downloading swissALTI3D tile E$e N$n ($year) failed: HTTP $status" >&2
          exit 1
        fi
      done
      if [[ ! -f "$tile" ]]; then
        echo "No swissALTI3D tile for E$e N$n in 2019-2025" >&2
        exit 1
      fi
    done
    echo "Downloaded tile row N$n"
  done
fi

cook="$root/build/tools/cook/meridian-cook"
if [[ ! -x "$cook" ]]; then
  echo "Build the project, then re-run this script to cook the assets."
  exit 0
fi

for model in "$root"/assets/source/*/*_2k.gltf; do
  name="$(basename "$(dirname "$model")")"
  [[ -d "$root/assets/$name" ]] || "$cook" "$model" "$root/assets/$name"
done
if [[ ! -d "$root/assets/AlphaBlendModeTest" ]]; then
  "$cook" "$root/assets/source/AlphaBlendModeTest/AlphaBlendModeTest.gltf" \
    "$root/assets/AlphaBlendModeTest"
fi

# Napf heightfield (page [0] of each tile; the rest are overviews): rows north to south, tiles west to east, float32 meters.
if [[ ! -f "$root/assets/terrain/napf.height" ]]; then
  rows=()
  for n in $(seq 1212 -1 1198); do
    row="$napf_tiles/row-$n.miff"
    tiles=()
    for e in $(seq 2631 2645); do tiles+=("$napf_tiles/$e-$n.tif[0]"); done
    magick "${tiles[@]}" +append "$row"
    rows+=("$row")
  done
  magick "${rows[@]}" -append -define quantum:format=floating-point -depth 32 -endian LSB \
    "gray:$napf_tiles/napf.f32"
  "$cook" --heightfield "$napf_tiles/napf.f32" 7500 7500 2 "$root/assets/terrain/napf.height"
  rm -f "${rows[@]}"
fi
# Its geo channels (4 m texels), which the causal placement layers read.
if [[ ! -f "$root/assets/terrain/napf.geo" ]]; then
  "$cook" --geo "$root/assets/terrain/napf.height" "$root/assets/terrain/napf.geo" 2
fi

# Megaplants: each zip is extracted into its own empty directory (untrusted
# data, kept apart from the scripts that read it), converted to an
# unflattened glTF, then cooked to 4M triangles (saplings 1M).
megascans="$root/assets/source/megascans"
if [[ -z "$megaplants" ]]; then
  echo "Skipping Megascans trees: pass the directory with the tree_*_usd.zip downloads"
else
  "$root/tools/usd2gltf/setup.sh"
  for zip in "$megaplants"/tree_*_usd.zip; do
    [[ -f "$zip" ]] || continue
    extract="$megascans/_extract/$(basename "$zip" .zip)"
    if [[ ! -d "$extract" ]]; then
      rm -rf "$extract.part"
      mkdir -p "$extract.part"
      unzip -q "$zip" -d "$extract.part"
      mv "$extract.part" "$extract"
    fi
    usd="$(find "$extract" -maxdepth 1 -iname '*_A.usd' | head -n 1)"
    if [[ -z "$usd" ]]; then
      echo "No variant A (*_A.usd) in $zip" >&2
      continue
    fi
    name="$(basename "${usd%.*}" | tr '[:upper:]' '[:lower:]')"
    # Saplings are a few meters tall: a quarter of a canopy tree's budget.
    triangles=4000000
    [[ "$name" == *sapling* ]] && triangles=1000000
    # The budget stamp is written last, so a partial cook or a new budget cooks again.
    out="$megascans/$name"
    [[ "$(cat "$out/budget" 2>/dev/null)" == "$triangles" ]] && continue
    "$root/tools/usd2gltf/.venv/bin/python" -I "$root/tools/usd2gltf/usd2gltf.py" "$usd" \
      "$megascans/_usd/$name.gltf"
    rm -rf "$out.part"
    "$cook" --tree "$megascans/_usd/$name.gltf" "$out.part" --triangles "$triangles" \
      --bark-texture "$bark"
    echo "$triangles" > "$out.part/budget"
    rm -rf "$out"
    mv "$out.part" "$out"
  done

  # The beech textured from Megascans scans (Fab, same directory): leaf atlas
  # unidentified_tree_leaves_pecjv2 (ovate, smooth-edged) on its modelled
  # leaves, beech bark thspcaes on the wood; the game prefers this cook.
  # Maple and oak stay untextured: textured, fewer of their crowns survive
  # the cluster LOD at a few hundred meters (docs/assets.md).
  beech="tree_european_beech_01_a"
  leaves="$megascans/_extract/unidentified_tree_leaves_pecjv2_4k"
  bark_scan="$megascans/_extract/beech_bark_thspcaes_4k_ue_high"
  for zip in "$megaplants/$(basename "$leaves").zip" "$megaplants/$(basename "$bark_scan").zip"; do
    extract="$megascans/_extract/$(basename "$zip" .zip)"
    if [[ -f "$zip" && ! -d "$extract" ]]; then
      rm -rf "$extract.part"
      mkdir -p "$extract.part"
      unzip -q "$zip" -d "$extract.part"
      mv "$extract.part" "$extract"
    fi
  done
  out="$megascans/${beech}_tex"
  if [[ -f "$megascans/_usd/$beech.gltf" && -d "$leaves" && -d "$bark_scan" && ! -d "$out" ]]; then
    python3 -I "$root/tools/usd2gltf/texture_tree.py" "$megascans/_usd/$beech.gltf" \
      "$megascans/_textured/${beech}_tex.gltf" --leaf-atlas "$leaves" --bark "$bark_scan/Textures"
    rm -rf "$out.part"
    "$cook" --tree "$megascans/_textured/${beech}_tex.gltf" "$out.part" --triangles 4000000
    mv "$out.part" "$out"
  fi

  # Megascans rocks for placement (Fab *_ue_high.zip downloads, same
  # directory): the tier 1 glTF and its 4K textures, cooked like the Poly
  # Haven models (which bakes the scan's Z-up centimeter node transform).
  for id in massive_sandstone_cliff_vmmdcj2; do
    zip="$megaplants/${id}_ue_high.zip"
    out="$megascans/$id"
    [[ -f "$zip" && ! -d "$out" ]] || continue
    extract="$megascans/_extract/${id}_ue_high"
    if [[ ! -d "$extract" ]]; then
      rm -rf "$extract.part"
      mkdir -p "$extract.part"
      unzip -q "$zip" -d "$extract.part" '*_tier_1.gltf' '*_tier_1.bin' 'Textures/*'
      mv "$extract.part" "$extract"
    fi
    gltf="$(find "$extract" -maxdepth 1 -name '*_tier_1.gltf' | head -n 1)"
    rm -rf "$out.part"
    "$cook" "$gltf" "$out.part"
    mv "$out.part" "$out"
  done

  # The "Forest European Broadleaf" pack's surfaces (ground, rock and the
  # detail_rock_* close-up maps) are cooked the same way.
  broadleaf_surfaces=()
  for zip in "$megaplants"/forest_european_broadleaf_*_4k_ue_high.zip; do
    [[ -f "$zip" ]] && broadleaf_surfaces+=("$(basename "$zip" _4k_ue_high.zip)")
  done
  # Megascans ground surfaces for the terrain material (Fab *_4k_ue_high.zip,
  # same directory; docs/assets.md has the roles and the ones left out):
  # base color and normal at the full 4K, ARM and height downscaled to 2K
  # (their detail matters less; half the memory). ORM is AO/roughness/metal,
  # the same packing as Poly Haven's ARM; the normal maps are OpenGL (green
  # up), checked against the height maps. layered_rock_vl0fdhdo has no
  # height map: its luminance stands in. albedo.txt holds the base color's
  # mean linear RGB, which the game matches to its category's.
  for id in uncut_grass_oilpt20 wild_grass_pjwgw0 lawn_grass_sbykrmp0 clover_vlzlbjon \
      forest_floor_sfjmafua forest_floor_xdfddec forest_path_ugsnfawlw soil_dirt_pjusf0 \
      soil_dirt_peioh0 soil_mud_olsetpp0 gravel_on_soil_rmrk3sp0 gravel_on_soil_rmsigwp0 \
      rock_cliff_tg1mcgbl layered_rock_vl0fdhdo "${broadleaf_surfaces[@]}"; do
    zip="$megaplants/${id}_4k_ue_high.zip"
    dest="$root/assets/terrain/$id"
    [[ -f "$zip" && ! -f "$dest/albedo.txt" ]] || continue
    extract="$megascans/_extract/${id}_4k_ue_high"
    if [[ ! -d "$extract" ]]; then
      rm -rf "$extract.part"
      mkdir -p "$extract.part"
      unzip -q "$zip" -d "$extract.part" 'Textures/*'
      mv "$extract.part" "$extract"
    fi
    staging="$megascans/_terrain/$id"
    mkdir -p "$staging" "$dest"
    for map in "B base_color color 4096" "N normal normal 4096" "ORM arm data 2048" \
        "H height data 2048"; do
      read -r suffix name usage size <<<"$map"
      png="$(find "$extract/Textures" -maxdepth 1 -iname "*_4K_${suffix}.png" | head -n 1)"
      if [[ -n "$png" ]]; then
        magick "$png" -resize "${size}x${size}" "$staging/$name.png"
      else
        magick "$staging/base_color.png" -colorspace Gray -auto-level -resize "${size}x${size}" \
          "$staging/$name.png"
      fi
      "$cook" --texture "$staging/$name.png" "$dest/$name.ktx2" "$usage"
    done
    magick "$staging/base_color.png" -colorspace RGB -scale 1x1\! \
      -format '%[fx:r] %[fx:g] %[fx:b]\n' info: > "$dest/albedo.txt"
  done
  # The "Forest European Broadleaf" pack's meshes (Fab *_gltf_high.zip):
  # Fab's converter glTF, centimeters under a parent node chain the loader
  # applies, 4K albedo, normal and ORM. Texture size by texel density: the
  # smallest power of two that gives the mesh's largest extent
  # broadleaf_texels_per_meter, clamped to [min, max] (512 per meter is one
  # texel per pixel at ~3.6 m at 2560 px and 70 degrees; closer, mip 0 is
  # magnified). The size goes into <out>/texture-size, so changing a value
  # and re-running recooks only what it changes; the zips are not touched.
  broadleaf_texels_per_meter=512
  broadleaf_min_texture=1024
  broadleaf_max_texture=4096
  fab_units_per_meter=100
  for zip in "$megaplants"/forest_european_broadleaf_*_gltf_high.zip; do
    [[ -f "$zip" ]] || continue
    name="$(basename "$zip" _gltf_high.zip)"
    name="${name#forest_european_broadleaf_}"
    out="$megascans/broadleaf/$name"
    extract="$megascans/_extract/$(basename "$zip" .zip)"
    if [[ ! -d "$extract" ]]; then
      rm -rf "$extract.part"
      mkdir -p "$extract.part"
      unzip -q "$zip" -d "$extract.part"
      mv "$extract.part" "$extract"
    fi
    gltf="$(find "$extract" -maxdepth 1 -name '*.gltf' | head -n 1)"
    extent="$(jq '[.meshes[].primitives[].attributes.POSITION as $p | .accessors[$p] |
      [range(3) as $i | .max[$i] - .min[$i]] | max] | max' "$gltf")"
    size="$(awk -v e="$extent" -v u="$fab_units_per_meter" -v d="$broadleaf_texels_per_meter" \
      -v lo="$broadleaf_min_texture" -v hi="$broadleaf_max_texture" \
      'BEGIN { s = lo; while (s < e / u * d && s < hi) s *= 2; print s }')"
    [[ "$(cat "$out/texture-size" 2>/dev/null)" != "$size" ]] || continue
    staging="$megascans/_broadleaf/$name"
    rm -rf "$staging"
    mkdir -p "$staging/textures"
    cp "$gltf" "$extract"/*.bin "$staging/"
    for texture in "$extract"/textures/*; do
      if [[ "$(magick identify -format '%w' "$texture")" -le "$size" ]]; then
        cp "$texture" "$staging/textures/"
      else
        magick "$texture" -resize "${size}x${size}" "$staging/textures/$(basename "$texture")"
      fi
    done
    rm -rf "$out.part"
    "$cook" "$staging/$(basename "$gltf")" "$out.part"
    echo "$size" > "$out.part/texture-size"
    rm -rf "$out"
    mv "$out.part" "$out"
  done

  # Megascans 3D plants for the grass and flower layers (Fab *_ue_high.zip,
  # same directory): the nonUE glTF when the zip has one (the fern has only
  # the UE one), LOD 0 of every variant in a row, cooked into assets/<id>/
  # (docs/assets.md). The billboard textures (all variants in one atlas)
  # are cooked beside it for the far LOD. The two 2K flower atlases have no
  # mesh: their textures are cooked to assets/<id>/ as card material.
  for id in grass_clumps_qmbr2 rough_meadow_grass_skfjj wild_grass_sjliz wild_grass_vlkhcbxia \
      common_fern_rhdso clovers_rbdjz field_poppy_vmcobd0ja goldenrods_xg1lbfxia \
      yellow_flower_xchjef3ia; do
    zip="$megaplants/${id}_ue_high.zip"
    out="$root/assets/$id"
    [[ -f "$zip" && ! -d "$out" ]] || continue
    extract="$megascans/_extract/${id}_ue_high"
    if [[ ! -d "$extract" ]]; then
      rm -rf "$extract.part"
      mkdir -p "$extract.part"
      unzip -q "$zip" -d "$extract.part" '*_tier_1*.gltf' '*_tier_1.bin' 'Textures/*'
      mv "$extract.part" "$extract"
    fi
    gltf=""
    [[ ! -d "$extract/standard" ]] ||
      gltf="$(find "$extract/standard" -maxdepth 1 -name '*_tier_1_nonUE.gltf' | head -n 1)"
    [[ -n "$gltf" ]] || gltf="$(find "$extract" -maxdepth 1 -name '*_tier_1.gltf' | head -n 1)"
    rm -rf "$out.part"
    "$cook" "$gltf" "$out.part"
    mkdir -p "$out.part/billboard"
    "$cook" --texture "$(find "$extract/Textures" -name '*_4K_Billboard_B-O.png')" \
      "$out.part/billboard/base_color.ktx2" color
    "$cook" --texture "$(find "$extract/Textures" -name '*_4K_Billboard_N-T.png')" \
      "$out.part/billboard/normal_translucency.ktx2" data
    mv "$out.part" "$out"
  done

  # Flower atlases without meshes (Fab *_2k.zip, JPEG): colour, opacity,
  # translucency, roughness and normal cooked as they are.
  for id in meadow_buttercup_sf2occci2 assorted_flowers_pepju2; do
    zip="$megaplants/${id}_2k.zip"
    out="$root/assets/$id"
    [[ -f "$zip" && ! -d "$out" ]] || continue
    extract="$megascans/_extract/${id}_2k"
    if [[ ! -d "$extract" ]]; then
      rm -rf "$extract.part"
      mkdir -p "$extract.part"
      unzip -q "$zip" -d "$extract.part"
      mv "$extract.part" "$extract"
    fi
    rm -rf "$out.part"
    mkdir -p "$out.part"
    for map in "BaseColor base_color color" "Opacity opacity opacity" "Translucency translucency data" \
        "Roughness roughness data" "Normal normal normal"; do
      read -r suffix name usage <<<"$map"
      "$cook" --texture "$(find "$extract" -iname "*_2K_${suffix}.jpg")" \
        "$out.part/$name.ktx2" "$usage"
    done
    mv "$out.part" "$out"
  done
fi

echo "Assets in $root/assets"
