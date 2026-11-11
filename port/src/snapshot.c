#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ultra64.h>
#include "platform.h"
#include "system.h"
#define PD_SNAPSHOT_EXCLUDE // this file's own state must stay outside the snapshot
#include "snapshot_section.h"
#include "snapshot.h"

/* See snapshot.h. A snapshot = the __PDSNAP segment (game/lib globals) + the used parts of the memp heap. */

#if PD_SNAPSHOT_SUPPORTED

#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

struct memorypool {
	u8 *start;
	u8 *leftpos;
	u8 *rightpos;
	u8 *end;
	u8 *prevallocation;
};

extern struct memorypool g_MempOnboardPools[9];
extern struct memorypool g_MempExpansionPools[9];
extern u8 *g_MempHeap;
extern u8 *g_GfxBuffers[];  // [0 .. NUM_GFXTASKS]: display list buffers (render output, rebuilt every frame)
extern u8 *g_VtxBuffers[];  // vertex/matrix/colour buffers of the same kind
#define SNAP_NUM_GFXTASKS 2
extern u32 g_MempHeapSize;

struct range {
	u8 *base;
	size_t size;
};

struct snapslot {
	u8 *buf;
	size_t cap;
	size_t used;
	struct range ranges[40];
	s32 numranges;
	s32 valid;
};

static struct range s_Data, s_Bss;
static struct snapslot *s_Slots = NULL;
static s32 s_NumSlots = 0;
static s32 s_Ready = 0;
static f64 s_LastSaveMs = 0, s_LastLoadMs = 0;

static f64 nowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (f64)ts.tv_sec * 1000.0 + (f64)ts.tv_nsec / 1e6;
}

static void findSections(void)
{
	unsigned long size = 0;
	s_Data.base = getsectiondata(&_mh_execute_header, PD_SNAPSHOT_SEGMENT, "__data", &size);
	s_Data.size = s_Data.base ? size : 0;
	size = 0;
	s_Bss.base = getsectiondata(&_mh_execute_header, PD_SNAPSHOT_SEGMENT, "__bss", &size);
	s_Bss.size = s_Bss.base ? size : 0;
}

/* Adds [base, base + size) minus the graphics buffers (render output, not simulation state). */
static s32 addRangeMinusGfx(struct range *out, s32 n, u8 *base, size_t size)
{
	u8 *holes[2][2] = {
		{ g_GfxBuffers[0], g_GfxBuffers[SNAP_NUM_GFXTASKS] },
		{ g_VtxBuffers[0], g_VtxBuffers[SNAP_NUM_GFXTASKS] },
	};
	u8 *cur = base;
	u8 *end = base + size;

	// holes in address order
	if (holes[1][0] && holes[0][0] && holes[1][0] < holes[0][0]) {
		u8 *t0 = holes[0][0], *t1 = holes[0][1];
		holes[0][0] = holes[1][0]; holes[0][1] = holes[1][1];
		holes[1][0] = t0; holes[1][1] = t1;
	}
	for (s32 h = 0; h < 2; h++) {
		u8 *hs = holes[h][0], *he = holes[h][1];
		if (!hs || he <= hs || he <= cur || hs >= end) {
			continue;
		}
		if (hs > cur) {
			out[n].base = cur;
			out[n].size = (size_t)(hs - cur);
			n++;
		}
		cur = he;
	}
	if (end > cur) {
		out[n].base = cur;
		out[n].size = (size_t)(end - cur);
		n++;
	}
	return n;
}

/* The memp regions in use: every pool's left part [start, leftpos) and right part [rightpos, end), of both
 * banks, merged (pools overlap), minus the graphics buffers. Taking each pool on its own matters: some pools
 * are unused or overlap the others, so "lowest rightpos .. highest end" of a bank can miss right-side
 * allocations (the shared texture pool lives there). */
static s32 mempRanges(struct range *out)
{
	struct { u8 *a, *b; } iv[2 * 2 * 9];
	s32 numiv = 0;
	struct memorypool *banks[2] = { g_MempOnboardPools, g_MempExpansionPools };

	for (s32 b = 0; b < 2; b++) {
		for (s32 i = 0; i < 9; i++) {
			struct memorypool *pool = &banks[b][i];
			if (!pool->start || !pool->end || pool->end <= pool->start) {
				continue;
			}
			if (pool->leftpos > pool->start && pool->leftpos <= pool->end) {
				iv[numiv].a = pool->start;
				iv[numiv].b = pool->leftpos;
				numiv++;
			}
			if (pool->rightpos >= pool->start && pool->rightpos < pool->end) {
				iv[numiv].a = pool->rightpos;
				iv[numiv].b = pool->end;
				numiv++;
			}
		}
	}

	// sort by start (tiny n) and merge
	for (s32 i = 1; i < numiv; i++) {
		for (s32 j = i; j > 0 && iv[j].a < iv[j - 1].a; j--) {
			u8 *ta = iv[j].a, *tb = iv[j].b;
			iv[j] = iv[j - 1];
			iv[j - 1].a = ta;
			iv[j - 1].b = tb;
		}
	}
	s32 n = 0;
	for (s32 i = 0; i < numiv;) {
		u8 *a = iv[i].a, *b = iv[i].b;
		s32 j = i + 1;
		while (j < numiv && iv[j].a <= b) {
			if (iv[j].b > b) b = iv[j].b;
			j++;
		}
		n = addRangeMinusGfx(out, n, a, (size_t)(b - a));
		i = j;
	}

	return n;
}

s32 snapshotSupported(void)
{
	return 1;
}

void snapshotInit(s32 numslots)
{
	findSections();
	free(s_Slots);
	s_Slots = calloc((size_t)numslots, sizeof(*s_Slots));
	s_NumSlots = numslots;
	s_Ready = s_Data.base || s_Bss.base;
	sysLogPrintf(LOG_NOTE, "snapshot: %d slots, globals %zu + %zu bytes", numslots, s_Data.size, s_Bss.size);
}

void snapshotSave(s32 slot)
{
	if (!s_Ready || slot < 0) {
		return;
	}
	const f64 t0 = nowMs();
	struct snapslot *s = &s_Slots[slot % s_NumSlots];
	struct range r[40];
	s32 n = 0;
	r[n++] = s_Data;
	r[n++] = s_Bss;
	n += mempRanges(&r[n]);

	size_t total = 0;
	for (s32 i = 0; i < n; i++) total += r[i].size;
	if (total > s->cap) {
		free(s->buf);
		s->cap = total + total / 4;
		s->buf = malloc(s->cap);
		if (!s->buf) {
			sysFatalError("snapshot: out of memory (%zu bytes)", s->cap);
		}
	}
	u8 *p = s->buf;
	for (s32 i = 0; i < n; i++) {
		memcpy(p, r[i].base, r[i].size);
		p += r[i].size;
		s->ranges[i] = r[i];
	}
	s->numranges = n;
	s->used = total;
	s->valid = 1;
	s_LastSaveMs = nowMs() - t0;
}

s32 snapshotLoad(s32 slot)
{
	if (!s_Ready || slot < 0) {
		return 0;
	}
	struct snapslot *s = &s_Slots[slot % s_NumSlots];
	if (!s->valid) {
		return 0;
	}
	const f64 t0 = nowMs();
	// the globals first: they include the memp pool positions, but the ranges recorded at save time are used
	const u8 *p = s->buf;
	for (s32 i = 0; i < s->numranges; i++) {
		memcpy(s->ranges[i].base, p, s->ranges[i].size);
		p += s->ranges[i].size;
	}
	s_LastLoadMs = nowMs() - t0;
	return 1;
}

s32 snapshotCompare(s32 slota, s32 slotb, char *report, size_t reportlen)
{
	struct snapslot *a = &s_Slots[slota % s_NumSlots];
	struct snapslot *b = &s_Slots[slotb % s_NumSlots];
	s32 diffs = 0;
	size_t pos = 0;
	report[0] = '\0';
	if (!a->valid || !b->valid) {
		return 0;
	}
	const u8 *pa = a->buf, *pb = b->buf;
	for (s32 i = 0; i < a->numranges && i < b->numranges; i++) {
		const size_t na = a->ranges[i].size, nb = b->ranges[i].size;
		const size_t n = na < nb ? na : nb;
		size_t first = (size_t)-1, count = 0;
		size_t spots[8];
		s32 numspots = 0;
		for (size_t k = 0; k < n; k++) {
			if (pa[k] != pb[k]) {
				if (first == (size_t)-1) first = k;
				count++;
				if (numspots < 8 && (numspots == 0 || k > spots[numspots - 1] + 16)) {
					spots[numspots++] = k;
				}
			}
		}
		if (count || na != nb || a->ranges[i].base != b->ranges[i].base) {
			diffs++;
			if (pos < reportlen) {
				pos += snprintf(report + pos, reportlen - pos,
					" [range %d base %p size %zu/%zu: %zu bytes differ, first at +0x%zx = %p]", i,
					(void *)a->ranges[i].base, na, nb, count, first == (size_t)-1 ? 0 : first,
					first == (size_t)-1 ? NULL : (void *)(a->ranges[i].base + first));
				for (s32 j = 0; j < numspots && pos < reportlen; j++) {
					const size_t k = spots[j] & ~(size_t)7;
					unsigned long long va = 0, vb = 0;
					if (k + 8 <= n) {
						memcpy(&va, pa + k, 8);
						memcpy(&vb, pb + k, 8);
					}
					pos += snprintf(report + pos, reportlen - pos, " @%p(%llx/%llx)", (void *)(a->ranges[i].base + spots[j]), va, vb);
				}
			}
		}
		pa += na;
		pb += nb;
	}
	return diffs;
}

size_t snapshotLastSize(s32 slot)
{
	return (s_Slots && slot >= 0) ? s_Slots[slot % s_NumSlots].used : 0;
}

f64 snapshotLastSaveMs(void)
{
	return s_LastSaveMs;
}

f64 snapshotLastLoadMs(void)
{
	return s_LastLoadMs;
}

#else

s32 snapshotSupported(void) { return 0; }
void snapshotInit(s32 numslots) { (void)numslots; }
void snapshotSave(s32 slot) { (void)slot; }
s32 snapshotLoad(s32 slot) { (void)slot; return 0; }
size_t snapshotLastSize(s32 slot) { (void)slot; return 0; }
s32 snapshotCompare(s32 a, s32 b, char *r, size_t n) { (void)a; (void)b; if (n) r[0] = 0; return 0; }
f64 snapshotLastSaveMs(void) { return 0; }
f64 snapshotLastLoadMs(void) { return 0; }

#endif
