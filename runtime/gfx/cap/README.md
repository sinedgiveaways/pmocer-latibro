# runtime/gfx/cap

Frame capture and its replay without the game: what the Vulkan back end receives over a few frames (game memory,
pools, mappings, registered textures and samplers, submits and presents) goes to a file, and `build/orbitals-together --replay DIR`
hands it to the back end again in seconds.

| File | What it is |
|---|---|
| `cap.h` | Record format and the functions of the three parts |
| `cap_io.c` | The zstd-compressed stream (multithreaded write, record-by-record read) |
| `cap_mem.c` | Game memory: snapshot of the populated pages and, before each record, the ones that changed (hash per page on several threads) |
| `cap_record.c` | `ORBITALS_CAPTURE=1`: wraps the back end from boot; the capture starts at `ORBITALS_CAPTURE_AT` or with `ctl capture DIR [N]` |
| `cap_replay.c` | `--replay DIR [--out DIR] [--tag NAME]`: serial replay or with `ORBITALS_REPLAY_PAR`, a PNG of each present; with `ORBITALS_TRACE=perf`, the GPU busy time of each frame |

Variables: `ORBITALS_CAPTURE=1` (wrap; without it nothing changes), `ORBITALS_CAPTURE_AT=<present>`, `ORBITALS_CAPTURE_DIR`
(`build/cap/capture`), `ORBITALS_CAPTURE_FRAMES` (2), `ORBITALS_CAPTURE_EXIT=0` (keep playing after capturing).
Linux only: the list of populated pages comes from `PAGEMAP_SCAN` (`plat_mem_populated`). What only the GPU holds goes
too: texture images written by the GPU (`vk_texture_gpu_content`) and chunks of physical pools (`vk/vk_mem_cap.c`).
`--replay` applies the game's depth mode and shows no window.
