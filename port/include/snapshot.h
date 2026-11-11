#ifndef _IN_SNAPSHOT_H
#define _IN_SNAPSHOT_H

#include <stddef.h>
#include <PR/ultratypes.h>

/*
 * Whole-simulation snapshots for rollback netplay (port/src/snapshot.c).
 * A snapshot is the __PDSNAP segment (every mutable global of the game and lib C code, see
 * snapshot_section.h) plus the used parts of the memp heap (which holds mema, the stage data,
 * graphics memory and the audio heap). Slots form a ring: slot = tick % numslots.
 */

s32 snapshotSupported(void);
void snapshotInit(s32 numslots);
void snapshotSave(s32 slot);
s32 snapshotLoad(s32 slot); /* 0 if the slot was never saved */
size_t snapshotLastSize(s32 slot);
/* byte comparison of two slots (SyncTest diagnostics): number of ranges that differ, details in report */
s32 snapshotCompare(s32 slota, s32 slotb, char *report, size_t reportlen);
f64 snapshotLastSaveMs(void);
f64 snapshotLastLoadMs(void);

/* the state at the start of the main loop, for online sessions started mid-game */
void snapshotSaveBoot(void);
s32 snapshotLoadBoot(void);

#endif
