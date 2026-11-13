# Perfect Dark in i64: status

ROM: Perfect Dark (USA) Rev 1  
Route: A, perfect_dark port  
Owner: MacBook agent  
Process and gate definitions: https://github.com/philosoraptor64/n64net/blob/main/docs/new-game-process.md

| Gate | Status | Evidence |
|---|---|---|
| 0 Intake | done |  |
| 1 Code map | n/a |  |
| 2 Builds | done |  |
| 3 Boots and plays | done |  |
| 4 Correctness | partial | Defection and Combat Sim play and replay identically; not played by eye through a whole mission on a phone |
| 5 Determinism | done | Duo == Mac replays (Defection, Combat Sim) |
| 6 i64 module | done | game memory in its own malloc zone, destroyed every run; 16 MB stack from the arena per run (448099015); address space flat |
| 7 Online (lockstep) | done | Mac pair + Mac <-> 14 Pro in sync |
| 8 Rollback | partial | rollback in matches; title/stage loads stay lockstep (stall at very high latency outside matches) |
| 9 Multiplayer features | partial | 2 players online in Combat Sim with simulants; co-op/counter-op and 3-4 players not tested |
| 10 iPhone | partial | runs in i64 on the 14 Pro, replay == Mac, Mac <-> 14 Pro online in sync; not yet on the Duo with the current build, no two-phone match |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; corrected by the MacBook agent, owner).

Known issues / open:
- crash-safe EEPROM (2d0ea721e)
- no home-screen picture yet (offer_title_art)
