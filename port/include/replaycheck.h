#ifndef _IN_REPLAYCHECK_H
#define _IN_REPLAYCHECK_H

#include <ultra64.h>

/*
 * Deterministic mode and replay check (netplay groundwork).
 *
 * Deterministic mode (g_DetFixedStep) makes the simulation depend only on the
 * controller input and the seed:
 *   - every logic tick advances exactly one 60th (frametimeCalculate still waits
 *     for real time, but diffframe60/240 are fixed and no lost time carries over);
 *   - the RNG seed comes from PD_SEED (default 1) instead of the clock;
 *   - the music sequencer renders a fixed number of samples per tick (it used to
 *     depend on how full the audio output queue was, and an AI command branches
 *     on the music event queue);
 *   - the mouse is ignored (its deltas bypass the controller).
 *
 * Environment:
 *   PD_DETERMINISTIC=1          deterministic mode only
 *   PD_SEED=n                   RNG seed (default 1)
 *   PD_REPLAY_CHECK=<file>      deterministic mode + one hash line per logic tick
 *   PD_REPLAY_RECORD=<file>     record the 4 controller pads per tick
 *   PD_REPLAY_PLAY=<file>       feed recorded pads instead of the real controllers
 *   PD_REPLAY_FRAMES=n          quit after n ticks (default: play = end of file + 30,
 *                               record = until the game quits)
 * In replay mode controller 1 always reads as connected and 2-4 as absent, so a
 * playback doesn't depend on which controllers the machine has.
 */

extern s32 g_DetFixedStep;
extern s32 g_ReplayActive;

void replayInit(void);
/* PD_REPLAY_BOTS=N: boot straight into a Combat Sim match against N simulants (stage = --boot-stage) */
s32 replayWantsMatch(void);
void replaySetupMatch(void); /* before mpReset() */
u64 replayGetSeed(void);
/* called by osContGetReadData after the real pads were read */
void replayFilterPads(OSContPad *pads, s32 count);
/* osContGetQuery: replay mode reports a fixed controller set */
void replayFilterStatus(OSContStatus *status, s32 count);
/* after each mainTick */
void replayEndTick(void);
/* rollback/SyncTest: the deterministic state hash, and the tick counter (index of the next tick) */
u64 replayStateHash(void);
s32 replayGetTick(void);
void replaySetTick(s32 tick);

/* online session started mid-game: deterministic mode from now on (tick 0, this seed) */
void replayBeginOnline(u64 seed);

#endif
