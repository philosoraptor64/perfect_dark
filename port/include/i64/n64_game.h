/*
 * n64_game.h: the interface between i64 (the launcher) and a game module.
 * DRAFT v1 (MacBook agent, 2026-11-09; v0 reviewed by the Studio agent).
 *
 * A game module (Smash, Mario Kart, ...) is a sealed embedded framework (iOS)
 * or dylib (Mac) built with -fvisibility=hidden that exports only the
 * functions marked N64_GAME_EXPORT below. Everything else (the game, its
 * decomp, its libultraship fork, ImGui, Torch) is linked inside it. i64
 * provides SDL2, the audio output, the network session and voice.
 *
 * Lifecycle (all calls on i64's main thread unless stated):
 *   n64_game_get_info()              any time, no side effects
 *   n64_game_import_rom()            on a worker thread, while no game runs
 *   n64_game_start()                 the game initialises on host->window
 *   n64_game_frame()  ... repeatedly  driven by i64's display link (one call
 *                                    per displayed frame) until it returns
 *                                    N64_GAME_FRAME_EXIT
 *   n64_game_stop()                  back to a clean slate; start() may be
 *                                    called again later in the same process
 * A module is loaded on first use and never unloaded (static destructors in
 * the ports aren't safe to run).
 *
 * SDL: i64 calls SDL_Init(VIDEO | GAMECONTROLLER | EVENTS) once and never
 * quits it, and sets the process-wide hints once (controller buttons by
 * position: SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=0; accelerometer not a
 * joystick). Modules may SDL_InitSubSystem/SDL_QuitSubSystem (refcounted) but
 * never SDL_Quit, must not change those hints, and pump SDL events only inside
 * n64_game_frame(); i64's own UI doesn't consume SDL events while a game runs.
 *
 * Threads: a module may run its own threads between start() and stop()
 * (audio mixing, resource loading) but must join them all in stop(). A
 * module whose loop can't be split into frames may run it on its own thread
 * and make frame() a no-op that only reports whether it is still running,
 * but rendering must then happen on that thread with i64's consent
 * (N64_GAME_INFO_OWN_THREAD); the default is frames on the main thread.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64_GAME_ABI_VERSION 1

#if defined(N64_GAME_BUILDING_MODULE)
#define N64_GAME_EXPORT __attribute__((visibility("default")))
#else
#define N64_GAME_EXPORT
#endif

/* ---------------------------------------------------------------- info */

typedef struct n64_game_rom {
    const char* sha1;  /* lowercase hex SHA-1 of the big-endian (.z64) dump */
    const char* label; /* "USA", "USA 1.0", ... (shown when a file doesn't match) */
} n64_game_rom;

typedef struct n64_game_info {
    uint32_t abi_version;          /* N64_GAME_ABI_VERSION */
    const char* id;                /* "mk64-us", "ssb64-us": folder names, game ids on the wire */
    const char* title;             /* "Mario Kart 64" */
    const n64_game_rom* roms;      /* dumps this module accepts */
    uint32_t rom_count;
    const char* netplay_build_id;  /* simulation fingerprint: peers must match exactly */
    uint32_t max_players;          /* humans per online session (2 today) */
    uint32_t target_fps;           /* n64_game_frame() calls per second i64 paces the display
                                      link at (Smash 60; ProMotion screens must not run it at 120) */
    uint32_t flags;                /* N64_GAME_INFO_* */
} n64_game_info;

#define N64_GAME_INFO_OWN_THREAD (1u << 0) /* see the lifecycle note above */

N64_GAME_EXPORT const n64_game_info* n64_game_get_info(void);

/* ---------------------------------------------------------------- host */

typedef enum n64_net_event {
    N64_NET_PEER_AWAY = 1,  /* the peer's app went to the background */
    N64_NET_PEER_BACK = 2,
    N64_NET_PEER_LEFT = 3,  /* left the game (back to i64's browser) or the session */
} n64_net_event;

/* Everything a module may use from i64. Function pointers may be called from
 * the main thread; those marked [any thread] also from the module's threads. */
typedef struct n64_host {
    uint32_t abi_version;

    /* Folders (UTF-8, no trailing slash) */
    const char* data_dir;    /* this game's writable data: config, saves, extracted assets
                                (Application Support/<id>, not shown in Files) */
    const char* bundle_dir;  /* the module's read-only resources (e.g. spaghetti.o2r) */

    /* Window: i64 creates the SDL window (and on iOS its view in i64's scene);
     * the module creates its renderer on it and must not destroy it. */
    void* window;            /* SDL_Window* */

    /* Audio [any thread]: interleaved stereo s16 at `rate`. i64 mixes it with
     * voice and plays it through the echo-cancelling unit. Modules never open
     * an audio device or touch the audio session. */
    void (*audio_write)(const int16_t* stereo, uint32_t frames, uint32_t rate);
    uint32_t (*audio_buffered)(uint32_t rate); /* frames queued, at `rate` */

    /* Network: the game channel of i64's session, for this run only. Datagrams
     * up to net_max_datagram bytes, unreliable and unordered like UDP; i64 tags
     * them with the run number and drops late ones from an earlier run. Bulk
     * data the guests must adopt travels in the session blob instead (reliable).
     * [any thread] */
    uint32_t net_max_datagram;
    int (*net_send)(const void* data, uint32_t size);
    int (*net_recv)(void* buf, uint32_t cap);  /* one datagram: size, 0 if none */
    /* Returns the next session event (0 = none); poll once per frame. */
    int (*net_poll_event)(void);

    /* Voice (i64 runs it all session; these serve in-game buttons/indicators) */
    void (*voice_set_muted)(int muted);
    void (*voice_set_talking)(int talking);  /* push-to-talk button held */
    int (*voice_local_speaking)(void);
    int (*voice_peer_speaking)(void);

    /* Box art: the module offers a picture of its title screen the first time
     * it shows it in a run (rgba, w*h*4 bytes, top row first); i64 keeps it as
     * the game's art unless the player picked their own. i64 may also do a
     * short offline run right after import to get it ("Preparing..."). */
    void (*offer_title_art)(const uint8_t* rgba, uint32_t w, uint32_t h);

    /* The player chose "Quit to i64" in the game's own menu: the module
     * finishes the frame and returns N64_GAME_FRAME_EXIT soon after. */
    void (*request_exit)(void);

    /* Global C++ allocation hooks (see n64_alloc_hooks). */
    void (*set_alloc_hooks)(const struct n64_alloc_hooks* hooks); /* NULL = back to malloc */

    void (*log)(const char* fmt, ...) __attribute__((format(printf, 1, 2))); /* [any thread] */
} n64_host;

/* i64's executable defines the global operator new/delete and forwards them
 * here. Mario Kart's rollback keeps its C++ simulation objects in its own
 * heap by routing operator new while race logic runs; with a sealed module,
 * libc++.dylib's out-of-line code would otherwise allocate and free with a
 * different operator new than the module's inline code. Only the running
 * module may set hooks (in start(); i64 clears them after stop()), and the
 * hooks must behave like malloc/free for every caller outside the module's
 * own routed scope: i64's code keeps running on the main thread between
 * frames with the hooks installed. [any thread] */
typedef struct n64_alloc_hooks {
    void* (*alloc)(size_t size);                  /* never NULL */
    void* (*alloc_aligned)(size_t size, size_t alignment);
    void (*free)(void* p);                        /* must accept memory from malloc too */
} n64_alloc_hooks;

/* ---------------------------------------------------------------- ROMs */

/* Extracts the game's assets from `rom_path` (any byte order; i64 checked the
 * SHA-1 after normalising) into host->data_dir. Runs on a worker thread while
 * no game runs. `progress` (may be NULL) gets 0..1. Returns 0 on success,
 * else writes a short reason for the player into err. */
N64_GAME_EXPORT int n64_game_import_rom(const n64_host* host, const char* rom_path,
                                        void (*progress)(float done, void* user), void* user,
                                        char* err, size_t err_size);


/* ---------------------------------------------------------------- runs */

typedef enum n64_launch_mode {
    N64_LAUNCH_OFFLINE = 0,
    N64_LAUNCH_ONLINE = 1,
} n64_launch_mode;

typedef struct n64_launch {
    uint32_t abi_version;
    n64_launch_mode mode;
    /* online only: */
    uint32_t local_player;      /* 0 = the session's host (player 1), 1 = guest, ... */
    uint32_t num_players;       /* humans in the session */
    uint32_t run;               /* i64's run number (also tags the game channel) */
    uint32_t seed;              /* chosen by the session host, the same for everyone */
    const void* session_blob;   /* everything the guests adopt from the host for this run, */
    uint32_t session_blob_size; /* opaque to i64 (see n64_game_session_blob) */
    const char* peer_name;      /* the other player's name, for "Waiting for ..." (2 players) */
    uint32_t flags;             /* N64_LAUNCH_* */
} n64_launch;

/* i64 drives n64_game_frame() from a display link at target_fps: the module
 * switches off its own frame pacing (sleeping to its frame schedule). */
#define N64_LAUNCH_HOST_PACED (1u << 0)

#define N64_GAME_SESSION_BLOB_MAX (64 * 1024)

/* Online, on the session host before a run: the module packs everything the
 * guests must adopt (gameplay-changing settings, rules, save data/unlocks...;
 * MK: CPU difficulty, custom cc, item and vehicle options) into `out` (cap =
 * N64_GAME_SESSION_BLOB_MAX). i64 delivers it reliably to the guests (control
 * channel, fragmented and acknowledged) and hands it to every
 * n64_game_start(), the host's included. Returns the size (0 = none). */
N64_GAME_EXPORT uint32_t n64_game_session_blob(const n64_host* host, void* out, uint32_t cap);

/* Starts the game on host->window. Returns 0, or nonzero (err filled) if the
 * game can't start (e.g. assets missing): i64 stays in the browser. */
N64_GAME_EXPORT int n64_game_start(const n64_host* host, const n64_launch* launch, char* err, size_t err_size);

typedef enum n64_frame_result {
    N64_GAME_FRAME_CONTINUE = 0,
    N64_GAME_FRAME_EXIT = 1,  /* the player quit, the session ended, or request_exit() */
} n64_frame_result;

/* One game frame: input, game logic, rendering, present. Never blocks:
 * waiting (for the peer, a handshake, loading) means returning and trying
 * again next frame. */
N64_GAME_EXPORT n64_frame_result n64_game_frame(void);

/* i64 keeps the game while its players are in i64's menu: 1 = this player
 * went back to i64, 2 = the friend did, 0 = back into the game. Online, both
 * sides tell each other (no one gives up on the silence meanwhile). */
N64_GAME_EXPORT void n64_game_set_paused(int paused);

/* Optional. After set_paused(1 or 2), i64 keeps calling frame() until this
 * returns 1 (at most ~1.5 s), then stops: a game can use those frames to open
 * its own pause menu, so it is already paused when the players come back.
 * Without it, i64 stops calling frame() at once. */
N64_GAME_EXPORT int n64_game_pause_ready(void);

/* Back to a clean slate: join threads, free game heaps, restore globals, flush
 * saves, release the renderer (not the window; on iOS also remove the Metal
 * view/layer it attached, e.g. SDL_Metal_DestroyView, so the next start
 * doesn't stack a second one). */
N64_GAME_EXPORT void n64_game_stop(void);

#ifdef __cplusplus
}
#endif
