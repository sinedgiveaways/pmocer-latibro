# Instructions for GitHub Copilot (and any coding agent) on orbitals-recomp

You are taking over the Orbitals port. Read, in this order, before changing anything:

1. [AGENTS.md](../AGENTS.md): the hard rules (no CPU or GPU emulation, nothing of the game in git, licenses, console
   defaults, fix rather than disable) and how to test without the game.
2. [CONTRIBUTING.md](../CONTRIBUTING.md): commits, pull requests, READMEs.
3. [docs/roadmap.md](../docs/roadmap.md): where the game is and the open problems, most blocking first.
4. [docs/shaders.md](../docs/shaders.md): how Orbitals' programs reach SPIR-V, the colony's frame pass by pass, what
   was found.
5. [docs/workflow.md](../docs/workflow.md): how to work with the player's machine, the tools, exchanging data.

## The project in one paragraph

A static recompilation of Orbitals Deluxe Edition (Switch 2) for Windows: the game's AArch64 code is lifted to C
(`recomp/`), the runtime replaces the console's SDK and NVN2 graphics over Vulkan (`runtime/`), and the game's SM86
shaders are translated to SPIR-V (`sm86/`). The game runs from boot to its first level (the colony, split screen) at
30 fps. Lichii (`sinedgiveaways/tool`, an Eden fork) is only a measuring tool for what the console does.

## Where the work is

- Since 25/11/2026 the work is committed and pushed directly on `orbitals-port` (the default branch); earlier work was
  on `diag/frame-history-trace` (PR #6). Never merge or force-push without the owner. Where a `gitlab` remote is
  configured, push the GitLab copy too.
- A push builds Windows (`Build Windows`) and runs `Tests`. Give the owner the run link
  (`gh api "repos/sinedgiveaways/pmocer-latibro/actions/runs?branch=orbitals-port&per_page=6"`, `.name=="Build
  Windows"`). Builds for the owner are named `kiro-...`: artifact `kiro-orbitals-recomp-exe` when only the runtime
  changes, the whole `kiro-orbitals-recomp-windows-x64` package and a setup run when the translator changes.
- `orbitals-frame-report.exe` (workflow `frame_report.exe`) names the game's functions on the owner's machine; the
  sandbox has no ExeFS, so `tools/analisis/q.py` does not work there (docs/workflow.md).

## The task in progress (27/11/2026)

Read [roadmap 6.8](../docs/roadmap.md) first: it is the current state. In short:

1. **Asteroid stripes** after going back to the main menu and loading the save: fixed in `e30dd58` (a revived texture
   entry kept its previous life's `gpu_lv`), waiting for the owner's confirmation in game.
2. **Freeze "GameThread timed out waiting for RenderThread"** (2 runs in 5): our kernel lost no wakeup (`c03f2a0`,
   logs81); the RenderThread waits without limit for an event nobody triggers. Next: the owner's `frame_report.txt`
   from `orbitals-frame-report.exe --stack`, to name the frames and the event.
3. Not studied yet: a bright blue disc above the right player in the colony corridor (intermittent), long grey lines
   in the corridor (particles?).

## Older task (19/11/2026, kept for history)

The colony's environment is dark; Lichii shows it bright. The lighting shader (`d18042401d56bbea` in frame dumps)
reads a 3D r32ui light grid per half and skips cells equal to `0xffffffff`; both grids are all `0xffffffff`. At the
level's load, compute `a29340cb16aff1e1` clears them (it is Unreal's ClearUAV, not the fill). The real fill is another
writer, most likely through a buffer over the same memory (the grid's guest memory went from garbage to zeros with no
CPU write and no re-upload of the image). The current build, with `ORBITALS_TRACE_3D=1`, logs any dispatch whose
storage buffers or buffer textures reach a 3D image's memory (`vk: trace3d <hash> (its storage buffer ... reaches the
3D image at ...)`), with its bindings and uniform blocks, and writes its SPIR-V to `<game>/trace3d/`. Next: get that
log and folder, read the writer (spirv-cross `--vulkan-semantics`), and make buffer writes into a texture's storage
reach the image the lighting samples (runtime/gfx/vk/tex: the image re-read from the buffer's mirror after such a
write, or a buffer-to-image copy at use). Update (recomp51): no buffer writer found; the CPU rewrites the grids' pages with zeros
(re-uploads at frames 1580, 1606, 1635) and the ClearUAV compute writes ones, so nothing puts lights in them on our
side. Compare with Lichii's GPU trace of the same pass before going further (roadmap 1.1b). Details: roadmap 1.1b,
docs/shaders.md.

Other open items: two refused fragment programs (`tex_scr_urc_`), bright speckled patches on lamps, a crash at
`main+0x1759b20` reading 0x104 in about one start in three (roadmap 1.2), silent logos 2 and 3, black title and menu
backgrounds.

## Working with the owner

- The owner writes in French and runs the game on Windows 11 with an RTX 4080; answer in French.
- They have no Python of their own: commands use the package's `.\setup\python\python.exe` from the game folder,
  with forward slashes in paths.
- A translator change reaches them without a build:
  `Invoke-WebRequest https://raw.githubusercontent.com/sinedgiveaways/pmocer-latibro/<commit>/<path> -OutFile setup/src/<path>`.
  A runtime change needs a build: give them the Actions run link.
- They upload logs, frame dumps and recordings as `.7z` to the `analysis-data` release of `sinedgiveaways/tool`.
  Download each asset (`gh api -H "Accept: application/octet-stream" repos/sinedgiveaways/tool/releases/assets/<id>`)
  and delete it right after (`gh api -X DELETE repos/sinedgiveaways/tool/releases/assets/<id>`). Use the name `tool`,
  not the former `Lichii-tool`.
- Before each run you ask for, say what you expect and what each outcome would mean; one run per hypothesis.
- The owner finds many runs tedious: prefer one decisive test (a dump at the moment of the defect, a diagnostic that
  answers at the next occurrence) over several blind runs. They let the agent choose between the options it proposes.
- Delete each `analysis-data` asset once its analysis is finished, without asking.
- The owner's game folder is `C:\Users\mco2v\Downloads\orbitals-recomp\` (`exefs\`, `romfs\`, `aoc\`, `logs\`,
  `orbitals.ini`). A key of `orbitals.ini` is an `ORBITALS_*` switch without the prefix.

## Rules that are easy to break

- Nothing derived from the game in a commit: no SPIR-V, GLSL, disassembly, dumps, pictures of the game. Game code
  (`show.py`, disassembly) only goes into the chat.
- Every folder has a README listing its files; `tools/docs/check.py` must pass. Settings the player sees need a row in
  `runtime/core/base/cfg.c`; `docs/environment-variables.md` is generated by `tools/run/envs.py --markdown`.
- Conventional Commits in English; say in the commit body and the PR that the change was AI-assisted (the commits of
  November 2026 end with "AI-assisted (Kiro).").
- Comments say why, with the measurement or source; cite code taken from other projects (MIT/BSD/Apache/GPL-compatible
  only) in the code and in `docs/research.md`.
- Keep `docs/roadmap.md` and `docs/shaders.md` current when something is found or fixed.
