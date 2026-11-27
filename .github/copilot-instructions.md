# Instructions for GitHub Copilot (and any coding agent) on orbitals-together

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
  Windows"`). Builds for the owner are named `kiro-...`: artifact `kiro-orbitals-together-exe` when only the runtime
  changes, the whole `kiro-orbitals-together-windows-x64` package and a setup run when the translator changes.
- `orbitals-frame-report.exe` (workflow `frame_report.exe`) names the game's functions on the owner's machine; the
  sandbox has no ExeFS, so `tools/analisis/q.py` does not work there (docs/workflow.md).

## The task in progress (27/11/2026)

Read [roadmap 6.8](../docs/roadmap.md) first: it is the current state. In short:

1. **Online Play through Steam Remote Play Together** (current work): the owner chose that the game's Online Play
   entry opens Steam's invite list, and the friend then plays the local mode for 2 by streaming. Built in `5ba7f29`
   (run 45919377416) and waiting for the owner's test. Whether Orbitals' Online Play makes either trigger call (nifm's
   error applet, the account sign-in) is unknown: the log's `online:` lines will say. Details:
   [steam-remote-play-status.md](../docs/steam-remote-play-status.md).
2. Remote Play Together itself works with a friend (the owner's test, 27/11/2026).
3. With build `81c27bf` the owner reports everything fixed: the asteroid stripes (`e30dd58`), the freeze, the blue
   disc and the grey lines. The freeze came in 2 runs out of 5, so its diagnostic (`81c27bf`: the timeout asked and the
   waited object at an abort) stays until more runs confirm it.

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
- The owner's game folder is `C:\Users\mco2v\Downloads\orbitals-together\` (`exefs\`, `romfs\`, `aoc\`, `logs\`,
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

## Public releases (the owner's order, 27/11/2026)

- Development and every test build stay on `sinedgiveaways/pmocer-latibro` (`orbitals-port`, artifacts `kiro-...`).
- Only builds the owner calls final are published, on the public repository **`sinedgiveaways/Orbitals-Together`**
  (renamed from the first, misspelled Obitals-Together on 27/11/2026; GitHub redirects the old name): a release per version tag (`v0.3.0` first), with the full Windows package
  (`orbitals-together-<version>-windows-x64.zip`) as its asset, step-by-step READMEs in English (`README.md`) and
  French (`README.fr.md`) (download, setup, local play, Remote Play Together, forking), and the full source of that tag
  so anyone can fork it (the tag's tree without `.kiro/` and this file; its `README.md` kept as `DEVELOPMENT.md`). Never publish there without the owner saying a build is final.
- To publish: on pmocer-latibro add the version's `CHANGELOG.md` section and `.rc` version, tag `vX.Y.Z`, push it,
  and have the owner test `Build Windows` on the tag (`.../build-windows.yml/dispatches -f ref=vX.Y.Z`). Then replace
  Orbitals-Together's tree with the tag's (`git archive vX.Y.Z`, without `.kiro/` and this file; `README.md` kept as
  `DEVELOPMENT.md`; its own `README.md`, `README.fr.md` with the new version and file name), commit on `main`, tag
  `vX.Y.Z` there, create the release (`gh api repos/sinedgiveaways/Orbitals-Together/releases -f tag_name=...`) and run
  its **Publish release** workflow (`.../publish.yml/dispatches -f ref=main -f "inputs[tag]=vX.Y.Z"`): it builds the
  zip from the tag and attaches it. The agent cannot upload release files itself (uploads.github.com is not reachable
  through its GitHub access). Put the zip's sha256 from the workflow log in the release notes. 0.3.0 was published so
  on 27/11/2026: https://github.com/sinedgiveaways/Orbitals-Together/releases/tag/v0.3.0. The launcher's update check
  and bug links point to Orbitals-Together.
