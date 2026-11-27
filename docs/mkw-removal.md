# MKW removal

What of the first game (Mario Kart World) and of the project this one is forked from (mkw-recomp) was removed from
the product, what was kept and why, and the proof from the built executable. Filled commit by commit on the branch
`cleanup/mkw-removal` (pull request #2 into `orbitals-port`). Its plan, starting inventory and audit, and the settings
migration script were removed on 27/11/2026 once the removal was done (in git history before `605a352`).

## Removed from the product

| What | Commit | Why |
|---|---|---|
| The game's own text tables `i18n/game/*.tsv` | `a970f7d` | Text of the first game; game content does not go in the repository |
| MKW's rows in `recomp/versions.py` and `runtime/core/boot/version.c`, `recomp/patches-mkw*.txt` | `9952d83` | Addresses and patch sites of MKW builds; Orbitals has its own row |
| Patch sets shadow, seed, aspect, output, camera, frame rate; `ov_zstd.c` | `9952d83` | Hooks on MKW functions and classes |
| `MKW_*` names: every setting is `ORBITALS_*`, no alias | `3c30c42` | Product name (the one-shot migration script was removed on 27/11/2026) |
| The settings FPS, aspect, shadow size | `3c30c42` | No effect without the MKW patch sets |
| Artifact names: `mkw.exe`, `libmkw.dll.a`, `mkw.def`, `mkw.rc`, linker sections, `orbital.ini`, logs, Linux data folder | `285e3cf` | Product name `orbitals-recomp` |
| `romfs_res.c`, the settings Resolution and Detail distance, `tools/romfs/resolution.py` | `b2ed262` | Reads a RomFS file Orbitals does not have ([romfs-res-audit.md](romfs-res-audit.md)) |
| Links to `adriadam10/mkw-recomp`, the `mkw-recomp:` link scheme | `09ea19e` | The project's repository is `sinedgiveaways/pmocer-latibro` |
| Known-issues signatures list (`tools/github/signatures.toml`) | `d4ee6d3` | Known-issues signatures list emptied: the MKW entries referenced issues from the original repo. Orbitals-specific signatures will be added as errors are discovered |
| Texts naming the first game, and the texts of removed settings, in `i18n/` | `1b5e55e` | The missing-game help and wrong-build error name Orbitals; 13 ids of removed settings dropped from the 21 catalogs |
| Unlock everything (`runtime/core/hacks/unlock.c`, `unlock_ids.c`, `ORBITALS_UNLOCK_ALL`) | `5dde6e8` | See "unlock.c audit" below |
| The Mods page's GameBanana tab (`mods_browse.cpp`, `tools/mod/gamebanana.py`, `--gb-install`, the `orbitals-recomp:` link handler, `plat_url_scheme`, `stb_image.h` used only for its previews, 17 texts) | `c04d70c` | GameBanana mods tab removed: it listed Mario Kart World mods (game ID 22152). Orbitals has no GameBanana presence. The mods page now only shows local mods |
| Quick Race (`ORBITALS_QUICK_RACE`, its menu row, 4 texts, the press scripts in `input_script.c`) | `ddad8e5` | Quick Race removed: it navigated MKW's menus (time trial, Mario, standard kart). Orbitals has no equivalent menu structure |
| Skip Movies (`runtime/core/hacks/skip.c`, `ORBITALS_SKIP_MOVIES`, its menu row and 2 texts) | `f108942` | Skip Movies removed: it targeted MKW's cutscene and circuit intro videos. Orbitals has its own video files (.bk2) but no equivalent skip mechanism was implemented |
| `runtime/core/diag/seqwatch.c` and the race run scripts (`race.sh`, `live.sh`, `at.sh`, `press-*.txt`, `rqwait.py`, `tools/analisis/seq_nodes.py`; the `ORBITALS_SEQ_*`, `ORBITALS_PROBE`, `ORBITALS_FN_WATCH`, `ORBITALS_UI_FOCUS` settings, Turbo's `until` mode (on until the race started), the ctl `seq` command (the active sequence nodes), script lines by game phase) | `b6203e3` | seqwatch.c and race.sh/live.sh removed: MKW-specific race measurement tools. Orbitals benchmarking will use the generic tools/run/ab.py (A/B) and the CI build timing. The A/B world clock now counts from the first present; `fnprof_hit` moved to `impcensus.c`, the hook's other user |
| The LAN setting's description | `8033680` | LAN description rewritten: no longer references MKW's Wireless Play. The setting stays: it still makes `nifm`'s local network mode and the sockets work at the SDK level; whether Orbitals uses them is not known yet. Translations cleared except French |
| The README's translations (`docs/i18n/`, `tools/docs/i18n_readme.py`) | `c1787e5` | Translated READMEs removed: all six described MKW (versions 1.0.0/1.8.0, cups, menus, race state). Will be regenerated from the current README when translators are available |
| The first game in the user documentation (README, AGENTS.md, roadmap, research, release notes template, issue forms, Remote Play guide) | `c1787e5` | The roadmap lists what blocks Orbitals; the research page holds Orbitals' measurements, the first game's are in git history |
| The program's name in runtime strings (`--replay`/`--unpack` usage, flight dump header, Steam log lines, the virtual gamepad "Orbitals virtual gamepad") | `0c96088` | They said `mkw` |
| `CHANGELOG.md` 0.1.0-0.1.12 | `eb22f15` | The earlier project's releases; in git history. 0.2.0 is the first Orbitals release |
| Comments naming MKW in the sources every package ships in `setup/src` (`recomp/`, `sm86/`, the tools of `tools/build/dist.py` SOURCES) | `2753db9` | They now say "the first game"; the scanner finds nothing there |

## Kept, with the reason

| What | Why |
|---|---|
| `LICENSE` (MIT, the original author's copyright) and the copyright lines in file headers | The MIT licence requires the notice to stay in copies |
| `recomp/LICENSE.totk-pc-recomp`, `runtime/LICENSE.totk-pc-recomp` | Same, for totk-pc-recomp |
| Third-party sources named in comments: `seedds/mkw-neo`, MacNativeGames' `mkw_vpool.c`, `mkw_xlate` and `mkw_nvn_ext.c`, MKW-THWOMP | Attribution of where code or ideas came from; comments, not in the executable |
| `docs/performance.md`'s table of the first game's reports | Historical measurements, under their own heading, with links to the reports where they were filed |
| `docs/research.md`'s sources | Third-party projects keep their names and links (MKW-THWOMP, Eglantine-MKW, mkw-neo, a news article's address) |
| `CHANGELOG.md` 0.2.0 | It contains "MKW-specific" and "MKW_*" as migration instructions for existing users. It is a user-facing document, not part of the executable |
| `tools/i18n/game_texts.py` (shipped in the package) | It rebuilds the first game's message archives for languages it lacks; with `i18n/game/` gone it does nothing. To remove or rewrite once Orbitals' text format is known |
| About 290 comments in the repository's other sources (`runtime/` 175, `tools/` 62, `tests/` 16, `.github/` 21, removal documents) | Notes of how a part was measured or found in the first game. They are not in the executable nor in the package; rewording them is left for when each part is next changed |

## unlock.c audit

`unlock.c` edited the save when the game opened `Play/account.sav`: it added every driver, vehicle and sticker id to
the owned lists, revealed the Special Cup and opened Mirror. Everything in it is the first game's: the file name, the
save's magic and key hashes, a fallback to that game's 1.0.0 offsets, and 1 100 item ids taken from Ca1nM's Mario Kart
World save editor (MIT). For any other save it logged "not a layout this knows" and changed nothing, so for Orbitals the
Hacks tab's "Unlock everything" row did nothing. Removed with its setting, menu row and texts; an Orbitals equivalent
would need its own save format, found from its saves.

## Proof in the executable

From the Windows build of pull request #2 (commit `2146a66`, run 43959504391, `.github/workflows/build-windows.yml`,
step "Old names in the executable"). `tools/build/check_names.py --strict` runs on `orbitals-recomp.exe` in that step
and fails the build on any match.

| Name looked for (case-insensitive) | UTF-8 | UTF-16LE |
|---|---|---|
| `MKW_` | 0 | 0 |
| `Mario Kart` | 0 | 0 |
| `MarioKart` | 0 | 0 |
| `mkw-recomp` | 0 | 0 |
| `adriadam10` | 0 | 0 |
| `mkw` as a word | 2, both not text (below) | 0 |
| `ORBITALS_` (for comparison) | 662 | 0 |

The two bare `mkw`, located by section:

| Where | Bytes | Why it is not text |
|---|---|---|
| `.rsrc+0x13168` | `...x\xe6mkw\x10\xd1K...` | Inside an IDAT chunk of the icon's 256 px PNG: compressed pixels of the Orbitals picture |
| `.text+0x24d117` | `...\x10\x15mkW\x00\xe9...` | Inside the machine code section: three bytes of x86-64 instructions |

The scanner lists such matches apart ("not text, not counted") and counts the longer names in every section. The
version resource reads `FileDescription` Orbitals Recomp, `ProductName` Orbitals Recomp, `OriginalFilename`
orbitals-recomp.exe, `ProductVersion` 0.2.0.

The package folder (`build/dist/windows`, what the zip holds) has 351 matches, all in third-party files: zig's
`lib/` (`__builtin_ve_vl_vfmkw...` intrinsics, a Linux header's `__mkw` macro) and the bundled Python (hashes and
certificates). Its `setup/src` (this repository's sources) has none.

## After the merge

- The workflows run on pushes and pull requests to `orbitals-port`; the first run after the merge should be checked.
- A run of `orbitals-recomp.exe` on the Orbitals `.nup` is the only regression test this branch did not have.
