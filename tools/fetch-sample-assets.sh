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
# - Ground textures as JPEG (base color, OpenGL normal, AO/roughness/metal,
#   height).
# From swisstopo swissALTI3D (open government data, free use with the
# attribution "© swisstopo"): the 2 m elevation tiles of the Napf region,
# E 2631..2645, N 1198..1212 (LV95 km, 15 x 15 tiles), mosaicked and cooked
# to assets/terrain/napf.height. Needs magick.
# From ambientCG (CC0): a bark texture set (Bark012, 2K) for imported trees.
# Megascans Megaplants trees (Fab, need a login, so not downloaded): pass the
# directory holding the tree_*_usd.zip downloads as the first argument to
# import variant A of each through tools/usd2gltf and meridian-cook --tree.
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

for texture in forest_ground_04 forest_leaves_04 rock_face_03 brown_mud_leaves_01 \
    rocky_trail dirt_floor mossy_rock; do
  # Maps can live in different directories, so take each one's own URL.
  while read -r url; do
    download "${url%/*}" "$root/assets/source/textures/$texture" "${url##*/}"
  done < <(polyhaven_files "$texture" |
      jq -r '.Diffuse, .nor_gl, .arm, .Displacement | .["2k"].jpg.url')
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

# Terrain layers: one KTX2 per map, as the game loads them.
for texture in forest_ground_04 forest_leaves_04 rock_face_03; do
  source="$root/assets/source/textures/$texture/$texture"
  dest="$root/assets/terrain/$texture"
  for map in "diff base_color color" "nor_gl normal normal" "arm arm data" \
      "disp height data"; do
    read -r suffix name usage <<<"$map"
    [[ -f "$dest/$name.ktx2" ]] ||
      "$cook" --texture "${source}_${suffix}_2k.jpg" "$dest/$name.ktx2" "$usage"
  done
done

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
fi

echo "Assets in $root/assets"
