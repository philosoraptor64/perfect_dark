/*
 * Perfect Dark as an i64 game module (port/include/i64/n64_game.h). Built only
 * with -DPD_I64_MODULE=ON, which makes the game a shared library exporting just
 * the n64_game_* functions (port/i64/exports.txt).
 *
 * PD's port runs everything on one thread: main() initialises, then mainProc()
 * loops forever (stage loads, then a tick at a time). Inside i64 that loop
 * runs as a coroutine on its own stack, still on i64's main thread (SDL, UIKit
 * and Metal see the same thread as in the stand-alone app): n64_game_frame()
 * switches to it, and the main loop switches back after every iteration
 * (pdmainHostYield, pdmain.c). Stopping drops the coroutine where it waits
 * (C code, no locks held there), releases audio/video/input/heaps, and puts
 * every global back to its value at the first start (__DATA and the __PDSNAP
 * segment, copied before any game code ran), so the module can start again.
 *
 * i64 provides the window (SDL Metal window: PD adds only its own Metal view),
 * the audio output (22020 Hz stereo through host->audio_write) and the data
 * folder (--basedir/--savedir = host->data_dir: the ROM pd.ntsc-final.z64,
 * pd.ini, saves).
 *
 * Online: pdnet's external mode on i64's game channel; without a session blob
 * (i64 doesn't send one yet) the host's save comes over the channel. Pause:
 * the game only runs when i64 calls frame(), so it waits by itself.
 */
#if defined(PD_I64_MODULE)

#define N64_GAME_BUILDING_MODULE 1
#include "i64/n64_game.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

#include "pdnet_build_id.h"

#define PD_KEEP __attribute__((section("__DATA,__pdkeep")))

/* game side */
extern int pdModuleMain(int argc, const char **argv);   /* main.c: init + mainProc, never returns */
extern void pdModuleShutdown(void);                     /* main.c: release audio/video/input/heaps */
extern void audioSetHostOutput(void (*write)(const short *, unsigned, unsigned), unsigned (*buffered)(unsigned));
extern void *g_gfxExternalWindow;                       /* gfx_sdl2.cpp */
extern volatile int g_PdEmbedded;                       /* pdmain.c */
extern volatile int g_PdQuitRequested;                  /* gfx_sdl2.cpp / system.c */

/* pdnet.h (same layout) */
struct pdnetexternal {
	int32_t localplayer;
	uint64_t seed;
	const void *blob;
	uint32_t blobsize;
	int (*send)(const void *data, uint32_t size);
	int (*recv)(void *buf, uint32_t cap);
	int32_t hostpaced;
};
extern struct pdnetexternal *g_PdNetExternal;
extern void pdnetExternalEvent(int32_t event);

/* pdcoro_arm64.S */
extern void pdcoro_switch(void **save_sp, void *load_sp);
extern void pdcoro_trampoline(void);

static const n64_game_rom kRoms[] = {
	{ "af8788ac4d1a57260eae9c53ffe851fcf2a3319b", "USA (Rev 1)" },
};

static const n64_game_info kInfo = {
	N64_GAME_ABI_VERSION,
	"pd-us",
	"Perfect Dark",
	kRoms,
	1,
	PDNET_BUILD_ID,
	2,
	60,
	0,
};

/* ---------------------------------------------------------------- re-entry */

typedef struct { uint8_t *base; size_t size; uint8_t *copy; } Piece;
#define MAX_PIECES (1 << 16)
PD_KEEP static Piece *sPieces;
PD_KEEP static int sPieceCount;
PD_KEEP static int sSaved;
extern const struct mach_header_64 __dso_handle;

static int allZero(const uint8_t *p, size_t n)
{
	const uint64_t *w = (const uint64_t *)p;
	for (size_t i = 0; i < n / 8; i++) if (w[i]) return 0;
	return 1;
}

static void addPiece(uint8_t *base, size_t size, int zero)
{
	if (!size || sPieceCount >= MAX_PIECES) return;
	Piece *p = &sPieces[sPieceCount++];
	p->base = base;
	p->size = size;
	p->copy = NULL;
	if (!zero) {
		p->copy = malloc(size);
		memcpy(p->copy, base, size);
	}
}

static void saveSection(const char *seg, const char *sect)
{
	unsigned long size = 0;
	uint8_t *base = getsectiondata(&__dso_handle, seg, sect, &size);
	if (!base || !size) return;
	const uintptr_t page = (uintptr_t)getpagesize();
	uintptr_t start = (uintptr_t)base, end = start + size;
	uintptr_t first = (start + page - 1) & ~(page - 1), last = end & ~(page - 1);
	if (first >= last) { addPiece(base, size, 0); return; }
	addPiece(base, first - start, 0);
	uintptr_t runStart = first;
	int runZero = allZero((const uint8_t *)first, page);
	for (uintptr_t p = first + page; p < last; p += page) {
		int z = allZero((const uint8_t *)p, page);
		if (z != runZero) {
			addPiece((uint8_t *)runStart, p - runStart, runZero);
			runStart = p;
			runZero = z;
		}
	}
	addPiece((uint8_t *)runStart, last - runStart, runZero);
	addPiece((uint8_t *)last, end - last, 0);
}

static void savePristine(void)
{
	if (sSaved) return;
	sSaved = 1;
	sPieces = calloc(MAX_PIECES, sizeof(Piece));
	saveSection("__DATA", "__data");
	saveSection("__DATA", "__bss");
	saveSection("__DATA", "__common");
	saveSection("__PDSNAP", "__data");   /* the game's globals (rollback snapshot segment) */
	saveSection("__PDSNAP", "__bss");
}

static void restorePristine(void)
{
	for (int i = 0; i < sPieceCount; i++) {
		const Piece *p = &sPieces[i];
		if (p->copy) {
			memcpy(p->base, p->copy, p->size);
		} else if (mmap(p->base, p->size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
			memset(p->base, 0, p->size);
		}
	}
}

/* --------------------------------------------------------------- coroutine */

#define GAME_STACK_SIZE (16u << 20)
#define GUARD_SIZE (64u << 10)
PD_KEEP static const n64_host *sHost;
PD_KEEP static int sRunning;
PD_KEEP static uint8_t *sStack;          /* guard page + stack */
PD_KEEP static void *sHostSp, *sGameSp;
PD_KEEP static int sInGame;              /* 1 while the coroutine runs */
PD_KEEP static void (*sAudioWrite)(const int16_t *, uint32_t, uint32_t);
PD_KEEP static uint32_t (*sAudioBuffered)(uint32_t);
PD_KEEP static char sDataDir[1024];

/* the game gives control back to i64 (pdmain.c, once per main-loop pass) */
void pdmainHostYield(void)
{
	if (!sInGame) return;
	sInGame = 0;
	pdcoro_switch(&sGameSp, sHostSp);
	sInGame = 1;
}

static void gameEntry(void *arg)
{
	(void)arg;
	const char *argv[] = { "pd", "--basedir", sDataDir, "--savedir", sDataDir, "--no-crash-handler", NULL };
	pdModuleMain(6, argv);
	for (;;) {   /* never: mainProc loops forever */
		g_PdQuitRequested = 1;
		pdmainHostYield();
	}
}

static void *prepareStack(void)
{
	uint8_t *top = sStack + GUARD_SIZE + GAME_STACK_SIZE;
	top = (uint8_t *)((uintptr_t)top & ~(uintptr_t)15);
	uint64_t *frame = (uint64_t *)(top - 160);
	memset(frame, 0, 160);
	frame[0] = (uint64_t)(uintptr_t)gameEntry;          /* x19 */
	frame[1] = 0;                                      /* x20 = arg */
	frame[10] = 0;                                     /* x29 */
	frame[11] = (uint64_t)(uintptr_t)pdcoro_trampoline; /* x30: where the first switch returns to */
	return frame;
}

static void runGame(void)
{
	sInGame = 1;
	pdcoro_switch(&sHostSp, sGameSp);
}

/* ------------------------------------------------------------------- audio */

static void hostAudioWrite(const short *stereo, unsigned frames, unsigned rate)
{
	if (sAudioWrite) sAudioWrite((const int16_t *)stereo, frames, rate);
}
static unsigned hostAudioBuffered(unsigned rate)
{
	return sAudioBuffered ? sAudioBuffered(rate) : 0;
}

/* -------------------------------------------------------------------- ABI */

N64_GAME_EXPORT const n64_game_info *n64_game_get_info(void)
{
	return &kInfo;
}

static int mkdirs(const char *path)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
			*p = '/';
		}
	}
	return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

/* The ROM is all PD needs: stored big-endian as pd.ntsc-final.z64 */
N64_GAME_EXPORT int n64_game_import_rom(const n64_host *host, const char *rom_path,
                                        void (*progress)(float, void *), void *user, char *err, size_t err_size)
{
	savePristine();
	if (!host || !rom_path || !host->data_dir || !host->data_dir[0]) {
		snprintf(err, err_size, "missing folders");
		return 1;
	}
	if (sRunning) {
		snprintf(err, err_size, "the game is running");
		return 1;
	}
	if (progress) progress(0.0f, user);
	FILE *f = fopen(rom_path, "rb");
	if (!f) { snprintf(err, err_size, "can't read the ROM"); return 1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < (8L << 20) || n > (64L << 20) || (n % 4) != 0) {
		fclose(f);
		snprintf(err, err_size, "that file isn't an N64 ROM");
		return 1;
	}
	uint8_t *rom = malloc((size_t)n);
	if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(rom);
		snprintf(err, err_size, "can't read the ROM");
		return 1;
	}
	fclose(f);
	if (!(rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)) {
		if (rom[0] == 0x37 && rom[1] == 0x80) {
			for (long i = 0; i + 1 < n; i += 2) { uint8_t t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
		} else if (rom[0] == 0x40 && rom[1] == 0x12) {
			for (long i = 0; i + 3 < n; i += 4) {
				uint8_t a = rom[i], b = rom[i + 1];
				rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
			}
		} else {
			free(rom);
			snprintf(err, err_size, "that file isn't an N64 ROM");
			return 1;
		}
	}
	char path[1100];
	if (mkdirs(host->data_dir) != 0) { free(rom); snprintf(err, err_size, "can't create the game's data folder"); return 1; }
	snprintf(path, sizeof(path), "%s/pd.ntsc-final.z64", host->data_dir);
	FILE *o = fopen(path, "wb");
	int ok = o && fwrite(rom, 1, (size_t)n, o) == (size_t)n;
	if (o) ok = (fclose(o) == 0) && ok;
	free(rom);
	if (!ok) { remove(path); snprintf(err, err_size, "not enough space to import the ROM"); return 1; }
	if (progress) progress(1.0f, user);
	return 0;
}

N64_GAME_EXPORT uint32_t n64_game_session_blob(const n64_host *host, void *out, uint32_t cap)
{
	(void)host; (void)out; (void)cap;
	return 0;   /* online runs not yet */
}

N64_GAME_EXPORT int n64_game_start(const n64_host *host, const n64_launch *launch, char *err, size_t err_size)
{
	if (sRunning) { snprintf(err, err_size, "already running"); return 1; }
	if (!host || !launch || host->abi_version != N64_GAME_ABI_VERSION || launch->abi_version != N64_GAME_ABI_VERSION) {
		snprintf(err, err_size, "i64 and this game module disagree on the interface version");
		return 1;
	}
	if (launch->mode == N64_LAUNCH_ONLINE && (!host->net_send || !host->net_recv || launch->local_player > 1)) {
		snprintf(err, err_size, "online needs i64's game channel and 2 players");
		return 1;
	}
	if (!host->data_dir || !host->data_dir[0]) { snprintf(err, err_size, "missing folders"); return 1; }
	{
		char p[1100];
		struct stat st;
		snprintf(p, sizeof(p), "%s/pd.ntsc-final.z64", host->data_dir);
		if (stat(p, &st) != 0) {
			snprintf(err, err_size, "import your Perfect Dark ROM first");
			return 1;
		}
	}
	savePristine();   /* first start: everything so far is static initialisation */
	sHost = host;
	sAudioWrite = host->audio_write;
	sAudioBuffered = host->audio_buffered;
	snprintf(sDataDir, sizeof(sDataDir), "%s", host->data_dir);
	g_gfxExternalWindow = host->window;
	g_PdEmbedded = 1;
	if (host->audio_write) audioSetHostOutput(hostAudioWrite, hostAudioBuffered);
	static struct pdnetexternal ext;   /* reset with the globals after the run */
	if (launch->mode == N64_LAUNCH_ONLINE) {
		/* the save and settings travel over the channel (no session blob from i64 yet);
		 * i64 calls the frames at 60 Hz and the game ticks once per frame */
		memset(&ext, 0, sizeof(ext));
		ext.localplayer = (int32_t)launch->local_player;
		ext.seed = launch->seed;
		ext.blob = launch->session_blob;
		ext.blobsize = launch->session_blob_size;
		ext.send = host->net_send;
		ext.recv = host->net_recv;
		ext.hostpaced = 1;
		g_PdNetExternal = &ext;
	}
	if (!sStack) {
		sStack = mmap(NULL, GUARD_SIZE + GAME_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (sStack == MAP_FAILED) { sStack = NULL; snprintf(err, err_size, "out of memory"); return 1; }
		mprotect(sStack, GUARD_SIZE, PROT_NONE);
	}
	sGameSp = prepareStack();
	sRunning = 1;
	runGame();   /* boots up to the first pass of the main loop */
	if (g_PdQuitRequested) {
		snprintf(err, err_size, "the game couldn't start (see pd.log in its data folder)");
		n64_game_stop();
		return 1;
	}
	return 0;
}

N64_GAME_EXPORT n64_frame_result n64_game_frame(void)
{
	if (!sRunning || g_PdQuitRequested) return N64_GAME_FRAME_EXIT;
	if (sHost && sHost->net_poll_event) {
		int ev;
		while ((ev = sHost->net_poll_event()) != 0) pdnetExternalEvent(ev);   /* same numbers */
	}
	runGame();
	return g_PdQuitRequested ? N64_GAME_FRAME_EXIT : N64_GAME_FRAME_CONTINUE;
}

N64_GAME_EXPORT void n64_game_set_paused(int paused)
{
	(void)paused;   /* the game only runs when i64 calls frame(): it waits by itself */
}

N64_GAME_EXPORT void n64_game_stop(void)
{
	if (!sRunning) return;
	pdModuleShutdown();   /* on the host stack: the coroutine is parked at a yield */
	restorePristine();
	sRunning = 0;
	sHost = NULL;
	sAudioWrite = NULL;
	sAudioBuffered = NULL;
}

#endif /* PD_I64_MODULE */
