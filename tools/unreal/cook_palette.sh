#!/usr/bin/env bash
# Cooks retinted copies of every cooked Megascans Trees variant
# (docs/assets.md "Megascans Trees from Unreal") beside the originals:
# assets/source/megascans/<species>-<palette name>/, selected in the game by
# --tree-palette NAME. The originals are not touched.
#
#   tools/unreal/cook_palette.sh tools/unreal/leaf-palette.json
set -euo pipefail
palette="$1"
root="$(cd "$(dirname "$0")/../.." && pwd)"
megascans="$root/assets/source/megascans"
cook="$root/build/tools/cook/meridian-cook"
name="$(jq -r .name "$palette")"

# The triangle budgets the originals were cooked with (docs/assets.md).
budget() {
  case "$1" in
    SM_EuropeanBeech_Field_01) echo 4000000 ;;
    SM_BlackAlder_Field_02) echo 2000000 ;;
    SM_EuropeanBeech_Forest_*) echo 1500000 ;;
    *_Sapling_*) echo 300000 ;;
    *) echo 1200000 ;;
  esac
}

for species in blackalder europeanbeech europeanhornbeam; do
  [[ -d "$megascans/$species" ]] || continue
  staging="$megascans/_unreal/$species-$name"
  python3 -I "$root/tools/unreal/retint_leaves.py" "$palette" "$megascans/_unreal/$species" "$staging"
  out="$megascans/$species-$name"
  mkdir -p "$out"
  # Content-addressed encodings: sharing the originals' cache only adds files.
  [[ -e "$out/.ktx2-cache" ]] || ln -s "../$species/.ktx2-cache" "$out/.ktx2-cache"
  for variant in "$megascans/$species"/SM_*/; do
    variant="$(basename "$variant")"
    rm -rf "$out/$variant"
    "$cook" --tree "$staging/$variant.gltf" "$out/$variant" --triangles "$(budget "$variant")"
  done
done
