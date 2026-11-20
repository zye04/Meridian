#!/usr/bin/env bash
# Cloud look development: captures views matched to the reference photos in
# assets/reference/clouds/, writes each beside its photo (OUT/<view>-<photo>.png,
# same height; OUT/sheet.png all of them) and prints the measured targets of
# the captures and the photos (tools/cloud-metrics.py).
# Usage: tools/cloud-compare.sh BUILD OUT [game args...]
set -u
build=$1; out=$2; shift 2
extra=("$@")
here=$(cd "$(dirname "$0")" && pwd)
refs=$here/../assets/reference/clouds
metrics="python3 -I $here/cloud-metrics.py"
for photo in cloud1 cloud2 cloud3 cloud4 cloud5; do
  [ -f "$refs/$photo.jpg" ] || { echo "missing reference photo $refs/$photo.jpg" >&2; exit 1; }
done
mkdir -p "$out"

# name | camera and sun (game args) | photos
views=(
  "ground|--camera 1119 1356 -431 -1.55 0.5 --sun 0.45 0.85 0.25|cloud1 cloud5"
  "wide|--camera 1119 1356 -431 -1.55 0.1 --sun 0.45 0.85 0.25|cloud3 cloud4"
  "sunset|--camera 1119 5500 -431 -1.55 -0.2 --sun -0.998 0.05 0.03 --weather broken|cloud2"
)

run() { # file view-args [args...]
  local file=$1 view=$2; shift 2
  # shellcheck disable=SC2086
  flock /tmp/meridian-gpu.lock "$build/game/game" --placement off $view "$@" "${extra[@]}" \
    --capture "$file" > "${file%.*}.log" 2>&1 || { echo "game failed (or validation errors): $file (see ${file%.*}.log)" >&2; exit 1; }
  grep "GPU passes" "${file%.*}.log" | grep -o "clouds [0-9.]*" | sed "s|^|$(basename "$file") |"
}
ev() { grep -o "EV100 [0-9.-]*" "$1" | cut -d' ' -f2; }

sheet=()
for view in "${views[@]}"; do
  IFS='|' read -r name args photos <<< "$view"
  run "$out/$name.png" "$args"
  run "$out/$name.pfm" "$args"

  run "$out/$name-off.pfm" "$args" --clouds off
  echo "== $name (capture)"
  $metrics capture "$out/$name.pfm" "$(ev "$out/$name.log")" "$out/$name-off.pfm" \
    "$(ev "$out/$name-off.log")"
  rm "$out/$name.pfm" "$out/$name-off.pfm"
  echo "== $name (capture image, measured as a photo)"
  $metrics photo "$out/$name.png"
  for photo in $photos; do
    echo "== $name vs $photo (photo)"
    $metrics photo "$refs/$photo.jpg"
    magick "$out/$name.png" -resize x720 \( "$refs/$photo.jpg" -resize x720 \) +append \
      "$out/$name-$photo.png"
    sheet+=("$out/$name-$photo.png")
  done
done
magick "${sheet[@]}" -resize x540 -gravity west -background black -append "$out/sheet.png"
