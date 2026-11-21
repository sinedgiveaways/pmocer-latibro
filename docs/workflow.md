# Working on Orbitals with a player's machine

How the work of November 2026 was done, for whoever takes it over: the game, its dump and the GPU are on a player's
Windows PC; the code, the builds and the analysis are elsewhere. What is open is in [roadmap.md](roadmap.md).

## Repositories and branches

| Repository | Branch | What it is |
|---|---|---|
| `sinedgiveaways/pmocer-latibro` | `orbitals-port` (default), `diag/frame-history-trace` (the work since 16/11/2026) | The static recompilation: `recomp/` lifts AArch64 to C, `runtime/` replaces the SDK and NVN2 (over Vulkan), `sm86/` translates shaders to SPIR-V |
| `sinedgiveaways/tool` (named `Lichii-tool` until 18/11/2026; the old URLs redirect for reads, not for deletes) | `analysis/recomp-build` | An Eden fork used as a measuring tool: it runs the real SDK and NVN2 driver; its GDB stub, `lichii-probe.exe`, its GPU traces (`docs/nx2/gpu-trace.md`) and its code say what the console does |

The recompilation never emulates the CPU or the GPU. Lichii is only a reference to measure against.

`sinedgiveaways/mkw-recomp` (the first game's recomp this one was forked from) is mirrored the same way at
`gitlab.com/sinedgiveaways/mkw-recomp`.

`pmocer-latibro` is also on GitLab, at `gitlab.com/sinedgiveaways/pmocer-latibro` (since 18/11/2026, every branch and
tag copied from GitHub). GitHub stays the main repository: issues, pull requests, the CI and the releases are there.
GitLab only holds the code, pushed by hand: after a push to GitHub, push the same branch to GitLab too
(`git push gitlab <branch>`; branches and tags as on GitHub).

## Builds

- A push to the branch starts the Windows build and the tests (`Build Windows`, `Tests`); `gh api -X POST
  repos/sinedgiveaways/pmocer-latibro/actions/workflows/build-windows.yml/dispatches -f ref=<branch>` starts one by
  hand. The artifact is `orbitals-recomp-windows-x64`; the run's page
  (`https://github.com/sinedgiveaways/pmocer-latibro/actions/runs/<id>`) is the link to give the player.
- The player copies it over their `orbitals-recomp` folder, keeping `spirv/`, `spirv-dump/` and `aoc/`.
- The game code (`orbitalgame.dll`) is generated and compiled on the player's machine by the setup. Its key is
  `recomp/*.py`, `runtime/cpu/*.h`, `tools/build/build.py` and `tools/prepare/gamecode.py`
  (`tools/prepare/prepare.py` `COMPILER`): a change there recompiles the game at the next start (minutes), a change
  elsewhere in `runtime/` only needs the new executable. To check a recompile took place: the log's
  `lifted code exe+...-exe+...` range changes size.
- The shader translator (`sm86/`, `tools/shaders/`) runs from `setup/src` with the package's own Python. A change
  there can reach the player without a build:
  `Invoke-WebRequest https://raw.githubusercontent.com/sinedgiveaways/pmocer-latibro/<commit>/<path> -OutFile setup/src/<path>`,
  pinned to a commit (the branch URL can serve a cached older file for minutes).

## Commands on the player's side

From the `orbitals-recomp` folder, in PowerShell. Paths use forward slashes: a `\n` in a pasted path (as in
`prepare\nup.py`) becomes a line break, which cut every early attempt of these commands in two.

| Command | What it does |
|---|---|
| `.\setup\python\python.exe setup/src/tools/prepare/nup.py addons "<game>.nup" .` | Unpacks the add-ons (the Deluxe content) into `aoc/<index>/`; the setup's romfs step does it too |
| `.\setup\python\python.exe setup/src/tools/prepare/dumped_shaders.py --dumped spirv-dump` | Translates every dumped program into `spirv/`; prints the valid count and the refusal reasons (`spirv/failed.tsv`). The runtime runs it by itself during a run since 21/11/2026 (`logs/shaders-auto.log`) |
| `.\setup\python\python.exe setup/src/tools/shaders/base_origin.py --dumped spirv-dump [--tex]` | Counts where the untraced storage buffer bases (or, `--tex`, texture handles) of the refused programs come from |
| `.\setup\python\python.exe setup/src/tools/shaders/c0_reads.py --dumped spirv-dump [--banks]` | Counts which constant bank words the programs read at fixed addresses, by stage |
| `.\setup\python\python.exe setup/src/tools/shaders/missing.py --log logs/orbitals.log` | The dumped programs without SPIR-V, by stage, with the translator's reason and whether that run met them |
| `.\setup\python\python.exe setup/src/tools/shaders/class_diff.py --keys BAD,GOOD` | Two programs side by side: render targets, depth output and inputs from the header, the IPA addresses and constant words only one reads, the instruction forms whose counts differ |
| `.\setup\python\python.exe setup/src/tools/shaders/base_origin.py --keys KEY,...` | Where the texture handles of those programs come from (constant words, OR'ed sampler words) |
| `.\setup\python\python.exe setup/src/tools/shaders/show.py [--stage 2] [--keys PREFIX]` | The dumped programs of one stage as decoded instructions (geometry by default). Game code: into a chat only |
| `.\setup\python\python.exe disasm_fn.py exefs main+0x<offset> --count N` | Disassembles game code at a crash address (`tool/tools/nx2/recomp/disasm_fn.py` in the Lichii repository); its output only goes into a chat, never into a file in a repository |

A run that meets new programs dumps them; translating them and running again is the loop that brought the level from
211 to 687 programs. Each pass can reveal more.

## Exchanging data

The player uploads `.7z` archives to the `analysis-data` release of `sinedgiveaways/tool`; the assets are listed with
`gh api repos/sinedgiveaways/tool/releases/tags/analysis-data --jq '.assets[]|"\(.id) \(.name)"'`, each one downloaded
with `gh api -H "Accept: application/octet-stream" repos/sinedgiveaways/tool/releases/assets/<id>` (`gh release
download` fails in some environments) and deleted right after (`gh api -X DELETE repos/sinedgiveaways/tool/releases/assets/<id>`).
Use the repository's current name: after the rename, deletes through `Lichii-tool` answered 401.

Nothing derived from the game goes into a repository or stays on a release:

| May be uploaded | Must not be uploaded |
|---|---|
| `orbitals.log`, `spirv/failed.tsv`, the census outputs above, a frame dump's `images.txt`, `passes.txt`, `stats.txt`, screen recordings | `spirv-dump/`, `spirv/*.spv`, a dump's `.png` and `sh_*.spv`, `exefs/`, the RomFS, Lichii GPU traces (`.lxtrace`), disassembly |

A recording is read by extracting frames and the audio level per second with ffmpeg (a contact sheet every few
seconds; `astats` RMS per second to tell a silent logo from a quiet one).

## Diagnosing

- **Log lines worth reading first:** `FATAL`, `device fault`, the GPU breadcrumbs (the hang lies after the last
  finished mark and up to the last started one), `gfx_shader: N programs without SPIR-V dumped`, `no prepared SPIR-V`,
  `write-backs merged by bytes since the last line`, `texel buffer views: the table was full`. Filter the noise with
  `Select-String -NotMatch` on `AllocateMemoryPages`, `sdk call`, `fs entry`, `nn::codec: queued`.
- **A frame dump:** `DUMP = <frame>` in `orbitals.ini` writes `dump/` (one PNG per pass, `images.txt` with each
  target's min and max per channel and a NaN count, `passes.txt` with every pipeline, its shaders and bindings,
  `stats.txt` with primitives and fragment invocations per pass). `images.txt` alone says at which pass a picture goes
  black. The frame number is counted from boot; the log's `frametime: exit` and `replay queue over the run to frame N`
  lines say where a scene was (the colony: 4500). Wait about 15 s after the stutter before quitting, or `stats.txt` is
  missing.
- **Which images a dump copies:** a whole frame fills the in-flight copy budget early ("too many copies in flight"
  from about pass 257 in the colony). Narrow it: `DUMP_FS=<hash>[,...]` copies only the render passes drawn by those
  fragment shaders (the SPIR-V hashes `passes.txt` prints; they stay the same while the translation does, pass numbers
  do not), `DUMP_FS_AFTER=<n>` the n passes after each of them too, `DUMP_PASSES=<from>-<to>` a range of one run's
  numbering, `DUMP_SKIP_RUNS=1` drops passes with the same targets as the one before. `DUMP_UBO=1` prints each
  pipeline's uniform blocks (rows with NaN or infinity listed: often integers seen as floats).
- **Program keys and SPIR-V hashes:** the runtime and the translator name a program by its key (sha256 of control and
  code, `spirv/<key>.*`); a dump names shaders by an FNV-1a hash of the SPIR-V (`sh_<hash>.spv`). Without the player's
  `spirv/`, match them by shape: `class_diff.py` gives a program's inputs, outputs and textures, `spirv-dis` on the
  dump's `sh_*.spv` the same (inputs, `UniformConstant` count).
- **A/B runs:** one setting changed, 3 to 4 starts each, the crash count compared. A crash that happens one start in
  three needs at least that many to rule a cause out.
- **A crash in game code:** the report gives the lifted function, `pc`, `lr` and the guest frames; `disasm_fn.py` on
  the player's exefs shows the code around them.
- **Lichii first** when the question is what the console does: the probe for SDK call arguments and results, its code
  for the NVN2 driver's behavior (root tables, push constants), a GPU trace for a whole frame.

Settings used to bisect (all in [environment-variables.md](environment-variables.md)):

| Setting | Values |
|---|---|
| `WB_MERGE` | 1 (default) shader stores merged back by bytes on the CPU; 0 copied back whole; `none` not copied back |
| `WB_UNSIZED` | `vram` (default), `merge`, `whole`: stores into storage buffers of unknown size |
| `TEXEL_VIEWS` | 1 (default); `read` storage texel buffers bind the zero view; 0 every texel buffer does |
| `SPIRV_DUMP` | where programs without SPIR-V are dumped (default `<game>/spirv-dump`); 0 none |
| `ASYNC_PREEMPT`, `FREEZE_IDLE`, `MEMORY` | 0 / 0 / `import`: preemption, page freezing, the VRAM mirror off |
| `PUSH_CS`, `VATTR_MAX` | 0: compute push constants dropped; 16-32: vertex attributes kept |
| `GPU_CRUMBS`, `SKIP_PROGRAMS`, `VK_VALIDATION` | breadcrumbs off; draws with listed programs skipped; the validation layer (needs the Vulkan SDK) |

## Checking a change without the game

- `PYTHONPATH=<cogapp> python3 tools/docs/check.py` (every folder's README, every file listed) before each commit.
- A C syntax check: `clang -std=gnu11 -fsyntax-only` over the changed file with every `runtime/` folder on the
  include path, the Vulkan headers, VMA and SDL3; `simd_fp.h`'s 27 errors are known and unrelated.
- Shaders of the runtime: `glslc --target-env=vulkan1.2` and `spirv-val`, then `tools/shaders/embed_spv.py`.
- The translator needs Python 3.14 (`compression.zstd`) and the SM86 spec, which `python sm86/spec/spec.py --out
  build/sm86 --cc clang` builds from NVIDIA's redistributable nvdisasm and ptxas (a download, about a minute).
- A program known only from `show.py`'s listing (geometry, 18/11/2026): rebuild it as `program.Insn` objects (a
  field dict whose absent predicates read PT and absent `@not`/`@negate` read 0), run `glsl.translate` on it and
  compile the GLSL with glslc and spirv-val. It does not check the decoder (the real words), only the translator.
- Real SASS for a test: assemble a small PTX with that ptxas (`-arch=sm_86`), take `.text.<kernel>` from the cubin
  and decode it with `sm86/translate/program.py`; flipping single bits of a decoded word finds the encoding of another
  instruction class (an `ldg_uniform__Ra32`, a ULDC of bank 3) when ptxas never emits it. The translated GLSL then goes
  through glslc.

## How a stage the runtime lacked was added (geometry, 18/11/2026)

For the next missing stage (tessellation, if a program has one), the path that worked:

1. `vk_exec_draw.c note_extra_stages` logs programs with the stage and looks it up, which dumps it into spirv-dump.
2. `show.py --stage N` prints it; the semantics come from Ryujinx (`Instructions/InstEmitAttribute.cs`,
   `InstEmitMove.cs`, `Translation/ShaderHeader.cs`; a mirror at `gitlab.com/rocketguedes/Ryujinx` answered when the
   official one did not) and attribute addresses from Mesa NAK `nak_private.h`.
3. The translator: a mixin in `sm86/translate/glsl/`, the stage accepted in `translate()`, its layouts in `head.py`,
   its glslc and shaderc kinds in `tools/shaders/translate.py` and `shaderc_lib.py`.
4. The runtime: the reflection stage in `gfx_shader.c` (`reflection_valid` and `reflect`; forgetting the first made
   the prepared file look absent), the shader in `VkPipeState` and `GKey`, the module in `create_graphics`, the
   pipeline kept monolithic (no libraries, not in the key file).
