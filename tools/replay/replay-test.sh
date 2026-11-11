#!/bin/bash
# Replay determinism check (see port/include/replaycheck.h).
#
#   tools/replay/replay-test.sh                 scripted input, 3 playbacks, compare hashes
#   INPUT=<file> tools/replay/replay-test.sh    play a recorded input file instead
#   RECORD=1 tools/replay/replay-test.sh        record a human run (window in front, real speed)
#
# Options: STAGE (0x30 Defection), TICKS (scripted length, 2400), RUNS (3), SEED (1),
#          SEED2 (second seed for the divergence sanity check; empty = skip), FAST (1 = uncapped).
# Output in build/replay-test/<stamp>/. The ROM must be at build/data/pd.ntsc-final.z64.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$ROOT/build/pd.arm64"
STAGE=${STAGE:-0x30}
TICKS=${TICKS:-2400}
RUNS=${RUNS:-3}
SEED=${SEED:-1}
SEED2=${SEED2-2}
FAST=${FAST:-1}
OUT="$ROOT/build/replay-test/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"

# fresh save folder per run (the game writes its save + pd.ini there)
mksave() {
    local d="$1"
    rm -rf "$d"; mkdir -p "$d"
    {
        echo "[Game]"
        echo "TickRateDivisor=$([ "$FAST" = 1 ] && echo 0 || echo 1)"
        echo "ExtraSleep=0"
        echo "[Video]"
        echo "VSync=$([ "$FAST" = 1 ] && echo 0 || echo 1)"
        echo "DefaultFullscreen=0"
        echo "DefaultWidth=${WIDTH:-640}"
        echo "DefaultHeight=${HEIGHT:-480}"
    } > "$d/pd.ini"
}

run() { # name seed [extra env...]
    local name=$1 seed=$2; shift 2
    mksave "$OUT/save-$name"
    ( cd "$ROOT/build" && env SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 \
        SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy \
        PD_SEED="$seed" PD_REPLAY_CHECK="$OUT/$name.hash" "$@" \
        "$BIN" --savedir "$OUT/save-$name" --boot-stage "$STAGE" > "$OUT/$name.out" 2>&1 ) || true
    printf '%-8s %6s ticks  last: %s\n' "$name" "$(grep -vc '^#' "$OUT/$name.hash")" "$(tail -1 "$OUT/$name.hash")"
}

if [ "${RECORD:-0}" = 1 ]; then
    mksave "$OUT/save-rec"
    sed -i '' 's/^TickRateDivisor=.*/TickRateDivisor=1/; s/^VSync=.*/VSync=1/' "$OUT/save-rec/pd.ini"
    echo "Recording: play, then quit the game. Input -> $OUT/input.txt"
    ( cd "$ROOT/build" && PD_SEED="$SEED" PD_REPLAY_RECORD="$OUT/input.txt" PD_REPLAY_CHECK="$OUT/rec.hash" \
        "$BIN" --savedir "$OUT/save-rec" --boot-stage "$STAGE" > "$OUT/rec.out" 2>&1 ) || true
    INPUT="$OUT/input.txt"
    echo "recorded $(grep -vc '^#' "$INPUT") ticks"
fi

if [ -z "${INPUT:-}" ]; then
    INPUT="$OUT/input.txt"
    # scripted P1: wait, skip the intro (START), then walk/turn/strafe/fire/jump patterns
    python3 -I - "$TICKS" > "$INPUT" <<'EOF'
import sys
n = int(sys.argv[1])
A, B, Z, START = 0x8000, 0x4000, 0x2000, 0x1000
CU, CD, CL, CR, R, L = 0x0008, 0x0004, 0x0002, 0x0001, 0x0010, 0x0020
print("# scripted: tick then 4 x (buttons sx sy rsx rsy err)")
for t in range(n):
    b = 0; x = 0; y = 0
    if t in (240, 241, 242):
        b |= START
    if t > 600:
        phase = (t // 90) % 6
        if phase == 0: y = 70
        elif phase == 1: y = 60; x = 45
        elif phase == 2: x = -55
        elif phase == 3: y = 50; b |= CL
        elif phase == 4: y = -40; b |= CR
        else: y = 70; x = -20
        if t % 37 < 6: b |= Z
        if t % 151 < 3: b |= A
        if t % 211 < 20: b |= R
    pads = [f"{b:x} {x} {y} 0 0 0"] + ["0 0 0 0 0 4"] * 3
    print(t, " ".join(pads))
EOF
fi
INPUT=$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")
ticks=$(grep -vc '^#' "$INPUT")
echo "input: $INPUT ($ticks ticks), stage $STAGE, seed $SEED, output $OUT"

for i in $(seq 1 "$RUNS"); do
    run "play$i" "$SEED" PD_REPLAY_PLAY="$INPUT"
done
[ -n "$SEED2" ] && run "seed$SEED2" "$SEED2" PD_REPLAY_PLAY="$INPUT"

status=0
for i in $(seq 2 "$RUNS"); do
    first=$(diff <(grep -v '^#' "$OUT/play1.hash") <(grep -v '^#' "$OUT/play$i.hash") | grep -m1 '^[<>]' || true)
    if [ -z "$first" ]; then echo "play$i == play1: IDENTICAL"; else echo "play$i != play1: DIVERGED at: $first"; status=1; fi
done
if [ -n "$SEED2" ]; then
    first=$(diff <(grep -v '^#' "$OUT/play1.hash" | awk '{print $1, $NF}') <(grep -v '^#' "$OUT/seed$SEED2.hash" | awk '{print $1, $NF}') | grep -m1 '^>' || true)
    echo "seed $SEED2 vs seed $SEED (should differ): ${first:-NO DIFFERENCE (hash misses the RNG?)}"
fi
exit $status
