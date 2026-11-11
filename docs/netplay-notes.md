# Netplay notes (philosoraptor64 fork)

Goal: deterministic input-exchange netplay (lockstep, then rollback) through
[n64net](https://github.com/philosoraptor64/n64net), Mac and iPhone first, the same
approach as our BattleShip (Smash 64) and SpaghettiKart (Mario Kart 64) forks.

## Status (2026-11-10)
- Branch `netplay` = upstream `port` at `a4b58e6dd` + `PD_STRICT_FP` (default ON, `-ffp-contract=off`).
- Builds unchanged on macOS 27 / Apple Silicon (Homebrew SDL2):
  `cmake -G"Unix Makefiles" -Bbuild -DCMAKE_OSX_ARCHITECTURES=arm64 . && cmake --build build --target pd -j12`
  -> `build/pd.arm64`. Put the US v1.1 ROM (SHA-1 af8788ac4d1a57260eae9c53ffe851fcf2a3319b) at
  `build/data/pd.ntsc-final.z64` (never commit it). The ROM is read at runtime: no asset extraction.
- Boots and runs (OpenGL 4.1 core context; "could not find function glVertexP*" errors are harmless).
  Not yet looked at or played by a human.

## Upstream netplay (`port-net` branch)
Upstream has an experimental **server-authoritative state-sync** netplay (`docs/netplay.md` on
`port-net`; Combat Sim only, up to 8 players, "really bad netcode", sims don't work, last updated
2026-05). Different model from ours (we exchange inputs and keep the simulation deterministic).
Decide before building: (a) our input-exchange approach on `port` (consistent with the other games,
i64, rollback), or (b) build on `port-net`. Recommendation: (a), but read port-net for the menu/lobby
code and what had to be synced.

## Determinism findings
1. **Variable logic step (the main hurdle).** `frametimeCalculate` (`src/game/timing.c`) derives
   `diffframe60/240` (and `lvupdate240` etc.) from `osGetCount()`, i.e. wall-clock time, so each tick
   advances the simulation by however much real time passed. Netplay must force a fixed step (e.g.
   diffframe60 = `g_TickRateDiv`/1 and diffframe240 = 4x, lostframetime = 0) whenever netplay or a
   replay check is active, and run exactly one logic update per network tick. Check the render/
   interpolation path stays separate.
2. **RNG seeded from the clock:** `rngSetSeed(osGetCount())` in `port/src/pdmain.c:344` (main RNG
   `g_RngSeed`, `src/lib/rng_c.c`). Second RNG `g_Rng2Seed` (`src/game/rng2_c.c`) is reseeded by game
   code. Both must come from the match config.
3. **Clock in AI:** `aiIfMusicEventQueueIsEmpty` (`src/game/chraicommands.c` ~9845) reads `osGetCount()`
   (value looks unused off-N64, verify). `src/game/weather.c` and `pak.c` read it for timing/profiling.
4. **FP:** Release builds use `-Og` (upstream's workaround for -O2 issues) plus now `-ffp-contract=off`.
   Trig comes from the system libm: fine Mac <-> iPhone, a risk vs. PC.
5. Graphics is OpenGL only: see "iPhone build" below.

## Deterministic mode + replay check (2026-11-10, MacBook)
`port/include/replaycheck.h` / `port/src/replaycheck.c`. `PD_DETERMINISTIC=1` (implied by any PD_REPLAY_* var):
fixed logic step (timing.c: diffframe60 = 1, diffframe240 = 4, no lost time; real-time pacing unchanged),
RNG seed = `PD_SEED` (default 1) instead of the clock (pdmain.c), the music sequencer renders a fixed 368
samples per tick (audiomgr.c; it used to follow the audio output queue level, and the AI command
`aiIfMusicEventQueueIsEmpty` branches on the music event queue; the queue limit in audio.c drops a buffer now and
then instead), mouse ignored (input.c; its deltas bypass the pads). Replay: `PD_REPLAY_CHECK=<file>` (per tick:
stage, lvframe, prop count, RNG, hash of both RNG seeds, players' health/angles/pos, chrs' action/damage/pos, active
props' type/pos), `PD_REPLAY_RECORD=<file>` / `PD_REPLAY_PLAY=<file>` (4 pads per tick incl. right stick and errnum),
`PD_REPLAY_FRAMES`. In replay mode pad 1 always reads as connected, 2-4 absent (osContGetQuery/GetReadData).
Test: `tools/replay/replay-test.sh` (scripted P1 in Defection: skip intro, walk/turn/strafe/fire; fresh `--savedir`
per run with TickRateDivisor=0 = uncapped; dummy audio; window kept in the background; `RECORD=1` = record a human
run; `INPUT=`, `STAGE=`, `TICKS=`, `FAST=0`). **Result:** 3 playbacks x 2430 ticks byte-identical, a real-speed
(60/s) playback identical to the uncapped ones, seed 2 diverges at tick 0. Not yet covered: a human recording,
Combat Sim with sims, the iPhone (cross-device), menus. Gotchas: one START too many opens the pause menu (the
scripted A press then aborts the mission); a relative INPUT path made the game stop at an error dialog (fixed).
Not deterministic yet / to check for netplay: per-player control settings (sensitivity, crosshair, aim assist
options in pd.ini) must come from the match config; `TickRateDivisor` must be 1 online; camdraw.c func0f14b394
reads the clock (looks like camera-spy/photo code: verify it can't feed the sim); joy.c pak polling uses the clock.

**Cross-device (2026-11-10): Duo (iPhone19,4) == Mac, 2430 ticks byte-identical** after fixing the aspect: the
camera aspect fed the simulation (player0f0bd358 scales by videoGetAspect; aim ray through the crosshair, on-screen
flags), so the first phone run differed in one explosion's position (tick 1037-2075; RNG and prop counts stayed equal),
and a Mac 640x480 vs 1017x699 run differed the same way. Fix: `gfx_fixed_aspect` (gfx_pc.cpp) renders the game at a
fixed aspect, centred with black bars (the existing game-window-viewport offsets); deterministic mode sets it to 16:9
(`PD_ASPECT=w/h` overrides; must come from the match config online). The HUD aspect scale now follows the game
rectangle, not the window. iOS: replay paths and `--savedir` may be relative = relative to Documents (the container
path changes with every install). Phone run: copy input.txt + a fresh save folder (pd.ini only, no eeprom.bin) to
Documents/replay/, then `devicectl device process launch --environment-variables
'{"PD_SEED":"1","PD_REPLAY_PLAY":"replay/input.txt","PD_REPLAY_CHECK":"replay/phone.hash"}'
io.github.philosoraptor.perfectdark --savedir replay/save2 --boot-stage 0x30`, it quits by itself (~40 s).
Also: `PD_REPLAY_DUMP=from:to` writes every player/chr/prop with exact (%a) values for those ticks.

## Combat Sim replay check (2026-11-10 night, MacBook)
**Mac: done** (3 playbacks x 2430 ticks identical on Skedar vs 3 normal simulants; they roam the map and kill
the player by tick 2400; seed 2 diverges). **Duo == Mac: 2430 ticks identical** (phone command as above plus
`"PD_REPLAY_BOTS":"3"`, `--savedir replay/save3 --boot-stage 0x32`).

## 2-player netplay: lockstep WORKS on the Mac (2026-11-11, Studio)
- n64net submodule `external/n64net` pinned at `0e4d8af` (n64net_pad now has rstick_x/rstick_y and buttons_hi =
  button bits 16-31, sent in an extended frame only when non-zero; other games' packets unchanged). Only
  `client/src/n64net_input.cpp` is compiled (CMake adds it + the include dir).
- `port/src/pdnet.c` / `port/include/pdnet.h` (env vars documented there): PD_NET_HOST=1 / PD_NET_PEER=ip:port,
  PD_NET_PORT, PD_NET_DELAY (min; auto delay up to 6), PD_NET_BOT=seed (scripted local input incl. right stick +
  fire), PD_NET_WAIT_SECS, PD_NET_TEST_DESYNC=tick. Netplay turns on deterministic mode; the handshake ('PDNH'
  HELLO/WELCOME/MISMATCH on the game socket) compares an FNV of seed, stage, bots, aspect and VERSION_HASH; the
  host learns the guest's address from HELLO. Hooks: replaycheck.c (pdnetInit at the end of replayInit;
  replayFilterPads/Status hand over to pdnet; replayEndTick -> pdnetEndTick with the state hash); pdmain.c
  numplayers = 2; replaySetupMatch chrslots 0x3 online. Inputs are exchanged once per tick (first pad read of the
  tick; later reads in the same tick get the cached pads; ticks without a read still get an entry); pads 1/2 =
  host/guest, 3/4 absent; each side samples its own controller 1. Sync check: state hash every 30 ticks
  (n64net_sync_record); stats line every 600 ticks.
- Test: `tools/net/pair-test.sh` (two copies on 127.0.0.1, separate --savedir under build/net-test/<stamp>/,
  both bots; STAGE/BOTS/FRAMES/SEED/DELAY/RENDERER/HOST_BOT/GUEST_BOT/DESYNC_TEST). Results: Skedar + 2 simulants,
  1800 ticks, **IN SYNC 61 checks**, guest stalls 1.3 s total; `DESYNC_TEST=500` -> both report DESYNC at tick 510.
  Screenshot: 2-player split screen on both.
- Next: (a) Mac <-> phone (needs a signed iOS build with this code: MacBook; the phone has no env vars, so add a
  Documents/netplay.cfg like BattleShip/SpaghettiKart, read by pdnetWanted/pdnetInit); (b) one full-screen view per
  device (as MK did) instead of split screen; (c) rollback (snapshots: globals + memp/mema heaps); (d) rendezvous
  (room codes) via n64net_rendezvous; (e) host picks the match in the real menus (like the Smash/MK sessions).

## (older) NEXT: 2-player netplay plan
Lockstep first, then rollback, as in SpaghettiKart (`SpaghettiKart/src/port/KartNet.c` = the model: n64net_input
with window 0, handshake, sync check). Steps: (1) add n64net as a submodule (philosoraptor64/n64net, pin the commit
i64 uses; client/include/n64net_input.h); (2) `port/src/pdnet.c`: per-tick local pad -> n64net_input, remote pad ->
the remote player's OSContPad in osContGetReadData (replayFilterPads is the hook point). **User decision (2026-11-10):
extend the n64net frame with the right stick (rstick_x/rstick_y), it is essential for PD**: add the two s8 to
`n64net_pad` (n64net is shared: keep the packet format versioned / backward compatible, the other games send 0, and
tell the Studio agent on the board since it owns n64net), host sends seed + aspect + stage + bot count + pd.ini
gameplay settings in the handshake, both boot with PD_REPLAY_BOTS-style setup and 2 players (chrslots bits 0+1,
numplayers 2 -> split screen; later each device shows only its own player, as in MK); (3) sync check = the replay
hash every 30 ticks; (4) Mac pair test script (two instances on 127.0.0.1), then Mac <-> Duo; (5) rollback needs
snapshots (globals + memp heap + mema; PD has no coroutines). Design: `PD_REPLAY_BOTS=N` (deterministic mode) boots straight into a Combat Sim match against N normal simulants
(stage = `--boot-stage`, e.g. 0x32 Skedar): replaycheck.c `replaySetupMatch()` sets g_MpSetup.stagenum/chrslots
(bit 0 = player 1, bits 4.. = bots) and g_BotConfigsArray[i].difficulty/type, called from the pdmain.c branch that
does `g_MpSetup.chrslots = 1; mpReset()` (that branch also gets `|| replayWantsMatch()`). Then
`STAGE=0x32 PD_REPLAY_BOTS=3 tools/replay/replay-test.sh` (3 Mac runs identical), then the same on the Duo
(see the phone command above). After that: 2-player netplay through n64net (Combat Sim, lockstep first).

## Next steps
1. Human check on the Mac: picture, sound, controller, play a level and Combat Simulator.
2. Fixed-step mode + seeded RNG behind an env var, then a replay check like
   SpaghettiKart's `src/port/ReplayCheck.{c,h}`: start Combat Sim with fixed settings, record P1
   input, hash the sim state per tick (players/props/chrs + both RNG seeds), 3 runs byte-identical.
3. iPhone build (see below), then n64net input exchange (2 players, Combat Sim), then rollback.

## iPhone build: scope (2026-11-10)
The renderer is Fast3D (`port/fast3d/`): `gfx_pc.cpp` (RDP emulation, ~2800 lines) drives a backend
through `struct GfxRenderingAPI` (38 function pointers, `gfx_rendering_api.h`); the only backend is
`gfx_opengl.cpp` (~1300 lines, already supports GLES via its `gl_es` flag, used by the Switch build);
the window/context is `gfx_sdl2.cpp` (`SDL_WINDOW_OPENGL` + GL context); `port/src/video.c` picks
`gfx_opengl_api` + `gfx_sdl`. The iOS 27.2 SDK still ships OpenGLES.framework (deprecated since iOS 12).

Three routes:
1. **OpenGL ES 3 (fastest, ~1 session):** iOS CMake toolchain, SDL asks for an ES 3.0 context, data/save
   dirs -> Documents, our UIScene-patched SDL (from i64 deps), Info.plist (controllers, local network).
   Risk: Apple may remove GLES; runs through Apple's GL-on-Metal layer.
2. **ANGLE (GLES on Metal, ~1-2 sessions):** same code, link ANGLE; big dependency.
3. **Native Metal backend (~2-4 sessions) — CHOSEN by the user (2026-11-10).** Long-term answer: speed,
   battery, no deprecated API, same backend family as our libultraship games (i64 should share one
   Metal renderer). Source to adapt: libultraship's `src/fast/backends/gfx_metal.cpp` (~1300 lines,
   our forks have the macOS 27 address-space fix and the SetColorWriteMask fix).

Needed for every route: touch controls don't exist (Bluetooth controller first), ROM goes in Documents
(the port reads it at runtime, no extraction), iOS background pause + frame pacing (as in our other
ports). Becoming an i64 module is separate work (this port doesn't use libultraship).

## Metal backend plan (route 3), small steps; push + update this file after each
Interface differences, PD (C function table) vs LUS `GfxRenderingAPIMetal` (C++ class):
- Same in spirit: get_name, max texture size, clip params, shader load/unload/create/lookup/get_info/
  clear, new/select/upload/delete texture, sampler params, viewport, scissor, draw_triangles, init,
  on_resize, start/end frame, finish_render, framebuffers (create, start draw, clear, resolve MSAA,
  texture id, select fb texture), texture filter get/set.
- PD only: `set_depth_mode(test, update, compare, source_prim, zmode)` (LUS splits it into
  SetDepthTestAndMask + SetZmodeDecal + SetCurrentPrimDepth), `set_depth_range`, `set_use_alpha(use, modulate)`,
  `update_framebuffer_parameters(... opengl_invert_y, render_target, has_depth, can_extract_depth)`,
  `copy_framebuffer(dst, src, left, top, flip_y, use_back)`, `select_texture(tile, id, linear_filter)`,
  `upload_texture(..., gen_mipmaps)`, sampler `mipmaps`, `set_mipmap_filter`, anisotropy get/set.
- LUS only (not needed): ReadFramebufferToCPU, GetPixelDepth, GetTextureById (ImGui).
- Shader ids: PD `(u64, u32)`, LUS `(u64, u64)`. PD's `CCFeatures` has extra options
  (opt_texture_edge, opt_noise, opt_2cyc, opt_alpha_threshold, opt_invisible, opt_grayscale, **opt_blur**,
  do_single/do_multiply/do_mix, color_alpha_same): the Metal shader generator must cover every option the
  GLSL generator in gfx_opengl.cpp handles (port its GLSL generation to MSL rather than reuse LUS's template).

Steps:
- [x] M1. Window (2026-11-10): `gfx_sdl2.cpp` has `bool gfx_sdl_use_metal` (default false): when true it
      creates an `SDL_WINDOW_METAL` window + `SDL_Metal_CreateView` (no GL context; falls back to GL if that
      fails), drawable size via `SDL_Metal_GetDrawableSize`, no `SDL_GL_SwapWindow` (the backend presents),
      swap interval only recorded (`vsync_enabled`; the backend must apply it to the layer).
      `extern "C" void* gfx_sdl_get_metal_layer(void)` returns the CAMetalLayer for the backend.
      Nothing sets the flag yet: OpenGL is unchanged (verified boot). Wiring the selection is part of M2.
- [x] M2. Skeleton (2026-11-10): `Video.Renderer` in pd.ini (0 = OpenGL default, 1 = Metal) or env
      `PD_RENDERER=metal|opengl` (overrides, for tests); `port/src/video.c` sets `gfx_sdl_use_metal` and picks
      `gfx_metal_api` (Apple only). `port/fast3d/gfx_metal.{h,mm}`: device/queue from the SDL CAMetalLayer
      (BGRA8, framebufferOnly), `end_frame` clears the drawable to dark red and presents; shaders are
      bookkeeping only (ShaderProgram = ids + num_inputs/used_textures from gfx_cc_get_features, map cache),
      textures/framebuffers hand out ids, every draw call is a no-op. CMake: OBJCXX enabled on APPLE, port/*.mm
      globbed with -fobjc-arc, Metal/QuartzCore/Foundation linked.
      Verified on the Mac: OpenGL unchanged; Metal boots ("SDL: created Metal window", "Metal: device Apple M4 Max")
      and runs the game loop without crashing. **User to confirm** the window is solid dark red
      (`cd build && PD_RENDERER=metal ./pd.arm64`). Note: if the Metal window can't be created, SDL falls
      back to GL but the Metal backend is still selected -> fatal "no CAMetalLayer" (fine for now).
- [x] M3a. MSL generator (2026-11-10): `port/fast3d/gfx_metal_shader.cpp` `gfx_metal_generate_msl(cc, three_point,
      &num_floats)` = line-by-line port of the GLSL generator (aliases vec2/3/4 = float2/3/4 and a floored GLSL `mod`,
      so the combiner expressions are identical; KEEP BOTH IN SYNC). Vertex shader reads the same interleaved floats
      by vertex_id from buffer(0) (no vertex descriptor); fragment: uTex0/1 = texture/sampler 0/1, buffer(0) =
      `FragUniforms {frame_count, noise_scale, three_point_filter0/1}`. `create_and_load_new_shader` compiles it
      (MTLMathModeSafe) into ShaderProgram.vertex_fn/fragment_fn and logs failures with the source.
      Test: `PD_RENDERER=metal PD_METAL_SHADERTEST=300 ./pd.arm64` compiles 300 random valid combiners (half
      three-point): **300 compiled, 0 failed** (random ids must skip items > SHADER_NOISE and INPUT_5..7: the GL
      generator doesn't handle those either). The three-point choice is baked in at creation (as in GL):
      `set_texture_filter` must clear the shader cache when the mode changes (check what gfx_pc does).
- [ ] M3b. (next) Pipeline states: cache keyed by (shader, use_alpha/blend, depth test/write/decal, MSAA sample
      count, colour format), built lazily in draw_triangles. Can be merged with M4.
- [x] M4a. Textures + samplers (2026-11-10): GL semantics kept (select_texture/set_sampler_parameters set the
      active tile; upload and sampler apply to the texture bound there; sampler state stored per texture).
      RGBA8Unorm shared textures, re-created on size/mip change; mipmaps (gen_mipmaps or three-point, as GL) via a
      blit command buffer; sampler states cached by (min/mag linear, mip mode, S/T address mode, anisotropy);
      filter table = gfx_opengl_set_sampler_parameters. Not visible yet (nothing draws). Boots, no errors.
- [x] M4b. Draw (2026-11-10): **first Metal pixels: the N64 logo intro renders like OpenGL** (window captures
      compared). start_frame takes the drawable, makes a Depth32Float depth texture and begins a pass
      (clear colour+depth); clear_framebuffer = end the encoder and begin a new pass with LoadActionClear;
      pipelines cached by (shader, use_alpha, modulate), blend = GL's (SRC_ALPHA, 1-SRC_ALPHA) or modulate
      (DST_COLOR, ZERO); depth-stencil states cached; zmode DEC = LessEqual + depth bias (-2, -2: tune vs GL's
      polygon offset); viewport/scissor flipped from GL bottom-left; vertices via setVertexBytes (<= 4 KB) or a
      new MTLBuffer per draw (TODO: per-frame ring buffer); 1x1 white texture/default sampler if a texture is missing.
      get_clip_parameters = {z 0..1, no y flip}. video.c turns `gfx_framebuffers_enabled` off and MSAA to 1 for Metal
      until M6. Runs clean under `MTL_DEBUG_LAYER=1`. `PD_METAL_STATS=1` logs draws/tris/skipped per 60 frames.
      Window-only screenshots: a tiny Swift CGWindowList tool for the window id, then `screencapture -x -o -l <id>`.
- [x] M4c (partial, 2026-11-10): intro compared with OpenGL at 30 s / 45 s (3D city fly-by: buildings,
      textures, ship, stars all render on Metal); the user watched it live: "looks and sounds clean and clear".
      Possible small difference: the satellite dish looked a little more see-through on Metal (could be timing;
      check with the same frame, e.g. by pausing). Not yet checked: menus, a level, Combat Sim, the blur menu
      background (opt_blur, needs framebuffers = M6).
- [x] M4d (2026-11-10): `tools/metal/compare.sh <name> <secs> [game args]` = Metal vs OpenGL window
      screenshots (build/metal-compare/; MTL_DEBUG_LAYER=1 passes through). `--skip-intro` = CI menu,
      `--boot-stage 0x30` = Defection. Fixed: **depth clamp** (GL enables GL_DEPTH_CLAMP; without
      `setDepthClipMode:Clamp` the menu frames, highlight bar and arrows were clipped away). Defection level matches.
- [x] M6a. Offscreen framebuffers (2026-11-10): per-fb BGRA8 colour (+ Depth32Float) textures, re-created on
      resize; `start_draw_to_framebuffer` switches the pass target (`sTargetFb`); pipelines keyed by has-depth;
      `select_texture_fb` samples the fb texture (linear clamp); `copy_framebuffer` = blit for unscaled rects
      (GL bottom-left -> top-left), a tiny copy shader for scaled copies. **No y flips anywhere**: GL's
      flip_y/invert_y only compensate for GL's bottom-left origin; drawable and fb textures are top-row-first in Metal.
      Immediate copies between frames read `sFrontTex` (a blit of each presented frame, GL's "front buffer")
      on their own command buffer. framebufferOnly = NO. Result: **the CI menu's blurred background matches GL**,
      clean under MTL_DEBUG_LAYER. Stats line now includes copies, target switches, fb samples.
- [x] Vertex ring buffer (2026-11-10): 3 x 8 MB shared buffers (frames in flight) + dispatch semaphore
      (wait in start_frame, signal on completion); draws > 4 KB copy into the ring (256-byte aligned), fallback
      newBuffer on overflow. Defection clean under MTL_DEBUG_LAYER.
- [x] M6b. MSAA (2026-11-10): offscreen fbs with msaa_level > 1 get Type2DMultisample colour/depth (sample
      count clamped to what the device supports) and aren't sampleable; pipelines keyed by sample count
      (rasterSampleCount); `resolve_msaa_color_buffer` = render-pass resolve (through a scratch texture + copy
      shader when sizes differ); copies from a multisampled source resolve first (GL's blit does it implicitly).
      video.c no longer forces MSAA 1 on Metal. Test: `--savedir <dir>` with a pd.ini `[Video] MSAA=4`: CI menu
      matches GL, clean under MTL_DEBUG_LAYER, 2 target switches/frame (game fb -> screen).
- [ ] (old M6b text, remaining items:) MSAA (video.c still forces gfx_msaa_level = 1 on Metal; game_framebuffer + resolve), then
      in-level fb effects (bondview.c: g_PrevFrameFb motion blur, g_BlurFb), different game resolution
      (game_renders_to_framebuffer path), vertex ring buffer, decal depth-bias tuning.
- [ ] M4e. (later) Play through menus/levels on Metal vs OpenGL and fix differences; vertex ring buffer. (Original M4 text: Textures + samplers (filters, mipmaps, clamp/mirror), vertex buffer ring, draw_triangles.
      (Visible result: the game renders.)
- [ ] M5. Depth modes (decal/zmode, prim depth), depth range, scissor/viewport, alpha/blend.
- [ ] M6. Framebuffers: create/update/copy/clear, MSAA resolve, fb textures (blur, motion effects,
      the PD-specific copy_framebuffer users). A/B screenshots vs OpenGL on the Mac for parity.
- [x] M7a. iOS build compiles + links (2026-11-10, Studio; NOT yet signed/installed: the Studio's Xcode has no
      Apple account). CMake: `CMAKE_SYSTEM_NAME=iOS` sets `PD_IOS` (macro `PD_IOS=1`; not PLATFORM_IOS, which
      <mach-o/loader.h> defines), skips the macOS deployment-target block, takes SDL from cache vars, zlib = -lz,
      links OpenGLES (SDL references EAGL) + UIKit etc., makes a MACOSX_BUNDLE with `ios/Info.plist.in`
      (bundle id `io.github.philosoraptor.perfectdark`, UIScene manifest -> SDLUIKitSceneDelegate, landscape,
      controllers, Files-app Documents, local network, MinimumOSVersion 17.0). Code: Metal forced on iOS
      (video.c), `SDL_main.h` + SDL_main signature (main.c), base and save dir = `$HOME/Documents` (fs.c): put the
      ROM there as `pd.ntsc-final.z64` (Files > On My iPhone > Perfect Dark). Build (needs an iOS SDL2 with the
      UIScene patch; BattleShip's iOS build has one):
      ```
      B=<BattleShip>/build-ios/_deps
      cmake -S . -B build-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
        -DSDL2_LIBRARY=$B/sdl2-build/Release-iphoneos/libSDL2.a \
        -DSDL2MAIN_LIBRARY=$B/sdl2-build/Release-iphoneos/libSDL2main.a \
        "-DSDL2_INCLUDE_DIR=$B/sdl2-src/include;$B/sdl2-build/include"
      cmake --build build-ios --target pd
      ```
      -> `build-ios/pd.arm64.app` (Mach-O arm64, LC_BUILD_VERSION platform 2 = iOS, minos 17.0).
      (On the MacBook, i64's deps-out/ios SDL should work too.)
- [x] M7b. Signed + installed on both iPhones (2026-11-10, MacBook; NOT yet launched). Built with the Ninja command
      above but SDL from SpaghettiKart's `build-ios/_deps` (UIScene patch). Profile: a throwaway XcodeGen stub app with
      bundle id io.github.philosoraptor.perfectdark built once with `-allowProvisioningUpdates` (existing certificate,
      no new one); then copy the stub's `embedded.mobileprovision` into pd.arm64.app, take its entitlements
      (`codesign -d --entitlements :- --xml <stub.app>`) and `codesign --force --sign <cert sha1> --entitlements ent.plist
      --generate-entitlement-der pd.arm64.app`; `devicectl device install app`; ROM copied to Documents/pd.ntsc-final.z64.
      The profile lasts 7 days (free team). Old M7b text:
- [ ] (old) M7b. (next, MacBook: it can sign) Sign + install: either `-G Xcode` (untested for this CMake) with
      DEVELOPMENT_TEAM + `-allowProvisioningUpdates`, or sign the Ninja-built .app with a profile for
      io.github.philosoraptor.perfectdark. NEVER create a new Apple Development certificate. Copy the ROM to
      Documents (`devicectl device copy to --domain-type appDataContainer ... --destination Documents/pd.ntsc-final.z64`).
      Done ahead (untested on a device): iOS background pause = `gfx_sdl_app_inactive` (gfx_sdl2.cpp, set on
      SDL_APP_WILLENTERBACKGROUND, cleared on DIDENTERFOREGROUND); gfx_metal start_frame draws/presents nothing
      while set. The game logic keeps running until iOS suspends the app (fine offline; netplay needs AWAY/BACK).
      Then: controller, touch?, audio session, frame pacing; log = Documents (check where
      sysLogPrintf writes: on iOS always `Documents/pd.log`, done in system.c; pull it with
      `devicectl device copy from --domain-type appDataContainer --domain-identifier io.github.philosoraptor.perfectdark --source Documents/pd.log`).

## Where the Metal work stopped (2026-11-10, end of Studio session)
Done and pushed: M1-M4d, M6a, M6b (MSAA), vertex ring, M7a (iOS build compiles/links), iOS background pause,
iOS log in Documents/pd.log, pipeline-cache fix. Decal depth bias (-2, -2) left as is (GL polygon offset on
24-bit depth is about the same as Metal depthBias on Depth32Float near z = 1). Last thing started: a Skedar MP map
comparison (`tools/metal/compare.sh skedar 20 --boot-stage 0x32`), not yet looked at.
Next: M7b sign + install on a phone (MacBook), then in-level fb effects (bondview.c g_PrevFrameFb / g_BlurFb)
compared with GL, then netplay determinism (fixed step in timing.c, RNG seeding).
