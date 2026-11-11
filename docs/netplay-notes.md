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
- [ ] M4d. (next) Play through menus/levels on Metal vs OpenGL and fix differences; vertex ring buffer. (Original M4 text: Textures + samplers (filters, mipmaps, clamp/mirror), vertex buffer ring, draw_triangles.
      (Visible result: the game renders.)
- [ ] M5. Depth modes (decal/zmode, prim depth), depth range, scissor/viewport, alpha/blend.
- [ ] M6. Framebuffers: create/update/copy/clear, MSAA resolve, fb textures (blur, motion effects,
      the PD-specific copy_framebuffer users). A/B screenshots vs OpenGL on the Mac for parity.
- [ ] M7. iOS target: toolchain, UIScene SDL, Documents paths, Info.plist, controller; install on a phone.
