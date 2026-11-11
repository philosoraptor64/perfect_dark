#ifndef _IN_PDNET_H
#define _IN_PDNET_H

#include <ultra64.h>

/*
 * 2-player netplay over n64net (external/n64net, n64net_input): lockstep for now.
 * See docs/netplay-notes.md, "NEXT: 2-player netplay".
 *
 * Both machines run the deterministic simulation (replaycheck.h) of the same Combat Sim match
 * with 2 players: controller 1 = the host (player 0), controller 2 = the guest (player 1).
 * Each machine samples its own physical controller 1; n64net exchanges the samples so both
 * simulate every tick with the same two pads. The host's match settings (seed, stage,
 * simulants, aspect, build) must equal the guest's: the handshake compares them.
 *
 * Environment:
 *   PD_NET_HOST=1            host (player 1); waits for the guest
 *   PD_NET_PEER=ip:port      guest: the host's address
 *   PD_NET_PORT=n            UDP port to bind (default: host 7301, guest 7302)
 *   PD_NET_DELAY=n           minimum input delay in ticks (default 2; auto delay up to 6)
 *   PD_NET_BOT=seed          test aid: drive the local player with scripted input
 *   PD_NET_WAIT_SECS=n       how long to wait for the other machine (default 60)
 *   PD_NET_VIEW=split        both players split screen on both machines (default: each machine shows its
 *                            own player full screen; every player is simulated with the full-screen view)
 *   PD_NET_ROLLBACK=0        lockstep instead of rollback (default: rollback after PD_NET_ROLLBACK_FROM=60
 *                            ticks, window PD_NET_ROLLBACK_WINDOW=8)
 *   PD_NET_TEST_DESYNC=tick  test aid: corrupt this side's RNG at that tick (sync check must report it)
 * On the iPhone (no environment variables) the same KEY=VALUE lines go in Documents/netplay.cfg; on other
 * machines PD_NET_CONFIG=<file> reads such a file. The environment wins over the file.
 * plus the deterministic-mode settings, equal on both: PD_SEED, --boot-stage (or PD_BOOT_STAGE), PD_REPLAY_BOTS,
 * PD_ASPECT, PD_REPLAY_FRAMES (quit after n ticks).
 */

/* 1 if PD_NET_HOST or PD_NET_PEER is set (checked before replayInit finishes) */
s32 pdnetWanted(void);
/* opens the socket and runs the handshake (blocks until the other machine is there) */
void pdnetInit(u64 seed, s32 stage, s32 bots, float aspect);
s32 pdnetActive(void);
/* osContGetReadData: replaces pads 1-2 with the exchanged inputs for the current tick */
void pdnetFilterPads(OSContPad *pads, s32 count, s32 tick);
void pdnetFilterStatus(OSContStatus *status, s32 count);
/* rollback netplay (PD_NET_ROLLBACK, default on; PD_NET_ROLLBACK_WINDOW=8, PD_NET_ROLLBACK_FROM=60 = lockstep
 * before that tick): called at the rollback point (rollback.c) */
s32 pdnetRollbackActive(void);
void pdnetRollbackPoint(void);
/* once per displayed frame: 1 = skip this frame's game step (time sync, this side is ahead) */
s32 pdnetTimeSyncSkip(void);
/* scripted test input (PD_NET_BOT, PD_TEST_BOT) */
void pdnetBotPad(s32 tick, u32 seed, OSContPad *pad);
/* after each simulated tick, with the deterministic state hash */
void pdnetEndTick(s32 tick, u64 hash);

#endif
