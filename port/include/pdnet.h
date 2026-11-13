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
 *   PD_NET_SESSION=1         the whole game online from boot (title, menus, matches, back to the menus), like
 *                            two controllers on one console; lockstep for now
 *   PD_NET_ROLLBACK=0        lockstep instead of rollback (default: rollback after PD_NET_ROLLBACK_FROM=60
 *                            ticks, window PD_NET_ROLLBACK_WINDOW=8)
 *   PD_NET_DROP_SECS=n       the other player silent this long = gone (default 20); a player who quits says so at
 *                            once. Then this machine plays on offline. A player whose iOS app is in the background is
 *                            waited for without a limit (the game holds on both machines until it's back).
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
/* 1 = boot straight into the 2-player Combat Sim (default); 0 in a session (PD_NET_SESSION=1: the whole game
 * online from boot, menus included; the guest plays on the host's save data) */
s32 pdnetMatchMode(void);
/* osContGetReadData: replaces pads 1-2 with the exchanged inputs for the current tick */
void pdnetFilterPads(OSContPad *pads, s32 count, s32 tick);
void pdnetFilterStatus(OSContStatus *status, s32 count);
/* rollback netplay (PD_NET_ROLLBACK, default on; PD_NET_ROLLBACK_WINDOW=8, PD_NET_ROLLBACK_FROM=60 = lockstep
 * before that tick): called at the rollback point (rollback.c) */
s32 pdnetRollbackActive(void);
void pdnetRollbackPoint(void);
/* once per displayed frame: 1 = skip this frame's game step (time sync, this side is ahead) */
s32 pdnetTimeSyncSkip(void);
/* inside a host app: why the match ends (a desync, the friend gone); "" = it doesn't */
const char *pdnetEndReason(void);
/* scripted test input (PD_NET_BOT, PD_TEST_BOT) */
void pdnetBotPad(s32 tick, u32 seed, OSContPad *pad);
/* puts this machine's own gameplay settings back (the session used both players'; called before pd.ini is
 * saved) */
void pdnetRestoreLocalSettings(void);
/* main loop, before each tick: 1 = this app is in the background, hold the game (the other player waits) */
s32 pdnetHoldWhileAway(void);
/* Online from the menus: connect on a background thread while the game runs (server "host:port" or NULL for
 * PD_NET_PEER direct; Host makes a fresh room code), then both games restart at the title screen together. */
enum { PDNET_ONLINE_IDLE, PDNET_ONLINE_CONNECTING, PDNET_ONLINE_CONNECTED, PDNET_ONLINE_FAILED, PDNET_ONLINE_ACTIVE };
void pdnetOnlineHost(const char *server, const char *password);
void pdnetOnlineJoin(const char *server, const char *password, const char *room);
void pdnetOnlineCancel(void);
void pdnetOnlineDismiss(void);   /* after a failure: back to idle */
s32 pdnetOnlineState(void);
const char *pdnetOnlineRoom(void);
s32 pdnetOnlineIsHost(void);
const char *pdnetOnlineError(void);
/* game thread: 1 = connected, leave the current stage now (every frame) */
s32 pdnetOnlinePoll(void);
/* game thread, top of the main loop: 1 = the session starts now (boot state restored; go to the title) */
s32 pdnetOnlineBegin(void);
/* A host app (i64) owns the connection and starts the game fresh on every machine for each online run. */
struct pdnetexternal {
	s32 localplayer;            /* 0 = the session's host (player 1), 1 = guest */
	u64 seed;                   /* the same on every machine */
	const void *blob;           /* the host's pdnetSessionBlob, delivered reliably by the host app */
	u32 blobsize;
	int (*send)(const void *data, u32 size);  /* game channel: unreliable datagrams */
	int (*recv)(void *buf, u32 cap);          /* one datagram: size, 0 if none */
	s32 hostpaced;              /* the host app calls the frames at 60 Hz: no own pacing */
};
enum { PDNET_EXT_PEER_AWAY = 1, PDNET_EXT_PEER_BACK = 2, PDNET_EXT_PEER_LEFT = 3 }; /* = n64_net_event */
/* host of the session, before every run: what the guests adopt (save image, game-wide settings); 0 = too big */
u32 pdnetSessionBlob(void *out, u32 cap);
/* at game start instead of the boot-time connection; 0, or -1 (pdnetOnlineError() says why) */
s32 pdnetInitExternal(const struct pdnetexternal *x);
/* the host app's session events (once per frame) */
void pdnetExternalEvent(s32 event);
s32 pdnetHostPaced(void);
/* tells the other player this machine is quitting (they play on offline) */
void pdnetShutdown(void);
/* after each simulated tick, with the deterministic state hash */
void pdnetEndTick(s32 tick, u64 hash);

#endif
