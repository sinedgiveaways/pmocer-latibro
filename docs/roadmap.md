# Roadmap

What is being worked on, in order of priority: Orbitals playable from boot (no crash, a picture as on the console),
then nothing hanging the GPU, then the frame rate. Updated 19/11/2026 (branch `diag/frame-history-trace`; how the
work is done with a player's machine: [workflow.md](workflow.md); the shaders: [shaders.md](shaders.md)).

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
too, (b). Also a runtime bug seen on the way: a storage-image write into a mirrored texture is not marked GPU-owned
(`vk_texture_storage_bound_locked`), so a later CPU page write re-uploads the storage over the GPU's result.

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

### 1.2a Kernel synchronization source audit (20/11/2026)

A source review of `runtime/core/kernel/kernel.c`, `kernel_svc.c` and the Windows platform lock/condition-variable layer found no confirmed missing-lock or double-wake path in mutex handoff, process-wide condition variables, address waits, or events. The global kernel lock `G` protects the shared wait list, handle table, event state and wake transitions. `WaitProcessWideKeyAtomic` sets/registers its key before releasing the guest mutex, and the existing kernel probe includes a negative control that reproduces the lost-wakeup bug if that order is reversed.

Two lazy configuration caches remain unsynchronized on first use: `g_aq_on` (`ADDR_QUEUES`) and `g_wait_ns` (`WAIT_NS`). Concurrent first callers can race while writing the same setting. This is a C data race, but there is no evidence yet that it causes the render-graph corruption. Next: make initialization race-free, run the kernel probe/CI, then add stress coverage for concurrent lock handoff and guarded critical sections. If that passes, trace the first invalid write to the render-graph array rather than treating the final `FMemory::Free` TLS lookup as the root cause.

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

- Shaders are translated by hand after a run (`dumped_shaders.py`); the setup could do it at each start when
  `spirv-dump` grew.
- A Linux package that starts on non-rolling distributions, and the launcher tested on more Windows machines.
- Steam Remote Play Together checked with Orbitals ([remote-play.md](remote-play.md)).
