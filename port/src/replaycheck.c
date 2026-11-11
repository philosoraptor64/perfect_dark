#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "platform.h"
#include "data.h"
#include "bss.h"
#include "types.h"
#include "system.h"
#include "replaycheck.h"

/* See replaycheck.h. */

s32 g_DetFixedStep = 0;
s32 g_ReplayActive = 0;

extern u64 g_RngSeed;
extern u64 g_Rng2Seed;
extern s32 g_StageNum;

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

static const char *envStr(const char *name)
{
	const char *v = getenv(name);
	return (v && v[0]) ? v : NULL;
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
	const char *check = envStr("PD_REPLAY_CHECK");
	const char *rec = envStr("PD_REPLAY_RECORD");
	const char *play = envStr("PD_REPLAY_PLAY");
	const char *s;

	g_ReplayActive = (check || rec || play) ? 1 : 0;
	g_DetFixedStep = g_ReplayActive || ((s = envStr("PD_DETERMINISTIC")) && atoi(s));

	if (!g_DetFixedStep) {
		return;
	}

	if ((s = envStr("PD_SEED"))) {
		s_Seed = strtoull(s, NULL, 0);
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

	sysLogPrintf(LOG_NOTE, "deterministic mode: fixed step, seed %llu%s%s%s",
		(unsigned long long)s_Seed, check ? ", hash log" : "", rec ? ", recording" : "", play ? ", playback" : "");
}

u64 replayGetSeed(void)
{
	return s_Seed;
}

void replayFilterStatus(OSContStatus *status, s32 count)
{
	if (!g_ReplayActive) {
		return;
	}

	for (s32 i = 0; i < count; i++) {
		if (i == 0) {
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
	if (!g_ReplayActive) {
		return;
	}

	if (count > REPLAY_PADS) {
		count = REPLAY_PADS;
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
		return;
	}

	// live input: only controller 1 exists in replay mode
	for (s32 i = 1; i < count; i++) {
		memset(&pads[i], 0, sizeof(pads[i]));
		pads[i].errnum = CONT_NO_RESPONSE_ERROR;
	}
	pads[0].errnum = 0;

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

	*outNumProps = numprops;
	return h;
}

void replayEndTick(void)
{
	if (!g_DetFixedStep) {
		return;
	}

	if (s_HashLog) {
		u32 numprops;
		u64 h = hashState(&numprops);
		fprintf(s_HashLog, "%d stage %02x lvframe %d props %u rng %016llx hash %016llx\n", s_Tick, g_StageNum,
			g_Vars.lvframenum, numprops, (unsigned long long)g_RngSeed, (unsigned long long)h);
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
