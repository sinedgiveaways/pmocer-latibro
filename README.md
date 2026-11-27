# pmocer-latibro

`orbitals-together`: a static recompilation of Orbitals (Nintendo Switch 2) built around a custom runtime and shader
translation pipeline.

> **To play:** the final builds, with a step-by-step guide (English and French) for the setup and Steam Remote Play
> Together, are on **[sinedgiveaways/Orbitals-Together](https://github.com/sinedgiveaways/Orbitals-Together)**
> ([latest release](https://github.com/sinedgiveaways/Orbitals-Together/releases/latest)). This repository is where
> the development, the test builds and the fixes happen: report problems in its
> [issues](https://github.com/sinedgiveaways/pmocer-latibro/issues).

This repository aims to recreate the game's behavior on x86-64 by recompiling the Switch 2 AArch64 code into C, replacing the console runtime with a native implementation, and translating SM86 shaders into SPIR-V for Vulkan-backed execution.

## What this project contains

- `recomp/` — translated game code and recompilation infrastructure
- `runtime/` — runtime implementation replacing the console's kernel and SDK behavior
- `sm86/` — SM86 shader translation and GPU-related tooling
- `tools/` — build, test, diagnostics, and documentation utilities
- `tests/` — validation and regression tests
- `docs/` — project documentation, roadmap, research notes, and release information
- `i18n/` — the menu and start screen's text catalogs
- `.github/` — CI workflows (Windows build, tests, the public release's `publish.yml`, `romfs_check.exe` and the other report tools), issue and PR templates, and `copilot-instructions.md` (the handover for coding agents)
- `.kiro/` — rules for the Kiro coding agent (`steering/`: how a session starts, how the builds given to the owner are named)
- `CHANGELOG.md` — each release's notes; `LICENSE` — GNU GPL version 3 or later; `LICENSE-MIT` — the MIT notice of
  the code written before 17/11/2026, which stays available under it
- `pyproject.toml`, `uv.lock` — the Python tools' dependencies (`uv sync`); `.gitignore` — what never goes in git

## Project goals

- Recompile the game without CPU emulation or JIT execution
- Reimplement the runtime at the API level instead of emulating console services
- Keep the runtime faithful to console defaults and behavior
- Validate fixes with targeted tests, captures, and probes before wider game runs

## Getting started

This project uses Python tooling via `uv`.

```bash
uv sync
uv run tools/analisis/bindb.py
```

For repository conventions and development guidelines, see:

- `AGENTS.md`
- `CONTRIBUTING.md`
- `docs/roadmap.md`

## Tools for developers

Windows executables built by the workflows of `.github/workflows/`, so a tester or another developer can answer a
question without installing Python or uv (Actions tab > the workflow > Run workflow, then the run's artifacts). Each
one reads the player's own files on their machine and writes a text report; nothing of the game goes in.

| Tool | Workflow | What it answers |
|---|---|---|
| `orbitals-frame-report.exe` | `frame_report.yml` | Names the game's functions from your own `exefs/` (no game code in the repo): `--stack` turns the thread dump of an abort in `orbitals.log` into named frames. How the freeze of November 2026 was read |
| `shader_analyzer.exe` | `log-analyzer.yml` | A report of a whole `orbitals.log`: how the run ended, key threads, missing shaders, NVN sync call sites ([docs/log-analyzer.md](docs/log-analyzer.md)) |
| `port_check.exe` | `port_check.yml` | The feasibility report of a game dump against the runtime: which SDK functions it imports that the runtime does not replace, module sizes, build ids (a new game version starts here) |
| `romfs_check.exe` | `romfs_check.yml` | What a `.nup`'s RomFS holds, from its table of contents only |

The Python originals are in `tools/analisis/` and `tools/run/`; `tools/README.md` lists the rest (replays, A/B
measurements, shader tools).

## Documentation

- `docs/README.md` — documentation index
- `docs/roadmap.md` — current work and priority items
- `docs/research.md` — source and research notes
- `i18n/README.md` — translation workflow and text catalogs

## Rules and expectations

The project follows strict rules for correctness and compatibility:

- no CPU interpreter or JIT
- no GPU emulation below NVN2
- no game assets or generated game code checked into git
- source code must comply with licensing and attribution rules
- fixes should be implemented at the source of the bug, not disabled globally
- tests and measurements should be run before and after relevant changes

## Status

This repository is a work in progress. See the roadmap and development notes for the current implementation priorities.
