# Log analyzer (`shader_analyzer.exe`)

Turns an `orbitals.log` into a report you can send back: what the run reached, how it ended, which shaders and SDK
calls are missing. It reads the log, never changes it and sends nothing. Facts only: a pattern present or absent is
reported as such; the report names no root cause.

Source: `tools/run/shader_analyzer.py` (standard library only; the thread-dump reading is shared with
`tools/run/analyze_timeout.py`). The Windows executable is built by `.github/workflows/log-analyzer.yml`; its name and
the report's header carry the version (`VERSION` in the script), which is raised with every change to the patterns.

## Use

Get `shader_analyzer-<version>.exe` from the run's artifacts (Actions → the run → Artifacts) or, for versions built on
`orbitals-port`, from the release `shader-analyzer-<version>`.

- **Double-click**: it looks for `logs\orbitals.log` (the current folder, its own folder, the parent), else asks for the
  file. It writes `orbitals-report-<version>.md` next to the log, opens it, and waits for Enter.
- **Command line** (PowerShell):

  ```powershell
  .\shader_analyzer-0.1.0.exe --input .\logs\orbitals.log --output report.md
  .\shader_analyzer-0.1.0.exe --input .\logs\orbitals.log --output report.json --format json
  ```

  Options: `--input/-i`, `--output/-o` (stdout when absent), `--format md|json`, `--context N` (lines around each
  excerpt, default 3), `--version`.
- Without Python: `python tools/run/shader_analyzer.py --input logs/orbitals.log`.

Send the `.md` file. Say which version made it (it is in the file name and the first line).

## Making a log worth analysing

`ORBITALS_TRACE` takes a comma-separated list of `svc, host, fs, gfx, perf, sched, audio, present, draw, all`
(`runtime/core/boot/main.c`). `ORBITALS_TRACE=1` is not a valid item; the host logs "unknown item". For the stall
diagnosis use `ORBITALS_TRACE=host` (the "host call" lines, which give the NVN call sites the report lists):

```powershell
$env:ORBITALS_TRACE = "host"; .\orbitals-recomp.exe
```

To record the waits and signals that can hold a frame back, add `ORBITALS_DIAG_FENCE=1` (the diagnosis of the roadmap's
first section; off by default, one load on the hooked paths while off):

```powershell
$env:ORBITALS_TRACE = "host"; $env:ORBITALS_DIAG_FENCE = "1"; .\orbitals-recomp.exe
```

It writes a line every 5 s (`diag-fence: t=… vsync tick … | submits … presents … | fence submitted … queued … done …`),
a line for each blocking wait open for more than 2 s, and, after the thread dump of a guest abort, the same counters, the
blocking waits still open, the counts per kind and the last 64 events (host thread id and guest handle, the handle the
thread dump prints). Source: `runtime/core/diag/diag_fence.c`.

In the thread dump each waiting guest thread also gets a line `diag-fence: waits on <key>: N wait(s), M signal(s), the last
X s ago by guest 0x…`: the key is what the thread waits on (the key of a condition variable or nn::os event, an address,
or the first event of a WaitSynchronization) and the counts are those of the whole run (SVC 0x1C/0x1D, 0x34/0x35, 0x11/0x18
and the host's own event signals). "Never signalled" means no thread signalled that key since the log began.

The log is `logs\orbitals.log` next to the program. `crit.bin` (`ORBITALS_CRIT`, `tools/run/crit_path.py`) is **not** read
by this version.

## What the report contains

| Section | From |
|---|---|
| header: input, size, sha256, runtime version, program/build id, GPU | `log:`, `version:`, `vk: using gpu` lines |
| Findings: one line each, level `info`, `warn` or `error` | rules in `findings()` |
| Key threads at the abort: state and top frames of GameThread, Render, RHI, Viewport, FrameFlip, FrameOffset | the abort's thread dump |
| ORBITALS_DIAG_FENCE: the heartbeat table, the waiting threads and their keys, the last events before the abort | `diag-fence` lines |
| Shaders without prepared SPIR-V: the hashes and the (stage, program) pairs | `no prepared SPIR-V`, `a stage N shader of program P` |
| NVN sync / event / present call sites | `host call nvn:nvn…Sync/Event/Fence/Finish/Flush/Present/Acquire/Submit` |
| SDK functions without a host override | `imports: … (no host override)` and its per-namespace summary |
| NVN2 calls ignored or dropped | `nvn2: … ignored/dropped`, numbers folded |
| Excerpts: lines around the first matches of a pattern | the table below |
| Counted, not listed: guest page allocations (count, MiB, failures), SDK-call census lines, RomFS lookups, decoder set-up | noise filtered out before context is taken |

## Patterns (`RULES`)

| Name | Matches | Excerpts kept |
|---|---|---|
| `abort` | `FATAL: svc #0x26 Break at` | 2 |
| `fatal` | any other `FATAL` | 4 |
| `watchdog` | the abort's stack text `GameThread timed out waiting for …` | counted only |
| `shader_missing` | `no prepared SPIR-V <sha256>` | 1 |
| `shader_stage` | `a stage N shader of program P has no prepared SPIR-V` | counted only |
| `draw_skipped` | `a draw … skipped` | 1 |
| `pipeline_error` | `vk:` / `vk_pipeline:` lines with fail, error, VK_ERROR, device lost | 3 |
| `diag_open` | `diag-fence: open wait: …` (a blocking wait open for over 2 s) | 2 |
| `problem` | unsupported, unimplemented, not implemented, trap, fault, crash (the generic "Break is not implemented" tail, import and thread lines excluded) | 4 |

Findings from the `diag-fence` lines (facts only): the heartbeat at which the counters last changed and how many later
beats show the same values; the vsync tick over that time; the state at the abort; the blocking waits open at the abort
(or that none was); the age of the last event of each kind; `CATCHUP_TIMEOUT` and `ACQUIRE_TIMEOUT` counts. Per-key counts of guest-side waits and signals
are in the thread dump; there is no per-event ring of them (a ring would hold only the last seconds of the audio threads'
timed waits).

## Reading it correctly

- The abort's thread dump is taken while the GameThread handles the abort; its stack is not the stack it waited on.
- "Running or ready" is a scheduler label, not proof of progress.
- The dump lists guest threads only; replay, recorder, present and submit threads (host threads) are not in it.
- `written back … at fence N` lines are capped at 16 by the host (`log_limited`), so the highest fence in them is a lower
  bound, not the last fence.
- Draws without a shader are logged once per cause; the number of skipped draws is unknown.

## Adding a pattern

1. Add a regex to `RULES` (name → regex, excerpts kept), or a `RE_…` regex and a field in `_facts()` for a number or a
   list. A line the log repeats thousands of times belongs in `NOISE`.
2. Say what it means in `findings()` in words that state what the log shows, not why.
3. Add the line to `tools/run/testdata/sample-orbitals.log` and, if it is a finding, a `grep` to the workflow's smoke test.
4. Raise `VERSION` (patch for a pattern, minor for a new section), and list the change here.

## Versions

- **0.5.1**: the 0.5.0 chain was empty: the SDK's wait code does not keep x29 as a frame pointer. The runtime now scans the
  stack above the wait for return addresses (an instruction after a BL or BLR), printed with their offset from sp; the
  report words them as nearest first and possibly stale.
- **0.5.0**: the call chain of each thread's last wait (`last block frame` lines), and the signals on that key: how many,
  when and by whom last. The GameThread's chain and key are in the findings.
- **0.4.0**: the abort dump's "last block" line, which every thread that ever waited has: the key, the timeout, when the
  wait began and ended, whether it timed out, and the caller. It gives the GameThread's own last wait, which the dump
  shows as running. The key lines also carry how many waits had a timeout and how many of those timed out, so waits
  minus signals is not read as lost signals. New table and a finding for the GameThread.
- **0.3.0**: the per-thread key statistics of the abort dump: which key each idle guest thread waits on, how often it was
  signalled, when and by whom last; findings for keys never signalled, signalled long ago, or signalled just before the abort.
- **0.2.0**: reads `ORBITALS_DIAG_FENCE` output: heartbeat table and the point where the counters stop changing, state at
  the abort, open waits, the last events; the `diag_open` pattern.
- **0.1.0**: first version: end of run (abort, watchdog text, wait target, runtime, memory), thread states and key
  threads, missing shaders, skipped draws, present lines, fences seen, NVN sync call sites, SDK imports without override,
  NVN2 ignored calls, allocation counts, excerpts, Markdown and JSON.
