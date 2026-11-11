#!/bin/bash
# Netplay between this Mac (host, build/pd.arm64) and the iOS build running in the iOS Simulator (guest,
# build-iossim/pd.arm64.app). Exercises the iOS-only code (Documents paths, netplay.cfg, Metal on iOS, the
# UIScene app) without signing or a phone. Both sides are driven by scripted bots.
# Usage: tools/net/sim-test.sh  (env: SESSION=1 = the whole game from boot; STAGE=0x32 BOTS=2 FRAMES=1801 SEED=7
#        DELAY=2 ROLLBACK=1 LATENCY/JITTER/LOSS; SERVER=local [ROOM=PDSM] [RELAY=1]; SIM=<device udid> (default:
#        the booted one); ROM=<path to the NTSC 1.1 ROM> (copied into the simulator's Documents once))
# Build the simulator app first (see docs/netplay-notes.md, "iOS Simulator").
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root/build"
SIM=${SIM:-booted}; BUNDLE=io.github.philosoraptor.perfectdark
STAGE=${STAGE:-0x32}; BOTS=${BOTS:-2}; FRAMES=${FRAMES:-1801}; SEED=${SEED:-7}; DELAY=${DELAY:-2}
out=net-test/sim-$(date +%Y%m%d-%H%M%S); mkdir -p "$out/host"

xcrun simctl install "$SIM" "$root/build-iossim/pd.arm64.app" || { echo "RESULT: FAILED (install)"; exit 1; }
data=$(xcrun simctl get_app_container "$SIM" $BUNDLE data) || { echo "RESULT: FAILED (no container)"; exit 1; }
docs="$data/Documents"; mkdir -p "$docs"
if [ ! -f "$docs/pd.ntsc-final.z64" ]; then
    [ -n "${ROM:-}" ] || { echo "Set ROM=<path to the NTSC 1.1 ROM> (copied into the simulator once)"; exit 1; }
    cp "$ROM" "$docs/pd.ntsc-final.z64"
fi
rm -f "$docs/pd.log" "$docs/eeprom.bin"

common=(PD_SEED=$SEED PD_REPLAY_BOTS=$BOTS PD_REPLAY_FRAMES=$FRAMES PD_NET_DELAY=$DELAY PD_NET_ROLLBACK=${ROLLBACK:-1}
        PD_NET_SIM_LATENCY=${LATENCY:-0} PD_NET_SIM_JITTER=${JITTER:-0} PD_NET_SIM_LOSS=${LOSS:-0})
stagearg=(--boot-stage "$STAGE")
if [ "${SESSION:-0}" = 1 ]; then common+=(PD_NET_SESSION=1); stagearg=(); else common+=(PD_BOOT_STAGE=$STAGE); fi

spid=
if [ "${SERVER:-}" = local ]; then
    python3 ../external/n64net/server/rendezvous.py --bind 127.0.0.1 --port 7790 > "$out/server.log" 2>&1 &
    spid=$!
    sleep 1
    hostnet=(PD_NET_SERVER=127.0.0.1:7790 PD_NET_ROOM=${ROOM:-PDSM} ${RELAY:+PD_NET_RELAY=$RELAY})
    guestnet=("${hostnet[@]}")
else
    hostnet=(PD_NET_PORT=7301)
    guestnet=(PD_NET_PEER=127.0.0.1:7301 PD_NET_PORT=7302)
fi

# the guest's settings go through the iPhone path: Documents/netplay.cfg
{
    echo "# written by tools/net/sim-test.sh"
    for kv in "${common[@]}" "${guestnet[@]}" PD_NET_BOT=22; do echo "$kv"; done
} > "$docs/netplay.cfg"

env "${common[@]}" "${hostnet[@]}" PD_NET_BOT=11 PD_NET_HOST=1 PD_RENDERER=opengl \
    ./pd.arm64 ${stagearg[@]+"${stagearg[@]}"} --savedir "$PWD/$out/host" > "$out/host.log" 2>&1 &
hpid=$!
sleep 1
SIMCTL_CHILD_SDL_AUDIODRIVER=dummy SIMCTL_CHILD_SDL_AUDIO_DRIVER=dummy \
    xcrun simctl launch --terminate-running-process "$SIM" $BUNDLE > /dev/null
trap 'kill $hpid $spid 2>/dev/null; xcrun simctl terminate "$SIM" $BUNDLE 2>/dev/null' INT TERM
wait $hpid
# the guest quits by itself after FRAMES ticks too; give it a moment, then collect its log
for _ in $(seq 1 30); do
    xcrun simctl spawn "$SIM" launchctl list 2>/dev/null | grep -q "$BUNDLE" || break
    sleep 1
done
xcrun simctl terminate "$SIM" $BUNDLE 2>/dev/null
[ -n "$spid" ] && kill $spid 2>/dev/null
cp "$docs/pd.log" "$out/guest.log" 2>/dev/null
# leave a disabled config behind so a normal launch plays offline
echo "# netplay off (sim-test.sh)" > "$docs/netplay.cfg"
for side in host guest; do
    echo "== $side"; grep -E 'netplay:|DESYNC|FATAL|fatal' "$out/$side.log" | tail -6
done
for side in host guest; do
    if ! grep -q 'netplay: connected' "$out/$side.log" 2>/dev/null; then echo "RESULT: FAILED ($side never connected)"; exit 1; fi
done
if grep -q DESYNC "$out/host.log" "$out/guest.log"; then echo "RESULT: DESYNC"; exit 1; fi
echo "RESULT: IN SYNC (logs in build/$out)"
