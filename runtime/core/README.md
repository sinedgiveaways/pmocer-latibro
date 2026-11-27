# runtime/core

Boot, modules, kernel, guest memory, settings and diagnostics. Every subfolder with headers is on the include path
(`tools/build/build.py`): `#include` goes by name.

| Folder | Contents |
|---|---|
| [boot/](boot/README.md) | Entry point, boot sequence, game code tables, module loader, patches, game clock |
| [kernel/](kernel/README.md) | Horizon kernel: handles, waits, threads, virtual cores, fibers, SVCs |
| [memory/](memory/README.md) | Guest address space, write tracking, read traps (`ORBITALS_EARLY_READ`) |
| [base/](base/README.md) | `base/host.h`, settings (`cfg`), `ORBITALS_CTL`, system info, SHA-256, UTF-16 |
| [hacks/](hacks/README.md) | Development helpers the console does not have (the control pipe's freeze) |
| [diag/](diag/README.md) | Flight recorder, counters, profiling, RenderDoc, `ORBITALS_IMPORT_CENSUS` |
