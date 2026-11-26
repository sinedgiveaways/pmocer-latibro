# Roadmap

What is being worked on, in order of priority: Orbitals playable from boot (no crash, a picture as on the console),
then nothing hanging the GPU, then the frame rate. Updated 27/11/2026 (branch `orbitals-port`; the latest state is
[6.8](#68-état-au-27112026-branche-orbitals-port-build-courant-ec65fcb); how the work is done with a player's machine:
[workflow.md](workflow.md); the shaders: [shaders.md](shaders.md)).

All measurements below are from one machine: Windows 11, RTX 4080, NVIDIA 616.92, Orbitals Deluxe Edition 1.0.5
(program `040032B023CA4000`, main build `d6efcc0a51c1c792`) with its add-on, from a `.nup` package. The reference is
the same dump on [Lichii](https://github.com/sinedgiveaways/tool) (repository `sinedgiveaways/tool`, formerly
`Lichii-tool`; an Eden fork, used as a measuring tool only).

## Where the game is

- Boot, the three logos, the warning, the title, the main menu, the Deluxe add-on ("CONTENU BONUS DÉVERROUILLÉ",
  "GALERIE Deluxe"), the controller screen, the hero choice, the intro cutscenes and the first level (the colony, split
  screen) run, with sound and both controllers, at 30 fps. Since `92f4c8c` (18/11/2026) the colony draws: characters,
  environment, lights and HUD in both halves; the environment is still too dark (1.1b).
- Shaders: Orbitals' programs live in Unreal containers the setup does not read, so a run dumps every program it has
  no SPIR-V for into `<game>/spirv-dump` (default since `ffce7da`) and `tools/prepare/dumped_shaders.py` translates
  them. After a run into the colony: 753 programs dumped (cs 99, ps 488, vs 165, gs 1), 751 translated; the two left
  are pixel programs `93b54007c4699bde` and `e2d9a1f0ae647869` (`tex_scr_urc_`, 1.3).
- A whole `orbitals.log` can be summarised with `shader_analyzer.exe` ([log-analyzer.md](log-analyzer.md)): how the
  run ended, key threads, missing shaders, NVN sync call sites.
- Geometry stages are translated and run (`sm86/translate/glsl/geom.py`, 1.1). Tessellation is not (no Orbitals
  program has it so far; `vk_exec_draw.c` logs any program that does).

## 1. Open problems, most blocking first

### 1.1 The level's world drew black (fixed 18/11/2026)

recomp45 (`92f4c8c`): the colony draws in both split-screen halves. The causes, in the order they were fixed:

1. Texture handles OR'ed from two constant words through uniform registers were not traced (`d1807eb`): the
   G-buffer passes wrote black. Fixed, the G-buffer is right.
2. The colour-grading LUT (32x32x32 rgb10a2) is drawn as one instanced draw into 32 layers, and the layer comes from a
   **geometry stage** the pipelines left out: only layer 0 was written, and the tonemapper `3196324f4cfd42ac` graded
   every colour to black. Fixed by translating geometry programs (`7e1ef12`) and accepting their prepared files
   (`92f4c8c`).

Lessons for the next black frame: `ORBITALS_SKIP_PROGRAMS` changes what later passes read (skipping the composite
`238181b4` left a stale target, and the tonemapper then showed the characters), so it points at the pass whose
output matters, not at the faulty one. `ORBITALS_DUMP_FS=<hash>` with `ORBITALS_DUMP_FS_AFTER=<n>` follows one pass
and the chain after it, whatever the pass numbers (they move between runs); the first all-zero image names the pass.
Then read that pass's inputs (`images.txt`: a 3D LUT with a channel max of 0 is a missing layer).

What remains visible: the environment is much darker than on Lichii (1.1b); some patches are wrong (a translucent
panel drawn as a flat grey quad, grainy surfaces), not looked at yet.

How it was found, for the record:

Only the HUD, emissive glows and a few edges show in the first level; Lichii shows the colony lit.

- Measured: a frame dump of the level (`ORBITALS_DUMP=3900`, `images.txt`): the depth prepass is filled (geometry is
  placed), but the G-buffer passes write a base colour of 0 in R, G and B (alpha varies), normals of 0.5 everywhere and
  almost nothing in the other targets; the 8-target pass after them is all zero. The materials' pixel shaders run and
  output black.
- Ruled out: missing programs (674 of 687 translated, 14 still missing in that run), the graphics push constants the
  runtime drops (NVN2 keeps them in root table 0, read as constant bank 24, bank 17 after Eden's Ampere lowering:
  Lichii `src/video_core/engines/maxwell_3d.h`; `tools/shaders/c0_reads.py --banks` finds no Orbitals program reading
  bank 17 or 24, nor c0 below 0x110).
- Textures resolve (dump with `fd6ef01`), and since the handle tracking of `d1807eb` the G-buffer passes are right.
  `base_origin.py --keys` shows the handles come from two fixed c3 words OR'ed, with nothing in between.
- Since `d1807eb` translates it, pixel program `238181b409956cac` turns the whole level black again:
  `ORBITALS_SKIP_PROGRAMS=238181b409956cac` alone brings the characters back, skipping its near twin `9255edc18c6332c0`
  does not. `class_diff.py --keys 238181b409956cac,9255edc18c6332c0` finds no form unique to it.
- `.FMZ` is not the cause: the dump shows `fmz=FTZ` (flush denormals) on every FMUL/FFMA of both programs, never
  `.FMZ`. The translator handles `.FMZ` anyway (`fmzmul` / `fmzfma`, `9f9ea7a`; encoding bit 76 checked with nvdisasm).
- `238181b4` has no interpolated input, reads only `gl_FragCoord.xy` (`a[0x70]`, `a[0x74]`) and writes rt0: a
  full-screen pass. In the 4500 dump (recomp39) it matches SPIR-V `69b9eb2009c11c75`, the only fragment shader of
  that shape: passes 364 and 396 (one per split-screen half, viewports 0,0 and 800,0 of 800x900). It reads depth,
  four 1920x1080 bgra8 G-buffer targets, two 256x256 rgba16f tables and the scene colour `0xf49130000`, and writes
  `0xf795a0000`, which pass 426 (`3196324f4cfd42ac`) reads: the lighting composite everything after it shows.
  Skipping it leaves the characters because the later passes then read whatever `0xf795a0000` held.
- The next dump (recomp40, `ORBITALS_DUMP_PASSES=360-400`) missed it: pass numbers move between runs, the composite
  was at 498 and 531. It showed the chain: the base pass writes scene colour `0xf351f0000` (G-buffer pass t0); the
  clustered deferred lighting `d18042401d56bbea` (pass 497, light grid r32ui 20x20x100) reads it with the G-buffer and
  writes `0xf2df60000`; the composite `69b9eb2009c11c75` (238181b4) reads that and writes `0xf6e800000`. Its uniform
  blocks look sane (c4: viewport rectangles and inverse sizes 1/800, 1/900); the "not finite" rows 280-295 of c5
  hold integers (12, 17, all-ones), not floats.
- With `ORBITALS_DUMP_FS=d18042401d56bbea,69b9eb2009c11c75` (recomp41, passes 451/452 and 482/484): the composite is
  right. Its output matches its input, with no NaN. In both, the characters are lit and the environment is black.
  So 238181b4 is not what blackens the frame. Two faults remain: (a) the environment is already black in the clustered
  lighting output, so the cause is upstream (G-buffer of the environment, or the lighting itself); (b) the frame turns
  black somewhere after the composite: bloom (`03b151f849cc3f6f`, the 960x540 to 15x17 chain), the 1x1 computes
  `b1c0ee32fec29921` / `88cbd364b84c0669` (eye adaptation?), `3196324f4cfd42ac` (TAA?), `899b938acbed2d8a`, tonemap.
  Skipping 238181b4 leaves `0xf6d6f0000`/`0xf6e800000` stale, and that changes what this chain computes.
- With `ORBITALS_DUMP_FS=69b9eb2009c11c75` and `ORBITALS_DUMP_FS_AFTER=80` (recomp42): bloom and exposure are
  right (the 1x1 exposure is 1.0667, no NaN). The frame goes black at the tonemapper `3196324f4cfd42ac` (pass 600:
  input max 6.94, output all 0). It grades through a 32x32x32 rgb10a2 LUT (tex 699, then 3789 for the other half),
  drawn just before by `vs b05e3b1c71c467e5` / `fs 31a8a47efac993e5` as one instanced draw into 32 layers. Only
  slice 0 holds data (a red/green ramp); slices 1-31 are 0 (blue max 0). So most colours grade to black.
- That vertex shader uses `gl_InstanceIndex` and writes no layer (no `a[0x64]` AST; the translator would refuse it).
  The layer must come from a stage the runtime does not run: the pipelines take VS and FS only (`vk_pipeline.c`).
  `vk_exec_draw.c` now logs programs with a geometry or tessellation stage (`program N has a geometry stage`).
  recomp43: 10 programs have one, all the same 1024-byte program (`show.py`): Unreal's layer selector. Per vertex
  of a triangle it copies the position and generic 0 and writes generic 1.x (the instance index from the VS) to
  `a[0x64]`, the layer; SPH: triangle strip out, 3 vertices.
- Geometry stages are now translated (`sm86/translate/glsl/geom.py`, Ryujinx semantics: `ISBERD` passes the vertex
  index, `S2R SR_INVOCATION_INFO` is input vertices << 16) and run on triangle draws (`VkPipeState.gs`, monolithic
  pipelines, no libraries; a stage with resources is refused). Checked offline: the listing rebuilt as instructions
  translates, compiles and passes spirv-val. recomp44: translated on the player's machine (751/753), but the runtime
  refused its `.res.json` (`reflection_valid` did not know `"gs"`): "no prepared SPIR-V", fixed in `92f4c8c`.
  recomp45: 32 programs logged `has a geometry stage ...: run on its triangle draws`; the colony draws.
- Skipping 238181b4 hid this: the tonemapper then read stale data. The dark environment in the lighting output is a
  separate fault.

### 1.1b The environment is too dark

The frame chain of the colony (recomp40-42, per split-screen half; SPIR-V hashes as `passes.txt` shows them):

| Step | Shader | Reads | Writes |
|---|---|---|---|
| G-buffer (base pass) | material shaders | | scene colour (rgba16f, emissive), 7 targets (rgb10a2, bgra8, bgra8srgb) |
| Clustered deferred lighting | fs `d18042401d56bbea` | depth, G-buffer, scene colour, light grid (3D r32ui 20x20x100), 1024x1024x8 r8 volume | lit scene colour |
| Composite / reflections | fs `69b9eb2009c11c75` (program 238181b4) | depth, G-buffer, two 256x256 rgba16f tables, lit scene | composite |
| Bloom | fs `03b151f849cc3f6f` and the 960x540 to 15x17 chain | composite | bloom 480x270 |
| Exposure | cs `b1c0ee32fec29921`, `88cbd364b84c0669` | | 1x1 rgba32f (1.0667) |
| LUT | vs `b05e3b1c71c467e5`, gs (the layer selector), fs `31a8a47efac993e5` | | 32x32x32 rgb10a2 |
| Tonemap | fs `3196324f4cfd42ac` | composite, bloom, LUT, depth | rgba16f |

The lighting `d18042401d56bbea` already outputs a dark environment while the characters are lit (recomp41).
Candidates: the light grid (built by compute) or the lights' data, the environment's G-buffer, shadows, the 3D r8
volume.

recomp46 (dump of that pass, and a Lichii recording of the same walk): on Lichii the colony is bright (lit walls,
teal floor, blue lights); here it is dark everywhere but the characters. The lighting's uniform blocks look sane
(view matrices, c7 light parameters 0.5, 30, 1.2, 1000, 8000...). Both its volumes are **mirrored textures uploaded
from guest memory and never written by the GPU in the frame**: the light grid (3D r32ui 20x20x100, uploads 1, pages
not written since) and a 1024x1024x8 r8 volume (uploads 2). In recomp39 that grid read all 0xffffffff (every cell
"no light"). No copy, clear or storage binding in the frame touches its range. If the console fills it in a way the
runtime does not see (a GPU write left in VRAM, `ORBITALS_WB_UNSIZED=vram`, or CPU writes the page tracking misses),
the lighting would see no light anywhere.

The dump did not copy the volumes: a texture first sampled in a skipped pass was marked done and never copied again;
fixed (`drop_luts`). The pass line now also gives a 3D texture's guest memory as words (all ones, zero, other).
recomp47 (that dump with the fix): there are two grids, one per half (`0xfe7bd3000`, `0xfe7c95000`). Their images
are all `0xffffffff`; their guest memory is all zero. The lighting shader (`sh_d18042401d56bbea.spv` through
spirv-cross) fetches a cell with `texelFetch` and skips it when it reads `0xffffffff`, so it finds no light anywhere.
Its lights come from a read-only storage buffer (binding 4) indexed through the grid. The image's ones did not come
from guest memory (that would be zeros), and nothing writes the grids in the dumped frame: no copy, clear, storage
image or buffer range. `nvnTextureWriteTexels` is never called; `ClearTexture` and `CopyBufferToTexture` are.
recomp48 (`ORBITALS_TRACE_3D=1`): each grid is written by compute `a29340cb16aff1e1` (dispatch 5x5x25) through a
**storage image** descriptor, and sampled by the lighting through **another descriptor of the same memory**. The
texture cache keys images by address and descriptor, so these are two Vulkan images. An entry created next to a live
one copied its content once (`alias_copy` at creation, right after the compute's clear to all ones); after that the
compute's writes stayed in its own image, and a storage binding did not even count as a GPU write. The lighting read
"no light" in every cell.
Fixed (`vk_tex_alias.c alias_sync`): each entry carries the order of its newest content (GPU write, storage binding,
upload, alias copy); when an entry is used, a live alias with newer GPU content and the same shape is copied into it
at that point of the replay, and the slots of an address with aliases re-resolve after a write. An alias of another
shape is logged (`an alias of another shape got newer content`).
recomp49 (with the sync): still dark, and no sync ran (no `took a newer alias's content`). So the sampled grid is
most likely the compute's own image: one upload per grid, and the compute `a29340cb16aff1e1` runs at the level's
load (frame ~1680 in recomp48) and in no dumped frame. It then wrote "empty" everywhere: its input (the light list)
was empty or unreadable to it. The sync stays (it is right for aliases), but this is not that case. Also visible:
bright speckled patches on lamps and ceilings (volumetric light shafts?), not looked at yet.
**Next:** `ORBITALS_TRACE_3D=1` now also logs, for the first write of each 3D storage image, the dispatch's bindings
with its uniform blocks (`vk: trace3d <hash>: ...`) and writes its SPIR-V to `<game>/trace3d/`. Read the light grid
compute: where its lights come from (a storage buffer, its count in a uniform block) and why it finds none.
recomp50: `a29340cb16aff1e1` is only a **clear** (Unreal's ClearUAV): one uniform block, c4[0] = the value
(`0xffffffff` x4), c4[1] = the offset, c4[2] = the size (20, 20, 100); it stores the value into every cell of the
3D image. So the real fill comes after it, from another writer. The grid's guest memory went from garbage at its
upload to all zeros with no CPU write and no re-upload: the fill most likely writes the same memory **through a
buffer** (a storage buffer or buffer texture) that the 3D image never sees. **Next:** `ORBITALS_TRACE_3D=1` now also
logs any dispatch whose storage buffers or buffer textures reach the memory of a 3D image written before (`vk:
trace3d <hash> (its storage buffer ... reaches the 3D image at ...)`), with its bindings and SPIR-V. If that is the
fill, the fix is a buffer/image alias: buffer writes into a texture's storage must reach the image (or the image be
re-read from the buffer's mirror).
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
too, (b). (A storage image the shader stores into is marked GPU-owned now: `vk_texture_storage_bound_locked` goes
through `vk_texture_gpu_written`.)
recomp52-53 (`ORBITALS_TRACE_3D` with the record/replay source hash, `706a86a`): the volumetric lightmap's
indirection (rgba8ui 144x128x64 at `0xf88b70000`) is copied in whole from a valid source (record and replay hashes
equal). Level streaming then moves it twice with `FMoveWholeIndirectionTextureCS` (`cdf018b79a597e9c`, dispatch
36x32x16) into two new textures whose memory held brick staging data (first word `0xaf776183`, the word the base
pass's samples showed). The translation of that compute program had two bugs, both fixed in the translator:
- its bounds check `TXQ` on the destination `RWTexture3D` was declared as a `sampler2D` (TXQ names no dimension),
  so the depth read 1 and only slice z = 0 was written; the rest kept the staging bytes. A TXQ-only handle that is
  also an image now takes its size from `imageSize` (`glsl/tex.py txq`, `glsl/head.py`; 2 programs);
- `@P0 EXIT P2` dropped its second predicate (BRA's was handled, EXIT's not): every cell whose brick index is at or
  above the moved range exited unwritten (`glsl/thread.py exit`; 26 programs, among them the Lumen translucency
  radiance cache mark pixel shaders, `FClearIndirectArgBufferCS`, `FDenoiserSpatialCS` and the
  `FOrbPostProcessMaterialPS` of `PP_A90_Stage_2`).
Full retranslation: 23 176 of 23 432 valid as before; `flow_check.py` finds no difference on the changed programs.
Confirmed in game (build `905745a`, 25/11/2026): the colony is lit in both split-screen halves, walls, floor and
characters close to the console's. Left: blocky noise patches, bright white blocks on ceilings and light shafts, and
multicoloured blocks on some surfaces (a sofa), the speckles of 1.1 above.
The blocks are in the G-buffer already (base colour and normals), so a material texture is read wrong. Nearly every
BC material texture of the base pass is GPU-owned, and `ORBITALS_TRACE_STALE=1` (logs26, `b978be8`) listed 262
sampled textures whose storage the CPU rewrote after the GPU wrote them: BC mip chains (NVN formats 0x45 to 0x50, 8
to 12 levels) written by one copy (`build/orbitals-recomp+0xd821f`), the storage rewritten from its start. Mask `0fff`
of a 2048x2048 BC1 is exactly its level 0 (0x200000 of 0x2ab400 bytes): Unreal's mip streaming copies the old mips on
the GPU and writes the new large mip with the CPU, which the runtime dropped for a GPU-owned texture. Fixed in
`vk_tex_upload.c merge_cpu_writes` (`93b106b`): the sofa, the tree and the props were clean in logs27. But that first
rule (any level with a changed page) also took stale storage over GPU-written single-level textures, and the
ceiling turned to big saturated blocks, likely the volumetric lightmap's brick atlases. Narrowed: only mip chains, and
only a level whose every page carries a CPU-origin stamp since the GPU write (`vk_mem_cpu_rewrote_all`).
Confirmed in game (`880ffd3`, logs28): sofa, tree, props and ceiling without blocks; 16 merges logged (BC1 and other
chains, 1 to 5 levels each). Left: one asteroid outside the window with green and purple speckles, not looked at yet.
logs29 (`8a14c5c`, per-level page counts): no level of a mip chain is rewritten in part; the 189 `PARTIAL` lines are
all one page at a level's edge, shared with the neighbouring level or texture. The asteroid was clean in that run.
The speckles came back on part of the asteroid in logs76 (`97bc308`, 26/11/2026): a race, not a partial rewrite by the
game. The replay runs behind the game, so when it reaches the GPU copy of the old mips the streaming thread has
already written part of the new level 0; those pages get the copy's `gpu_gen`, the rest a newer stamp, and the level
is never "whole" for the merge. Now the copies name the level they write (`vk_texture_gpu_written_level`, `gpu_lv`),
and a level the GPU never wrote is the storage's: any CPU page since the last read (`wgen`) brings it over, and a
write-back leaves it alone. Renders, clears and storage images still mark every level (`6c1523e`). The speckles stayed
(logs77). logs80 (27/11/2026) found when: clean on the first visit, orange and black stripes on the asteroid's right
part after going back to the main menu and loading the save. The G-buffer's base colour holds them, so a texture of
the base pass samples another texture's texels. A texture finalized and initialized again at the same address with the
same descriptor takes back the released entry (`entry_of`), and its `gpu_lv` was the old texture's: the new
texture's CPU mips met the whole-level rule again. Now a revived entry's copy-named levels are cleared (`revived` in
vk_tex_alias.c; renders and alias copies kept). CI build, not run in game yet.
Against the reference video (`ORBITALS #01 - LE DÉPART`, 8:30-9:40, the same walk through the airlock, outside and
into the burning deck): the red alarm light is the game's, but (a) the fires are big yellow masses with stair-step
edges where the console draws small cel-shaded orange flames with smoke, and (b) the red rooms look washed out (pink
haze, lifted shadows) where the console's stay dark. **Next:** a dump of a fire through `ORBITALS_CTL`
(`dump DIR passes A-B`, new) to find its draws.
Checked against Liftoff v0.1.5 (`Dimi-Sama/OrbitalsPCRecomp`, AGPL-3.0: ideas only here, `GRAPHICS_ACCURACY.md`), whose
fires and smoke came back through two fixes: (1) compute shared memory sized from control+0x64 (Unreal's GPU radix sort
for Niagara particles uses 8 836 bytes, cut to 4 096 it sorted garbage); ours already reads it there (`compute_shape`:
upsweep 2 209 words, spine 1 241, downsweep 2 361, all four sort programs translated); (2) Niagara buffers read through
2D or 1D image declarations; ours gives 1D fetches a `samplerBuffer` twin, and the colony dump has no 2D declaration
bound to a buffer texture (every unresolved binding is a 1D one with its twin). Their third, `Volatile` shared accesses
for the sort's downsweep, concerns non-NVIDIA drivers only. logs29 logs no skipped draw, copy or layer.

### 1.2 A crash after the logos in about one run in three

`access violation ... reading address 0x104` in `f_7001759b20` (`main+0x1759b20`), Unreal's render graph popping an
element of an internal array (`main+0x1759d9c` loop, elements of 0xe8 and 0x40 bytes) and freeing its allocation
through `FMemory::Free` (`main+0xbf040`, a tail call to `GMalloc`'s vtable slot 0x50, which reads the thread's TLS).
The structure was corrupted earlier; the code reading it is right.

- Ruled out by A/B runs of 3-4 starts each: GPU write-backs (`ORBITALS_WB_MERGE=none`), asynchronous preemption
  (`ORBITALS_ASYNC_PREEMPT=0`), page freezing (`ORBITALS_FREEZE_IDLE=0`), the VRAM mirror (`ORBITALS_MEMORY=import`),
  and STLR ordered as a release store (fixed in `7b4a312`, a correct change, the crash stayed). The game library was
  checked rebuilt (its lifted code grew by 4 544 bytes).
- **Next:** the kernel's synchronization (`runtime/core/kernel/`: mutex arbitration, condition variables, events) for
  a path that lets two threads in at once; then the SDK functions the runtime replaces for threads and TLS.

### 1.3 Smaller visible differences

- The title and main menu backgrounds (nebula, starfield, menu art) are black, while their logos and text show.
- The "MAINTENIR (B) POUR PASSER" prompt is not drawn during cutscenes; holding B (Xbox B) does skip them.
- Logos 2 and 3 (Shapefarm, Kepler) are silent; on Lichii they have sound from about 10 to 18 s after start. Not
  investigated yet; the movie library (`nn::movie`, subsdk0) is initialized but no playback call shows in the logs.
- 2 programs are still refused by the translator, pixel programs `93b54007c4699bde` and `e2d9a1f0ae647869`:
  `tex_scr_urc_`, a texture handle in a uniform register not traced to a constant bank (likely a predicated UMOV or
  ULOP3 on the way; `tools/shaders/base_origin.py --tex` and `handle_trace.py` show the writes). Fixed since
  17/11/2026: `TLD4.AOFFI` (`d59d10a`), shared `ATOMS` and untraced `RED`/`ATOMG` bases through a run-time search
  (`1211e9c`), handles OR'ed through uniform registers (`d1807eb`), geometry programs (`7e1ef12`).
- The three boot logos cannot be skipped. The game reads Unreal's command line from `rom:/UECommandLine.txt` and calls
  `nn::os::GetHostArgc/Argv`, so arguments could be passed (a mod replacing the file, or the runtime answering
  GetHostArgv); whether any argument skips these logos is not known (`-nomoviestartup` is Unreal 3's). Keep them in
  diagnostic runs: the 1.2 crash happens right after them.

## 2. Known trade-offs in the code

- Shader stores into storage buffers of unknown size (an address read from a uniform block) stay in the pool's VRAM
  mirror and are never copied to guest memory (`ORBITALS_WB_UNSIZED`, default `vram`): GPU work after them reads them,
  the CPU does not. Both ways back to guest memory failed (merged on the GPU: device lost; copied whole: corruption).
- Other shader stores go back merged by bytes on the CPU at the fence (`ORBITALS_WB_MERGE`, default 1). A merge in a
  compute shader lost the device inside its own dispatches four times; the cause was not found and that code is gone.
- Texel buffer views are cached (4096) and evicted least recently bound first.
- TXD's source order follows Ryujinx and is not checked against a picture yet (`sm86/translate/glsl/tex.py txd`).

## 3. Stability and memory

- Zero Vulkan validation errors (not measured on Orbitals yet; needs the Vulkan SDK on the player's machine).
- The NVN2 coverage: 171 procs listed and not implemented, 195 names without an implementation, out of 731 (from
  before the Orbitals runs; `tools/analisis/port_check.py`, `sdk_gaps.py`).

## 4. Frame rate

30 fps in the level with 2.6 GB a log interval compared by the merged write-back; no profile of Orbitals yet. The
replay thread's work found on the first game (recordings reused across frames, parallel recording) applies; its
numbers for Orbitals come from `tools/run/ab.py` runs (`ORBITALS_AB`).

## 5. The player's side

- Shaders are translated in the background during a run (`runtime/gfx/nvn/host/gfx_shader_auto.c`, 21/11/2026);
  not yet checked on the player's machine.
- A Linux package that starts on non-rolling distributions, and the launcher tested on more Windows machines.
- Steam Remote Play Together checked with Orbitals ([remote-play.md](remote-play.md)).

## 6. Update du 24/11/2026 — état réel après les derniers logs

Cette section fait foi sur les conclusions les plus récentes et corrige les hypothèses historiques des sections 1.2 et 1.1b. Les détails des dumps et des instructions sont conservés dans [orbitals-startup-crash-analysis.md](orbitals-startup-crash-analysis.md).

### 6.1 Crash B : le read à 0x104 n'est pas un crash TLS

- Le nom nn::os::GetTlsValue était un symbole voisin trompeur. Le défaut se produit dans le code du jeu, dans f_7001759b20 = main+0x1759b20.
- Le dump du 22/11 montre un enregistrement de 0xE8 octets valide, présent dans la liste, mais dont le pointeur de sous-table à +0x00 est nul et l'état à +0x50 vaut -1. Le code calcule ensuite une adresse à partir de cette base nulle et lit 0x104.
- Le correctif rec-null-guard / v2 protège cette lecture et permet à l'exécution de dépasser ce point. Cela constitue un contournement ciblé du crash, pas encore une preuve que les valeurs d'une sous-table absente sont sémantiquement correctes.
- La PR #19 (fix/record-subtable-175dc04) ajoute un second garde indépendant à main+0x175dbec, validé contre le mot d'instruction original f9400008. Le fallback redirige la base nulle vers la page zéro ; il doit encore être validé dans le jeu.
- Ne pas modifier TPIDR, l'allocation TLS ou le scheduler sur la seule base de ce crash : les éléments recueillis ne démontrent pas une panne de TLS.

### 6.2a Crash memcpy : l'index -3 sélectionne le record fautif (25/11/2026)

- Le helper `+0x40CE0` reçoit le même `Cpu *` que le caller et lit `Cpu.x0/x1/x2`. `rt_dispatch(Cpu *c)` et `hostfn_entry(Cpu *c)` transmettent le pointeur `c` sans copie.
- Le chemin crash est le large-copy branch : `X25=0x01490140`; `X2=X25*4=0x05240500` (86 246 656 octets); `ja 0x1B5A937E` contourne le petit-copy branch.
- Source `X1=0x01460149013D0140` chargée depuis `[rcx+r8-0x10]`; count depuis `[rcx+r8-0x08]`. Le premier champ est sauvegardé en local puis remis à zéro; le second fournit le low dword utilisé pour X25.
- **Lien direct avec l'objet -3 :** `Cpu.x27=0x0CE01F2C00`, `[x27+0xD0]=0`, `[x27+0xD8]=-3`. Le caller prend la base `x27+0x10`, applique `-3*0x40=-0xC0), puis sélectionne le record base `0x0CE01F2B10`.
- Dans ce record, source `+0x30 = 0x0CE01F2B40`; count `+0x38 = 0x0CE01F2B48`. Le source field contenait `0x01460149013D0140` avant d'être effacé; le count low dword est `0x01490140`.
- **Correction :** l'index -3 est directement impliqué, mais via une stride de `0x40`, pas le calcul indépendant `(2-EDX)*0x38` à `+0x1B5A8FF0`.
- **Prochaine étape :** identifier le propriétaire et le producteur du record `0x0CE01F2B10), et comprendre pourquoi le slot sélectionné par -3 contient une valeur non-pointer dans son champ source et un count de `0x01490140`. Vérifier si l'index -3 est attendu, stale ou hors limites. Ne pas patcher memcpy/memmove.
- L'extracteur GitHub dump maintenant le record dérivé, le count qword, les occurrences des valeurs fautives et les régions mémoire. La relance Actions est encore en file d'attente; aucun nouvel artefact de cette extraction n'est disponible à cet instant.
- Détails : [index -3 et record source/count exact](orbitals-startup-crash-analysis.md#25112026-the-index--3-directly-selects-the-bad-sourcecount-record).

### 6.2 Crash A : le Binned3 tente de libérer sa propre pile

- Le log du 24/11 (build 0.2.0-g1617c23, fichier daté 20261124-002310) ne montre plus le read AV à 0x104. Il se termine plus tard, au chargement / choix du personnage (~15 s), par FMallocBinned3 Attempt to free an unrecognized pointer %p.
- Chemin relevé : main+0x175a16c -> main+0xb9214 -> main+0x181cc8 -> main+0x1f9878, puis AbortImpl. Le dump hexadécimal de pile ajouté par la PR #18 permet de retrouver l'argument variadique %p.
- Dans le log du 24/11, le pointeur transmis à Free est 0x1183f9f1c et le SP du thread qui avorte vaut 0x1183f9e00 : le pointeur vaut exactement SP + 0x11c. Le log du 23/11 présente la même forme (x1=0x115eaef1c). C'est donc une adresse dans la pile du thread, pas une allocation heap valide.
- Conclusion actuelle : le parcours de records récupère dans le champ +0x30 d'un élément imbriqué une adresse de pile, puis la transmet à Free. Le code de nettoyage copie cette valeur ; le producteur qui l'a initialement écrite reste à localiser. Les adresses 0x84014a400, 0x84014a410 et 0x84014a4dc repérées comme voisines dans le dump ne sont pas le pointeur fautif confirmé ; ne pas les utiliser comme cible de patch.
- Outil ajouté : recomp/peek_words.py, lecture seule du .text NSO, pour extraire les mots d'instruction au format patches-v1. Il faut l'exécuter sur l'ExeFS local aux trois fenêtres main+0x1f9878, main+0xb9214 et main+0x181cc8. Le patch ne doit être écrit qu'après identification de l'instruction qui propage la valeur de pile vers Free.
- Aucun correctif Crash A n'est encore établi. Ne pas confondre ce problème avec le Crash B et ne pas présenter le garde 0x104 comme une correction du Binned3.

### 6.3 Portée des builds et des PR

- La PR #18 ajoute le dump brut de 0x2000 octets de pile au chemin svc_fatal ; c'est un changement diagnostique.
- La PR #19 est un garde runtime/générateur optionnel pour la seconde lecture de sous-table ; elle ne contient pas une DLL de jeu déjà recompilée. Le workflow Windows runtime-only ne régénère pas le code du jeu. L'activation effective du patch généré exige encore le cycle de génération/compilation de la DLL de jeu.
- La PR #16 (barrière mémoire partagée Niagara) et la PR #17 (suivi de propriété GPU des storage images) sont présentes dans l'historique fusionné. Leur présence dans le dépôt ne prouve pas à elle seule que chaque correctif a été testé visuellement dans Orbitals.
- Ne pas lancer une régénération/recompilation complète pour une simple analyse de logs ou l'extraction des mots du NSO.

### 6.4 Graphismes : ce que les traces établissent

- La trace 3D montre que la grille de lumières 3D (20x20x100) est effacée à 0xffffffff, puis réuploadée depuis une mémoire invitée qui contient des zéros ; aucune écriture de remplissage de grille n'a été observée. La cause peut être en amont (culling CPU / données de lumières) ou dans le rôle réel de cette grille : ce n'est pas encore tranché.
- Le volume R8 1024x1024x8 est écrit par le compute 9d8184019bd59e6d à la frame 417 puis échantillonné à la frame 1505 ; aucune réupload ultérieure depuis la mémoire invitée n'a été observée dans la trace fournie.
- Le fragment shader d'éclairage d18042401d56bbea dépend notamment de la profondeur, du G-buffer, de la couleur de scène, de la grille, du volume R8 et du SSBO binding 4. La trace fournie ne contient pas encore tous les éléments permettant de conclure sur ses entrées.
- La dernière session de log atteint le menu et se ferme normalement ; elle ne prouve pas que textures, ombres, lumières et reflets sont visuellement corrects. Il faut une capture ciblée du shader d'éclairage et comparer ses entrées/sorties avec Lichii.
- Le correctif de Niagara concerne les dépendances implicites de mémoire partagée du shader radix-sort d032ce5090303e01. Il ne faut pas le présenter comme une correction générale de tous les shaders.

### 6.5 Prochaines actions vérifiables

1. Extraire les mots d'instruction du NSO local autour des trois offsets du chemin Binned3 avec recomp/peek_words.py.
2. Suivre la valeur SP+0x11c en remontant le code généré jusqu'à l'écriture fautive ou au champ de record mal initialisé ; proposer ensuite un patch local validé par mot original.
3. Garder Crash A et Crash B comme deux défauts distincts dans les rapports, les branches et les tests.
4. Pour les graphismes, capturer le fragment d18042401d56bbea et ses ressources au même passage/frame que les traces 3D ; comparer avec Lichii avant de modifier le shader ou le suivi mémoire.


**Correction du log exact fourni (20261124-002310) :** SP fatal = `0x1183f9e00`, x1 = `0x1183f9f1c`, soit exactement `SP+0x11c`. La paire `0x11e2b4...` ne correspond pas à ce fichier de log. L'analyse NSO précise désormais la chaîne : `main+0x1759e38` lit X9 depuis `[X8-0x10]`, `main+0x1759e4c` le copie dans `[SP+0x80]`, `main+0x1759e50` efface le champ source, puis `main+0x175a160/168` recharge cette valeur et appelle `FMemory::Free`. Le code qui écrit initialement `[X8-0x10]` reste à identifier.


**Précision structurelle du 24/11 :** le chemin Crash A repasse par le même tableau de records de `0xE8) octets que Crash B (`[X21+0x158] + index*0xE8`). Chaque record contient un tableau imbriqué de 64 octets (pointeur à +208, compteur à +216, stockage inline possible à +16). Le nettoyage lit le champ +0x30 de l'élément (`[X8-0x10]`), le déplace dans `[SP+0x80]`, efface le champ d'origine puis le libère. Cela lie les deux crashes à la même famille de records, sans prouver que leurs champs fautifs ont le même producteur.

### 6.6 Shaders traduits au setup (25/11/2026)

- Les programmes d'Orbitals sont lus dans les bibliothèques de shaders Unreal (`Orbitals-Switch2.utoc/.ucas`) par
  `tools/shaders/iostore.py`; les groupes, compressés en Oodle par Unreal, sont décodés par `orbitals-recomp
  --oodle-batch` (ooz, GPL-3.0-or-later, `runtime/third_party/ooz`). 23 432 programmes, dont les 820 clés relevées dans
  `pipeline-keys.bin` d'une partie. L'étape `shaders` du setup les traduit tous avant le premier lancement.
- À vérifier sur la machine du joueur: durée de la traduction, nombre d'échecs (`spirv/failed.tsv`), et l'effet sur
  l'image (plus de passage noir aux endroits nouveaux).
- Crashs: `rec-null-guard` corrigé (`asr` au lieu de `lsr`, et un `lsl #8` en trop) et second garde
  `rec-null-guard-vector` à `main+0x175dbec`; le log du build `b1bcbf4` atteint ce second site, ce qui confirme le
  premier correctif.
- Traducteur, sur les 23 432 programmes hors jeu (25/11/2026): 97,5 % traduits au départ; les handles OR'és par un
  ULOP3 sous garde uniforme (`VOTEU.ANY`, `PLOP3 P = UP`) étaient la première cause. Après le correctif: 98,8 %
  (280 échecs, 0 nouveau), et les 3 programmes vus en jeu qui échouaient passent, dont `93b54007c4699bde` et
  `e2d9a1f0ae647869` (1.3, refusés depuis le 17/11). Restent surtout `tex_scr_b_noConst_` (82), `tex_scr_urc_` (68),
  `STS.S16` (62).

### 6.7 Cause des crashs aléatoires trouvée (25/11/2026)

- Le parcours de records du jeu (`f_7001759b20` et voisines) lit une liste `{index, cible}` que le GPU écrit puis
  recopie dans un tampon de relecture (`main+0x1583dc0`). Le CPU la lisait toujours fausse (compte > 0xfff), et une
  paire au hasard faisait dépiler un record sous l'index 0: les crashs memcpy, Free d'adresse de pile et 0x104.
- Producteur: compute `567ed7cdc5b1df36` (compte atomique au mot 0, paires ensuite), bien traduit. Le compte n'était
  jamais remis à 0: `565a0b530c43107c` le fait par un `STG` sur une adresse chargée de c[3][0x10], que la recherche à
  l'exécution ne connaissait pas (store perdu). Corrigé dans le traducteur (docs/shaders.md, 25/11/2026). Les deux
  gardes de records restent comme protection. Les sondes (`ORBITALS_TRACE_READBACK`, `ORBITALS_TRACE_CS`) restent
  jusqu'à la confirmation en jeu.
- **Confirmé en jeu (25/11/2026, build `3d49420`):** 4 parties jusqu'au niveau, fermées normalement, aucun crash. Les 96
  lectures du tampon de relecture donnent un compte de 2 et les paires `{0,3} {1,3}` (avant: compte aléatoire à chaque
  lecture). Aucun programme sans SPIR-V dans ces parties.

### 6.8 État au 27/11/2026 (branche `orbitals-port`, build courant `ec65fcb`)

Cette section fait foi pour la reprise. Les détails techniques de l'astéroïde sont en 1.1b (fin du paragraphe sur le
streaming des mips).

**Confirmé en jeu**

- Scintillement corrigé (`6906362`, logs76) : les tampons transitoires n'avaient pas les bits d'usage texel buffer.
  `0a474bf` est conservé : une buffer texture ne se résout jamais en image.
- Flammes et nuage de fumée noire corrects depuis logs76 (dit par le joueur).

**Astéroïde à rayures : correction `e30dd58`, à confirmer**

- Le défaut : des taches ou rayures (vert, violet, jaune, orange et noir) sur la partie droite du gros astéroïde, vu
  par le hublot, côté gauche de l'écran partagé, vers 50 s après la sortie du sas. Le motif change d'une partie à
  l'autre.
- logs80 a donné la condition : propre au premier passage, cassé après un retour au menu principal suivi de
  « Continuer ». Le dump (avec `DUMP_TEX=1920x1080`) montre les rayures dans la couleur de base du G-buffer : une
  texture de la passe de base contient les texels d'une autre texture.
- La cause : une texture finalisée puis recréée à la même adresse avec le même descripteur reprend l'entrée libérée
  (`entry_of`, vk_tex_alias.c), avec le `gpu_lv` de l'ancienne texture. Les mips écrits par le CPU tombaient alors sous
  la règle « niveau entier » de `merge_cpu_writes`, que la course du streaming fait échouer (le cas de `6c1523e`).
  `e30dd58` efface les niveaux nommés par des copies à la reprise (`revived`). Les rendus, clears, images de stockage
  et alias (`gpu_lv` = ~0u) ne sont pas touchés.
- logs81 (build `e30dd58`) : la correction se déclenche (lignes `taken back at its address; the levels its last
  life's copies wrote (...) are the storage's again`, dont une texture 512x512 à 10 niveaux). Le joueur n'a pas encore
  dit si les rayures ont disparu.
- **Test à faire :** astéroïde, menu principal, Continuer, de nouveau l'astéroïde. Si les rayures restent : capture
  et log sans dump. Puis décider de garder ou non `6c1523e` (gardé : la course qu'il traite existe dans le code).

**Gel « GameThread timed out waiting for RenderThread » (2 parties sur 5 ; logs75, logs78, logs81)**

- Abandon du jeu après 120 s (`svc 0x26 Break`). La pile du RenderThread est identique à chaque fois, adresse pour
  adresse : `sdk+0x1670c8` (`SdkConditionVariableType::TimedWait`), `sdk+0x173a28`, `main+0x89ac8`, `main+0x3bfec`,
  `main+0x24c68c`, `main+0x1d96aac`, `main+0x1d9b64c`, `main+0x1719050`, `main+0x1d77d64`, `main+0x1d761e0`,
  `main+0x1641f84`, `main+0x32a0c`.
- Le diagnostic `c03f2a0` (logs81) établit que la variable de condition (clé `0x5a009f1e4`) a reçu son dernier signal
  120,061 s plus tôt, de Foreground Worker #1, **avant** le début de l'attente (40 770 signaux au total), et aucun
  depuis. Notre noyau n'a donc perdu aucun réveil : personne n'a signalé.
- `wait_woken` (kernel.c) termine une attente à durée finie à son échéance. Une attente de 120 s dans `TimedWait`
  signifie donc un délai demandé par le jeu supérieur à 120 s, en pratique infini.
- RHIThread, RHIInterruptThread et les workers attendent du travail. Dans logs81, Foreground Worker #0 était sur un
  cœur (travail audio Wwise, réveillé par `AK::AudioMgr`), mais il était au repos dans logs75 et logs78 : sans
  rapport.
- Hypothèse, non vérifiée : le RenderThread attend sa prochaine commande sur l'événement de sa file de tâches Unreal,
  pendant que le GameThread attend une barrière de rendu ; le réveil de la file aurait été manqué.
- Vérifié sans défaut : la traduction des barrières (`DMB` en fence seq_cst), LDAR/STLR (seq_cst), CAS/CASP et LSE,
  et l'émulation LDXR/STXR et LDXP/STXP par CAS sur la valeur lue (`runtime/cpu/cpu.h`).
- Les fonctions sont nommées par `orbitals-frame-report.exe --stack` (`ec65fcb`), lancé chez le joueur sur logs81
  (frame_report du 27/11/2026) :
  - `main+0x89ac8` est une attente libc++ avec échéance. La boucle relit l'octet `[x19+0x10]` (`ldaprb`) et continue
    tant que son bit 0 est à 1. Elle compare `steady_clock::now()` à l'échéance, puis appelle
    `pthread_cond_clockwait(x19+8, mutex, CLOCK_MONOTONIC, &abstime)`. Sa fonction utilise
    `LowLevelTasks::FOversubscriptionTls`.
  - `sdk+0x173970` est le `pthread_cond_clockwait` de musl. Il passe `cond+4` comme variable de condition SDK, donc
    la clé du noyau est `objet+0xc` et l'octet testé est `clé+4`. Dans logs81 : clé `0x5a009f1e4`, objet
    `0x5a009f1d8`.
  - `main+0x24a0f0` (l'appelant, via `main+0x3bfec` qui passe un délai en double, `d8`) utilise
    `Runtime/Core/Private/Tasks/TaskPrivate.cpp` : c'est l'attente d'une tâche Unreal (`UE::Tasks`).
  - Les deux horloges de cette attente (`steady_clock::now` et l'horloge monotone de musl) sont les nôtres et
    concordent (`ov_time2.c` ; les pointeurs de musl n'ont été remis qu'au démarrage, logs81).
- Le RenderThread attend donc la fin d'une tâche que personne n'a terminée, ou dont la fin n'a pas été notifiée. Le
  dernier signal sur la clé est arrivé 1 ms avant le début de l'attente.
- **Diagnostic suivant (commit après `9450295`) :** au vidage des threads, chaque attente de variable de condition
  donne le délai demandé à `svc 0x1C` (`timeout asked`) et les mots invités autour de la clé, avec l'octet `clé+4`.
  - Délai borné dépassé : le défaut est chez nous.
  - Pas de délai : le jeu attend sans limite.
  - Octet à 0 : la tâche est finie et le réveil a été manqué (une notification juste avant l'attente).
  - Octet à 1 : la tâche n'a jamais été terminée (file de tâches ou réveil des workers).

**Constatés, pas encore étudiés**

- Un rond bleu vif au-dessus du personnage de droite, dans le couloir de la colonie (capture de logs79). Il n'est pas
  dans l'image du dump de la même partie : intermittent. Peut-être le même mécanisme de texture reprise ; à revoir avec
  `e30dd58`.
- De longues lignes grises dans le couloir de la colonie (capture de logs78), probablement des particules mal placées.

**Diagnostics à retirer quand ils ne servent plus :** `ORBITALS_OCCLUSION=visible` (`68d2a22`), le log des écritures
dans une copie transitoire (`97bc308`), les notes de signal des variables de condition (`c03f2a0`, utile tant que le
gel n'est pas résolu).
