#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ultra64.h>
#include "platform.h"
#include "system.h"
#include "constants.h"
#include "types.h"
#include "pdnet.h"
#include "rollback.h"
#include "snapshot.h"
#include "replaycheck.h"
#include "n64net_input.h"
#include "n64net_rendezvous.h"
#include "pdnet_build_id.h"
#include <stdbool.h>
#include <SDL.h>
#include <pthread.h>
#include "config.h"

/* See pdnet.h. Lockstep (rollback window 0) through n64net_input. */

#define PDNET_HS_MAGIC 0x50444e48u /* 'PDNH' handshake */
#define PDNET_HELLO 1
#define PDNET_WELCOME 2
#define PDNET_MISMATCH 3
#define PDNET_BYE 4 /* the sender is quitting */
#define PDNET_SAVE_MAGIC 0x5044534bu /* 'PDSK' host's save image, in chunks */
#define PDNET_SAVE_REQ 1
#define PDNET_SAVE_DATA 2
#define PDNET_SAVE_CHUNK 1024
#define PDNET_SET_MAGIC 0x50445354u /* 'PDST' gameplay settings, both directions */
#define PDNET_SET_REQ 1
#define PDNET_SET_DATA 2

void osEepromGetImage(u8 *out);
void osEepromUseImage(const u8 *data);
u32 osEepromImageSize(void);
#define PDNET_SYNC_EVERY 30
#define PDNET_STATS_EVERY 600

static s32 s_Active = 0;
static s32 s_Session = 0;            // PD_NET_SESSION=1: the whole game online from boot (menus included)
static u8 *s_SaveImage = NULL;       // host: its save image; guest: the one being received
static u32 s_SaveSize = 0;
static u32 s_SaveChunksHave = 0;     // guest: bit per received chunk

/* Gameplay settings from pd.ini that change the simulation. Each player brings their own per-player settings
 * (their Player1 section: FOV, crosshair sway/edge, crouch mode, PC controls...) for their slot (host = player 1,
 * guest = player 2); the host's game-wide ones (MaxExplosions) apply on both. Game.MemorySize can't change after
 * boot, so it is part of the handshake hash instead. The local values come back before pd.ini is saved. */
struct pdnetsettings {
	struct extplayerconfig player;
	s32 maxexplosions;
	u32 seedlo, seedhi; // host: the session's RNG seed (sessions started mid-game)
};
static u64 s_SessionSeed = 0;
extern struct extplayerconfig g_PlayerExtCfg[MAX_PLAYERS];
extern s32 g_MaxExplosions;
extern s32 g_OsMemSizeMb;
static struct pdnetsettings s_PeerSettings;
static s32 s_HavePeerSettings = 0;
static struct extplayerconfig s_LocalExtCfg[MAX_PLAYERS]; // restored before pd.ini is written
static s32 s_LocalMaxExplosions = 0;
static s32 s_SettingsApplied = 0;
s32 g_NetFullViews = 0;   // see src/game/player.c VIEWPLAYERCOUNT and lv.c
s32 g_NetLocalPlayer = 0;
static s32 s_IsHost = 0;
static s32 s_Sock = -1;
static struct sockaddr_in s_Peer;
static s32 s_HavePeer = 0;
static n64net_input *s_Net = NULL;
static u64 s_ConfigHash = 0;
static u32 s_BotSeed = 0;
static s32 s_WaitSecs = 60;
static volatile s32 s_HandshakeMismatch = 0;

/* The other player gone (quit = BYE, or silent for PD_NET_DROP_SECS, 20): the session ends and this machine plays on offline with its own player (the other one stands
 * still). The guest still never saves. */
static s32 s_Offline = 0;
static s32 s_PeerLeft = 0;
static s32 s_DropSecs = 20;
static s32 s_AwaySent = 0;
extern bool gfx_sdl_app_inactive; // gfx_sdl2.cpp: the iOS app is in the background

static s32 s_LastTick = -1;
static OSContPad s_TickPads[2];
static u64 s_StallUsecs = 0;
static u32 s_Stalls = 0;
static s32 s_DesyncReported = 0;

// rollback (PD_NET_ROLLBACK=1, the default; 0 = lockstep)
#define PDNET_SLOTS 16
#define PDNET_HASH_RING 64
static s32 s_Rollback = 0;
static u32 s_RollbackWindow = 8;
static s32 s_RollbackFrom = 60;     // ticks before this run in lockstep (match start)
static s32 s_WindowOpen = 0;
static u64 s_TickHash[PDNET_HASH_RING]; // state hash before tick t (refreshed by every resimulation)
static s32 s_TickHashValid[PDNET_HASH_RING];
static s32 s_LastSyncRecorded = -1;
static u32 s_Rollbacks = 0, s_RollbackTicks = 0, s_MaxDepth = 0, s_Skips = 0;

static const char *envStr(const char *name)
{
	const char *s = getenv(name);
	return (s && *s) ? s : NULL;
}

static u64 nowUsecs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000ull + (u64)ts.tv_nsec / 1000ull;
}

static u64 fnv(u64 h, const void *data, size_t len)
{
	const u8 *p = data;
	for (size_t i = 0; i < len; i++) {
		h = (h ^ p[i]) * 0x100000001b3ull;
	}
	return h;
}

static void netLog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	size_t n = strlen(buf);
	while (n && buf[n - 1] == '\n') {
		buf[--n] = '\0';
	}
	sysLogPrintf(LOG_NOTE, "%s", buf);
}

/* Test aid: simulated bad link on our outgoing packets (PD_NET_SIM_LATENCY / _JITTER in ms, _LOSS in %). */
#define SIM_QUEUE 1024
struct simpacket {
	u64 at;
	u32 size;
	u8 data[512];
};
static struct simpacket *s_SimQueue = NULL;
static s32 s_SimHead = 0, s_SimCount = 0;
static u32 s_SimLatencyMs = 0, s_SimJitterMs = 0, s_SimLossPct = 0;
static u32 s_SimRand = 0x12345678;

static u32 simRand(void)
{
	s_SimRand = s_SimRand * 1103515245u + 12345u;
	return s_SimRand >> 8;
}

/* A host app (i64) owns the connection: datagrams go through its functions (pdnetInitExternal) */
static int (*s_ExtSend)(const void *data, uint32_t size) = NULL;
static int (*s_ExtRecv)(void *buf, uint32_t cap) = NULL;
static s32 s_ExtPeerAway = 0;
static s32 s_HostPaced = 0;

static void rawSend(const uint8_t *data, uint32_t size)
{
	if (s_ExtSend) {
		s_ExtSend(data, size);
		return;
	}
	if (s_Sock >= 0 && s_HavePeer) {
		sendto(s_Sock, data, size, 0, (struct sockaddr *)&s_Peer, sizeof(s_Peer));
	}
}

static void simFlush(void)
{
	const u64 now = nowUsecs();
	// the queue is in send order; jitter may make a later packet due earlier: deliver every due packet
	for (s32 i = 0; i < s_SimCount;) {
		struct simpacket *p = &s_SimQueue[(s_SimHead + i) % SIM_QUEUE];
		if (p->at <= now) {
			rawSend(p->data, p->size);
			// remove by moving the head entry into this place
			*p = s_SimQueue[s_SimHead];
			s_SimHead = (s_SimHead + 1) % SIM_QUEUE;
			s_SimCount--;
		} else {
			i++;
		}
	}
}

static void netSend(void *user, const uint8_t *data, uint32_t size)
{
	(void)user;
	if (!s_SimLatencyMs && !s_SimJitterMs && !s_SimLossPct) {
		rawSend(data, size);
		return;
	}
	if (s_SimLossPct && simRand() % 100 < s_SimLossPct) {
		return;
	}
	if (s_SimCount >= SIM_QUEUE || size > sizeof(s_SimQueue[0].data)) {
		rawSend(data, size);
		return;
	}
	struct simpacket *p = &s_SimQueue[(s_SimHead + s_SimCount) % SIM_QUEUE];
	p->at = nowUsecs() + (u64)s_SimLatencyMs * 1000 + (s_SimJitterMs ? (u64)(simRand() % (s_SimJitterMs * 1000)) : 0);
	p->size = size;
	memcpy(p->data, data, size);
	s_SimCount++;
}

static void sendHandshake(u8 type)
{
	u8 buf[16];
	const u32 magic = htonl(PDNET_HS_MAGIC);
	memcpy(buf, &magic, 4);
	buf[4] = type;
	memcpy(buf + 5, &s_ConfigHash, 8);
	netSend(NULL, buf, 13);
}

/* Reads every waiting datagram. Returns 1 once the other machine is known to be in the match. */
static void sendSettings(u8 kind)
{
	u8 out[8 + sizeof(struct pdnetsettings)];
	memset(out, 0, sizeof(out));
	const u32 m = htonl(PDNET_SET_MAGIC);
	memcpy(out, &m, 4);
	out[4] = kind;
	u32 len = 8;
	if (kind == PDNET_SET_DATA) {
		struct pdnetsettings mine;
		memset(&mine, 0, sizeof(mine));
		mine.player = s_LocalExtCfg[0]; // this machine's own player = its Player1 section
		mine.maxexplosions = s_LocalMaxExplosions;
		mine.seedlo = (u32)s_SessionSeed;
		mine.seedhi = (u32)(s_SessionSeed >> 32);
		memcpy(out + 8, &mine, sizeof(mine));
		len += sizeof(mine);
	}
	netSend(NULL, out, len);
}

static s32 peerGone(void)
{
	if (s_PeerLeft) {
		return 1;
	}
	if (n64net_input_peer_away(s_Net) || s_ExtPeerAway) {
		return 0; // its app is in the background: wait for it as long as it takes (it says when it's back)
	}
	return n64net_input_peer_silence_usecs(s_Net) > (u64)s_DropSecs * 1000000ull;
}

static void goOffline(void)
{
	if (s_Offline) {
		return;
	}
	s_Offline = 1;
	s_WindowOpen = 0;
	const char *why = s_PeerLeft ? "The other player left the game." : "Lost the connection to the other player.";
	sysLogPrintf(LOG_WARNING, "netplay: %s Playing on offline.", why);
	char msg[256];
	snprintf(msg, sizeof(msg), "%s\n\nThe online game is over; you can keep playing on your own.%s", why,
		s_Session && !s_IsHost ? "\nProgress isn't saved (you were playing on the host's save)." : "");
	if (!s_BotSeed) { // scripted test runs show no dialog
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Perfect Dark", msg, NULL);
	}
}

/* offline after a disconnect: this machine's controller drives its own player, the other player stands still */
static void offlinePads(const OSContPad *local)
{
	const s32 me = s_IsHost ? 0 : 1;
	s_TickPads[me] = *local;
	memset(&s_TickPads[1 - me], 0, sizeof(s_TickPads[0]));
}

static s32 pollSocket(void)
{
	s32 started = 0;
	u8 buf[1500];

	if (s_SimCount) {
		simFlush();
	}

	for (;;) {
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		memset(&from, 0, sizeof(from));
		const ssize_t n = s_ExtRecv ? (ssize_t)s_ExtRecv(buf, sizeof(buf))
			: recvfrom(s_Sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
		if (n <= 0) {
			break;
		}
		u32 magic;
		if (n >= 6 && (memcpy(&magic, buf, 4), ntohl(magic) == PDNET_SAVE_MAGIC)) {
			const u8 kind = buf[4], chunk = buf[5];
			const u32 off = (u32)chunk * PDNET_SAVE_CHUNK;
			if (s_IsHost && kind == PDNET_SAVE_REQ && s_SaveImage && off < s_SaveSize) {
				u8 out[8 + PDNET_SAVE_CHUNK];
				const u32 len = (s_SaveSize - off) < PDNET_SAVE_CHUNK ? (s_SaveSize - off) : PDNET_SAVE_CHUNK;
				const u32 m = htonl(PDNET_SAVE_MAGIC);
				memcpy(out, &m, 4);
				out[4] = PDNET_SAVE_DATA;
				out[5] = chunk;
				memcpy(out + 8, s_SaveImage + off, len);
				netSend(NULL, out, 8 + len);
			} else if (!s_IsHost && kind == PDNET_SAVE_DATA && s_SaveImage && off < s_SaveSize && n >= 8) {
				const u32 len = (u32)n - 8;
				if (off + len <= s_SaveSize) {
					memcpy(s_SaveImage + off, buf + 8, len);
					s_SaveChunksHave |= 1u << chunk;
				}
			}
			continue;
		}
		if (n >= 8 && (memcpy(&magic, buf, 4), ntohl(magic) == PDNET_SET_MAGIC)) {
			if (buf[4] == PDNET_SET_REQ) {
				sendSettings(PDNET_SET_DATA);
			} else if (buf[4] == PDNET_SET_DATA && n >= 8 + (ssize_t)sizeof(struct pdnetsettings)) {
				memcpy(&s_PeerSettings, buf + 8, sizeof(s_PeerSettings));
				s_HavePeerSettings = 1;
			}
			continue;
		}
		if (n >= 13 && (memcpy(&magic, buf, 4), ntohl(magic) == PDNET_HS_MAGIC)) {
			u64 hash;
			memcpy(&hash, buf + 5, 8);
			if (buf[4] == PDNET_BYE) {
				if (s_Active && !s_PeerLeft) {
					sysLogPrintf(LOG_NOTE, "netplay: the other player quit");
				}
				s_PeerLeft = 1;
			} else if (s_IsHost && buf[4] == PDNET_HELLO) {
				s_Peer = from; // the guest's address as the host sees it
				s_HavePeer = 1;
				if (hash != s_ConfigHash) {
					sendHandshake(PDNET_MISMATCH);
					sysLogPrintf(LOG_WARNING, "netplay: the guest's match settings or build differ from ours");
					continue;
				}
				sendHandshake(PDNET_WELCOME);
				started = 1;
			} else if (!s_IsHost && buf[4] == PDNET_WELCOME) {
				started = 1;
			} else if (!s_IsHost && buf[4] == PDNET_MISMATCH) {
				s_HandshakeMismatch = 1;
			}
			continue;
		}
		if (s_Net && n64net_input_handle_packet(s_Net, buf, (uint32_t)n)) {
			started = 1; // input packets from the peer: it is past the handshake
		}
	}

	return started;
}

/* Settings file for machines without environment variables (iPhone: Documents/netplay.cfg; elsewhere
 * PD_NET_CONFIG=<file>): one KEY=VALUE per line (PD_* keys only, '#' comments), applied as environment
 * variables unless the environment already sets them. */
static void loadConfigFile(void)
{
	static s32 loaded = 0;
	char path[1024];
	char line[512];

	if (loaded) {
		return;
	}
	loaded = 1;

	if (envStr("PD_NET_CONFIG")) {
		snprintf(path, sizeof(path), "%s", envStr("PD_NET_CONFIG"));
	} else {
#ifdef PD_IOS
		snprintf(path, sizeof(path), "%s/Documents/netplay.cfg", getenv("HOME") ? getenv("HOME") : ".");
#else
		return;
#endif
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	s32 n = 0;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || strncmp(p, "PD_", 3) != 0) {
			continue;
		}
		char *eq = strchr(p, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *val = eq + 1;
		size_t len = strlen(val);
		while (len && (val[len - 1] == '\n' || val[len - 1] == '\r' || val[len - 1] == ' ')) {
			val[--len] = '\0';
		}
		setenv(p, val, 0);
		n++;
	}
	fclose(f);
	sysLogPrintf(LOG_NOTE, "netplay: %d settings from %s", n, path);
}

extern struct pdnetexternal *g_PdNetExternal;

s32 pdnetWanted(void)
{
	if (g_PdNetExternal) {
		return 1;   /* i64 online run: fixed step from tick 0 */
	}
	loadConfigFile();
	if (envStr("PD_NET_LATE")) {
		return 0; // test aid: connect later, from the running game (pdnetOnlinePoll)
	}
	// a server alone (no host flag, no room) is just the Play Online menu's default: no connection at boot
	return envStr("PD_NET_HOST") || envStr("PD_NET_PEER") || (envStr("PD_NET_SERVER") && envStr("PD_NET_ROOM"));
}

static void rendezvousYield(void)
{
	usleep(1000);
}

s32 pdnetActive(void)
{
	return s_Active;
}

s32 pdnetMatchMode(void)
{
	return s_Active && !s_Session;
}

/* Reads the PD_NET_* settings that don't depend on how the session starts. */
static void readEnv(void)
{
	const char *s;
	if ((s = envStr("PD_NET_BOT"))) {
		s_BotSeed = (u32)strtoul(s, NULL, 0) | 1;
	}
	if ((s = envStr("PD_NET_WAIT_SECS"))) {
		s_WaitSecs = atoi(s);
	}
	if ((s = envStr("PD_NET_DROP_SECS"))) {
		s_DropSecs = atoi(s) > 0 ? atoi(s) : 20;
	}
	if ((s = envStr("PD_NET_SIM_LATENCY"))) s_SimLatencyMs = (u32)atoi(s);
	if ((s = envStr("PD_NET_SIM_JITTER"))) s_SimJitterMs = (u32)atoi(s);
	if ((s = envStr("PD_NET_SIM_LOSS"))) s_SimLossPct = (u32)atoi(s);
	s_Rollback = !(envStr("PD_NET_ROLLBACK") && atoi(envStr("PD_NET_ROLLBACK")) == 0) && snapshotSupported();
	if ((s = envStr("PD_NET_ROLLBACK_WINDOW"))) {
		s_RollbackWindow = (u32)atoi(s);
	}
	if ((s = envStr("PD_NET_ROLLBACK_FROM"))) {
		s_RollbackFrom = atoi(s);
	}
}

static char s_ConnError[256];

#define CONN_FAIL(...) do { snprintf(s_ConnError, sizeof(s_ConnError), __VA_ARGS__); goto fail; } while (0)

/* Socket, room server or direct address, handshake, the host's save (sessions) and both players' settings.
 * Runs on the game thread at boot or on the online thread (pdnetOnlineHost/Join) while the game keeps running;
 * touches no game state. Returns 0, or -1 with s_ConnError set (socket closed). `cancel` may stop the wait. */
static s32 connectPeer(const char *server, const char *room, const char *password, s32 port, u64 hash,
	const volatile int *cancel)
{
	const char *s;
	s_ConnError[0] = '\0';
	s_HavePeerSettings = 0;
	s_SaveChunksHave = 0;
	s_PeerLeft = 0;

	if (!s_IsHost && !server) {
		char host[128];
		s32 peerport = 7301;
		const char *peer = envStr("PD_NET_PEER");
		const char *colon = peer ? strrchr(peer, ':') : NULL;
		if (!peer) {
			CONN_FAIL("Netplay: the guest needs PD_NET_PEER=host:port");
		}
		snprintf(host, sizeof(host), "%.*s", colon ? (int)(colon - peer) : (int)strlen(peer), peer);
		if (colon) {
			peerport = atoi(colon + 1);
		}
		memset(&s_Peer, 0, sizeof(s_Peer));
		s_Peer.sin_family = AF_INET;
		s_Peer.sin_port = htons((u16)peerport);
		if (inet_pton(AF_INET, host, &s_Peer.sin_addr) != 1) {
			CONN_FAIL("Netplay: PD_NET_PEER must be an IPv4 address:port, got `%s`", peer);
		}
		s_HavePeer = 1;
	}

	s_Sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (s_Sock < 0) {
		CONN_FAIL("Netplay: socket() failed: %s", strerror(errno));
	}
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons((u16)port);
	if (bind(s_Sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		CONN_FAIL("Netplay: can't use UDP port %d: %s", port, strerror(errno));
	}
	fcntl(s_Sock, F_SETFL, fcntl(s_Sock, F_GETFL, 0) | O_NONBLOCK);

	if (server) {
		// room codes: the room server pairs us with the other player (hole punching, relay as a fallback)
		char game[96];
		snprintf(game, sizeof(game), "pd-%s+%s", VERSION_ROMID, PDNET_BUILD_ID);
		N64NetRendezvousConfig rc;
		memset(&rc, 0, sizeof(rc));
		rc.server = server;
		rc.game = game;
		rc.room = room;
		rc.password = password;
		rc.invite_pass = envStr("PD_NET_INVITE");
		rc.is_host = s_IsHost;
		rc.force_relay = envStr("PD_NET_RELAY") && atoi(envStr("PD_NET_RELAY")) != 0;
		rc.wait_secs = envStr("PD_NET_WAIT_SECS") ? atoi(envStr("PD_NET_WAIT_SECS")) : 600;
		rc.log = netLog;
		rc.yield = rendezvousYield;
		rc.cancel = cancel;
		N64NetRendezvousResult res;
		memset(&res, 0, sizeof(res));
		sysLogPrintf(LOG_NOTE, "netplay: %s room %s on %s", s_IsHost ? "hosting" : "joining",
			room ? room : "(new)", server);
		if (n64net_rendezvous_connect(s_Sock, &rc, &res) != 0) {
			if (!strncmp(res.error, "version", 7)) {
				CONN_FAIL("The other player has a different build of the game. Both players need the same version.");
			} else if (!strcmp(res.error, "cancelled")) {
				CONN_FAIL("Cancelled.");
			} else if (!strcmp(res.error, "noroom")) {
				CONN_FAIL("Room %s isn't open. Check the code with the host.", room ? room : "");
			} else if (!strcmp(res.error, "auth")) {
				CONN_FAIL("The room server refused the password.");
			} else if (!strcmp(res.error, "timeout")) {
				CONN_FAIL("Nobody joined in time.");
			}
			CONN_FAIL("Couldn't connect through the room server (%s).", res.error);
		}
		s_Peer = res.peer;
		s_HavePeer = 1;
		sysLogPrintf(LOG_NOTE, "netplay: room %s, %s connection", res.room, res.relayed ? "relayed" : "direct");
	}

	sysLogPrintf(LOG_NOTE, "netplay: build %s", PDNET_BUILD_ID);
	// this machine's own settings (sent to the other player; restored before pd.ini is saved)
	memcpy(s_LocalExtCfg, g_PlayerExtCfg, sizeof(s_LocalExtCfg));
	s_LocalMaxExplosions = g_MaxExplosions;
	s_ConfigHash = hash;

	n64net_input_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.num_players = 2;
	cfg.local_player = s_IsHost ? 0 : 1;
	cfg.session_id = (u32)(hash ^ (hash >> 32));
	cfg.start_tick = 0;
	cfg.input_delay = 2;
	if ((s = envStr("PD_NET_DELAY"))) {
		cfg.input_delay = (u32)atoi(s);
	}
	cfg.rollback_window = 0; // lockstep until s_RollbackFrom (match start), then s_RollbackWindow
	cfg.ticks_per_second = 60;
	cfg.send = netSend;
	cfg.log = netLog;
	s_Net = n64net_input_create(&cfg);
	// rollback hides most latency: keep the delay low; lockstep needs more to avoid stalls
	n64net_input_set_auto_delay(s_Net, cfg.input_delay, s_Rollback ? 3 : 6);

	sysLogPrintf(LOG_NOTE, "netplay: %s on UDP %d, waiting up to %d s for the %s", s_IsHost ? "host" : "guest", port,
		s_WaitSecs, s_IsHost ? "guest" : "host");

	// handshake: the guest says HELLO until the host answers; the host answers every HELLO
	{
		const u64 deadline = nowUsecs() + (u64)s_WaitSecs * 1000000ull;
		u64 nextHello = 0;
		s_HandshakeMismatch = 0;
		for (;;) {
			if (pollSocket()) {
				break;
			}
			if (s_HandshakeMismatch) {
				CONN_FAIL("Netplay: the host's match settings or game build differ from ours.\n"
					"Both players need the same build, PD_SEED, stage, PD_REPLAY_BOTS, PD_ASPECT and Game.MemorySize.");
			}
			if (cancel && *cancel) {
				CONN_FAIL("Cancelled.");
			}
			const u64 now = nowUsecs();
			if (now > deadline) {
				CONN_FAIL("Netplay: the other player didn't connect within %d seconds.", s_WaitSecs);
			}
			if (!s_IsHost && now >= nextHello) {
				sendHandshake(PDNET_HELLO);
				nextHello = now + 100000;
			}
			usleep(1000);
		}
	}

	if (s_Session) {
		// both machines must start from the same save: the guest plays on the host's image (never writing it)
		s_SaveSize = osEepromImageSize();
		free(s_SaveImage);
		s_SaveImage = sysMemZeroAlloc(s_SaveSize);
		const u32 chunks = (s_SaveSize + PDNET_SAVE_CHUNK - 1) / PDNET_SAVE_CHUNK;
		if (s_IsHost) {
			osEepromGetImage(s_SaveImage); // served to the guest from pollSocket
		} else {
			const u64 deadline2 = nowUsecs() + (u64)s_WaitSecs * 1000000ull;
			u64 nextReq = 0;
			while (s_SaveChunksHave != (1u << chunks) - 1) {
				pollSocket();
				const u64 now = nowUsecs();
				if (now > deadline2) {
					CONN_FAIL("Netplay: didn't receive the host's save data.");
				}
				if (now >= nextReq) {
					for (u32 c = 0; c < chunks; c++) {
						if (!(s_SaveChunksHave & (1u << c))) {
							u8 req[8] = { 0 };
							const u32 m = htonl(PDNET_SAVE_MAGIC);
							memcpy(req, &m, 4);
							req[4] = PDNET_SAVE_REQ;
							req[5] = (u8)c;
							netSend(NULL, req, 8);
						}
					}
					nextReq = now + 50000;
				}
				usleep(1000);
			}
		}
	}

	// gameplay settings: each side asks for the other's until it has them (pollSocket answers requests,
	// during the game too)
	{
		const u64 deadline3 = nowUsecs() + (u64)s_WaitSecs * 1000000ull;
		u64 nextReq = 0;
		while (!s_HavePeerSettings) {
			pollSocket();
			const u64 now = nowUsecs();
			if (now > deadline3) {
				CONN_FAIL("Netplay: didn't receive the other player's settings.");
			}
			if (now >= nextReq) {
				sendSettings(PDNET_SET_REQ);
				nextReq = now + 50000;
			}
			usleep(1000);
		}
	}
	return 0;

fail:
	sysLogPrintf(LOG_WARNING, "netplay: %s", s_ConnError);
	if (s_Net) {
		n64net_input_destroy(s_Net);
		s_Net = NULL;
	}
	if (s_Sock >= 0) {
		close(s_Sock);
		s_Sock = -1;
	}
	s_HavePeer = 0;
	return -1;
}

/* Game thread, once connected: the session's shared settings take effect. */
static void activate(void)
{
	if (s_Session && !s_IsHost) {
		osEepromUseImage(s_SaveImage);
		sysLogPrintf(LOG_NOTE, "netplay: using the host's save data (%u bytes, not written here)", s_SaveSize);
	}
	const struct extplayerconfig *hostcfg = s_IsHost ? &s_LocalExtCfg[0] : &s_PeerSettings.player;
	const struct extplayerconfig *guestcfg = s_IsHost ? &s_PeerSettings.player : &s_LocalExtCfg[0];
	g_PlayerExtCfg[0] = *hostcfg;
	g_PlayerExtCfg[1] = *guestcfg;
	g_MaxExplosions = s_IsHost ? s_LocalMaxExplosions : s_PeerSettings.maxexplosions;
	s_SettingsApplied = 1;
	sysLogPrintf(LOG_NOTE, "netplay: settings: player 1 fov %.0f, player 2 fov %.0f, max explosions %d",
		g_PlayerExtCfg[0].fovy, g_PlayerExtCfg[1].fovy, g_MaxExplosions);

	if (s_Rollback) {
		snapshotInit(PDNET_SLOTS);
		rollbackEnableRoomCache(); // before the stage loads its rooms
	}
	s_Offline = 0;
	s_LastTick = -1;
	s_Active = 1;
	g_NetLocalPlayer = s_IsHost ? 0 : 1;
	// one full-screen view per machine (PD_NET_VIEW=split = classic split screen on both)
	g_NetFullViews = !(envStr("PD_NET_VIEW") && strcmp(envStr("PD_NET_VIEW"), "split") == 0);
	sysLogPrintf(LOG_NOTE, "netplay: connected as player %d (%s, input delay %u)", s_IsHost ? 1 : 2,
		s_Rollback ? "rollback" : "lockstep", n64net_input_get_delay(s_Net));
}

static u64 configHash(u64 seed, s32 stage, s32 bots, float aspect)
{
	// everything that must be equal on both machines
	u64 h = 0xcbf29ce484222325ull;
	h = fnv(h, &g_OsMemSizeMb, sizeof(g_OsMemSizeMb)); // the memp heap size (set at boot)
	h = fnv(h, &seed, sizeof(seed));
	h = fnv(h, &stage, sizeof(stage));
	h = fnv(h, &bots, sizeof(bots));
	h = fnv(h, &aspect, sizeof(aspect));
	h = fnv(h, PDNET_BUILD_ID, strlen(PDNET_BUILD_ID));
	return h;
}

/* test aid PD_NET_EXTERNAL_TEST=1: stands in for i64 (pdnetInitExternal): a plain UDP socket as the "host app's"
 * game channel (host 7301, guest 7302 -> PD_NET_PEER) and the session blob through a file
 * (PD_NET_EXTERNAL_BLOB: the host writes it, the guest waits for it) */
static int s_TestSock = -1;
static struct sockaddr_in s_TestPeer;
static s32 s_TestHavePeer = 0;

static int testSend(const void *data, u32 size)
{
	return s_TestHavePeer ? (int)sendto(s_TestSock, data, size, 0, (struct sockaddr *)&s_TestPeer, sizeof(s_TestPeer)) : -1;
}

static int testRecv(void *buf, u32 cap)
{
	struct sockaddr_in from;
	socklen_t len = sizeof(from);
	const ssize_t n = recvfrom(s_TestSock, buf, cap, 0, (struct sockaddr *)&from, &len);
	if (n > 0 && !s_TestHavePeer) {
		s_TestPeer = from;
		s_TestHavePeer = 1;
	}
	return n > 0 ? (int)n : 0;
}

static void externalTest(u64 seed)
{
	const s32 host = envStr("PD_NET_HOST") && atoi(envStr("PD_NET_HOST")) != 0;
	const char *blobpath = envStr("PD_NET_EXTERNAL_BLOB");
	s_TestSock = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons(host ? 7301 : 7302);
	bind(s_TestSock, (struct sockaddr *)&local, sizeof(local));
	fcntl(s_TestSock, F_SETFL, fcntl(s_TestSock, F_GETFL, 0) | O_NONBLOCK);
	if (!host) {
		memset(&s_TestPeer, 0, sizeof(s_TestPeer));
		s_TestPeer.sin_family = AF_INET;
		s_TestPeer.sin_port = htons(7301);
		inet_pton(AF_INET, "127.0.0.1", &s_TestPeer.sin_addr);
		s_TestHavePeer = 1;
	}
	static u8 blob[64 * 1024];
	u32 blobsize = 0;
	if (host) {
		blobsize = pdnetSessionBlob(blob, sizeof(blob));
		FILE *f = blobpath ? fopen(blobpath, "wb") : NULL;
		if (f) { fwrite(blob, 1, blobsize, f); fclose(f); }
	} else {
		for (s32 i = 0; i < 200 && !blobsize; i++) {
			FILE *f = blobpath ? fopen(blobpath, "rb") : NULL;
			if (f) { blobsize = (u32)fread(blob, 1, sizeof(blob), f); fclose(f); }
			if (!blobsize) usleep(50000);
		}
	}
	// the guest says hello first so the host learns its address (i64 knows it already)
	if (!host) {
		for (s32 i = 0; i < 3; i++) testSend("hi", 2);
	}
	struct pdnetexternal x;
	memset(&x, 0, sizeof(x));
	x.localplayer = host ? 0 : 1;
	x.seed = seed;
	x.blob = blob;
	x.blobsize = blobsize;
	x.send = testSend;
	x.recv = testRecv;
	if (pdnetInitExternal(&x) != 0) {
		sysFatalError("%s", s_ConnError);
	}
}

void pdnetInit(u64 seed, s32 stage, s32 bots, float aspect)
{
	const char *s;

	if (g_PdNetExternal) {   /* i64: the host app's connection */
		if (pdnetInitExternal(g_PdNetExternal) != 0) {
			sysFatalError("%s", s_ConnError);
		}
		return;
	}

	if (envStr("PD_NET_EXTERNAL_TEST")) {
		externalTest(seed);
		return;
	}
	if (!pdnetWanted()) {
		return;
	}

	readEnv();
	s_IsHost = envStr("PD_NET_HOST") && atoi(envStr("PD_NET_HOST")) != 0;
	const char *server = envStr("PD_NET_SERVER");
	s32 port = server ? 0 : (s_IsHost ? 7301 : 7302); // room codes: any free port
	if ((s = envStr("PD_NET_PORT"))) {
		port = atoi(s);
	}
	if (s_SimLatencyMs || s_SimJitterMs || s_SimLossPct) {
		s_SimQueue = sysMemZeroAlloc((u32)(SIM_QUEUE * sizeof(*s_SimQueue)));
		s_SimRand ^= (u32)port;
		sysLogPrintf(LOG_NOTE, "netplay: simulating %u ms + %u ms jitter, %u%% loss on sent packets", s_SimLatencyMs,
			s_SimJitterMs, s_SimLossPct);
	}
	s_Session = envStr("PD_NET_SESSION") && atoi(envStr("PD_NET_SESSION")) != 0;

	if (connectPeer(server, envStr("PD_NET_ROOM"), envStr("PD_NET_PASSWORD"), port,
			configHash(seed, stage, bots, aspect), NULL) != 0) {
		sysFatalError("%s", s_ConnError);
	}
	activate();
}

/* PD_NET_BOT / PD_TEST_BOT: wanders, turns, aims with the right stick and fires in bursts (deterministic per tick). */
void pdnetBotPad(s32 tick, u32 seed, OSContPad *pad)
{
	u32 x = seed ^ ((u32)(tick / 20) * 2654435761u);
	x ^= x >> 13;
	x *= 0x5bd1e995u;
	x ^= x >> 15;
	memset(pad, 0, sizeof(*pad));
	pad->stick_x = (s8)((s32)(x % 121) - 60);
	pad->stick_y = (s8)((s32)((x >> 8) % 121) - 40);
	pad->rstick_x = (s8)((s32)((x >> 16) % 81) - 40);
	pad->rstick_y = (s8)((s32)((x >> 4) % 41) - 20);
	if ((x >> 24) & 1) {
		pad->button |= Z_TRIG; // fire
	}
	if (((x >> 25) & 7) == 0) {
		pad->button |= B_BUTTON; // reload / use
	}
}

static n64net_pad toNet(const OSContPad *pad)
{
	n64net_pad n;
	memset(&n, 0, sizeof(n));
	n.buttons = (u16)(pad->button & 0xffff);
	n.buttons_hi = (u16)(pad->button >> 16);
	n.stick_x = pad->stick_x;
	n.stick_y = pad->stick_y;
	n.rstick_x = pad->rstick_x;
	n.rstick_y = pad->rstick_y;
	return n;
}

static void fromNet(const n64net_pad *n, OSContPad *pad)
{
	memset(pad, 0, sizeof(*pad));
	pad->button = (u32)n->buttons | ((u32)n->buttons_hi << 16);
	pad->stick_x = n->stick_x;
	pad->stick_y = n->stick_y;
	pad->rstick_x = n->rstick_x;
	pad->rstick_y = n->rstick_y;
	pad->errnum = 0;
}

static void exchangeTick(s32 tick, const OSContPad *local)
{
	if (s_Offline) {
		offlinePads(local);
		return;
	}
	n64net_input_set_local(s_Net, (u32)tick, toNet(local));

	if (!n64net_input_ready(s_Net, (u32)tick)) {
		const u64 start = nowUsecs();
		u64 lastPeerCheck = start;
		s_Stalls++;
		while (!n64net_input_ready(s_Net, (u32)tick)) {
			pollSocket();
			n64net_input_resend(s_Net);
			const u64 now = nowUsecs();
			if (s_PeerLeft || now - lastPeerCheck > 1000000) {
				lastPeerCheck = now;
				if (peerGone()) {
					s_StallUsecs += now - start;
					goOffline();
					offlinePads(local);
					return;
				}
			}
			usleep(250);
		}
		s_StallUsecs += nowUsecs() - start;
	}

	for (s32 p = 0; p < 2; p++) {
		n64net_pad n;
		n64net_input_get(s_Net, p, (u32)tick, &n);
		fromNet(&n, &s_TickPads[p]);
	}

	// PD_NET_RECORD=<file>: the pads every tick used (lockstep: final), in the replay check's input format,
	// so a session can be replayed offline (PD_REPLAY_PLAY + PD_REPLAY_PADS=2)
	static FILE *rec = NULL;
	static s32 recInit = 0;
	if (!recInit) {
		recInit = 1;
		if (envStr("PD_NET_RECORD")) {
			rec = fopen(envStr("PD_NET_RECORD"), "w");
			if (rec) fprintf(rec, "# pd netplay input: tick then 4 x (buttons sx sy rsx rsy err)\n");
		}
	}
	if (rec) {
		fprintf(rec, "%d", tick);
		for (s32 p = 0; p < 4; p++) {
			const OSContPad *pp = p < 2 ? &s_TickPads[p] : NULL;
			if (pp) {
				fprintf(rec, " %x %d %d %d %d 0", pp->button, pp->stick_x, pp->stick_y, pp->rstick_x, pp->rstick_y);
			} else {
				fprintf(rec, " 0 0 0 0 0 %d", CONT_NO_RESPONSE_ERROR);
			}
		}
		fputc('\n', rec);
		fflush(rec);
	}
}

void pdnetFilterPads(OSContPad *pads, s32 count, s32 tick)
{
	if (!s_Active) {
		return;
	}

	if (g_RollbackResim) {
		// resimulation: the ledger's inputs for that tick (confirmed or the newest prediction)
		for (s32 p = 0; p < 2; p++) {
			n64net_pad n;
			n64net_input_get(s_Net, p, (u32)tick, &n);
			fromNet(&n, &s_TickPads[p]);
		}
	} else {
		pollSocket();
	}

	if (!g_RollbackResim && tick != s_LastTick) {
		// the local player's physical controller is always controller 1
		OSContPad local = pads[0];
		if (s_BotSeed) {
			pdnetBotPad(tick, s_BotSeed, &local);
		}
		// ticks in which the game read no pads still get an input (unchanged), so the ledgers stay in step
		for (s32 t = s_LastTick + 1; t <= tick; t++) {
			exchangeTick(t, &local);
		}
		s_LastTick = tick;
	}

	for (s32 i = 0; i < count; i++) {
		if (i < 2) {
			pads[i] = s_TickPads[i];
		} else {
			memset(&pads[i], 0, sizeof(pads[i]));
			pads[i].errnum = CONT_NO_RESPONSE_ERROR;
		}
	}
}

void pdnetFilterStatus(OSContStatus *status, s32 count)
{
	if (!s_Active) {
		return;
	}

	for (s32 i = 0; i < count; i++) {
		if (i < 2) {
			status[i].errnum = 0;
			status[i].type = CONT_ABSOLUTE;
			status[i].status = CONT_CARD_ON;
		} else {
			status[i].errnum = CONT_NO_RESPONSE_ERROR;
			status[i].type = 0;
			status[i].status = 0;
		}
	}
}

void pdnetEndTick(s32 tick, u64 hash)
{
	if (!s_Active || s_Offline) {
		return;
	}

	pollSocket();
	if (peerGone()) { // rollback runs ahead without waiting: notice a departure here too
		goOffline();
		return;
	}

	// test aid: PD_NET_TEST_DESYNC=tick nudges this side's RNG once (the sync check must notice)
	static s32 desyncAt = -2;
	if (desyncAt == -2) {
		desyncAt = envStr("PD_NET_TEST_DESYNC") ? atoi(envStr("PD_NET_TEST_DESYNC")) : -1;
	}
	if (tick == desyncAt) {
		extern u64 g_RngSeed;
		g_RngSeed ^= 0x9e3779b97f4a7c15ull;
		sysLogPrintf(LOG_WARNING, "netplay: test desync injected at tick %d", tick);
	}

	// lockstep: every simulated tick is confirmed (rollback records confirmed ticks in pdnetRollbackPoint)
	if (!s_Rollback && tick % PDNET_SYNC_EVERY == 0) {
		n64net_sync_record(s_Net, (u32)tick, 0, (u32)(hash ^ (hash >> 32)));
	}

	u32 dtick;
	u8 dkind;
	if (!s_DesyncReported && n64net_sync_desynced(s_Net, &dtick, &dkind)) {
		s_DesyncReported = 1;
		sysLogPrintf(LOG_ERROR, "netplay: DESYNC detected at tick %u", dtick);
	}

	if (tick % PDNET_STATS_EVERY == 0 && tick > 0) {
		n64net_input_stats st;
		n64net_input_get_stats(s_Net, &st);
		sysLogPrintf(LOG_NOTE,
			"netplay: tick %d, sync checks matched %u%s, stalls %u (%.2f s), delay %u, rtt avg %u ms max %u ms, "
			"rollbacks %u (%u ticks, max %u), skips %u, save %.2f ms",
			tick, n64net_sync_matched(s_Net), s_DesyncReported ? " (DESYNC)" : "", s_Stalls,
			(double)s_StallUsecs / 1e6, n64net_input_get_delay(s_Net), st.rtt_avg_usecs / 1000,
			st.rtt_max_usecs / 1000, s_Rollbacks, s_RollbackTicks, s_MaxDepth, s_Skips, snapshotLastSaveMs());
	}
}

/* --- Rollback ------------------------------------------------------------- */

static void cbSave(void *user, uint32_t slot)
{
	(void)user;
	snapshotSave((s32)slot);
}

static void cbLoad(void *user, uint32_t slot)
{
	(void)user;
	snapshotLoad((s32)slot);
	replaySetTick((s32)n64net_rollback_callback_tick(s_Net)); // the state before that tick
}

static void cbAdvance(void *user)
{
	(void)user;
	rollbackStep();
	const s32 t = replayGetTick();
	s_TickHash[t % PDNET_HASH_RING] = replayStateHash();
	s_TickHashValid[t % PDNET_HASH_RING] = t;
}

extern s32 g_StageNum;
extern s32 g_MainChangeToStageNum;
static s32 s_LastStage = -1;
static s32 s_StageStartTick = 0;

/* Rollback can't span a stage load (memp is reset): the window is open only inside a gameplay stage, from
 * s_RollbackFrom ticks after it started; when a stage change is requested every prediction is settled first. */
static s32 rollbackWanted(s32 tick)
{
	if (g_StageNum != s_LastStage) {
		s_LastStage = g_StageNum;
		s_StageStartTick = tick;
	}
	if (g_MainChangeToStageNum >= 0) {
		return 0;
	}
	if (g_StageNum >= STAGE_TITLE) {
		return 0; // title, menus
	}
	return tick >= s_StageStartTick + s_RollbackFrom;
}

static void waitConfirmed(s32 tick)
{
	// all inputs up to tick - 1 confirmed (the ones the current state was built from)
	while (tick > 0 && !s_Offline) {
		const uint32_t c = n64net_input_confirmed_tick(s_Net);
		if (c != UINT32_MAX && (s32)c >= tick - 1) {
			break;
		}
		pollSocket();
		n64net_input_resend(s_Net);
		if (peerGone()) {
			goOffline();
		}
		usleep(250);
	}
}

s32 pdnetRollbackActive(void)
{
	return s_Active && s_Rollback && !s_Offline;
}

void pdnetRollbackPoint(void)
{
	const s32 tick = replayGetTick(); // the state now is "before tick `tick`"

	const s32 want = rollbackWanted(tick);
	s32 closing = 0;
	if (s_WindowOpen && !want) {
		// settle every prediction before the stage changes (the correction below may even cancel the change)
		waitConfirmed(tick);
		closing = 1;
	} else if (!s_WindowOpen && want) {
		n64net_input_set_window(s_Net, s_RollbackWindow);
		s_WindowOpen = 1;
	}

	n64net_rollback_callbacks cb;
	memset(&cb, 0, sizeof(cb));
	cb.save = cbSave;
	cb.load = cbLoad;
	cb.advance = cbAdvance;
	cb.slot_count = PDNET_SLOTS;

	g_RollbackResim = 1;
	const int depth = n64net_rollback_frame(s_Net, (uint32_t)tick, &cb);
	g_RollbackResim = 0;

	if (closing) {
		n64net_input_set_window(s_Net, 0);
		s_WindowOpen = 0;
	}

	if (depth > 0) {
		s_Rollbacks++;
		s_RollbackTicks += (u32)depth;
		if ((u32)depth > s_MaxDepth) s_MaxDepth = (u32)depth;
	} else if (depth < 0) {
		sysLogPrintf(LOG_ERROR, "netplay: rollback needed a state older than the snapshot ring");
	}

	s_TickHash[tick % PDNET_HASH_RING] = replayStateHash();
	s_TickHashValid[tick % PDNET_HASH_RING] = tick;

	// sync checks on confirmed states: the state before tick t is final once inputs up to t - 1 are confirmed
	const uint32_t confirmed = n64net_input_confirmed_tick(s_Net);
	if (confirmed != UINT32_MAX) {
		for (s32 t = s_LastSyncRecorded + 1; t <= (s32)confirmed + 1 && t <= tick; t++) {
			if (t % PDNET_SYNC_EVERY == 0 && s_TickHashValid[t % PDNET_HASH_RING] == t) {
				const u64 h = s_TickHash[t % PDNET_HASH_RING];
				n64net_sync_record(s_Net, (u32)t, 0, (u32)(h ^ (h >> 32)));
			}
			s_LastSyncRecorded = t;
		}
	}
}

s32 pdnetTimeSyncSkip(void)
{
	if (!s_Active || !s_Rollback || !s_WindowOpen || s_Offline) {
		return 0;
	}
	pollSocket();
	if (n64net_timesync_should_skip(s_Net)) {
		s_Skips++;
		return 1;
	}
	return 0;
}

void pdnetRestoreLocalSettings(void)
{
	if (s_SettingsApplied) {
		memcpy(g_PlayerExtCfg, s_LocalExtCfg, sizeof(s_LocalExtCfg));
		g_MaxExplosions = s_LocalMaxExplosions;
		s_SettingsApplied = 0;
	}
}

void pdnetShutdown(void)
{
	if (s_Active && !s_Offline && s_Sock >= 0) {
		for (s32 i = 0; i < 3; i++) {
			sendHandshake(PDNET_BYE);
		}
	}
}

void gfx_sdl_pump_events(void);

s32 pdnetHoldWhileAway(void)
{
	if (!s_Active || s_Offline || g_RollbackResim) {
		return 0;
	}
	if (!gfx_sdl_app_inactive) {
		if (s_AwaySent) {
			s_AwaySent = 0;
			for (s32 i = 0; i < 3; i++) {
				n64net_input_set_away(s_Net, 0);
			}
			sysLogPrintf(LOG_NOTE, "netplay: back");
		}
		return 0;
	}
	if (!s_AwaySent) {
		// the game stops here at once: any input sent after AWAY would tell the other side we're back
		s_AwaySent = 1;
		for (s32 i = 0; i < 3; i++) {
			n64net_input_set_away(s_Net, 1);
		}
		sysLogPrintf(LOG_NOTE, "netplay: away (app in the background), the game waits");
	}
	pollSocket();
	if (s_PeerLeft) {
		goOffline();
		return 0;
	}
	gfx_sdl_pump_events(); // notices the return to the foreground
	usleep(10000);
	return 1;
}

/* --- Online from the menus (sessions started mid-game) ---------------------
 * pdnetOnlineHost/Join connect on a background thread while the game keeps running; once connected the game
 * thread leaves the current stage (pdnetOnlinePoll), then at the top of the main loop (pdnetOnlineBegin) both
 * machines restore the state the game had at the start of its main loop, switch to deterministic mode with the
 * host's seed and start the session at the title screen, like two consoles booted together. */

static pthread_t s_OnlineThread;
static volatile s32 s_OnlineState = PDNET_ONLINE_IDLE;
static volatile int s_OnlineCancel = 0;
static s32 s_OnlineThreadRunning = 0;
static char s_OnlineServer[128], s_OnlinePassword[128], s_OnlineRoom[8];
static s32 s_OnlineLeaving = 0;

static void *onlineThread(void *arg)
{
	(void)arg;
	// sessions started mid-game agree on: the boot state (title), no bots, 16:9, the build and memory size
	const u64 hash = configHash(0, 0x5a /* STAGE_TITLE */, 0, 16.f / 9.f);
	const s32 ok = connectPeer(s_OnlineServer[0] ? s_OnlineServer : NULL, s_OnlineRoom[0] ? s_OnlineRoom : NULL,
		s_OnlinePassword[0] ? s_OnlinePassword : NULL, envStr("PD_NET_PORT") ? atoi(envStr("PD_NET_PORT")) : 0, hash,
		&s_OnlineCancel) == 0;
	s_OnlineState = ok ? PDNET_ONLINE_CONNECTED : PDNET_ONLINE_FAILED;
	return NULL;
}

static void onlineStart(s32 host, const char *server, const char *password, const char *room)
{
	if (s_Active || s_OnlineState == PDNET_ONLINE_CONNECTING || s_OnlineState == PDNET_ONLINE_CONNECTED) {
		return;
	}
	if (s_OnlineThreadRunning) {
		pthread_join(s_OnlineThread, NULL);
		s_OnlineThreadRunning = 0;
	}
	readEnv();
	s_IsHost = host;
	s_Session = 1;
	snprintf(s_OnlineServer, sizeof(s_OnlineServer), "%s", server ? server : "");
	snprintf(s_OnlinePassword, sizeof(s_OnlinePassword), "%s", password ? password : "");
	if (host && (!room || !room[0])) {
		// a fresh code (no 0/O/1/I to misread)
		static const char abc[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		u32 x = (u32)nowUsecs() ^ (u32)(uintptr_t)&x;
		for (s32 i = 0; i < 4; i++) {
			x = x * 1664525u + 1013904223u;
			s_OnlineRoom[i] = abc[(x >> 16) % (sizeof(abc) - 1)];
		}
		s_OnlineRoom[4] = '\0';
	} else {
		snprintf(s_OnlineRoom, sizeof(s_OnlineRoom), "%s", room ? room : "");
		for (char *c = s_OnlineRoom; *c; c++) {
			if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
		}
	}
	s_SessionSeed = host ? (nowUsecs() * 6364136223846793005ull + 1442695040888963407ull) : 0;
	s_OnlineCancel = 0;
	s_OnlineState = PDNET_ONLINE_CONNECTING;
	s_ConnError[0] = '\0';
	if (pthread_create(&s_OnlineThread, NULL, onlineThread, NULL) != 0) {
		snprintf(s_ConnError, sizeof(s_ConnError), "Couldn't start the network thread.");
		s_OnlineState = PDNET_ONLINE_FAILED;
		return;
	}
	s_OnlineThreadRunning = 1;
}

void pdnetOnlineHost(const char *server, const char *password)
{
	onlineStart(1, server, password, NULL);
}

void pdnetOnlineJoin(const char *server, const char *password, const char *room)
{
	onlineStart(0, server, password, room);
}

void pdnetOnlineCancel(void)
{
	if (s_OnlineState == PDNET_ONLINE_CONNECTING) {
		s_OnlineCancel = 1;
	}
}

s32 pdnetOnlineState(void)
{
	return s_OnlineState;
}

s32 pdnetOnlineIsHost(void)
{
	return s_IsHost;
}

const char *pdnetOnlineRoom(void)
{
	return s_OnlineRoom;
}

const char *pdnetOnlineError(void)
{
	return s_ConnError;
}

void pdnetOnlineDismiss(void)
{
	if (s_OnlineState == PDNET_ONLINE_FAILED) {
		s_OnlineState = PDNET_ONLINE_IDLE;
	}
}

s32 pdnetOnlinePoll(void)
{
	{
		extern void onlineMenuTestTick(s32 tick);
		onlineMenuTestTick(replayGetTick());
	}
	// test aid PD_NET_LATE=seconds: play offline that long, then host (PD_NET_HOST=1) or join like the Online menu
	// (PD_NET_SERVER + PD_NET_ROOM, or PD_NET_PEER direct)
	static u64 lateAt = 0;
	if (envStr("PD_NET_LATE") && s_OnlineState == PDNET_ONLINE_IDLE && !s_Active) {
		if (!lateAt) {
			lateAt = nowUsecs() + (u64)(atof(envStr("PD_NET_LATE")) * 1e6);
		} else if (nowUsecs() >= lateAt) {
			lateAt = ~0ull;
			const s32 host = envStr("PD_NET_HOST") && atoi(envStr("PD_NET_HOST")) != 0;
			sysLogPrintf(LOG_NOTE, "netplay: test: %s now (PD_NET_LATE)", host ? "hosting" : "joining");
			onlineStart(host, envStr("PD_NET_SERVER"), envStr("PD_NET_PASSWORD"), envStr("PD_NET_ROOM"));
		}
	}
	// game thread, every frame: connected -> leave the current stage for the session start
	if (s_OnlineState == PDNET_ONLINE_CONNECTED && !s_OnlineLeaving) {
		s_OnlineLeaving = 1;
		sysLogPrintf(LOG_NOTE, "netplay: connected, restarting both games at the title screen");
		return 1;
	}
	return 0;
}

s32 pdnetOnlineBegin(void)
{
	// game thread, top of the main loop
	if (s_OnlineState != PDNET_ONLINE_CONNECTED || !s_OnlineLeaving) {
		return 0;
	}
	pthread_join(s_OnlineThread, NULL);
	s_OnlineThreadRunning = 0;
	s_OnlineLeaving = 0;
	s_OnlineState = PDNET_ONLINE_ACTIVE;
	if (!s_IsHost) {
		s_SessionSeed = (u64)s_PeerSettings.seedlo | ((u64)s_PeerSettings.seedhi << 32);
	}

	configKeepValues(); // settings in game memory keep their current values
	if (!snapshotLoadBoot()) {
		sysLogPrintf(LOG_WARNING, "netplay: no boot state kept; the session may not stay in sync");
	}
	configRestoreValues();
	rollbackRoomCacheReset();
	replayBeginOnline(s_SessionSeed);
	activate();
	return 1;
}

/* --- A host app owns the connection (i64) -----------------------------------
 * i64 has already paired the players, checked the build fingerprint and agreed on a seed; it starts the game
 * fresh on every machine for each run. The host's save image and game-wide settings travel in i64's session blob
 * (reliable); each player's own per-player settings still go over the game channel ('PDST'). */

#define PDNET_BLOB_MAGIC 0x50445342u /* 'PDSB' */

struct pdnetblobhdr {
	u32 magic;
	u32 version;
	u32 eepromsize;
	s32 maxexplosions;
};

u32 pdnetSessionBlob(void *out, u32 cap)
{
	struct pdnetblobhdr h;
	h.magic = PDNET_BLOB_MAGIC;
	h.version = 1;
	h.eepromsize = osEepromImageSize();
	h.maxexplosions = g_MaxExplosions;
	if (sizeof(h) + h.eepromsize > cap) {
		return 0;
	}
	memcpy(out, &h, sizeof(h));
	osEepromGetImage((u8 *)out + sizeof(h));
	return (u32)sizeof(h) + h.eepromsize;
}

void pdnetExternalEvent(s32 event)
{
	switch (event) {
	case PDNET_EXT_PEER_AWAY:
		s_ExtPeerAway = 1;
		sysLogPrintf(LOG_NOTE, "netplay: the other player switched to another app; waiting");
		break;
	case PDNET_EXT_PEER_BACK:
		s_ExtPeerAway = 0;
		break;
	case PDNET_EXT_PEER_LEFT:
		if (!s_PeerLeft) {
			sysLogPrintf(LOG_NOTE, "netplay: the other player left");
		}
		s_PeerLeft = 1;
		break;
	}
}

s32 pdnetHostPaced(void)
{
	return s_HostPaced;
}

/* i64 module: the handshake below runs on the game's coroutine; give i64 its
 * frame back between polls instead of blocking its main thread */
#if defined(PD_I64_MODULE)
extern void pdmainHostYield(void);
static void extWait(void) { pdmainHostYield(); }
#else
static void extWait(void) { usleep(1000); }
#endif
struct pdnetexternal *g_PdNetExternal = NULL;   /* set by the i64 module for an online run */

s32 pdnetInitExternal(const struct pdnetexternal *x)
{
	readEnv();
	s_ExtSend = x->send;
	s_ExtRecv = x->recv;
	s_HostPaced = x->hostpaced;
	s_IsHost = x->localplayer == 0;
	s_Session = 1;
	s_SessionSeed = x->seed;
	s_ConnError[0] = '\0';

	const struct pdnetblobhdr *h = x->blob;
	const s32 noBlob = !h || x->blobsize == 0;   /* i64 doesn't deliver session blobs yet */
	if (!noBlob && (x->blobsize < sizeof(*h) || h->magic != PDNET_BLOB_MAGIC || h->version != 1
			|| sizeof(*h) + h->eepromsize > x->blobsize || h->eepromsize != osEepromImageSize())) {
		snprintf(s_ConnError, sizeof(s_ConnError), "Netplay: the host's session data is from another version.");
		return -1;
	}
	free(s_SaveImage);
	if (noBlob) {
		/* the host's save travels over the channel instead (PDSK chunks, as in the
		 * boot-time connection): the host serves it from pollSocket */
		s_SaveSize = osEepromImageSize();
		s_SaveImage = sysMemZeroAlloc(s_SaveSize);
		s_SaveChunksHave = 0;
		if (s_IsHost) {
			osEepromGetImage(s_SaveImage);
		}
	} else {
		s_SaveSize = h->eepromsize;
		s_SaveImage = sysMemAlloc(s_SaveSize);
		memcpy(s_SaveImage, (const u8 *)x->blob + sizeof(*h), s_SaveSize);
	}

	memcpy(s_LocalExtCfg, g_PlayerExtCfg, sizeof(s_LocalExtCfg));
	s_LocalMaxExplosions = g_MaxExplosions;

	n64net_input_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.num_players = 2;
	cfg.local_player = s_IsHost ? 0 : 1;
	cfg.session_id = (u32)(x->seed ^ (x->seed >> 32)) ^ 0x50440000u;
	cfg.input_delay = envStr("PD_NET_DELAY") ? (u32)atoi(envStr("PD_NET_DELAY")) : 2;
	cfg.rollback_window = 0;
	cfg.ticks_per_second = 60;
	cfg.send = netSend;
	cfg.log = netLog;
	s_Net = n64net_input_create(&cfg);
	n64net_input_set_auto_delay(s_Net, cfg.input_delay, s_Rollback ? 3 : 6);

	// each player's own per-player settings (the game channel is unreliable: ask until answered)
	s_HavePeerSettings = 0;
	const u64 deadline = nowUsecs() + (u64)s_WaitSecs * 1000000ull;
	u64 nextReq = 0;
	if (noBlob && !s_IsHost) {
		const u32 chunks = (s_SaveSize + PDNET_SAVE_CHUNK - 1) / PDNET_SAVE_CHUNK;
		while (s_SaveChunksHave != (1u << chunks) - 1) {
			pollSocket();
			const u64 now = nowUsecs();
			if (s_PeerLeft || now > deadline) {
				snprintf(s_ConnError, sizeof(s_ConnError), "Netplay: didn't receive the host's save data.");
				n64net_input_destroy(s_Net);
				s_Net = NULL;
				return -1;
			}
			if (now >= nextReq) {
				for (u32 c = 0; c < chunks; c++) {
					if (!(s_SaveChunksHave & (1u << c))) {
						u8 req[8] = { 0 };
						const u32 m = htonl(PDNET_SAVE_MAGIC);
						memcpy(req, &m, 4);
						req[4] = PDNET_SAVE_REQ;
						req[5] = (u8)c;
						netSend(NULL, req, 8);
					}
				}
				nextReq = now + 50000;
			}
			extWait();
		}
		nextReq = 0;
	}
	while (!s_HavePeerSettings) {
		pollSocket();
		const u64 now = nowUsecs();
		if (s_PeerLeft || now > deadline) {
			snprintf(s_ConnError, sizeof(s_ConnError), "Netplay: the other player didn't start the game.");
			n64net_input_destroy(s_Net);
			s_Net = NULL;
			return -1;
		}
		if (now >= nextReq) {
			sendSettings(PDNET_SET_REQ);
			nextReq = now + 50000;
		}
		extWait();
	}

	replayBeginOnline(x->seed);
	activate();
	// the host's game-wide settings: from the blob, else from its settings packet
	g_MaxExplosions = !noBlob ? h->maxexplosions : (s_IsHost ? s_LocalMaxExplosions : s_PeerSettings.maxexplosions);
	sysLogPrintf(LOG_NOTE, "netplay: running on the host app's connection (player %d)", s_IsHost ? 1 : 2);
	return 0;
}
