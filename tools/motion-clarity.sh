#!/usr/bin/env bash
# Clarity in motion at user speeds. Usage: tools/motion-clarity.sh BUILD OUT [game args...]
# Each motion runs twice, ending on frame 120 and 121 at the same pose (the
# scripted path always ends at --camera): their difference is jitter/shimmer
# while moving; frame 120 against the still capture is the smear.
# Columns: sharp = Laplacian std of the motion frame (ratio to still),
# jitter = mean |frame120 - frame121| (0-255) in the region, error = mean
# |motion - still| (0-255) in the region.
set -u
build=$1; out=$2; shift 2
extra=("$@")
gates="python3 -I $(cd "$(dirname "$0")" && pwd)/image-gates.py"
mkdir -p "$out"
fail=0

forest="600 1205.6 -400 -1.55 0.02"
ground="1119 1355.9 -431 -1.55 -0.25"
sky="1119 1355.9 -431 -1.55 0.15"
yaw=-1.55

vel() { # mode speed -> "x 0 z" along the view (fwd) or to its right (side)
  python3 -c "import math,sys;y=$yaw;s=float(sys.argv[2])
print(*((s*math.sin(y),0,-s*math.cos(y)) if sys.argv[1]=='fwd' else (s*math.cos(y),0,s*math.sin(y))))" "$1" "$2"
}

run() { # name frames camera args...
  local name=$1 frames=$2 cam=$3; shift 3
  flock /tmp/meridian-gpu.lock "$build/game/game" --camera $cam --frames "$frames" \
    --capture "$out/$name.png" "$@" "${extra[@]}" >"$out/$name.log" 2>&1 ||
    { echo "capture $name failed" >&2; fail=1; }
}
pair() { run "$1_120" 120 "${@:2}"; run "$1_121" 121 "${@:2}"; }

forestArgs=(--trees --time-scale 0)
pair forest_still "$forest" "${forestArgs[@]}"
pair forest_side5 "$forest" "${forestArgs[@]}" --camera-velocity $(vel side 5)
pair forest_side30 "$forest" "${forestArgs[@]}" --camera-velocity $(vel side 30)
pair forest_turn1 "$forest" "${forestArgs[@]}" --camera-yaw-rate 1

groundArgs=(--time-scale 0)
pair ground_still "$ground" "${groundArgs[@]}"
pair ground_side5 "$ground" "${groundArgs[@]}" --camera-velocity $(vel side 5)
pair ground_fwd30 "$ground" "${groundArgs[@]}" --camera-velocity $(vel fwd 30)
pair ground_turn1 "$ground" "${groundArgs[@]}" --camera-yaw-rate 1

skyArgs=(--placement off)
pair sky_still "$sky" "${skyArgs[@]}" --time-scale 0
pair sky_drift "$sky" "${skyArgs[@]}" --time-scale 1
pair sky_turn1 "$sky" "${skyArgs[@]}" --time-scale 0 --camera-yaw-rate 1

# Region fractions x0,y0,x1,y1.
all="all=0,0,1,1"
clouds="clouds=0,0,1,0.45"
num() { awk '{print $2}'; }
printf "%-14s %-7s %8s %7s %8s %8s\n" case region sharp ratio jitter error
row() { # case region-spec still [error: yes|no]
  local c=$1 spec=$2 still=$3 s r j e="-"
  s=$($gates laplacian "$out/${c}_120.png" | num)
  r=$($gates ratio "$out/${c}_120.png" "$out/${still}_120.png" | num)
  j=$($gates shimmer "$out/${c}_120.png" "$out/${c}_121.png" "$spec" | num)
  [ "$4" = yes ] && e=$($gates shimmer "$out/${c}_120.png" "$out/${still}_120.png" "$spec" | num)
  printf "%-14s %-7s %8s %7s %8s %8s\n" "$c" "${spec%%=*}" "$s" "$r" "$j" "$e"
}
row forest_still "$all" forest_still no
row forest_side5 "$all" forest_still yes
row forest_side30 "$all" forest_still yes
row forest_turn1 "$all" forest_still yes
row ground_still "$all" ground_still no
row ground_side5 "$all" ground_still yes
row ground_fwd30 "$all" ground_still yes
row ground_turn1 "$all" ground_still yes
row sky_still "$clouds" sky_still no
row sky_drift "$clouds" sky_still no
row sky_turn1 "$clouds" sky_still yes

echo "resolution: $(magick identify -format '%wx%h' "$out/forest_still_120.png")"
grep -L " 0 validation errors" "$out"/*.log | sed 's/^/validation errors or no summary in: /'
exit $fail
