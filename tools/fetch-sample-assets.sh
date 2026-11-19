#!/usr/bin/env bash
# Downloads test assets into assets/ (not tracked by git).
# FlightHelmet: Khronos glTF-Sample-Assets, CC0-1.0, KTX2 (Basis Universal)
# variant.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
base="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/FlightHelmet/glTF-KTX-BasisU"
dest="$root/assets/FlightHelmet"
mkdir -p "$dest"

files=(FlightHelmet.gltf FlightHelmet.bin)
for material in GlassPlasticMat LeatherPartsMat LensesMat MetalPartsMat RubberWoodMat; do
  for map in BaseColor Normal OcclusionRoughMetal; do
    files+=("FlightHelmet_Materials_${material}_${map}.ktx2")
  done
done

for file in "${files[@]}"; do
  if [[ ! -f "$dest/$file" ]]; then
    echo "Downloading $file"
    curl -fsSL "$base/$file" -o "$dest/$file"
  fi
done
echo "Assets in $dest"
