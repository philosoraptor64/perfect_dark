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
| 4 Correctness | partial | to verify (owner) |
| 5 Determinism | done | Duo == Mac replays (Defection, Combat Sim) |
| 6 i64 module | partial | own malloc zone per run, address space flat; not in the arena (MacBook decision) |
| 7 Online (lockstep) | done | Mac pair + Mac <-> 14 Pro in sync |
| 8 Rollback | partial | title/stage loads lockstep; to verify (owner) |
| 9 Multiplayer features | partial | to verify (owner) |
| 10 iPhone | partial | 14 Pro module; to verify (owner) |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; owners please correct).

Known issues / open:
- crash-safe EEPROM (2d0ea721e)
