#!/bin/bash
# Metal vs OpenGL screenshots of the same moment (window only, never the desktop).
# Usage: tools/metal/compare.sh <name> <seconds> [game args...]   e.g. compare.sh ci 15 --skip-intro
# Output: build/metal-compare/<name>-{metal,opengl}.png
set -euo pipefail
cd "$(dirname "$0")/../../build"
name=$1; secs=$2; shift 2
out=metal-compare; mkdir -p "$out"
[ -x "$out/winid" ] || swiftc -O ../tools/metal/winid.swift -o "$out/winid"
for r in metal opengl; do
    PD_RENDERER=$r ./pd.arm64 "$@" > "$out/$name-$r.log" 2>&1 &
    pid=$!
    sleep "$secs"
    id=$("$out/winid" pd.arm64 | head -1)
    if [ -n "$id" ]; then screencapture -x -o -l "$id" "$out/$name-$r.png"; sips -Z 640 "$out/$name-$r.png" >/dev/null; fi
    kill $pid 2>/dev/null; wait $pid 2>/dev/null || true
    sleep 1
done
ls "$out/$name-"*.png
