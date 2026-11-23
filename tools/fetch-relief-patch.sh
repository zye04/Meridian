#!/usr/bin/env bash
# Relief test patch (terrain triage, plan step 1): a 4x4 km square of the
# Napf (E 2637..2640, N 1203..1206, LV95 km; the start ridge and the summit)
# from swisstopo swissALTI3D at 0.5 m and at 2 m, each tile pair from the
# same survey year, cooked to assets/terrain/relief-0.5.height (8000^2) and
# relief-2.height (2000^2). Both are centred on E 2639 N 1205, so a view on
# napf.height at (x, z) is at (x - 500, z - 500) here. "© swisstopo".
# Needs magick and a built meridian-cook.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tiles_dir="$root/assets/source/relief"
url="https://data.geo.admin.ch/ch.swisstopo.swissalti3d"
cook="$root/build/tools/cook/meridian-cook"
[[ -x "$cook" ]] || { echo "Build the project first (needs $cook)" >&2; exit 1; }
mkdir -p "$tiles_dir" "$root/assets/terrain"

fetch() { # name resolution out
  local status
  status=$(curl -sS -w '%{http_code}' "$url/$1/${1}_${2}_2056_5728.tif" -o "$3.part") ||
    status=curl-failed
  if [[ "$status" == 200 ]]; then mv "$3.part" "$3"; return 0; fi
  rm -f "$3.part"
  [[ "$status" == 404 ]] && return 1
  echo "Downloading $1 ($2 m) failed: HTTP $status" >&2
  exit 1
}

for n in $(seq 1206 -1 1203); do
  for e in $(seq 2637 2640); do
    fine="$tiles_dir/$e-$n-0.5.tif" coarse="$tiles_dir/$e-$n-2.tif"
    [[ -f "$fine" && -f "$coarse" ]] && continue
    rm -f "$fine" "$coarse"
    for year in 2025 2024 2023 2022 2021 2020 2019; do
      name="swissalti3d_${year}_${e}-${n}"
      if fetch "$name" 0.5 "$fine"; then
        fetch "$name" 2 "$coarse" || { echo "No 2 m tile for $name" >&2; exit 1; }
        echo "Tile E$e N$n: $year"
        break
      fi
    done
    [[ -f "$fine" ]] || { echo "No swissALTI3D tile for E$e N$n in 2019-2025" >&2; exit 1; }
  done
done

# Page [0] of each tile (the rest are overviews): rows north to south,
# tiles west to east, float32 meters.
for res in 0.5 2; do
  out="$root/assets/terrain/relief-$res.height"
  [[ -f "$out" ]] && continue
  rows=()
  for n in $(seq 1206 -1 1203); do
    tiles=()
    for e in $(seq 2637 2640); do tiles+=("$tiles_dir/$e-$n-$res.tif[0]"); done
    magick "${tiles[@]}" +append "$tiles_dir/row-$n.miff"
    rows+=("$tiles_dir/row-$n.miff")
  done
  magick "${rows[@]}" -append -define quantum:format=floating-point -depth 32 -endian LSB \
    "gray:$tiles_dir/relief-$res.f32"
  size=2000
  [[ $res == 0.5 ]] && size=8000
  "$cook" --heightfield "$tiles_dir/relief-$res.f32" "$size" "$size" "$res" "$out"
  rm -f "${rows[@]}" "$tiles_dir/relief-$res.f32"
done
