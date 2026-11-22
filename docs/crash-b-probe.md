# Crash B: synchronized runtime probe

This is a temporary, opt-in diagnostic for the 1.2.1 `main` module. It records live guest registers at the producer and consumer sites implicated by the read of `0x104`, plus bounded raw memory dumps. It does not change TLS behavior or patch `thr_new`.

## What it captures

The generator inserts `rt_crash_b_probe(c, pc)` immediately before these guest instructions, after any branch label and after spilling the generated register locals into `Cpu`:

- `main+0x174e180`: producer function entry
- `main+0x174e20c`: callback site
- `main+0x174e4a8`: producer copy site
- `main+0x175a01c` and `main+0x175a030`: consumer path

At producer sites it logs x0-x30 and raw qwords at x24 (source) and x1 (destination). At consumer sites it logs x0-x30, the pointer read from `[x21+0x158]`, the first 0x40 bytes at that array base, and the 0xE8 bytes at x27. These are raw values; the probe deliberately does not assign count/capacity meanings to fields.

The producer sites are capped at 64 records each; each consumer site is capped at 256. The helper is inert unless `ORBITALS_CRASH_B_PROBE=1` is set.

## Incremental generation

Run the generator against the already-extracted ExeFS and the existing generated-output directory:

```powershell
uv run python recomp/gen.py --exefs game/exefs --out build/generated --modules main --only-addresses 0x700174E180,0x700174E20C,0x700174E4A8,0x700175A01C,0x700175A030 --jobs 4
```

`--only-addresses` still performs function discovery for `main`, but emits only the existing C chunk(s) containing those addresses. It does not rewrite the module function table or manifest. The unchanged chunks retain their timestamps. Do not run `prepare.py` or the game extraction/setup cycle for this probe.

Then use the repository's normal incremental build step. The runtime adds one small C source file; only the generated chunk(s) containing these sites need changed game code. The `cpu.h` ABI header is intentionally unchanged so the other generated chunks do not become stale through a header dependency.

## Run

In the same PowerShell session used to launch the game:

```powershell
$env:ORBITALS_CRASH_B_PROBE = '1'
```

After reproducing the crash, search the log for `[CRASH_B_PROBE]`. Compare the producer's x1 destination / x24 source with the consumer's x21+0x158 array base / x27 record. The probe output is evidence for those exact runtime visits; it does not by itself prove that the producer and consumer refer to the same container unless the addresses match.