#ifndef _IN_ROLLBACK_H
#define _IN_ROLLBACK_H

#include <PR/ultratypes.h>

/*
 * Rollback support (port/src/rollback.c, snapshots in snapshot.c).
 *
 * The rollback point is right after replayEndTick() in the main loop: the state there is "before tick T"
 * (T = replayGetTick()), and one resimulated step is rollbackStep() = the rest of the previous loop
 * iteration (schedEndFrame: input for tick T, audio frame) + schedStartFrame + mainTick + replayEndTick.
 * While g_RollbackResim is set nothing is drawn, no sound reaches the output, the frame timer doesn't
 * wait for real time and controllers come from the per-tick pad ring / the netplay ledger.
 *
 * Test: PD_SYNCTEST=N [PD_SYNCTEST_FROM=tick] rewinds N ticks and resimulates them after every tick and
 * compares the state hash with the original run ("SyncTest:" log lines). PD_TEST_BOT=seed drives controller 1.
 */

extern s32 g_RollbackResim;

void rollbackInit(void);
void rollbackStep(void);
void rollbackAfterTick(void); /* main loop, right after replayEndTick() */
s32 rollbackSyncTestMismatches(void);

/* Room geometry buffers (bg.c). The port loads rooms into malloc'ed memory, outside the snapshot. While
 * rollback is enabled each room keeps one buffer per kind that is never freed: a restored state's
 * pointers stay valid, and a reload writes the same bytes again (room data is read-only once loaded).
 * Otherwise these are plain sysMemAlloc/sysMemFree. */
void *rollbackRoomAlloc(s32 roomnum, s32 kind, u32 len);
void rollbackRoomFree(s32 roomnum, s32 kind, void *ptr);
/* stage end (bgStop, after every room was unloaded): frees the kept buffers */
void rollbackRoomCacheReset(void);
/* on for SyncTest and rollback netplay (before the stage loads) */
void rollbackEnableRoomCache(void);

#endif
