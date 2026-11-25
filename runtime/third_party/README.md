# runtime/third_party

| Folder | What it is |
|---|---|
| `imgui/` | Dear ImGui 1.91.9 (MIT) with its back ends: Vulkan (in-game menu), SDL3 and SDL_Renderer (start screen). One local change, marked `orbitals-recomp:` in `imgui_draw.cpp`: word wrap does not break after a dot or comma followed by a digit |
| `nxformat/` | `nx_reader.hpp` from [NXFormat](https://github.com/fab918/nxformat) @ 208b815 (MIT, its `LICENSE` beside it), unchanged: the reader of `.nx` dumps, called from `sdk/fs/nx.cpp` |
| `bc7enc/` | `bc7enc.cpp` from [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) @ b9438627 (MIT), unchanged; `bc7enc_c.cpp` calls it from C (`vk_texpre.c`) |
| `ooz/` | The decompressor of [ooz](https://github.com/powzix/ooz) @ 0503806 (GPL-3.0-or-later, notice at the top of `kraken.cpp`): Kraken, Mermaid, Selkie and Leviathan. `kraken.cpp` is the original's lines 1-4150 with one change, `#include "stdafx.h"` became `"ooz_compat.h"` (the Windows headers it took). Not taken: ooz's `main` and Windows glue, and `lzna.cpp`/`bitknit.cpp`, which carry no license notice (`ooz_stubs.cpp` makes those two formats fail). Ours: `ooz_compat.h`, `ooz_stubs.cpp`, `ooz_c.cpp`, `ooz.h`. Called by `sdk/fs/oodle_batch.c` |
