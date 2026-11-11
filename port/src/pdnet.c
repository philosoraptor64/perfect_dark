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
#include "pdnet.h"
#include "rollback.h"
#include "snapshot.h"
#include "replaycheck.h"
#include "n64net_input.h"
#include "n64net_rendezvous.h"

/* See pdnet.h. Lockstep (rollback window 0) through n64net_input. */

#define PDNET_HS_MAGIC 0x50444e48u /* 'PDNH' handshake */
#define PDNET_HELLO 1
#define PDNET_WELCOME 2
#define PDNET_MISMATCH 3
#define PDNET_SYNC_EVERY 30
#define PDNET_STATS_EVERY 600

static s32 s_Active = 0;
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

static void rawSend(const uint8_t *data, uint32_t size)
{
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
		const ssize_t n = recvfrom(s_Sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
		if (n <= 0) {
			break;
		}
		u32 magic;
		if (n >= 13 && (memcpy(&magic, buf, 4), ntohl(magic) == PDNET_HS_MAGIC)) {
			u64 hash;
			memcpy(&hash, buf + 5, 8);
			if (s_IsHost && buf[4] == PDNET_HELLO) {
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
				sysFatalError("Netplay: the host's match settings or game build differ from ours.\n"
					"Both players need the same build, PD_SEED, stage, PD_REPLAY_BOTS and PD_ASPECT.");
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

s32 pdnetWanted(void)
{
	loadConfigFile();
	return envStr("PD_NET_HOST") || envStr("PD_NET_PEER") || envStr("PD_NET_SERVER");
}

static void rendezvousYield(void)
{
	usleep(1000);
}

s32 pdnetActive(void)
{
	return s_Active;
}

void pdnetInit(u64 seed, s32 stage, s32 bots, float aspect)
{
	const char *s;

	if (!pdnetWanted()) {
		return;
	}

	s_IsHost = envStr("PD_NET_HOST") && atoi(envStr("PD_NET_HOST")) != 0;
	const char *server = envStr("PD_NET_SERVER");
	s32 port = server ? 0 : (s_IsHost ? 7301 : 7302); // room codes: any free port
	if ((s = envStr("PD_NET_PORT"))) {
		port = atoi(s);
	}
	if ((s = envStr("PD_NET_BOT"))) {
		s_BotSeed = (u32)strtoul(s, NULL, 0) | 1;
	}
	if ((s = envStr("PD_NET_WAIT_SECS"))) {
		s_WaitSecs = atoi(s);
	}
	if ((s = envStr("PD_NET_SIM_LATENCY"))) s_SimLatencyMs = (u32)atoi(s);
	if ((s = envStr("PD_NET_SIM_JITTER"))) s_SimJitterMs = (u32)atoi(s);
	if ((s = envStr("PD_NET_SIM_LOSS"))) s_SimLossPct = (u32)atoi(s);
	if (s_SimLatencyMs || s_SimJitterMs || s_SimLossPct) {
		s_SimQueue = calloc(SIM_QUEUE, sizeof(*s_SimQueue));
		s_SimRand ^= (u32)port;
		sysLogPrintf(LOG_NOTE, "netplay: simulating %u ms + %u ms jitter, %u%% loss on sent packets", s_SimLatencyMs,
			s_SimJitterMs, s_SimLossPct);
	}

	if (!s_IsHost && !server) {
		char host[128];
		s32 peerport = 7301;
		const char *peer = envStr("PD_NET_PEER");
		const char *colon = peer ? strrchr(peer, ':') : NULL;
		if (!peer) {
			sysFatalError("netplay: the guest needs PD_NET_PEER=host:port");
		}
		snprintf(host, sizeof(host), "%.*s", colon ? (int)(colon - peer) : (int)strlen(peer), peer);
		if (colon) {
			peerport = atoi(colon + 1);
		}
		memset(&s_Peer, 0, sizeof(s_Peer));
		s_Peer.sin_family = AF_INET;
		s_Peer.sin_port = htons((u16)peerport);
		if (inet_pton(AF_INET, host, &s_Peer.sin_addr) != 1) {
			sysFatalError("netplay: PD_NET_PEER must be an IPv4 address:port, got `%s`", peer);
		}
		s_HavePeer = 1;
	}

	s_Sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (s_Sock < 0) {
		sysFatalError("netplay: socket() failed: %s", strerror(errno));
	}
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons((u16)port);
	if (bind(s_Sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		sysFatalError("netplay: can't bind UDP port %d: %s", port, strerror(errno));
	}
	fcntl(s_Sock, F_SETFL, fcntl(s_Sock, F_GETFL, 0) | O_NONBLOCK);

	if (server) {
		// room codes: the room server pairs us with the other player (hole punching, relay as a fallback)
		char game[96];
#ifdef VERSION_HASH
		snprintf(game, sizeof(game), "pd-%s+%s", VERSION_ROMID, VERSION_HASH);
#else
		snprintf(game, sizeof(game), "pd-%s+dev", VERSION_ROMID);
#endif
		N64NetRendezvousConfig rc;
		memset(&rc, 0, sizeof(rc));
		rc.server = server;
		rc.game = game;
		rc.room = envStr("PD_NET_ROOM");
		rc.password = envStr("PD_NET_PASSWORD");
		rc.invite_pass = envStr("PD_NET_INVITE");
		rc.is_host = s_IsHost;
		rc.force_relay = envStr("PD_NET_RELAY") && atoi(envStr("PD_NET_RELAY")) != 0;
		rc.wait_secs = envStr("PD_NET_WAIT_SECS") ? atoi(envStr("PD_NET_WAIT_SECS")) : 600;
		rc.log = netLog;
		rc.yield = rendezvousYield;
		N64NetRendezvousResult res;
		memset(&res, 0, sizeof(res));
		sysLogPrintf(LOG_NOTE, "netplay: %s room %s on %s", s_IsHost ? "hosting" : "joining",
			rc.room ? rc.room : "(new)", server);
		if (n64net_rendezvous_connect(s_Sock, &rc, &res) != 0) {
			if (!strncmp(res.error, "version", 7)) {
				sysFatalError("Netplay: the other player has a different build of the game.\n"
					"Both players need the same version.");
			}
			sysFatalError("Netplay: couldn't connect through the room server (%s).", res.error);
		}
		s_Peer = res.peer;
		s_HavePeer = 1;
		sysLogPrintf(LOG_NOTE, "netplay: room %s, %s connection", res.room, res.relayed ? "relayed" : "direct");
	}

	// everything that must be equal on both machines
	u64 h = 0xcbf29ce484222325ull;
	h = fnv(h, &seed, sizeof(seed));
	h = fnv(h, &stage, sizeof(stage));
	h = fnv(h, &bots, sizeof(bots));
	h = fnv(h, &aspect, sizeof(aspect));
#ifdef VERSION_HASH
	h = fnv(h, VERSION_HASH, strlen(VERSION_HASH));
#endif
	s_ConfigHash = h;

	n64net_input_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.num_players = 2;
	cfg.local_player = s_IsHost ? 0 : 1;
	cfg.session_id = (u32)(h ^ (h >> 32));
	cfg.start_tick = 0;
	cfg.input_delay = 2;
	if ((s = envStr("PD_NET_DELAY"))) {
		cfg.input_delay = (u32)atoi(s);
	}
	s_Rollback = !(envStr("PD_NET_ROLLBACK") && atoi(envStr("PD_NET_ROLLBACK")) == 0) && snapshotSupported();
	if ((s = envStr("PD_NET_ROLLBACK_WINDOW"))) {
		s_RollbackWindow = (u32)atoi(s);
	}
	if ((s = envStr("PD_NET_ROLLBACK_FROM"))) {
		s_RollbackFrom = atoi(s);
	}
	cfg.rollback_window = 0; // lockstep until s_RollbackFrom (match start), then s_RollbackWindow
	cfg.ticks_per_second = 60;
	cfg.send = netSend;
	cfg.log = netLog;
	s_Net = n64net_input_create(&cfg);
	// rollback hides most latency: keep the delay low; lockstep needs more to avoid stalls
	n64net_input_set_auto_delay(s_Net, cfg.input_delay, s_Rollback ? 3 : 6);
	if (s_Rollback) {
		snapshotInit(PDNET_SLOTS);
		rollbackEnableRoomCache(); // before the stage loads its rooms
	}

	sysLogPrintf(LOG_NOTE, "netplay: %s on UDP %d, waiting up to %d s for the %s", s_IsHost ? "host" : "guest", port,
		s_WaitSecs, s_IsHost ? "guest" : "host");

	// handshake: the guest says HELLO until the host answers; the host answers every HELLO
	const u64 deadline = nowUsecs() + (u64)s_WaitSecs * 1000000ull;
	u64 nextHello = 0;
	for (;;) {
		if (pollSocket()) {
			break;
		}
		const u64 now = nowUsecs();
		if (now > deadline) {
			sysFatalError("Netplay: the other player didn't connect within %d seconds.", s_WaitSecs);
		}
		if (!s_IsHost && now >= nextHello) {
			sendHandshake(PDNET_HELLO);
			nextHello = now + 100000;
		}
		usleep(1000);
	}

	s_Active = 1;
	g_NetLocalPlayer = s_IsHost ? 0 : 1;
	// one full-screen view per machine (PD_NET_VIEW=split = classic split screen on both)
	g_NetFullViews = !(envStr("PD_NET_VIEW") && strcmp(envStr("PD_NET_VIEW"), "split") == 0);
	sysLogPrintf(LOG_NOTE, "netplay: connected as player %d (%s, input delay %u)", s_IsHost ? 1 : 2,
		s_Rollback ? "rollback" : "lockstep", cfg.input_delay);
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
	n64net_input_set_local(s_Net, (u32)tick, toNet(local));

	if (!n64net_input_ready(s_Net, (u32)tick)) {
		const u64 start = nowUsecs();
		u64 lastPeerCheck = start;
		s_Stalls++;
		while (!n64net_input_ready(s_Net, (u32)tick)) {
			pollSocket();
			n64net_input_resend(s_Net);
			const u64 now = nowUsecs();
			if (now - lastPeerCheck > 1000000) {
				lastPeerCheck = now;
				const u64 silence = n64net_input_peer_silence_usecs(s_Net);
				if (silence > (u64)s_WaitSecs * 1000000ull) {
					sysFatalError("Netplay: the other player stopped responding.");
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
	if (!s_Active) {
		return;
	}

	pollSocket();

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

s32 pdnetRollbackActive(void)
{
	return s_Active && s_Rollback;
}

void pdnetRollbackPoint(void)
{
	const s32 tick = replayGetTick(); // the state now is "before tick `tick`"

	if (!s_WindowOpen && tick >= s_RollbackFrom) {
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
	if (!s_Active || !s_Rollback || !s_WindowOpen) {
		return 0;
	}
	pollSocket();
	if (n64net_timesync_should_skip(s_Net)) {
		s_Skips++;
		return 1;
	}
	return 0;
}
