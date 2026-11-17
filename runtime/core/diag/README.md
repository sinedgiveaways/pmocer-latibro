# runtime/core/diag

Diagnostics: flight recorder, counters and profiling, RenderDoc, game phases and the census of SDK calls.

| File | What it is |
|---|---|
| `flight.c`, `flight.h`, `perf.c`, `prof*`, `perfmon.*`, `rdoc.c` | Diagnostics: flight recorder, counters, profiling, RenderDoc |
| `diag_fence.c`, `diag_fence.h` | `ORBITALS_DIAG_FENCE=1`: a ring of the last waits and signals of the NVN layer and the Vulkan back end (submits, blocking fence and sync waits, acquires, presents, the completion thread moving `done`, event signals), the blocking waits still open, and a line every 5 s of the vsync thread; printed after the thread dump of a guest abort. For a stall with every guest thread idle ([log-analyzer.md](../../../docs/log-analyzer.md)) |
| `impcensus.c` | `ORBITALS_IMPORT_CENSUS=<file>`: which SDK functions the game code calls during the run, how many times and from which frame, through the function-entry hook of the lifted code (`fnprof_hit`); read by `tools/analisis/sdk_gaps.py --census` |
