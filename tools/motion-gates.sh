#!/usr/bin/env bash
# Motion-clarity gates. Usage: tools/motion-gates.sh BUILD OUT [game args...]
# Thresholds: MIN_* env vars override. GPU_RUNS=5 repeats the ground still and prints per-pass medians.
set -u
build=$1; out=$2; shift 2
extra=("$@")
here=$(cd "$(dirname "$0")" && pwd)
gates="python3 -I $here/image-gates.py"
mkdir -p "$out"
fail=0
ground="1119 1355.9 -431 -1.55 -0.25"
beech="1115.47 1366.85 -429.72 1.223 0"
gyaw=-1.55
byaw=1.223

vel() { # yaw mode: fwd|side -> "x 0 z" for 5 m/s
  python3 -c "import math,sys;y=float(sys.argv[1]);f=sys.argv[2]=='fwd'
print(*( (5*math.sin(y),0,-5*math.cos(y)) if f else (5*math.cos(y),0,5*math.sin(y)) ))" "$1" "$2"
}

run() { # name camera args...
  local name=$1 cam=$2; shift 2
  flock /tmp/meridian-gpu.lock "$build/game/game" --placement off \
    --camera $cam --capture "$out/$name.png" "$@" "${extra[@]}" \
    >"$out/$name.log" 2>&1
  local rc=$?
  if [ $rc -ne 0 ]; then echo "capture $name failed (exit $rc)" >&2; fail=1; fi
}

run ground_still "$ground"
run ground_fwd "$ground" --camera-velocity $(vel $gyaw fwd)
run ground_side "$ground" --camera-velocity $(vel $gyaw side)
run ground_pitch "$ground" --camera-angular-velocity 0 0.02
run beech_still "$beech" --scatter 100
run beech_fwd "$beech" --scatter 100 --camera-velocity $(vel $byaw fwd)
run beech_f120 "$beech" --scatter 100 --frames 120
run beech_f121 "$beech" --scatter 100 --frames 121

if [ -n "${GPU_RUNS:-}" ] && [ "$GPU_RUNS" -gt 1 ]; then
  for i in $(seq 2 "$GPU_RUNS"); do run ground_still_$i "$ground"; done
fi

row() { printf "%-30s %8s %8s  %-6s %s\n" "$@"; }
cmp_ge() { python3 -c "import sys;print('pass' if float(sys.argv[1])>=float(sys.argv[2]) else 'FAIL')" "$1" "$2"; }
cmp_le() { python3 -c "import sys;print('pass' if float(sys.argv[1])<=float(sys.argv[2]) else 'FAIL')" "$1" "$2"; }
val() { awk '{print $2}'; }

# Sharpness in motion is the motion frame's own Laplacian std, thresholds
# anchored to TAA v2 at 1440p (0.114, 0.118, 0.226, 0.108; minimum = 90% of
# each, rounded down, docs/decisions.md); the motion/still
# ratio is informational (a sharper still, as under Ray Reconstruction,
# lowers it without making the motion frame worse).
row gate value minimum result "(motion/still ratio)"
sharp() { # label motion still minimum
  local v r
  v=$($gates laplacian "$out/$2.png" | val); r=$($gates ratio "$out/$2.png" "$out/$3.png" | val)
  row "$1" "$v" ">=$4" "$(cmp_ge "$v" "$4")" "$r"
}
sharp "laplacian fwd5 ground" ground_fwd ground_still ${MIN_FWD_GROUND:-0.10}
sharp "laplacian side5 ground" ground_side ground_still ${MIN_SIDE_GROUND:-0.10}
sharp "laplacian fwd5 beech" beech_fwd beech_still ${MIN_FWD_BEECH:-0.20}
sharp "laplacian pitch0.02 ground" ground_pitch ground_still ${MIN_PITCH_GROUND:-0.09}
sh=$($gates shimmer "$out/beech_f120.png" "$out/beech_f121.png")
v=$(echo "$sh" | awk '$1=="crown"{print $2}'); row "shimmer crown" "$v" "<=2.0" "$(cmp_le "$v" 2.0)"
v=$(echo "$sh" | awk '$1=="shadow"{print $2}'); row "shimmer shadow" "$v" "<=2.0" "$(cmp_le "$v" 2.0)"
v=$($gates ssim "$out/ground_fwd.png" "$out/ground_still.png" | val); row "ssim fwd5 ground" "$v" ">=0.98" "$(cmp_ge "$v" 0.98)"

echo "resolution: $(magick identify -format '%wx%h' "$out/ground_still.png")"
echo "GPU passes (ground_still):"; grep "GPU passes:" "$out/ground_still.log"
for f in "$out"/*.log; do echo "$(basename "$f" .log): $(grep 'GPU passes:' "$f")"; done

if [ -n "${GPU_RUNS:-}" ] && [ "$GPU_RUNS" -gt 1 ]; then
  echo "per-pass median over $GPU_RUNS ground-still runs:"
  python3 - "$out" "$GPU_RUNS" <<'P'
import re, statistics, sys
out, n = sys.argv[1], int(sys.argv[2])
runs = []
for i in range(n):
    f = f"{out}/ground_still.log" if i == 0 else f"{out}/ground_still_{i+1}.log"
    line = next(l for l in open(f) if "GPU passes:" in l)
    runs.append({k: float(v) for k, v in re.findall(r"([\w +]+?) ([0-9.]+)(?:,|$| ms)", line.split("GPU passes:")[1])})
for k in runs[0]:
    print(f"  {k.strip()}: {statistics.median(r[k] for r in runs if k in r):.3f} ms")
P
fi
exit $fail
