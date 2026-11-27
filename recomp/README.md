# recomp

Generator: translates the AArch64 code of the game's NSOs (`main`, `subsdk0`, `sdk`) to C, one C function per game
function.

Copied from [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) `recomp/` @ c3b9a4e (MIT, license in
`LICENSE.totk-pc-recomp`; they took it in turn from LAS Recomp). Without their TotK patches (`patches-v*.txt`); the
setup finds this game's spin loops itself (no checked-in table). Kept close to the original so the two can be
compared; ruff does not check it (`pyproject.toml`).

Changes against the original:

- `lifter.py`: `RETAA`/`RETAB` as `ret`, `LDAPR*` as a load-acquire, `PAC*`/`AUT*`/`XPAC*` as identity (Switch 2's
  ARMv8.3).
- `gen.py`: the `GENERATED_MARKER` (`ORBITALS_RECOMP_…`) lives in the file itself instead of `tools/check_content.py`;
  `tools/git/pre-commit` watches for it. Every back edge of a loop without calls carries a compiler barrier
  (`__asm__ volatile("" ::: "memory")`): without it clang hoists out of the loop the loads another thread changes and
  leaves a `jmp` to itself (`main+0x27ebb20`, which `spin_waits.py` does not recognise). Function discovery also takes
  a `PACIBSP`/`PACIASP`/`BTI c` right after a zero word (the linker's padding) as a start: Orbitals calls
  `main+0x4137a50` only through a C++ member-function pointer, and no seed or materialised address names it.

Usage: `uv run tools/cpu/spin_waits.py --exefs game/exefs > build/patches-port.txt`, then
`uv run python recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches build/patches-port.txt`
(`uv run recomp/versions.py game/exefs` names the dump's build).

| File | What it does |
|---|---|
| `nso.py` | Loads an NSO: LZ4 segments, image, MOD0, symbols and relocations |
| `module.py` | A module loaded at a base: `.text` words, function seeds (`.eh_frame_hdr`, symbols, relocations) |
| `lifter.py` | AArch64 decoder → C statements per instruction |
| `simd_c.py` | NEON/FP → C |
| `flags.py`, `regs.py` | NZCV and the most used registers in local variables |
| `jumptables.py` | Jump tables resolved symbolically |
| `gen.py` | Generates the C by function, in parallel processes |
| `layout.py` | Load addresses of the modules |
| `patches.py` | Instruction patches by address and spin loops (`spin`) |
| `versions.py` | The game builds the port knows, by `exefs/main`'s build id: name, `RtGame.version` number, program ids, module build ids, patch table, setup steps that do not apply |
| `peek_words.py` | Prints a module's raw `.text` words around main-relative offsets in `patches-v1.txt`'s word format, from the extracted ExeFS, to state a patch site with the game's own words. Read-only |

Coverage: `tools/cpu/lift_coverage.py`.
