# runtime/core/boot

Boot: the program entry point, the game code tables, module loading, patches and the game clock.

| File | What it is |
|---|---|
| `main.c`, `boot.c` | Program entry (`ORBITALS_ALLOW_PTRACE=1` lets gdb attach) and boot sequence; `--boot` stops at MKW's first NVN call (1.0.0 `main+0xcf3a5c`, 1.8.0 `main+0xdd07c8`) |
| `gamelib.c`, `gamelib.h` | Game code tables (modules, functions, ExeFS hashes, patches): linked into `build/orbitals-together`, or loaded from `orbitalgame.so`/`orbitalgame.dll` in the selected installation folder in the player's build (`build.py --dist`), with its header id |
| `loader.c` | Loads the modules at their addresses, relocations, import binding, constructor flag (with Switch 2's `PACIBSP`); `--probe-loader` compares against MKW 1.0.0's numbers (another version's are printed only) |
| `game.c` | The game clock (`frame_step_*`, counts presents; totk-pc-recomp takes it from `frame_rate.c` and its patches); the world clock of A/B runs (`ORBITALS_AB`) counts from the first present |
| `version.c`, `version.h` | The fixed values of each game build (Orbitals), chosen by `RtGame.version`: the NVN2 proc table (else the dump's), the NVN2 device table, the boot checkpoint and `init_array` sizes; a field at 0 turns its feature off |
| `patches.c` | Patch sets by address: their switches at start and while the game runs |
