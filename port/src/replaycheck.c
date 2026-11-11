#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "platform.h"
#include "data.h"
#include "bss.h"
#include "types.h"
#include "constants.h"
#include "system.h"
#include "replaycheck.h"
#include "pdnet.h"
#include "rollback.h"

/* See replaycheck.h. */

s32 g_DetFixedStep = 0;
s32 g_ReplayActive = 0;

extern u64 g_RngSeed;
extern u64 g_Rng2Seed;
extern s32 g_StageNum;
extern float gfx_fixed_aspect; // gfx_pc.cpp

#define REPLAY_PADS 4

struct replayframe {
	OSContPad pads[REPLAY_PADS];
};

static u64 s_Seed = 1;
static FILE *s_HashLog = NULL;
static FILE *s_RecordFile = NULL;
static struct replayframe *s_Play = NULL;
static s32 s_PlayCount = 0;
static s32 s_MaxTicks = -1;
static s32 s_Tick = 0; // logic ticks since boot (index of the tick being simulated)
static s32 s_Bots = 0; // PD_REPLAY_BOTS
static s32 s_DumpFrom = -1, s_DumpTo = -1;
extern s32 g_RollbackResim;
// pads per tick as the simulation saw them, so resimulated ticks (rollback, SyncTest) get the same input
#define PAD_RING 512
static OSContPad s_PadRing[PAD_RING][REPLAY_PADS];
static u32 s_TestBotSeed = 0; // PD_TEST_BOT=seed: scripted controller 1 for offline tests (SyncTest) // PD_REPLAY_DUMP=from:to = per-object lines in the hash log

static const char *envStr(const char *name)
{
	const char *v = getenv(name);
	return (v && v[0]) ? v : NULL;
}

// a file path from the environment; on iOS a relative path is relative to the app's Documents
// folder (its absolute path changes with every install)
static const char *envPath(const char *name)
{
	const char *v = envStr(name);
#ifdef PD_IOS
	if (v && v[0] != '/') {
		static char bufs[4][1024];
		static s32 next = 0;
		char *b = bufs[next++ & 3];
		snprintf(b, sizeof(bufs[0]), "%s/Documents/%s", getenv("HOME") ? getenv("HOME") : ".", v);
		return b;
	}
#endif
	return v;
}

static void loadPlayback(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	s32 cap = 0;

	if (!f) {
		sysFatalError("replay: can't open %s", path);
	}

	while (fgets(line, sizeof(line), f)) {
		s32 tick, n = 0, consumed;
		const char *p = line;
		struct replayframe fr;

		if (line[0] == '#' || sscanf(p, "%d%n", &tick, &consumed) != 1) {
			continue;
		}

		p += consumed;
		memset(&fr, 0, sizeof(fr));

		for (s32 i = 0; i < REPLAY_PADS; i++) {
			u32 b;
			s32 x, y, rx, ry, err;
			if (sscanf(p, "%x %d %d %d %d %d%n", &b, &x, &y, &rx, &ry, &err, &consumed) != 6) {
				break;
			}
			p += consumed;
			fr.pads[i].button = b;
			fr.pads[i].stick_x = x;
			fr.pads[i].stick_y = y;
			fr.pads[i].rstick_x = rx;
			fr.pads[i].rstick_y = ry;
			fr.pads[i].errnum = err;
			n++;
		}

		if (n != REPLAY_PADS || tick < 0) {
			continue;
		}

		if (tick >= cap) {
			s32 newcap = cap ? cap * 2 : 4096;
			while (newcap <= tick) newcap *= 2;
			s_Play = realloc(s_Play, newcap * sizeof(*s_Play));
			memset(s_Play + cap, 0, (newcap - cap) * sizeof(*s_Play));
			cap = newcap;
		}

		s_Play[tick] = fr;
		if (tick + 1 > s_PlayCount) {
			s_PlayCount = tick + 1;
		}
	}

	fclose(f);
	sysLogPrintf(LOG_NOTE, "replay: playing %d ticks from %s", s_PlayCount, path);
}

void replayInit(void)
{
	const char *check = envPath("PD_REPLAY_CHECK");
	const char *rec = envPath("PD_REPLAY_RECORD");
	const char *play = envPath("PD_REPLAY_PLAY");
	const char *s;

	g_ReplayActive = (check || rec || play || envStr("PD_SYNCTEST")) ? 1 : 0;
	if ((s = envStr("PD_TEST_BOT"))) {
		s_TestBotSeed = (u32)strtoul(s, NULL, 0) | 1;
	}
	g_DetFixedStep = g_ReplayActive || pdnetWanted() || ((s = envStr("PD_DETERMINISTIC")) && atoi(s));

	if (!g_DetFixedStep) {
		return;
	}

	// PD_BOOT_STAGE=0x32 = --boot-stage for machines without a command line (iPhone netplay.cfg)
	if ((s = envStr("PD_BOOT_STAGE"))) {
		g_StageNum = (s32)strtol(s, NULL, 0);
	}

	if ((s = envStr("PD_SEED"))) {
		s_Seed = strtoull(s, NULL, 0);
	}

	// the camera aspect feeds the simulation (aim rays, on-screen checks), so every device renders the
	// game at the same aspect, letterboxed (PD_ASPECT=w/h or a number; 0 = window shape, not deterministic)
	gfx_fixed_aspect = 16.f / 9.f;
	if ((s = envStr("PD_ASPECT"))) {
		float w = 0, h = 0;
		if (sscanf(s, "%f/%f", &w, &h) == 2 && h > 0) {
			gfx_fixed_aspect = w / h;
		} else {
			gfx_fixed_aspect = atof(s);
		}
	}

	if ((s = envStr("PD_REPLAY_BOTS"))) {
		s_Bots = atoi(s);
		if (s_Bots < 0) s_Bots = 0;
		if (s_Bots > MAX_BOTS) s_Bots = MAX_BOTS;
	}

	if ((s = envStr("PD_REPLAY_DUMP"))) {
		if (sscanf(s, "%d:%d", &s_DumpFrom, &s_DumpTo) != 2) {
			s_DumpFrom = s_DumpTo = atoi(s);
		}
	}

	if ((s = envStr("PD_REPLAY_FRAMES"))) {
		s_MaxTicks = atoi(s);
	}

	if (check) {
		s_HashLog = fopen(check, "w");
		if (!s_HashLog) {
			sysFatalError("replay: can't write %s", check);
		}
		fprintf(s_HashLog, "# pd replay check: seed %llu\n", (unsigned long long)s_Seed);
	}

	if (rec) {
		s_RecordFile = fopen(rec, "w");
		if (!s_RecordFile) {
			sysFatalError("replay: can't write %s", rec);
		}
		fprintf(s_RecordFile, "# pd replay input: tick then 4 x (buttons sx sy rsx rsy err)\n");
	}

	if (play) {
		loadPlayback(play);
		if (s_MaxTicks < 0) {
			s_MaxTicks = s_PlayCount + 30;
		}
	}

	sysLogPrintf(LOG_NOTE, "deterministic mode: fixed step, aspect %.4f, seed %llu%s%s%s", gfx_fixed_aspect,
		(unsigned long long)s_Seed, check ? ", hash log" : "", rec ? ", recording" : "", play ? ", playback" : "");

	rollbackInit();

	// netplay: both machines must agree on everything above before tick 0
	pdnetInit(s_Seed, g_StageNum, s_Bots, gfx_fixed_aspect);
}

s32 replayWantsMatch(void)
{
	// never in an online session: there the players set matches up in the menus
	const s32 session = pdnetActive() && !pdnetMatchMode();
	return g_DetFixedStep && !session && (s_Bots > 0 || pdnetMatchMode()) && g_StageNum < STAGE_TITLE;
}

void replaySetupMatch(void)
{
	if (!replayWantsMatch()) {
		return;
	}

	g_MpSetup.stagenum = g_StageNum;
	g_MpSetup.chrslots = pdnetMatchMode() ? 0x3 : 1; // player 1 (+ player 2 online)

	for (s32 i = 0; i < s_Bots; i++) {
		g_MpSetup.chrslots |= 1 << (MAX_PLAYERS + i);
		g_BotConfigsArray[i].type = BOTTYPE_GENERAL;
		g_BotConfigsArray[i].difficulty = BOTDIFF_NORMAL;
	}

	// PD_REPLAY_TIMELIMIT=minutes: a match that ends (tests of what happens after it)
	const char *tl = envStr("PD_REPLAY_TIMELIMIT");
	if (tl) {
		const s32 minutes = atoi(tl);
		g_MpSetup.timelimit = minutes >= 1 ? (u8)(minutes - 1) : 60;
	}

	sysLogPrintf(LOG_NOTE, "replay: Combat Sim on stage 0x%02x against %d simulants", g_StageNum, s_Bots);
}

u64 replayGetSeed(void)
{
	return s_Seed;
}

void replayFilterStatus(OSContStatus *status, s32 count)
{
	if (pdnetActive()) {
		pdnetFilterStatus(status, count);
		return;
	}

	if (!g_ReplayActive) {
		return;
	}

	// PD_REPLAY_PADS=2: controller 2 is plugged in too (idle), like an online session
	static s32 numpads = -1;
	if (numpads < 0) {
		numpads = envStr("PD_REPLAY_PADS") ? atoi(envStr("PD_REPLAY_PADS")) : 1;
	}

	for (s32 i = 0; i < count; i++) {
		if (i < numpads) {
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

void replayFilterPads(OSContPad *pads, s32 count)
{
	if (pdnetActive()) {
		pdnetFilterPads(pads, count, s_Tick);
		return;
	}

	if (!g_ReplayActive) {
		return;
	}

	if (count > REPLAY_PADS) {
		count = REPLAY_PADS;
	}

	if (g_RollbackResim) {
		memcpy(pads, s_PadRing[s_Tick % PAD_RING], sizeof(OSContPad) * count);
		return;
	}

	if (s_Play) {
		for (s32 i = 0; i < count; i++) {
			if (s_Tick < s_PlayCount) {
				pads[i] = s_Play[s_Tick].pads[i];
			} else {
				memset(&pads[i], 0, sizeof(pads[i]));
				pads[i].errnum = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
			}
		}
		memcpy(s_PadRing[s_Tick % PAD_RING], pads, sizeof(OSContPad) * count);
		return;
	}

	// live input: only controller 1 exists in replay mode
	for (s32 i = 1; i < count; i++) {
		memset(&pads[i], 0, sizeof(pads[i]));
		pads[i].errnum = CONT_NO_RESPONSE_ERROR;
	}
	pads[0].errnum = 0;

	if (s_TestBotSeed) {
		pdnetBotPad(s_Tick, s_TestBotSeed, &pads[0]);
	}
	// PD_TEST_BOT2=seed (with PD_REPLAY_PADS=2): controller 2 is driven too, like a session's second player
	static u32 bot2 = 1;
	if (bot2 == 1) {
		bot2 = envStr("PD_TEST_BOT2") ? ((u32)strtoul(envStr("PD_TEST_BOT2"), NULL, 0) | 1) : 0;
	}
	if (bot2 && count > 1) {
		pdnetBotPad(s_Tick, bot2, &pads[1]);
		pads[1].errnum = 0;
	}

	memcpy(s_PadRing[s_Tick % PAD_RING], pads, sizeof(OSContPad) * count);

	if (s_RecordFile) {
		// the game may read the pads more than once per tick: keep the last read
		static s32 lastTick = -1;
		static OSContPad last[REPLAY_PADS];
		if (lastTick >= 0 && lastTick != s_Tick) {
			fprintf(s_RecordFile, "%d", lastTick);
			for (s32 i = 0; i < REPLAY_PADS; i++) {
				fprintf(s_RecordFile, " %x %d %d %d %d %d", last[i].button, last[i].stick_x, last[i].stick_y,
					last[i].rstick_x, last[i].rstick_y, last[i].errnum);
			}
			fputc('\n', s_RecordFile);
		}
		memcpy(last, pads, sizeof(OSContPad) * count);
		lastTick = s_Tick;
	}
}

/* FNV-1a 64 */
static u64 hashBytes(u64 h, const void *data, size_t len)
{
	const u8 *p = data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

#define HASHV(h, v) h = hashBytes(h, &(v), sizeof(v))

static u64 hashState(u32 *outNumProps)
{
	u64 h = 0xcbf29ce484222325ull;
	u32 numprops = 0;

	HASHV(h, g_RngSeed);
	HASHV(h, g_Rng2Seed);
	HASHV(h, g_Vars.lvframenum);
	HASHV(h, g_Vars.lvframe60);
	HASHV(h, g_Vars.lvupdate240);
	HASHV(h, g_StageNum);
	HASHV(h, g_MpSetup); // chosen in the menus (online sessions)

	for (s32 i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		if (!p) {
			continue;
		}
		HASHV(h, i);
		HASHV(h, p->bondhealth);
		HASHV(h, p->vv_theta);
		HASHV(h, p->vv_verta);
		if (p->prop) {
			HASHV(h, p->prop->pos);
		}
	}

	for (s32 i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *c = &g_ChrSlots[i];
		if (!c->prop) {
			continue;
		}
		HASHV(h, c->chrnum);
		HASHV(h, c->actiontype);
		HASHV(h, c->damage);
		HASHV(h, c->prop->pos);
	}

	for (struct prop *pr = g_Vars.activeprops; pr && numprops < 100000; pr = pr->next) {
		HASHV(h, pr->type);
		HASHV(h, pr->pos);
		numprops++;
	}

	if (outNumProps) {
		*outNumProps = numprops;
	}
	return h;
}

u64 replayStateHash(void)
{
	return hashState(NULL);
}

s32 replayGetTick(void)
{
	return s_Tick;
}

void replaySetTick(s32 tick)
{
	s_Tick = tick;
}

void replayEndTick(void)
{
	if (!g_DetFixedStep) {
		return;
	}

	if (g_RollbackResim) {
		s_Tick++; // resimulation: no hash log, netplay bookkeeping or quitting
		return;
	}

	if (s_HashLog) {
		u32 numprops;
		u64 h = hashState(&numprops);
		fprintf(s_HashLog, "%d stage %02x lvframe %d props %u rng %016llx hash %016llx\n", s_Tick, g_StageNum,
			g_Vars.lvframenum, numprops, (unsigned long long)g_RngSeed, (unsigned long long)h);
	}

	if (s_HashLog && s_Tick >= s_DumpFrom && s_Tick <= s_DumpTo) {
		// exact values (%a) so two devices' dumps can be diffed
		for (s32 i = 0; i < MAX_PLAYERS; i++) {
			struct player *p = g_Vars.players[i];
			if (p && p->prop) {
				fprintf(s_HashLog, "  player %d pos %a %a %a theta %a verta %a health %a\n", i, p->prop->pos.x,
					p->prop->pos.y, p->prop->pos.z, p->vv_theta, p->vv_verta, p->bondhealth);
			}
		}
		for (s32 i = 0; i < g_NumChrSlots; i++) {
			struct chrdata *c = &g_ChrSlots[i];
			if (c->prop) {
				fprintf(s_HashLog, "  chr %d num %d action %d damage %a pos %a %a %a\n", i, c->chrnum, c->actiontype,
					c->damage, c->prop->pos.x, c->prop->pos.y, c->prop->pos.z);
			}
		}
		s32 n = 0;
		for (struct prop *pr = g_Vars.activeprops; pr && n < 100000; pr = pr->next, n++) {
			s32 objtype = -1;
			if ((pr->type == PROPTYPE_OBJ || pr->type == PROPTYPE_WEAPON || pr->type == PROPTYPE_DOOR) && pr->obj) {
				objtype = pr->obj->type;
			}
			fprintf(s_HashLog, "  prop %d type %d obj %d flags %02x pos %a %a %a\n", n, pr->type, objtype, pr->flags,
				pr->pos.x, pr->pos.y, pr->pos.z);
		}
	}

	if (pdnetActive()) {
		pdnetEndTick(s_Tick, hashState(NULL));
	}

	s_Tick++;

	if (s_MaxTicks >= 0 && s_Tick >= s_MaxTicks) {
		sysLogPrintf(LOG_NOTE, "replay: %d ticks done, quitting", s_Tick);
		if (s_HashLog) fclose(s_HashLog);
		if (s_RecordFile) fclose(s_RecordFile);
		s_HashLog = s_RecordFile = NULL;
		exit(0);
	}
}
