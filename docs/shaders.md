# Orbitals' shaders: from the game's programs to SPIR-V, and what was found

Everything learned about Orbitals' shaders from 16 to 19/11/2026 (branch `diag/frame-history-trace`), for whoever
continues. The open problems are in [roadmap.md](roadmap.md); how to work with the player's machine is in
[workflow.md](workflow.md). Nothing here contains game code: program keys, SPIR-V hashes, counts and behaviour only.

## How a program gets SPIR-V

1. Orbitals' programs are in Unreal's shader libraries inside `Orbitals/Content/Paks/Orbitals-Switch2.utoc/.ucas`. Since
   25/11/2026 the setup reads them (`tools/shaders/iostore.py programs`, the Oodle groups decoded by the runtime's
   `--oodle-batch` with ooz) and translates all 23 432 (3 556 vertex, 16 459 pixel, 3 415 compute, 2 geometry) before the
   first start; the keys of all 820 programs of a run's `pipeline-keys.bin` were found among them. Steps 2-4 below stay
   for a program the game would build at run time.
2. At run time, NVN2 hands the runtime a program (control section and code). The runtime looks for
   `<game>/spirv/<key>.spv` and `<key>.res.json`; the key is a sha256 of the control section (0xd8 bytes) and the
   trimmed code (`nvn2_program_key`, `runtime/gfx/nvn/procs/nvn2.c`).
3. Missing, the runtime dumps the program into `<game>/spirv-dump/<key>.{ctl,code}` (default since `ffce7da`;
   `ORBITALS_SPIRV_DUMP=0` turns it off) and draws without it (logged `no prepared SPIR-V <key>`).
4. `tools/prepare/dumped_shaders.py --dumped spirv-dump` translates every dumped program with `sm86/translate`
   (Python 3.14, the SM86 spec from NVIDIA's nvdisasm, a download on its first run) into `spirv/`. Since 21/11/2026 the
   runtime runs it itself in the background, a few seconds after the last dump (`gfx_shader_auto.c`,
   `ORBITALS_SHADERS_AUTO=0` turns it off), and the replay looks the programs up again when it finishes: a fresh
   installation draws after a short black spell at the logos, and each new place a short while after it is reached.
   Its output goes to `logs/shaders-auto.log`. The command still works by hand.
5. The runtime checks the `.res.json` (`gfx_shader.c reflection_valid`): its `"stage"` must be one it knows (`vs`, `ps`,
   `cs`, `gs`, `task`, `mesh`). A stage missing there makes a translated program look absent; it happened to `gs`.

The programs and their SPIR-V stay on the player's machine: they are the game's and are not distributed (AGENTS.md);
keep a copy of `spirv/` and `spirv-dump/` before replacing an installation (an emptied folder drew a black screen,
21/11/2026). Each run that reaches new places dumps more. The loop "run, translate, run" took the first level from 211 to 753
programs; 751 translate (19/11/2026).

| Stage | Dumped (colony run) | Translated |
|---|---|---|
| compute | 99 | 99 |
| fragment | 488 | 486 |
| vertex | 165 | 165 |
| geometry | 1 | 1 |

The two refused: fragment programs `93b54007c4699bde` and `e2d9a1f0ae647869`, `tex_scr_urc_` (a texture handle in a
uniform register not traced to a constant bank; likely a predicated UMOV/ULOP3 on the way).

## What the translator learned for Orbitals (sm86/translate)

| Commit | What |
|---|---|
| `933e676` | Storage buffer addresses held in user uniform blocks (`ULDC.64 c[3][x]`) |
| `54369d1`, `a27332c`, `4de72c8`, `3cb33c1`, `da3f9bd`, `d1807eb` | Texture handles OR'ed from two constant words (texture word, sampler word), in uniform or general registers, under guards, through UMOV and any LOP3/ULOP3 table. `d1807eb` made the G-buffer right |
| `e4b61f8` | The bindless TEX handle placed among its sources |
| `2230502` | TXD (Ryujinx source order, not checked on a picture), images used both plainly and for atomics (r32ui helpers) |
| `30a10db` | BREV; fragment Layer, PrimitiveId and ViewportIndex inputs |
| `1211e9c` | Shared-memory ATOMS (CAS loops for S32 MIN/MAX, INC, DEC); RED/ATOMG on an untraced base through a run-time search |
| `d59d10a` | TLD4 with a packed texel offset (AOFFI, 6-bit signed fields; Ryujinx) |
| `9f9ea7a` | FMUL/FFMA `.FMZ` (a zero factor gives 0); encoding bit 76 checked against nvdisasm. Orbitals uses `.FTZ`, not `.FMZ` |
| `7e1ef12` | **Geometry programs** (`glsl/geom.py`): per-vertex `ALD` (`gl_in`, input arrays), `AST` to the layer (`a[0x64]`) and viewport index (`a[0x68]`), `OUT.EMIT`/`CUT`, `ISBERD` (passes the vertex index), `S2R SR_INVOCATION_INFO` (input vertices << 16), layouts from SPH words 3-4 (topology, vertex count); triangles in |

Not translated: tessellation (no Orbitals program seen with it; `vk_exec_draw.c` logs any program that has a
geometry or tessellation stage, `has a geometry stage ...: run on its triangle draws` or `not run`).

## What the runtime learned (runtime/gfx)

- Geometry stages run (`7e1ef12`, `92f4c8c`): `VkPipeState.gs`, looked up for triangle draws only (`gs_for`), a
  monolithic pipeline (no graphics pipeline libraries, not written to the pipeline key file), a stage with resources
  refused with a log line.
- Texture aliases stay in sync (`3146f37`): two descriptors of one storage are two Vulkan images here and one memory on
  the console. Each entry records the order of its newest content (GPU write, storage binding, upload, alias copy);
  when it is used, a live alias with newer GPU content and the same shape is copied into it. Before, the copy happened
  only at the second image's creation. Logged: `took a newer alias's content` (first sync),
  `an alias of another shape got newer content` (not synced). It did not change the colony (see below).

## The colony's frame, pass by pass

Per split-screen half (two 800x900 viewports in 1920x1080 targets). Shaders by the SPIR-V hash a frame dump prints.

| Step | Shader | Reads | Writes |
|---|---|---|---|
| Light grid (once, at the level's load) | cs `a29340cb16aff1e1`, dispatch 5x5x25 | the light list (to find) | 3D r32ui 20x20x100 per half: per cell, `0xffffffff` = empty |
| G-buffer | material shaders | | scene colour rgba16f (emissive), 7 targets (rgb10a2, bgra8, bgra8srgb) |
| Lighting | fs `d18042401d56bbea` | depth, G-buffer, scene colour, the light grid (texelFetch), a 1024x1024x8 r8 volume, a read-only storage buffer (binding 4: the lights), c7 light parameters | lit scene |
| Composite | fs `69b9eb2009c11c75` (program 238181b4) | depth, G-buffer, two 256x256 rgba16f tables, lit scene | composite |
| Bloom | fs `03b151f849cc3f6f`, 960x540 down to 15x17 and back | composite | 480x270 |
| Exposure | cs `b1c0ee32fec29921`, `88cbd364b84c0669` | | 1x1 rgba32f (1.0667) |
| Grading LUT | vs `b05e3b1c71c467e5`, gs (Unreal's layer selector), fs `31a8a47efac993e5` | | 3D rgb10a2 32x32x32 |
| Tonemap | fs `3196324f4cfd42ac` | composite, bloom, LUT, depth | rgba16f |

## Findings, in order

1. **The level drew black** because the G-buffer was black: texture handles OR'ed through uniform registers were not
   traced. Fixed in `d1807eb`.
2. **Then black after the tonemapper**: its LUT had only slice 0. The LUT is one instanced draw into 32 layers, the
   layer picked by a geometry stage the pipelines left out. Fixed by geometry support (`7e1ef12`, `92f4c8c`).
   Lesson: `ORBITALS_SKIP_PROGRAMS` changes what later passes read (skipping the composite showed characters through
   stale data); follow the chain with `ORBITALS_DUMP_FS` and `ORBITALS_DUMP_FS_AFTER` and read the first zero image.
3. **The environment is dark** (open). The lighting skips a cell whose grid value is `0xffffffff` (read from its SPIR-V
   with spirv-cross); both grids are all `0xffffffff`, so it finds no light anywhere while Lichii's colony is bright.
   The grids' guest memory is all zero; nothing writes them in a colony frame. With `ORBITALS_TRACE_3D=1` (recomp48)
   each grid was uploaded once and written by `a29340cb16aff1e1` at the level's load. The alias fix found no alias to
   sync, so the sampled grid is that compute's own image: it wrote "empty" everywhere. **Next:** what that compute
   reads (`ORBITALS_TRACE_3D=1` logs its bindings and uniform blocks and keeps its SPIR-V in `<game>/trace3d/`).
   recomp50: that compute is a clear (ClearUAV: c4[0] the value, c4[1] the offset, c4[2] the size), so another writer
   fills the grid, most likely through a buffer over the same memory (guest memory went from garbage to zeros with no
   CPU write). The trace now logs dispatches whose buffers reach a 3D image's memory.
recomp51 (the buffer-writer trace): no dispatch reaches the colony grids' memory through a buffer; the hits logged
are other volumes (the range estimate, width x height x depth x 16 bytes, is wide). What the log does show for the two
grids (`0xfe8290000`, `0xfe8352000`): uploaded at frame 1579 (guest memory all zero), cleared to all ones by the
ClearUAV compute at 1580, then **uploaded again** at 1580, 1606 and 1635 from guest memory that is still all zero: the
CPU writes those pages (the re-uploads follow page writes) and writes zeros. A second texture id at the same address
(12177 against 13325) is sampled by compute `d62b11d7a6c6d9a3` (35x35x175 volumes, likely volumetric fog). So on our
side nobody puts lights in the grid: the CPU writes zeros, the GPU writes ones.
Two readings remain: (a) the game's CPU light culling finds no light (a runtime/SDK difference upstream, in game
code), or (b) the grid is not where Orbitals' colony light comes from and the cause is elsewhere (the storage buffer
of binding 4, c7, the r8 volume). **Next, decisive:** the same grids and the lighting pass's inputs on Lichii (its GPU
trace, `docs/nx2/gpu-trace.md` in `sinedgiveaways/tool`): if Lichii's grids hold light indices, (a); if they are empty
too, (b). Also a runtime bug seen on the way: a storage-image write into a mirrored texture is not marked GPU-owned
(`vk_texture_storage_bound_locked`), so a later CPU page write re-uploads the storage over the GPU's result.
4. Bright, speckled patches on lamps and ceilings in the colony (light shafts?): not looked at.

## Tools

On the player's machine (from the game folder, forward slashes; outputs carry no game code unless said):

| Tool | Use |
|---|---|
| `tools/prepare/dumped_shaders.py --dumped spirv-dump` | Translate the dumped programs |
| `tools/shaders/missing.py --log logs/orbitals.log` | Programs without SPIR-V by stage and reason |
| `tools/shaders/class_diff.py --keys BAD,GOOD` | Two programs compared: interface, IPA addresses and constant words, instruction forms |
| `tools/shaders/base_origin.py [--tex] [--keys K]` | Where untraced bases and handles come from |
| `tools/shaders/c0_reads.py [--banks]` | Which constant words programs read |
| `tools/shaders/show.py [--stage N] [--keys K]` | Decoded instructions of dumped programs. **Game code: chat only** |

Runtime switches for shader work: `ORBITALS_DUMP=<frame>` with `ORBITALS_DUMP_FS=<hash>[,...]`,
`ORBITALS_DUMP_FS_AFTER=<n>`, `ORBITALS_DUMP_PASSES=<a>-<b>`, `ORBITALS_DUMP_UBO=1`; `ORBITALS_TRACE_3D=1`;
`ORBITALS_SKIP_PROGRAMS=<key prefixes>`; `ORBITALS_SPIRV_DUMP`. A frame dump names shaders by an FNV-1a hash of the
SPIR-V (`sh_<hash>.spv`), not by program key; match them by shape (`class_diff.py` on the player's side, `spirv-dis` or
spirv-cross `--vulkan-semantics` on the dump's `sh_*.spv`).

Offline (no game): a translator path can be checked from `show.py`'s listing rebuilt as `program.Insn` objects, or from
real SASS assembled with ptxas (`-arch=sm_86`), then glslc and spirv-val ([workflow.md](workflow.md#checking-a-change-without-the-game)).
