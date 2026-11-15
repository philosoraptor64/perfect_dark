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

/* ABI versions: 1 = two machines; 2 = up to 4 players over up to 4 machines (n64_launch.players[],
 * n64_host.net_send_to/net_recv_from/net_poll_event_from, drop-out choices). i64 fills every struct's
 * abi_version with the version it implements; a module reads v2 fields only when that is >= 2, and
 * an i64 accepts v1 modules (they see a 2-machine session as before). */
#define N64_GAME_ABI_VERSION 2

/* i64's game memory arena (n64_host.arena_*; docs/game-memory-arena.md): in the
 * free low slot iOS 27 / macOS 27 leave below the system's shared region. A
 * game with fixed addresses (GoldenEye) packs them into these bounds. */
#define I64_ARENA_BASE 0x110000000ULL
#define I64_ARENA_SIZE (576ULL << 20)

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
    uint32_t max_players;          /* players (controllers) the game supports in one session, 1-4
                                      (v1 modules: humans per online session, 2) */
    uint32_t target_fps;           /* n64_game_frame() calls per second i64 paces the display
                                      link at (Smash 60; ProMotion screens must not run it at 120) */
    uint32_t flags;                /* N64_GAME_INFO_* */
    /* v2: */
    uint32_t dropout_caps;         /* N64_DROPOUT_CAN_*: what i64 may offer when a player drops out
                                      ("end the game" is always possible) */
    uint32_t max_machines;         /* devices the game supports online, 2-4 (0 = 2) */
} n64_game_info;

#define N64_DROPOUT_CAN_CPU    (1u << 0)  /* a CPU takes over the player (n64_game_cpu_options) */
#define N64_DROPOUT_CAN_REMOVE (1u << 1)  /* the player leaves the match, which goes on */
#define N64_DROPOUT_CAN_IDLE   (1u << 2)  /* the player stays, with no input */
#define N64_DROPOUT_CAN_JOIN   (1u << 3)  /* someone arriving later may take over a player a CPU or idle took
                                             over (n64_game_admit; the newcomer starts with N64_LAUNCH_JOINING) */

#define N64_GAME_INFO_OWN_THREAD (1u << 0) /* see the lifecycle note above */
#define N64_GAME_INFO_CAN_UNLOCK_ALL (1u << 1) /* the game can start with everything unlocked (N64_LAUNCH_UNLOCK_ALL) */

N64_GAME_EXPORT const n64_game_info* n64_game_get_info(void);

/* ---------------------------------------------------------------- host */

typedef enum n64_net_event {
    N64_NET_PEER_AWAY = 1,  /* the peer's app went to the background */
    N64_NET_PEER_BACK = 2,
    N64_NET_PEER_LEFT = 3,  /* left the game (back to i64's browser) or the session */
} n64_net_event;

/* v2: the most machines in a session, and the most players (controllers, all machines together) */
#define N64_MAX_MACHINES 4
#define N64_MAX_PLAYERS 4

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

    /* The game memory arena (docs/game-memory-arena.md). i64 reserves ONE block
     * of address space at launch (no memory until used), sized for any single
     * game: [I64_ARENA_BASE, I64_ARENA_BASE + I64_ARENA_SIZE). The running game
     * takes its big memory (N64 RAM, rollback/sim heaps, thread stacks, fixed
     * windows) from it; after the game stops i64 wipes the whole arena, so the
     * next game gets all of it again. A game must not keep arena memory, or
     * pointers into it, across stop()/start().
     *
     * take_reserved: claim the fixed range [addr, addr + size) (must lie inside
     * the arena and not overlap anything taken this run). Returns 1 on success:
     * the module owns it and maps it with VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE
     * (an atomic replace) or mprotect. NULL in an older i64.
     * [main thread, in start()] */
    int (*take_reserved)(uint64_t addr, uint64_t size);

    /* arena_reserve: claim size bytes (rounded up to 16 KB) anywhere in the arena,
     * aligned to align (a power of two, at least 16 KB; 0 = 16 KB). Returns the
     * address (still reserved, PROT_NONE: mprotect or map it FIXED|OVERWRITE),
     * or 0 when the arena is full: then fall back to mmap and log it. NULL in an
     * older i64. [main thread, in start()] */
    uint64_t (*arena_reserve)(uint64_t size, uint64_t align);
    uint64_t arena_base, arena_size;   /* 0 when i64 couldn't reserve its arena */

    /* v2: sessions of up to 4 machines (machine 0 = the session's host). net_send sends to every other
     * machine; net_recv returns any machine's datagram. [any thread] */
    int (*net_send_to)(uint32_t machine, const void* data, uint32_t size);   /* one machine only */
    int (*net_recv_from)(void* buf, uint32_t cap, uint32_t* machine);       /* like net_recv + who sent it */
    /* Like net_poll_event, with the machine the event is about (AWAY/BACK/LEFT). */
    int (*net_poll_event_from)(uint32_t* machine);
    int (*voice_machine_speaking)(uint32_t machine);  /* that machine's voice is audible now */
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

    /* v2 (offline too: one machine, 1-4 local controllers): */
    uint32_t num_machines;      /* devices in the session (1 offline); == num_players for v1 */
    uint32_t local_machine;     /* this device (0 = the session's host); == local_player for v1 */
    uint32_t player_count;      /* player slots in use, 1-4: players[0..player_count-1] */
    struct n64_launch_player {
        uint8_t machine;        /* which device controls this player */
        uint8_t pad;            /* that device's local controller index (0 = its first) */
        uint8_t local;          /* 1 if it is this device */
        uint8_t reserved;
    } players[N64_MAX_PLAYERS];  /* player number = index; ordered by machine (the host's first, then
                                    join order), each machine's controllers consecutive */
    const char* machine_names[N64_MAX_MACHINES];  /* players' chosen names, per machine */
} n64_launch;

/* i64 drives n64_game_frame() from a display link at target_fps: the module
 * switches off its own frame pacing (sleeping to its frame schedule). */
#define N64_LAUNCH_HOST_PACED (1u << 0)
/* v2: i64 shows the session's host the drop-out menu and calls n64_game_dropout when a machine leaves a game
 * of more than two machines (the others wait for the decision, up to 2 minutes). Without it a module ends the
 * match on a drop-out, as nobody would decide. */
#define N64_LAUNCH_DROPOUT_MENU (1u << 1)
/* v2: this machine joins the game while it runs, in the place of machine local_machine (whose device left;
 * a CPU or idle player took its players over). The launch is the one everyone started with (seed, session
 * blob, player table with this machine in that place). The module catches up (n64net_input: config.joining,
 * fast-forward through the host's input log; no pacing, drawing or sound needed) and its players become
 * human again at the tick the host announces. */
#define N64_LAUNCH_JOINING (1u << 2)
/* everything unlocked (characters, modes, stages, options: what the game would otherwise unlock by playing), from
 * i64's per-game setting; online the host's choice, the same on every machine. The module makes it part of the
 * game's starting state and never writes it into the player's save file: an unlocked run keeps a save of its own
 * (recomp runtime: recomp_save_subfolder "unlocked"), so its progress is kept too. */
#define N64_LAUNCH_UNLOCK_ALL (1u << 3)

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
/* Optional. Right after n64_game_frame() returned N64_GAME_FRAME_EXIT: why the
 * game ended, for the player (e.g. "The games got out of step, so the match
 * was stopped."). Writes "" when it ended normally (the player quit). i64 shows
 * it in its menu. */
N64_GAME_EXPORT void n64_game_exit_message(char* out, size_t cap);

/* ---------------------------------------------------------------- drop-outs (v2) */

/* When a machine leaves a running online game, i64 pauses every machine (as for set_paused) and shows
 * the session's host a menu built from dropout_caps: a CPU takes over (with the options below), the
 * player is removed, stays idle, or the game ends. Guests see "<host> is deciding...". */

typedef enum n64_dropout_action {
    N64_DROPOUT_END = 0,     /* end the game for everyone (i64 stops it; no call) */
    N64_DROPOUT_CPU = 1,
    N64_DROPOUT_REMOVE = 2,
    N64_DROPOUT_IDLE = 3,
} n64_dropout_action;

typedef struct n64_cpu_choice {
    const char* label;        /* "Difficulty", "Character" */
    uint32_t count;           /* options, 2-16 */
    const char* options[16];  /* "Easy", "Normal", "Hard" */
    uint32_t initial;         /* preselected option */
} n64_cpu_choice;

typedef struct n64_cpu_options {
    uint32_t choice_count;    /* 0-3: i64 shows one picker per choice, in this order */
    n64_cpu_choice choices[3];
} n64_cpu_options;

/* Optional, with N64_DROPOUT_CAN_CPU: what i64's CPU dialog offers for `player` (e.g. Smash: "Level",
 * 1-9; MK: "Difficulty"). Returns 1 (out filled) or 0 = this player can't become a CPU now. Called on
 * the host, on the main thread, while the game is paused. */
N64_GAME_EXPORT int n64_game_cpu_options(uint32_t player, n64_cpu_options* out);

/* Optional (the host's module only): the host chose `action` for `player` (selection[i] = option of
 * choice i for N64_DROPOUT_CPU). The module makes the change deterministic: it sends the choice as an
 * input event (n64net_input_set_event) on its next tick, and every machine applies it when it plays
 * that tick, after the leaver's last input (the module stops waiting for the leaver at that tick).
 * Returns 0, or nonzero if it can't (i64 then ends the game). i64 resumes every machine after it. */
N64_GAME_EXPORT int n64_game_dropout(uint32_t player, n64_dropout_action action, const uint32_t* selection,
                                     uint32_t selection_count);

/* Optional, with N64_DROPOUT_CAN_JOIN (the module of the machine that started the game): a new device takes
 * over `machine`'s players (that machine left; they are CPUs or idle now). The new device starts the game
 * with N64_LAUNCH_JOINING; this module brings it up to date (n64net_input_admit). Returns 0, or nonzero if
 * it can't now. */
N64_GAME_EXPORT int n64_game_admit(uint32_t machine);

#ifdef __cplusplus
}
#endif
