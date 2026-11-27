# Starting a session on the Orbitals port

You are continuing work already under way. Do not start over: the state is written down.

## Read first, in this order

1. `docs/roadmap.md` section **6.8** (the current state; dated, newest wins over older sections).
2. `.github/copilot-instructions.md` (branch, builds, the owner's way of working).
3. `AGENTS.md` (hard rules), then `docs/workflow.md` (dumps, the control pipe, tools on the owner's machine).
4. `git log --oneline -15` on `orbitals-port`: what changed since 6.8 was written.

5. `docs/steam-remote-play-status.md` when the work is Steam Remote Play Together or Online Play.

## The exchange repository: logs, captures, videos, dumps

The owner never sends files in the chat. They upload them, usually as a `.7z`, to the release **`analysis-data`** of
the repository **`sinedgiveaways/tool`** (formerly `Lichii-tool`, an Eden fork used only as a measuring tool), then
write a short message such as "log up", "c up" or "crash log up". An upload is the answer to the last test asked for.

- List: `gh api repos/sinedgiveaways/tool/releases/tags/analysis-data --jq '.assets[] | "\(.id) \(.name) \(.size) \(.updated_at)"'`
- Download: `gh api -H "Accept: application/octet-stream" repos/sinedgiveaways/tool/releases/assets/<id> > file.7z`,
  then extract it (py7zr).
- Delete each asset once its analysis is finished, without asking:
  `gh api -X DELETE repos/sinedgiveaways/tool/releases/assets/<id>`
- What comes there: `orbitals.log`, screenshots (4K), videos, frame dumps (`dump_*` folders, about 1 GB),
  `frame_report.txt`, `census.txt`. None of it goes into git. The console reference video of the first level
  ("ORBITALS #01 - LE DÉPART") is in `_work/ref/` when that folder exists.

## The owner

- Writes in French in short messages ("c up", "crash log up"): answer in French, plainly.
- Windows 11, RTX 4080; game folder `C:\Users\mco2v\Downloads\orbitals-together\` (`exefs\`, `logs\`, `orbitals.ini`).
- Wants few, decisive tests: say what you expect and what each outcome means; one run per hypothesis. When you offer
  options, they usually let you choose ("choisis la meilleure solution").
- Builds to test are GitHub Actions run links, artifacts `kiro-orbitals-together-exe` (runtime only) or
  `kiro-orbitals-together-windows-x64` plus a setup run (translator changes). See `build-naming.md`.

## Working rules you must keep

- Commit and push on `orbitals-port`; Conventional Commits in English ending with "AI-assisted (Kiro).".
- Nothing of the game in git (no dumps, captures, disassembly, SPIR-V); the owner's files go through the release only.
- "Fix, don't disable"; nothing serial on the game or replay threads; `flow_check.py` before any control-flow change
  of the shader translator.
- Update the folder README when adding a file, and `docs/roadmap.md` 6.8 (or a newer dated section) when something
  is found or fixed, so the next session can continue.

## The sandbox

- The runtime builds only on CI; locally only a syntax check:
  `INC=$(find runtime -type d -not -path "*/.*" | sed 's/^/-I/' | tr '\n' ' '); clang -fsyntax-only -std=gnu11 $INC <file>`
  (missing `vulkan/vulkan.h` and the `simd_fp.h` builtins errors are expected).
- There is no ExeFS: `tools/analisis/q.py` cannot name game functions here. The owner runs
  `orbitals-frame-report.exe` (workflow `frame_report.exe`, `--stack` for an abort's frames) and uploads the report.
- Tools under `tools/` use Python 3.14 syntax; `uv` may be unusable (read-only home). A work folder from earlier
  sessions may exist at `/projects/sandbox/_work` (Python venvs with py7zr/cv2/numpy, the console reference video under
  `ref/`, extracted logs); use it if present, never commit from it.
