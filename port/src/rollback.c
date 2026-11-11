#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultrasched.h>
#include "platform.h"
#include "system.h"
#include "snapshot.h"
#include "replaycheck.h"
#include "rollback.h"
#include "pdnet.h"
#include "types.h"

/* See rollback.h. */

s32 g_RollbackResim = 0;

extern OSSched g_Sched;
void schedStartFrame(OSSched *sc);
void schedEndFrame(OSSched *sc);
void mainTick(void);
extern s32 g_MainChangeToStageNum;

#define HASH_RING 64

static s32 s_SyncTest = 0;     // PD_SYNCTEST=N: rewind N ticks and resimulate, every tick
static s32 s_SyncTestFrom = 0; // PD_SYNCTEST_FROM=tick: first tick to test
static s32 s_Slots = 0;
static u64 s_Hash[HASH_RING];
static u32 s_Checks = 0, s_Mismatches = 0;
static f64 s_ResimMsTotal = 0;

void rollbackInit(void)
{
	const char *s = getenv("PD_SYNCTEST");
	if (s && *s) {
		s_SyncTest = atoi(s);
		if (s_SyncTest < 1) s_SyncTest = 1;
		if (s_SyncTest > HASH_RING - 2) s_SyncTest = HASH_RING - 2;
		s = getenv("PD_SYNCTEST_FROM");
		s_SyncTestFrom = (s && *s) ? atoi(s) : 60;
		s_Slots = s_SyncTest + 2;
		snapshotInit(s_Slots);
		rollbackEnableRoomCache();
		sysLogPrintf(LOG_NOTE, "SyncTest: rewinding %d ticks every tick from tick %d", s_SyncTest, s_SyncTestFrom);
	}
}

void rollbackStep(void)
{
	// the rest of the previous iteration (input for the next tick, audio frame), then one tick
	schedEndFrame(&g_Sched);
	schedStartFrame(&g_Sched);
	mainTick();
	replayEndTick();
}

// PD_SYNCTEST_TEXCHECK: walk the shared texture pool list and report the first broken link
struct texpool_hdr { u8 *start; void *head; u8 *leftpos; void *rightpos; };
extern struct texpool_hdr g_TexSharedPool;
extern u8 *g_MempHeap;
extern u32 g_MempHeapSize;

static s32 texListBroken(void)
{
	u8 *node = g_TexSharedPool.head;
	s32 n = 0;
	while (node && n < 100000) {
		if (node < g_MempHeap || node >= g_MempHeap + g_MempHeapSize) {
			return n + 1;
		}
		uintptr_t next = ((struct tex *)node)->next;
		node = (u8 *)next;
		n++;
	}
	return 0;
}

static void syncTestPoint(void)
{
	const s32 tick = replayGetTick(); // the state now = the state before tick `tick`

	s_Hash[tick % HASH_RING] = replayStateHash();
	snapshotSave(tick % s_Slots);

	static s32 texcheck = -1, texreported = 0;
	if (texcheck < 0) texcheck = getenv("PD_SYNCTEST_TEXCHECK") != NULL;
	if (texcheck && !texreported && texListBroken()) {
		texreported = 1;
		sysLogPrintf(LOG_WARNING, "SyncTest: texture list broken BEFORE resim at tick %d (node %d)", tick, texListBroken());
		fflush(stdout);
	}

	const s32 from = tick - s_SyncTest;
	if (from < s_SyncTestFrom || g_MainChangeToStageNum >= 0) {
		return;
	}

	if (!snapshotLoad(from % s_Slots)) {
		return;
	}
	const f64 loadms = snapshotLastLoadMs();
	replaySetTick(from);
	g_RollbackResim = 1;
	for (s32 t = from; t < tick; t++) {
		rollbackStep();
	}
	g_RollbackResim = 0;

	if (texcheck && !texreported && texListBroken()) {
		texreported = 1;
		sysLogPrintf(LOG_WARNING, "SyncTest: texture list broken AFTER resim from %d to %d (node %d)", from, tick, texListBroken());
		fflush(stdout);
	}

	const u64 h = replayStateHash();
	s_Checks++;

	// PD_SYNCTEST_FULL: compare the whole resimulated state with the original one saved before this tick
	// (scratch slot (tick + 1) % s_Slots holds the oldest state, which is no longer needed)
	if (getenv("PD_SYNCTEST_FULL")) {
		static s32 reported = 0;
		char report[2048];
		const s32 scratch = (tick + 1) % s_Slots;
		snapshotSave(scratch);
		if (snapshotCompare(tick % s_Slots, scratch, report, sizeof(report)) && reported++ < 20) {
			sysLogPrintf(LOG_WARNING, "SyncTest: state differs before tick %d:%s", tick, report);
		}
	}
	if (h != s_Hash[tick % HASH_RING] || replayGetTick() != tick) {
		if (s_Mismatches++ < 10) {
			sysLogPrintf(LOG_WARNING, "SyncTest: MISMATCH before tick %d (rewound %d): %016llx resimulated, %016llx original",
				tick, s_SyncTest, (unsigned long long)h, (unsigned long long)s_Hash[tick % HASH_RING]);
		}
	}

	if (tick % 600 == 0 || (getenv("PD_SYNCTEST_VERBOSE") && tick % 60 == 0)) {
		sysLogPrintf(LOG_NOTE, "SyncTest: tick %d, %u checks, %u mismatches, snapshot %zu KB, save %.2f ms, load %.2f ms",
			tick, s_Checks, s_Mismatches, snapshotLastSize(tick % s_Slots) / 1024, snapshotLastSaveMs(), loadms);
	}
}

void rollbackAfterTick(void)
{
	if (s_SyncTest) {
		syncTestPoint();
	} else if (pdnetRollbackActive()) {
		pdnetRollbackPoint();
	}
}

#define ROOM_CACHE_ROOMS 1024

struct roombuf {
	void *ptr;
	u32 cap;
};

static s32 s_RoomCache = 0;
static struct roombuf s_RoomBufs[2][ROOM_CACHE_ROOMS];

void rollbackEnableRoomCache(void)
{
	s_RoomCache = 1;
}

void *rollbackRoomAlloc(s32 roomnum, s32 kind, u32 len)
{
	if (!s_RoomCache || roomnum < 0 || roomnum >= ROOM_CACHE_ROOMS) {
		return sysMemAlloc(len);
	}
	struct roombuf *b = &s_RoomBufs[kind][roomnum];
	if (!b->ptr || b->cap < len) {
		sysMemFree(b->ptr); // only reached when no live state can point at it (bigger room of a new stage)
		b->ptr = sysMemAlloc(len);
		b->cap = b->ptr ? len : 0;
	}
	return b->ptr;
}

void rollbackRoomFree(s32 roomnum, s32 kind, void *ptr)
{
	if (!s_RoomCache || roomnum < 0 || roomnum >= ROOM_CACHE_ROOMS) {
		sysMemFree(ptr);
		return;
	}
	// kept for the room's next load (or a rollback to before this unload)
}

s32 rollbackSyncTestMismatches(void)
{
	return (s32)s_Mismatches;
}
