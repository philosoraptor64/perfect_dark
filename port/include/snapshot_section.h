/* Rollback snapshot section placement (see port/src/snapshot.c).
 *
 * Force-included into every C file under src/ (game, libs, audio sequencer: see CMakeLists.txt). It puts their
 * mutable globals, including function-static locals, into the __PDSNAP segment, so a snapshot of the
 * simulation is that segment plus the used parts of the memp heap. port/ code (renderer, input, netplay,
 * replay check) keeps the normal sections: a restore never touches it. PD_SNAPSHOT_OUTSIDE moves one global
 * of a snapshotted file back out.
 *
 * Apple (Mach-O) only; elsewhere this is a no-op and rollback is unavailable. */
#ifndef PD_SNAPSHOT_SECTION_H
#define PD_SNAPSHOT_SECTION_H

#if defined(__APPLE__) && defined(__clang__)
#define PD_SNAPSHOT_SUPPORTED 1
#define PD_SNAPSHOT_SEGMENT "__PDSNAP"
#define PD_SNAPSHOT_OUTSIDE __attribute__((section("__DATA,__bss")))
#define PD_SNAPSHOT_OUTSIDE_DATA __attribute__((section("__DATA,__data"))) /* initialized globals */
#if !defined(PD_SNAPSHOT_EXCLUDE) && !defined(__cplusplus)
#pragma clang section bss = "__PDSNAP,__bss" data = "__PDSNAP,__data"
#endif
#else
#define PD_SNAPSHOT_SUPPORTED 0
#define PD_SNAPSHOT_OUTSIDE_DATA
#define PD_SNAPSHOT_OUTSIDE
#endif

#endif
