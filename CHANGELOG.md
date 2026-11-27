# Changelog

Each version's section is that release's notes: `tools/build/notes.py` puts it into `docs/release-template.md` (install,
updating, known issues, reporting) for the GitHub release. A section is headed `## [X.Y.Z] - YYYY-MM-DD` and has a
summary paragraph, `### Changes`, `### Tested` (what was run before publishing) and optionally `### Known issues`.
The releases of the project this one was forked from (0.1.0 to 0.1.12) are in git history, not here.

## [0.1.0] - 2026-11-27

First release of Orbitals Together, the new name of `orbitals-recomp`. Orbitals runs from boot to its levels in split
screen at 30 fps, and the game's Online Play opens Steam's Remote Play Together invite list so a friend can join the
local mode for 2.

### Changes

- The program is `orbitals-together.exe` (was `orbitals-recomp.exe`), the window, the start screen, the version
  resource and the package say "Orbitals Together". `orbitals.ini`, `ORBITALS_*` and the folders are unchanged.
- The game's Online Play ("Partie en ligne") opens the Remote Play Together invite list (`nifm::HandleNetworkRequestErrorResult`).
- Fixed in game: flickering, stripes on the asteroid after a return to the menu, the blue disc and the grey lines.

### Tested

- Played with a friend over Remote Play Together (27/11/2026), build `d116ac3` before the rename.

## [0.2.0] - 2026-11-16

First Orbitals release. `orbitals-recomp` recompiles Orbitals (Nintendo Switch 2) from your own dump; it no longer
carries anything of the game the runtime was first written for.

### Changes

- Removed all MKW-specific code, settings, menu entries, and documentation ([docs/mkw-removal.md](docs/mkw-removal.md)):
  that game's version rows, patch tables and patch sets, its text tables, and the hacks and tools built on its data.
- Renamed all `MKW_*` environment variables to `ORBITALS_*`; the former names are not read. Run
  `tools/migrate_config.py --apply` once to migrate existing settings (a dry run without `--apply`). The settings file
  is `orbitals.ini`, the log `logs/orbitals.log`, the program `orbitals-recomp` / `orbitals-recomp.exe`
  ([docs/environment-variables.md](docs/environment-variables.md)).
- Removed the GameBanana mods tab, Quick Race, Skip Movies, seqwatch, `race.sh`/`live.sh`, `unlock.c` and
  `romfs_res.c` (with the Resolution and Detail distance settings): each read the first game's menus, movies, save or
  RomFS files. The Mods page shows the installed mods.
- The A/B measurement clock (`ORBITALS_AB`, `tools/run/ab.py`) now starts at the first frame displayed (previously at
  the race start). Baselines must be measured again.
- Translated READMEs removed; they will be regenerated from the current README.
- The project's repository is [sinedgiveaways/pmocer-latibro](https://github.com/sinedgiveaways/pmocer-latibro): the
  update check, the issue links and the issue forms point there. The known-errors list starts empty.
- The executable carries a version resource (Orbitals Recomp 0.2.0) and the Orbitals icon.

### Tested

- The Windows executable built on GitHub Actions (`.github/workflows/build-windows.yml`) with no former name in it:
  0 `MKW_`, `Mario Kart`, `mkw-recomp` and `adriadam10`, in UTF-8 and UTF-16.
- The Python tests (106 passed) and `tools/migrate_config.py` on a Windows runner (user environment, PowerShell
  profile, `mkw.ini`).
- `romfs_check.exe` on the Orbitals `.nup` (base v65536, update v327680, add-on).
- Not tested: a run of the game itself.

### Known issues

- Orbitals does not get far yet: its boot constants are not measured, 991 of its 1 305 SDK imports and 366 of its 731
  NVN2 procs are not covered, its shaders (in `.pak`/`.utoc`) are not extracted, and the shader translator lacks the
  Ampere image atomic with its handle in a register (instruction 394). See [docs/roadmap.md](docs/roadmap.md).
