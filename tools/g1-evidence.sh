#!/usr/bin/env bash
# Evidence for the distant-forest work (G1): each view with the voxels as the
# renderer draws them and with triangles everywhere at the default LOD error
# (the voxels' target; full detail overflows the cull lists at forest views),
# plus a fly-toward sequence across the switch distances.
#   tools/g1-evidence.sh BUILD OUT [game args]
# VOXEL_OFF_FLAG selects the triangles-everywhere A/B flag.
set -euo pipefail
build=$1 out=$2
shift 2
off=${VOXEL_OFF_FLAG:---voxel-trees off}
mkdir -p "$out"
game=("$build/game/game" --trees "$@")

views=(
  "edge 600 1205.6 -400 -1.55 0.02"
  "summit 1119 1356 -431 -1.55 -0.02"
  "overview 700 1450 -400 -1.55 -0.5"
  "high 1119 4354 -431 -1.55 -0.35"
)

run() { # name args...
  local name=$1
  shift
  flock /tmp/meridian-gpu.lock "${game[@]}" "$@" > "$out/$name.log" 2>&1
  grep -m1 '^GPU passes' "$out/$name.log" | sed "s/^/$name: /" >> "$out/gpu.txt"
}

: > "$out/gpu.txt"
for v in "${views[@]}"; do
  read -r name x y z yaw pitch <<< "$v"
  run "$name" --camera "$x" "$y" "$z" "$yaw" "$pitch" --capture "$out/$name.png"
  # shellcheck disable=SC2086
  run "${name}_tri" --camera "$x" "$y" "$z" "$yaw" "$pitch" $off \
    --capture "$out/${name}_tri.png"
done
run fly --camera 700 1300 -400 -1.55 -0.1 --camera-velocity -40 -3 0 \
  --capture-sequence 10 --frames 300 --capture "$out/fly.png"
echo "evidence in $out"
