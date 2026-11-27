# Audit: MKW traces and current state

Étape 0, 16/11/2026, branch `orbitals-port` at `cc455f0`. Read-only audit: no source file was changed.

The full line-by-line inventory (file, line, file type, kind, snippet) is in
[`mkw-inventory.tsv`](mkw-inventory.tsv), generated from `git ls-files` with the case-insensitive pattern
`mkw|mario ?kart|mkworld`. This page summarises it and states the current state of the project.

## 1. Main finding

The repository is not yet an Orbitals port with a few leftover MKW names. It is mkw-recomp with a partial text rename.

- `README.md`, `docs/remote-play.md` and several log names already say "orbital" / `orbital.ini` / `logs/orbital.log`
  (119 hits in `runtime/` and `tools/`), but the code still reads `mkw.ini` (6 hits in `runtime/` and `tools/`, plus 9 for `mkw.log`), builds `build/mkw[.exe]`, and
  reads only `MKW_*` environment variables.
- The Orbitals-specific parts are small: commits tagged "port mode" (`loader.c`, `impcensus.c`, `fs_host.c`),
  `tools/analisis/port_check.py`, `tools/prepare/nup.py`, `.github/workflows/port_check.yml` and `port_windows.yml`, and
  comments in `recomp/README.md` and `recomp/gen.py` (Orbitals is an Unreal game).
- The recompiler is still keyed to MKW builds: `recomp/versions.py` lists MKW 1.0.0 (`9e244438c3d8ef1c`) and 1.8.0
  (`d00e4f399e411d06`) and raises "not a known version of Mario Kart World" for any other `exefs/main`.
  `recomp/patches-mkw*.txt` are MKW-specific patches (102 spin waits in `main` and so on). `runtime/core/boot/*.c`
  hard-code MKW 1.0.0/1.8.0 addresses (`main+0xcf3a5c`, `main+0xdd07c8`, ...) and `loader.c` compares import tables
  and `DT_INIT` counts against MKW 1.0.0 values.
- So a text rename alone will not make an Orbitals binary. Version tables, patch files and boot addresses need
  Orbitals data, which only your `.nup` can provide (see the questions in section 6).

## 2. Inventory totals

475 of 698 tracked files contain a trace; 3481 matching lines.

| Kind | Lines | Meaning | Strategy |
|---|---|---|---|
| `env-var` | 2246 | `MKW_*` names (407 distinct; 55 more through `rt_knob("X")`, which adds the prefix) | Rename to `ORBITALS_*` |
| `artifact-path` | 431 | `build/mkw`, `mkw.exe`, `mkw.ini`, `logs/mkw.log`, `libmkw.dll.a`, `patches-mkw*.txt`, `mkw.rc`/`.manifest`/`.ico`/`.def` | Rename files and references together |
| `prose/other` | 358 | Comments and docs that say "MKW" in running text | Reword; drop when MKW-only |
| `upstream-ref` | 224 | `mkw-recomp`, `adriadam10` (URLs, release names, wiki links, `~/.local/share/mkw-recomp`, `mkw-recomp-run`) | See licence note below |
| `game-name` | 164 | "Mario Kart World" | Replace with the Orbitals name, or remove |
| `identifier` | 36 | `ov_mkw_probe`, `ov_mkw_list`, `mkw_vpool`, `probe_*_mkw`, `__start_mkw_ovr/obs/fns` (linker sections), `mkw_xlate`, `mkw_pid`, `mkw_rc`, `mkwgame`, `mkwmod` | Rename; linker section names are in the binary |
| `game-version-comment` | 22 | "MKW 1.8.0's sdk", "MKW 1.0.0" in comments | Reword |

Kind is assigned by a regex, so a few lines sit in a neighbouring class. The per-line column is a starting point for
the Étape 1 plan, not a final classification.

### By area

| Area | Files | Lines |
|---|---|---|
| `runtime/gfx` | 132 | 1135 |
| `tools` | 108 | 489 |
| `runtime/core` | 50 | 445 |
| `docs/i18n` (READMEs in 6 languages) | 12 | 262 |
| `i18n` (`.po`/`.pot` catalogues) | 22 | 224 |
| `runtime/ui` | 23 | 211 |
| `runtime/probes` | 16 | 193 |
| `runtime/sdk` | 24 | 143 |
| `runtime/input` | 12 | 76 |
| `docs` (excluding `docs/i18n`) | 7 | 54 |
| `recomp` | 8 | 46 |
| `tests` | 12 | 39 |
| `CHANGELOG.md` | 1 | 24 |
| `runtime/platform` | 8 | 24 |
| `runtime/audio`, `sm86`, `runtime/steam`, `runtime/cpu`, `.github`, rest | | 125 |

## 3. Things that need a decision, not just a rename

1. **Central prefix.** `runtime/core/base/cfg.c:21` defines `#define PREFIX "MKW_"`, and `cfg_get` accepts the name
   with or without the prefix. `rt_knob` (`runtime/core/diag/perf.c`) adds `MKW_` itself. Changing these two places
   handles reads. The ~2000 string literals such as `cfg_get("MKW_VOLUME")` still need rewriting, and
   `tools/run/envs.py` (regex `MKW_[A-Z0-9_]+`) plus `tests/test_cfg.py` must follow.
2. **Environment names you listed all exist** (counts of references): `MKW_REPLAY_CORE` 2, `MKW_REPLAY_ASYNC` 11,
   `MKW_VK_TEX_POOL_MIN` 7, `MKW_GPU_CRUMBS` 5, `MKW_SUBMIT_CATCHUP` 9, `MKW_REPORT_GPU_END` 3,
   `MKW_REPORT_AT_SUBMIT` 5, `MKW_QUEUE_WAITS` 6, `MKW_ACQUIRE_GPU` 2, `MKW_VRAM_HEADROOM` 4,
   `MKW_GPU_MEM_LIMIT` 19, `MKW_VK_PRESENT` 12. Most used: `MKW_TRACE` 98, `MKW_REPLAY_PAR` 42, `MKW_GPU_GUARD` 39,
   `MKW_VOLUME` 36.
3. **Game text in the repository.** `i18n/game/{ca,eu,gl}.tsv` (4639 lines, plus `i18n/game/1.8.0/`) are translations
   of the game's own message files, including the MKW credits and "You started Mario Kart World!". That is game
   content. It conflicts with "no game file in the repository" and is MKW-only. Proposal: remove from the tree (history
   keeps it), and decide separately whether Orbitals gets an equivalent generated at setup time.
4. **Upstream attribution.** `LICENSE` is MIT, "Copyright (c) 2026 Adrià", and `recomp/LICENSE.totk-pc-recomp` and
   `runtime/LICENSE.totk-pc-recomp` carry the totk-pc-recomp licence. MIT requires the copyright notice and permission
   text to stay in copies of substantial portions. Removing "mkw-recomp" from user-facing text, binaries and logs is
   fine. Deleting those licence files or the author's notice is not. Proposal: keep `LICENSE`, `LICENSE.*`, and a
   `THIRD_PARTY_NOTICES` file; do not embed the upstream name in the binary. I can't judge the licence of your
   fork's own line of descent beyond what the files say; please confirm.
5. **Binary metadata.** `runtime/platform/mkw.rc` and `mkw.manifest` only name the file (`mkw.ico`, `mkw.manifest`).
   There is no `VERSIONINFO` block, so product name and version resources are currently absent and need adding
   (`ProductName`, `OriginalFilename`, `FileDescription`). `tools/build/icon.py` draws a chequered flag (a racing
   motif): change it for an Orbitals icon.
6. **Linux data paths**: `~/.local/share/mkw-recomp`, `mkw-recomp-run`, `$WINEPREFIX/.mkw-ready`. Renaming moves
   users' existing saves and settings, so it needs a one-time migration.

## 4. State of the project

### Works (per `CHANGELOG.md` 0.1.12 and docs; not re-run here, no Orbitals files in this sandbox)

- Pipeline `recomp/` (AArch64 → C), `runtime/` (kernel/SDK in C, NVN2 on Vulkan via `runtime/gfx/nvn` and `runtime/gfx/vk`),
  `sm86/` (SM86 → SPIR-V), launcher UI and settings, input, audio, 19 UI languages.
- Steam Remote Play Together exists in code: `runtime/steam/steam_rpt.c` (488 lines), `steam_flat.h`, `docs/remote-play.md`,
  `tools/build/steamlib.py` (pins Steamworks SDK 1.65 library hashes), settings `steam_appid`, `steam_appids`,
  `steam_invite`, hooks in `main.c` and `input.c`. It was written and tested against MKW. Étape 3 will grade it
  for Orbitals.
- A/B tooling: `tools/run/ab.py` (reads `MKW_AB=<seconds>` perf logs), `perf_split.sh`, `crit_path.py`, `run_perf.sh`.
- Port-mode diagnostics: `tools/analisis/port_check.py` (feasibility report from a `.nup`), `sdk_gaps.py`, impcensus,
  `fs_host.c` path tracing, and a Windows `port_check.exe` workflow.

### Blocks or unknown

- No Orbitals `versions.py` entry, no Orbitals patch file, no Orbitals boot addresses (section 1).
- The named blocker, Ampere instruction 394 (image atomic with register handle), is not mentioned anywhere in the
  repository (searched `394`, "register handle"). The shader translator lives in `sm86/`; I have not read it yet.
- Nothing runnable can be verified in this sandbox: it is Linux, has no `.nup`, and the Windows build needs your
  machine.

### In progress (`docs/roadmap.md`, updated 09/11/2026; all about MKW races)

1. 60 fps: a race frame is 31-33 ms; Vulkan recording on the replay thread is ~19 ms, game ~3.4 ms, GPU ~9.7 ms,
   completion ~1.5 ms. Done so far: draw recordings reused across frames, recording overlapped with the game, submits on
   their own thread, shadow copies off the replay thread, replay on its own core, waits by address. Next: dynamic state
   recorded only on change, per-draw descriptor sets kept across frames (`MKW_DRAW_CACHE`), parallel recording (blocked
   on the texture lock), the kernel's global lock, `-O1` generated C then PGO.
2. Picture: shader translator bugs (3 of 6 fixed), push constants of constant bank 24, 25 validation errors,
   task/mesh shaders as compute.
3. Menus: screens still to check. 4. GPU hangs on AMD, GPU memory (sparse tiles). 5. Loading stutter.

The roadmap's numbers come from a Radeon 660M on Linux, not your RTX 4080 on Windows.

### Differences from your brief

| Brief says | Repository has |
|---|---|
| `scripts/build-windows.ps1` | No `scripts/` directory and no `.ps1` build script. Windows build is `uv run tools/build/build.py --target windows` (`windeps.py`, `dist.py`, `release.py`). Only `tools/run/Analyze-Timeout.ps1` is PowerShell |
| `docs/environment-variables.md` | Does not exist. `tools/run/envs.py` generates the list from the code |
| `ORBITALS_*` variables | None yet |
| `i18n/` references | Yes: `i18n/mkw.pot`, 22 `.po` files with "Mario Kart World" in the missing-game and wrong-version messages |
| `docs/steam-remote-play-status.md` | Absent; existing doc is `docs/remote-play.md` |
| `tools/run/ab.py` | Present, MKW-named variables |

## 5. Verification plan for the final binary (needs your machine)

The binary can't be built here. On Windows, after each rename commit:

```powershell
uv run tools/build/build.py --target windows --dist
Get-ChildItem build\dist\windows -Recurse -File | Select-String -Pattern "MKW|MarioKart|Mario Kart|mkw-recomp" -Encoding Byte -List
```

`Select-String` on a binary reads it as text and misses UTF-16 strings. I'd add `tools/build/check_names.py`, which
scans bytes as UTF-8 and UTF-16LE, and also reads the PE version resource and the `.manifest`. It would be a CI step
and the source of the proof in `docs/mkw-removal.md`. That's an Étape 1 proposal, not done.

## 6. Questions before Étape 1

1. What is Orbitals' exact name for user-facing text and the executable: `orbitals-together.exe` (already used in
   `port_windows.yml`, `launcher.cpp`, `dist.py`, `release.py` and others), `orbitals`, or `orbital` (used in README and `orbital.ini`)? The repo mixes `orbital` and
   `orbitals`. The `ORBITALS_` prefix is clear; the file names are not.
2. Does `mkw-recomp` have to disappear from the Git history and `LICENSE`, or only from the product? (Section 3.4.)
3. Is it OK to delete `i18n/game/*.tsv` (MKW game text)?
4. Should `MKW_*` stay readable as a deprecated alias during a transition? That keeps the string `MKW_` in the binary,
   which contradicts the "zero trace" criterion. Options: (a) no alias, clean break, a one-line note in the changelog
   (cleanest, breaks old scripts); (b) alias built from split pieces so the literal is not in the binary
   (works, but it is concealment and I'd avoid it); (c) a separate `tools/` migration script that rewrites
   `mkw.ini` and shell profiles once, with no alias in the runtime (clean binary, covers the real transition).
   I recommend (c).
5. Do you have the Orbitals build id(s) (`uv run tools/analisis/port_check.py "<your .nup>"` gives the report)? Without
   them `versions.py` and the boot constants stay MKW's.
6. Sources in your brief I couldn't check: the `sinedgiveaways/mkw-recomp` `v0.1.11-rpt8` tag, Hookshot, NX2navi, DKB
   Recomp and the others. I have not fetched them and will not copy code from any of them; I'll read them for ideas
   in Étapes 3 and 4 when you want.
