#!/bin/bash
# Two Perfect Dark copies on this Mac playing one netplay match (lockstep), both driven by
# scripted bots (PD_NET_BOT), then a summary of the sync checks.
# Usage: tools/net/pair-test.sh   (env: STAGE=0x32 BOTS=2 FRAMES=1801 SEED=7 DELAY=2 RENDERER=opengl|metal
#        HOST_BOT=11 GUEST_BOT=22; HOST_BOT=0 = play the host yourself with controller 1;
#        DESYNC_TEST=tick = corrupt the guest at that tick: the result must be DESYNC;
#        LATENCY/JITTER (ms) LOSS (%) = simulated bad link on both sides; ROLLBACK=0 = lockstep;
#        SERVER=local [ROOM=PDTS] [RELAY=1] = connect through a local room server with a room code)
set -uo pipefail
cd "$(dirname "$0")/../../build"
STAGE=${STAGE:-0x32}; BOTS=${BOTS:-2}; FRAMES=${FRAMES:-1801}; SEED=${SEED:-7}; DELAY=${DELAY:-2}
RENDERER=${RENDERER:-opengl}; HOST_BOT=${HOST_BOT:-11}; GUEST_BOT=${GUEST_BOT:-22}
out=net-test/$(date +%Y%m%d-%H%M%S); mkdir -p "$out/host" "$out/guest"
sim=(PD_NET_SIM_LATENCY=${LATENCY:-0} PD_NET_SIM_JITTER=${JITTER:-0} PD_NET_SIM_LOSS=${LOSS:-0} PD_NET_ROLLBACK=${ROLLBACK:-1})
common=("${sim[@]}" PD_SEED=$SEED PD_REPLAY_BOTS=$BOTS PD_REPLAY_FRAMES=$FRAMES PD_NET_DELAY=$DELAY PD_RENDERER=$RENDERER)
hostbot=(); [ "$HOST_BOT" != 0 ] && hostbot=(PD_NET_BOT=$HOST_BOT)
# SERVER=local: a room server on 127.0.0.1:7790 (n64net/server/rendezvous.py) and room codes instead of
# addresses (ROOM=PDTS, RELAY=1 forces the relay path)
spid=
if [ "${SERVER:-}" = local ]; then
    python3 ../external/n64net/server/rendezvous.py --bind 127.0.0.1 --port 7790 > "$out/server.log" 2>&1 &
    spid=$!
    sleep 1
    hostnet=(PD_NET_SERVER=127.0.0.1:7790 PD_NET_ROOM=${ROOM:-PDTS} ${RELAY:+PD_NET_RELAY=$RELAY})
    guestnet=("${hostnet[@]}")
else
    hostnet=(PD_NET_PORT=7301)
    guestnet=(PD_NET_PEER=127.0.0.1:7301 PD_NET_PORT=7302)
fi
env "${common[@]}" "${hostbot[@]}" "${hostnet[@]}" PD_NET_HOST=1 \
    ./pd.arm64 --boot-stage "$STAGE" --savedir "$PWD/$out/host" > "$out/host.log" 2>&1 &
hpid=$!
sleep 1
env "${common[@]}" ${DESYNC_TEST:+PD_NET_TEST_DESYNC=$DESYNC_TEST} PD_NET_BOT=$GUEST_BOT "${guestnet[@]}" \
    ./pd.arm64 --boot-stage "$STAGE" --savedir "$PWD/$out/guest" > "$out/guest.log" 2>&1 &
gpid=$!
trap 'kill $hpid $gpid $spid 2>/dev/null' INT TERM
wait $hpid; wait $gpid
[ -n "$spid" ] && kill $spid 2>/dev/null
for side in host guest; do
    echo "== $side"; grep -E 'netplay:|DESYNC|FATAL|fatal' "$out/$side.log" | tail -6
done
if grep -q DESYNC "$out/host.log" "$out/guest.log"; then echo "RESULT: DESYNC"; exit 1; fi
echo "RESULT: IN SYNC (logs in build/$out)"
