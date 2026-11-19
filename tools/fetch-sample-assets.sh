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
# Once the project is built, everything is also cooked to KTX2.
set -euo pipefail

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

echo "Assets in $root/assets"
