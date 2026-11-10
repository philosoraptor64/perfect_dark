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
5. Graphics is OpenGL only: iOS needs a GLES 3 path (SDL + GLES) or ANGLE; check before investing.

## Next steps
1. Human check on the Mac: picture, sound, controller, play a level and Combat Simulator.
2. Fixed-step mode + seeded RNG behind an env var, then a replay check like
   SpaghettiKart's `src/port/ReplayCheck.{c,h}`: start Combat Sim with fixed settings, record P1
   input, hash the sim state per tick (players/props/chrs + both RNG seeds), 3 runs byte-identical.
3. iOS build feasibility (GLES), then n64net input exchange (2 players, Combat Sim), then rollback.
