# tools/analisis

| Script | What it does | Usage |
|---|---|---|
| `romfs_check.py` | What a `.nup`'s RomFS holds, from its table of contents only: file count, top folders, formats, and whether the MKW-style Gyml files `runtime/sdk/fs/romfs_res.c` reads are there. Built as `romfs_check.exe` by `.github/workflows/romfs_check.yml` (release `romfs-check`) | `uv run tools/analisis/romfs_check.py GAME.nup` |
| `prof_win.py` | Report of a Windows profile (`ORBITALS_PROFILE=<path>`, `runtime/core/diag/prof_win.c`): samples per thread, per module (driver, ntdll…) and per function of `orbitals-together.exe` and `orbitalgame.dll` with `llvm-symbolizer` from `build/win` | `uv run tools/analisis/prof_win.py PROFILE [--top 40]` |
| `bindb.py` | Builds `build/bindb.sqlite` from `main`, `sdk` and `subsdk0`: functions, symbols (with their readable C++ name), PLT stubs resolved to the import, `BL`/`B` calls, `ADRP`+`ADD`/load-store references, strings, `SVC` instructions (table `svc`) and data words that point to a function (table `fptr`: 219 757, in 21 382 vtable columns; 109 475 functions are only reached that way); the NVN2 driver functions of `sdk` with their name (`tools/gpu/nvn2_procs.py`). Addresses from `recomp/layout.py`, the same as in the generated C | `uv run tools/analisis/bindb.py` (~6 s) |
| `q.py` | Short queries over that database | `uv run tools/analisis/q.py <command> <arg>` |
| `linux/` | Probes without root: `ioctl` time per thread and request (`LD_PRELOAD`), each thread's state and `wchan` sampled through `/proc` | see `linux/README.md` |
| `recdiff.py` | Compares two NVN recordings command by command (aligned by type): how much of a frame repeats in the next and which fields change; recordings come from `mkw --replay` with `ORBITALS_CAP_RECS=<dir>` | `uv run tools/analisis/recdiff.py A.bin B.bin` |
| `opjoin.py` | Two serial replays of a capture compared draw by draw (same position in the same submit): cost in each, per submit and by what B did (reused, captured, normal); between two replays the frame moves ±3 ms, more than a per-draw change. Dumps from `build/orbitals-together --replay` with `ORBITALS_TRACE=draw ORBITALS_OP_DUMP=FILE` | `uv run tools/analisis/opjoin.py A.txt B.txt` |
| `recruns.py` | Which spans of an NVN recording repeat in another without the UBO addresses: draws in equal passes and in runs of equal spans by length (the unit of `ORBITALS_REUSE`) | `uv run tools/analisis/recruns.py A.bin B.bin` |
| `sdk_gaps.py` | The system work list: imports of `main` with no replacement in `runtime/`, by namespace, first the ones that depend on a console service (the runtime only serves `audren` and `sm`); with `--census`, which ones the game called in a run with `ORBITALS_IMPORT_CENSUS` and how many times | `uv run tools/analisis/sdk_gaps.py [--namespace nn::hid] [--census build/run/NAME.imports]` |
| `port_check.py` | Feasibility of porting another Switch 2 game onto this runtime, from its own dump (`.nup` or a folder with `exefs/`): modules and build ids, SDK version, engine and middleware markers, its NVN2 driver's procs against the runtime's tables, its SDK imports the runtime does not replace (console-service namespaces first), the services of `main.npdm`. Writes `build/port_check.md`, which names the game's functions: kept out of git | `uv run tools/analisis/port_check.py GAME.nup\|DUMP_DIR [--out FILE]` |
| `sdk_class.py` | Each import of `main` by where it ends up as recompiled SDK code: a console service, the runtime's kernel or nothing of the console (pure library), following the direct calls down to their `SVC`s; gives the README's SDK bar (`tools/docs/progress.py`) | `uv run tools/analisis/sdk_class.py [--class kernel] [--namespace nn::os]` |
| `muerto.py` | Dead code in the runtime inherited from totk-pc-recomp: global functions nothing asks for (`nm`), `cfg.c` keys without a reader (menu options that do nothing), `#if` on macros `build.py` fixes, sources not built in any target and catalogue texts never shown; list only | `uv run tools/analisis/muerto.py [funcs\|keys\|ifdefs\|files\|texts]` (<1 s, after a build) |
| `findf.py` | float32 constants (±1e-4) in `main`'s image | `python3 tools/analisis/findf.py VALUE...` |
| `disasm.py` | Disassembly of a single function (the one containing the address) with calls and imports named from the database; the last resort when `q.py` is not enough | `uv run tools/analisis/disasm.py main+0x19e5860 [--count N]` |
| `frame_report.py` | One text report on `nn::vi::ListFrameHistory`: its symbols, the game's code around the two calls (`main+0x1540460`, `main+0x15404c0`, the Viewport Thread's wait at `+0x1540434`) and the SDK's own function, to read the layout of `nn::vi::FrameInfo`. With `--stack LOG`, names the game frames of an abort's thread dump in an `orbitals.log` (`q.py fn` for each, and the code before the call), for Unreal's RenderThread timeout; double-clicked next to `exefs\` with a `logs\orbitals.log`, it does that. Built as `orbitals-frame-report.exe` by `.github/workflows/frame_report.yml` (artifact; on pushes to `orbitals-port` that change it, or by hand) so it runs without git, uv or Python | `uv run tools/analisis/frame_report.py [EXEFS_DIR] [--out frame_report.txt] [--stack orbitals.log]` |

`q.py` commands (addresses can be `0x7000…`, `main+0x19e5860`, a symbol name or an NVN2 function name):

| Command | Answers |
|---|---|
| `at ADDR` | Which function an address falls in |
| `fn ADDR` | Summary: who calls it, what it calls (named first), which strings it uses |
| `callers ADDR` / `callees ADDR` | Who calls it (also from other modules through their PLT) / what it calls |
| `str TEXT` | Strings containing it and which functions use them |
| `sym PATTERN` | Symbols (exported, imported, objects) by name |
| `imports PATTERN` | Imports of `main` with how many calls each has |
| `refs ADDR` | Functions that reference a datum |
| `vptrs ADDR` | Which data words point to the function (indirect calls): vtable column, index and who loads the column (constructors) |
| `vtable ADDR` | The pointers of the vtable column containing that datum |

Limits: functions end where the next one starts; a `BLR` does not say what it calls: `vptrs` gives the candidates
(pointers in data), not the call site.
