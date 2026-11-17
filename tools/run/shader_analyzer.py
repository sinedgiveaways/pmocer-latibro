#!/usr/bin/env python3
"""Read an orbitals.log and write a report: what the run reached, how it ended, which shaders and SDK calls are missing.

Standard library only, so PyInstaller makes one Windows executable of it (.github/workflows/log-analyzer.yml). It reads
the log, never changes it and sends nothing anywhere. Facts only: a pattern that is present or absent is reported as
such; the report does not name a root cause.

Usage:
  shader_analyzer.exe                                   (double-click: finds logs\\orbitals.log or asks for the file)
  shader_analyzer.exe --input orbitals.log --output report.md [--format md|json] [--context 3]
  python tools/run/shader_analyzer.py --input logs/orbitals.log
See docs/log-analyzer.md for the patterns, the report layout and how to add one.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_timeout import (  # noqa: E402  the thread-dump reading is shared with the plain timeout summary
    TEXT_RE,
    classify_state,
    clean_powershell_prefix,
    find_fatal_region,
    parse_threads,
)

VERSION = "0.2.0"

# Lines that fill a run's log (guest page allocations, decoder set-up, SDK call census, RomFS lookups): counted, not kept.
NOISE = re.compile(r"AllocateMemoryPages|nn::codec: queued decoder|^\[host\] sdk call \d+|^\[host\] fs entry ")
NOISE_KEYS = ("AllocateMemoryPages", "nn::codec: queued decoder", "] sdk call ", "] fs entry ")
ALLOC = re.compile(r"AllocateMemoryPages address (0x[0-9a-f]+) size (0x[0-9a-f]+) result (-?\d+)")

# name -> (regex, how many matches get a context excerpt). A rule only counts what the log contains.
RULES = {
    "abort": (re.compile(r"FATAL: svc #0x26 Break at"), 2),
    "fatal": (re.compile(r"\bFATAL\b(?!: svc #0x26)"), 4),
    "watchdog": (re.compile(r'"GameThread timed out waiting for'), 0),
    "shader_missing": (re.compile(r"no prepared SPIR-V [0-9a-f]{64}"), 1),
    "shader_stage": (re.compile(r"a stage \d+ shader of program \d+ has no prepared SPIR-V"), 0),
    "draw_skipped": (re.compile(r"\ba draw .*\bskipped\b"), 1),
    "pipeline_error": (re.compile(r"vk(_pipeline)?:.*(fail|error|VK_ERROR|device lost)", re.I), 3),
    "diag_open": (re.compile(r"diag-fence:? open wait: "), 2),
    "problem": (
        re.compile(r"\b(unsupported|unimplemented|not implemented|trap|fault|crash)\b", re.I),
        4,
    ),
}
PROBLEM_SKIP = re.compile(r"Break is not implemented|^\[host\] imports:|weak import|^\s*(thread|frame|guest frame) ")

SYNC_PROC = re.compile(r"host call nvn:(nvn\w*(?:Sync|Event|Fence|Finish|Flush|Present|Acquire|Submit)\w*) from (\S+)")
RE_VERSION = re.compile(r"\[host\] version: program (\w+), build (\w+), game version (\d+)")
RE_APP = re.compile(r"\[host\] log: .*\((.+)\)\s*$")
RE_GPU = re.compile(r"\[host\] vk: using gpu (\d+): (.+)$")
RE_DT_INIT = re.compile(r"boot: DT_INIT (\w+) state=(\d+) \((\d+)\)")
RE_PRESENT = re.compile(r"present of NVN format")
RE_INTERVAL = re.compile(r"present interval (\d+) -> (\d+)")
RE_HASH = re.compile(r"no prepared SPIR-V ([0-9a-f]{64})")
RE_STAGE = re.compile(r"a stage (\d+) shader of program (\d+) has no prepared SPIR-V")
RE_SPIRV_DIR = re.compile(r"gfx_shader: prepared SPIR-V from (.+)$")
RE_FENCE = re.compile(r"written back to its storage at fence (\d+)")
RE_IMPORT = re.compile(r"imports: (\w+) calls the SDK's (\S+) \(no host override\)")
RE_IMPORT_SUM = re.compile(r"imports: (\w+) calls (\d+) .*no host override; per namespace: (.*)$")
RE_WEAK = re.compile(r"(\w+): weak import (\S+) is unresolved")
RE_NVN2 = re.compile(r"nvn2: (.*?) (ignored|dropped)\s*$")
RE_MEMORY = re.compile(r"memory: process (\d+) MiB resident, (\d+) MiB private; system (\d+) of (\d+) MiB")
RE_RUNTIME = re.compile(r"run time at the abort: ([\d.]+) s")
# ORBITALS_DIAG_FENCE=1 (runtime/core/diag/diag_fence.c): a line every 5 s, and at an abort the same line, the open waits,
# the counts and the last events.
RE_BEAT = re.compile(
    r"diag-fence(?: at the abort)?: t=(\d+)s vsync tick (\d+) \(([-\d.]+)s ago\) \| submits (\d+) presents (\d+) "
    r"acquires (\d+) fence waits (\d+) \| fence submitted (\d+) queued (\d+) done (\d+) \| frames (\d+) \| "
    r"last event ([-\d.]+)s ago"
)
RE_OPEN = re.compile(r"diag-fence(:)? open wait: (.+)$")  # with the colon: a heartbeat; without: the abort dump
RE_RING = re.compile(
    r"^\s+-\s*([\d.]+)s\s+tid\s+(\d+)\s+guest\s+(0x[0-9a-f]+|0)\s+(\w+)\s+obj\s+(0x[0-9a-f]+)\s+a\s+(\d+)\s+b\s+(\d+)"
)
RE_DF_COUNTS = re.compile(r"diag-fence counts of (\d+) events:(.*)$")
BEAT_FIELDS = (
    "t",
    "tick",
    "tick_ago",
    "submits",
    "presents",
    "acquires",
    "fence_waits",
    "submitted",
    "queued",
    "done",
    "frames",
    "last_event_ago",
)


def read_log(path: Path):
    """One pass over the log: counters and facts, plus the lines that are not noise (kept for context and threads)."""
    h = hashlib.sha256()
    kept: list[str] = []
    noise = Counter()
    alloc = {"count": 0, "bytes": 0, "largest": 0, "failed": 0}
    hits = {k: [] for k in RULES}  # rule -> indexes into kept
    f = {
        "present_lines": 0, "intervals": [], "dt_init": {}, "nn_main": False, "gpu": None, "version": None, "app": None,
        "hashes": [], "stages": [], "spirv_dir": None, "fences": [], "imports": {}, "import_sums": {}, "weak": [],
        "nvn2": Counter(), "sync": {}, "memory": None, "runtime": None, "lines": 0,
        "df_on": False, "beats": [], "abort_beat": None, "df_open": [], "df_open_beats": [], "df_none_open": False, "ring": [], "df_counts": None,
    }  # fmt: skip
    with path.open("rb") as raw:
        for b in raw:
            h.update(b)
            f["lines"] += 1
            line = b.decode("utf-8", errors="replace").rstrip("\r\n")
            if ".log:" in line:
                line = clean_powershell_prefix(line)
            if NOISE.search(line):
                noise[next(k for k in NOISE_KEYS if k in line)] += 1
                m = ALLOC.search(line)
                if m:
                    size = int(m.group(2), 16)
                    alloc["count"] += 1
                    alloc["bytes"] += size
                    alloc["largest"] = max(alloc["largest"], size)
                    alloc["failed"] += m.group(3) != "0"
                continue
            idx = len(kept)
            kept.append(line)
            for name, (rx, _) in RULES.items():
                if rx.search(line) and not (name == "problem" and PROBLEM_SKIP.search(line)):
                    hits[name].append(idx)
            _facts(line, f)
    return h.hexdigest(), kept, noise, alloc, hits, f


def _facts(line: str, f: dict) -> None:
    for rx, key, fn in (
        (RE_VERSION, "version", lambda m: m.groups()),
        (RE_APP, "app", lambda m: m.group(1)),
        (RE_GPU, "gpu", lambda m: m.group(2)),
        (RE_SPIRV_DIR, "spirv_dir", lambda m: m.group(1)),
        (RE_MEMORY, "memory", lambda m: tuple(int(x) for x in m.groups())),
        (RE_RUNTIME, "runtime", lambda m: float(m.group(1))),
    ):
        m = rx.search(line)
        if m:
            f[key] = fn(m)
    if "diag-fence" in line:
        _diag_fence_facts(line, f)
    elif (m := RE_RING.match(line)) and f["df_on"]:
        f["ring"].append(
            (float(m.group(1)), int(m.group(2)), m.group(3), m.group(4), m.group(5), int(m.group(6)), int(m.group(7)))
        )
    if RE_PRESENT.search(line):
        f["present_lines"] += 1
    if m := RE_INTERVAL.search(line):
        f["intervals"].append((int(m.group(1)), int(m.group(2))))
    if m := RE_DT_INIT.search(line):
        f["dt_init"][m.group(1)] = (int(m.group(2)), int(m.group(3)))
    if "ENTER nnMain" in line:
        f["nn_main"] = True
    if m := RE_HASH.search(line):
        if m.group(1) not in f["hashes"]:
            f["hashes"].append(m.group(1))
    if m := RE_STAGE.search(line):
        f["stages"].append((int(m.group(1)), int(m.group(2))))
    if m := RE_FENCE.search(line):
        f["fences"].append(int(m.group(1)))
    if m := RE_IMPORT.search(line):
        f["imports"].setdefault(m.group(1), []).append(m.group(2))
    if m := RE_IMPORT_SUM.search(line):
        f["import_sums"][m.group(1)] = (int(m.group(2)), m.group(3))
    if m := RE_WEAK.search(line):
        f["weak"].append((m.group(1), m.group(2)))
    if m := RE_NVN2.search(line):
        f["nvn2"][re.sub(r"\d+", "N", m.group(1)) + " " + m.group(2)] += 1
    if m := SYNC_PROC.search(line):
        f["sync"].setdefault(m.group(1), set()).add(m.group(2))


def _diag_fence_facts(line: str, f: dict) -> None:
    f["df_on"] = True
    if m := RE_BEAT.search(line):
        beat = dict(zip(BEAT_FIELDS, (float(x) if "." in x else int(x) for x in m.groups()), strict=True))
        if "at the abort" in line:
            f["abort_beat"] = beat
        else:
            f["beats"].append(beat)
    elif m := RE_OPEN.search(line):
        f["df_open_beats" if m.group(1) else "df_open"].append(m.group(2))
    elif "no blocking wait of the NVN layer" in line:
        f["df_none_open"] = True
    elif m := RE_DF_COUNTS.search(line):
        f["df_counts"] = {k: int(v) for k, v in re.findall(r"(\w+) (\d+),", m.group(2))}
    elif "the last" in line and "events, oldest first" in line:
        f["ring"] = []  # a later abort dump replaces an earlier one


def analyse(path: Path, context: int) -> dict:
    sha, kept, noise, alloc, hits, f = read_log(path)
    d: dict = {
        "tool": f"shader_analyzer {VERSION}", "input": str(path), "sha256": sha, "size_bytes": path.stat().st_size,
        "lines": f["lines"], "noise_lines": dict(noise), "generated": datetime.now().isoformat(timespec="seconds"),
    }  # fmt: skip
    d["run"] = {
        "app": f["app"], "gpu": f["gpu"],
        "program": dict(zip(("program_id", "build_id", "game_version"), f["version"])) if f["version"] else None,
        "dt_init": {k: {"state": v[0], "entries": v[1]} for k, v in f["dt_init"].items()}, "nn_main_entered": f["nn_main"],
    }  # fmt: skip

    region = find_fatal_region(kept)
    fatal = kept[region[0] : region[1]] if region else []
    texts = [m.group(1) for line in fatal if (m := TEXT_RE.match(line))]
    threads = parse_threads(fatal)
    states = Counter(classify_state(t["state"]) for t in threads)
    key = [t for t in threads if re.search(r"GameThread|Render|RHI|Viewport|FrameFlip|FrameOffset", t["name"])]
    d["end"] = {
        "aborted": bool(region), "runtime_s": f["runtime"],
        "watchdog_text": next((t for t in texts if "timed out waiting" in t), None),
        "wait_targets": [t for t in texts if t in ("RenderThread", "RHIThread", "GameThread")],
        "memory_mib": dict(zip(("resident", "private", "system_used", "system_total"), f["memory"])) if f["memory"] else None,
    }  # fmt: skip
    d["threads"] = {
        "count": len(threads),
        "states": dict(states),
        "running": [t["name"] for t in threads if "running or ready" in t["state"]],
        "key": [{"name": t["name"], "state": classify_state(t["state"]), "frames": t["frames"][:5]} for t in key],
    }
    d["shaders"] = {
        "spirv_dir": f["spirv_dir"], "missing_hashes": f["hashes"], "stages_without_spirv": sorted(set(f["stages"])),
        "skip_messages": len(hits["draw_skipped"]), "match_lines": len(hits["shader_missing"]),
    }  # fmt: skip
    d["frames"] = {
        "present_lines": f["present_lines"],
        "interval_changes": f["intervals"],
        "highest_fence_in_log": max(f["fences"], default=None),
        "fence_lines": len(f["fences"]),
    }
    d["nvn_sync_call_sites"] = {k: sorted(v) for k, v in sorted(f["sync"].items())}
    d["sdk"] = {
        "no_override_by_module": {m: sorted(set(v)) for m, v in f["imports"].items()},
        "summaries": {m: {"count": n, "per_namespace": ns} for m, (n, ns) in f["import_sums"].items()},
        "weak_unresolved": sorted(set(f["weak"])),
    }
    d["nvn2_ignored"] = dict(f["nvn2"].most_common())
    d["allocations"] = alloc
    d["patterns"] = {
        name: {
            "matches": len(idx),
            "excerpts": [
                {"line_in_filtered_log": i, "text": kept[max(0, i - context) : i + context + 1]}
                for i in idx[: RULES[name][1]]
            ],
        }
        for name, idx in hits.items()
    }
    d["diag_fence"] = {
        "on": f["df_on"],
        "heartbeats": f["beats"],
        "at_abort": f["abort_beat"],
        "open_waits_at_abort": f["df_open"],
        "open_waits_in_heartbeats": f["df_open_beats"],
        "no_open_wait_at_abort": f["df_none_open"],
        "counts": f["df_counts"],
        "last_events_oldest_first": [
            dict(zip(("ago_s", "host_tid", "guest", "kind", "obj", "a", "b"), e, strict=True)) for e in f["ring"]
        ],
    }
    d["findings"] = findings(d)
    return d


def findings(d: dict) -> list[dict]:
    out = []

    def add(level, text):
        out.append({"level": level, "text": text})

    run, end, th, sh, fr = d["run"], d["end"], d["threads"], d["shaders"], d["frames"]
    if run["nn_main_entered"]:
        add(
            "info",
            "The guest reached nnMain (static constructors: "
            + ", ".join(f"{k} {v['entries']}" for k, v in run["dt_init"].items())
            + ").",
        )
    elif not d["lines"]:
        add("error", "The log is empty.")
    else:
        add(
            "warn",
            "No 'ENTER nnMain' line: the run stopped during boot, or the log was made without ORBITALS_TRACE=host "
            "(the trace that writes the 'host call' lines; this line is probably one of them).",
        )
    if end["aborted"]:
        w = end["watchdog_text"] or "an SDK abort (svc #0x26 Break)"
        tgt = ", ".join(end["wait_targets"]) or "not captured"
        add(
            "error",
            f"The run ended with {w}; wait target in the abort stack text: {tgt}; "
            f"run time at the abort: {end['runtime_s']} s.",
        )
        key = th["key"]
        if th["count"]:
            others = [k for k in key if k["name"] != "GameThread"]
            idle = [k for k in others if k["state"].startswith("waiting")]
            if others and len(idle) == len(others):
                add(
                    "warn",
                    f"Every other thread named Render/RHI/Viewport/FrameFlip/FrameOffset ({len(others)}) was waiting in the "
                    "dump and none was running. That fits a missing signal, not a busy loop. The dump lists guest threads only: a "
                    "host thread (replay, recorder, present) is not in it.",
                )
            add(
                "info",
                f"{th['count']} guest threads in the dump: "
                + "; ".join(f"{k} {v}" for k, v in th["states"].items())
                + ".",
            )
    elif d["lines"]:
        add(
            "info",
            "No svc #0x26 Break found: the log ends without the abort dump (window closed, other fatal, or truncated).",
        )
    if sh["missing_hashes"]:
        add(
            "warn",
            f"{len(sh['missing_hashes'])} distinct shader(s) have no prepared SPIR-V, so their draws are skipped "
            f"({sh['skip_messages']} 'skipped' message(s); each is logged once, so the number of draws is not known). "
            "This report does not show that they cause a stall.",
        )
    if sh["spirv_dir"]:
        add("info", f"Prepared SPIR-V is read from {sh['spirv_dir']}.")
    if fr["present_lines"] == 0:
        add("warn", "No 'present of NVN format' line: no frame was presented, or the line was cut from this log.")
    else:
        add(
            "info",
            f"{fr['present_lines']} 'present of NVN format' line(s) (this line may be logged once per format, not "
            "once per frame).",
        )
    if fr["highest_fence_in_log"] is not None:
        add(
            "info",
            f"Highest fence seen in a 'written back' line: {fr['highest_fence_in_log']}. These lines are capped at 16 "
            "by the host, so this is a lower bound, not the last fence.",
        )
    if end["memory_mib"]:
        m = end["memory_mib"]
        add(
            "info",
            f"Memory at the abort: {m['resident']} MiB resident, {m['private']} MiB private; system {m['system_used']} "
            f"of {m['system_total']} MiB.",
        )
    if d["allocations"]["failed"]:
        add("warn", f"{d['allocations']['failed']} guest page allocation(s) returned a non-zero result.")
    if d["sdk"]["summaries"]:
        n = sum(v["count"] for v in d["sdk"]["summaries"].values())
        add("info", f"{n} SDK function(s) the game imports have no host override ({', '.join(d['sdk']['summaries'])}).")
    if d["sdk"]["weak_unresolved"]:
        add(
            "info",
            f"{len(d['sdk']['weak_unresolved'])} weak import(s) unresolved (read as 0; Unreal thread-local hooks are common).",
        )
    if d["nvn2_ignored"]:
        add("info", f"{len(d['nvn2_ignored'])} kind(s) of NVN2 call ignored or dropped.")
    df = d["diag_fence"]
    if df["on"]:
        _diag_fence_findings(df, add)
    elif d["lines"]:
        add(
            "info",
            "No diag-fence line: the run was made without ORBITALS_DIAG_FENCE=1 (or with a build that lacks it).",
        )
    if not d["nvn_sync_call_sites"]:
        add("info", "No NVN sync/event/fence call site in this log (log without ORBITALS_TRACE=host?).")
    return out


def _diag_fence_findings(df: dict, add) -> None:
    beats = df["heartbeats"]
    add(
        "info",
        f"ORBITALS_DIAG_FENCE was on: {len(beats)} heartbeat line(s)"
        + (f", the last at t={beats[-1]['t']} s." if beats else "."),
    )
    key = ("submits", "presents", "fence_waits", "done", "submitted")
    changed = [i for i in range(1, len(beats)) if any(beats[i][k] != beats[i - 1][k] for k in key)]
    if len(beats) >= 2:
        last = changed[-1] if changed else 0
        flat = len(beats) - 1 - last
        if flat >= 1:
            a, b = beats[last], beats[-1]
            add(
                "warn",
                f"The counters (submits, presents, fence waits, fence submitted/done) last changed at the heartbeat t={a['t']} s; "
                f"{flat} later heartbeat(s), up to t={b['t']} s, show the same values (presents {b['presents']}, submits "
                f"{b['submits']}, fence done {b['done']} of {b['submitted']} submitted). The vsync tick went from {a['tick']} to {b['tick']}.",
            )
        else:
            add("info", f"The counters were still changing at the last heartbeat (t={beats[-1]['t']} s).")
    ab = df["at_abort"]
    if ab:
        add(
            "info",
            f"At the abort: submits {ab['submits']}, presents {ab['presents']}, acquires {ab['acquires']}, fence waits "
            f"{ab['fence_waits']}; fence submitted {ab['submitted']}, queued {ab['queued']}, done {ab['done']}; vsync tick "
            f"{ab['tick']} ({ab['tick_ago']} s ago); last event {ab['last_event_ago']} s ago.",
        )
    if df["open_waits_in_heartbeats"]:
        w = df["open_waits_in_heartbeats"]
        add("warn", f"{len(w)} heartbeat line(s) report a blocking wait open for over 2 s; the last: {w[-1]}")
    if df["open_waits_at_abort"]:
        add("warn", "Blocking waits still open at the abort: " + " | ".join(df["open_waits_at_abort"]))
    elif df["no_open_wait_at_abort"]:
        add(
            "info",
            "No blocking wait of the NVN layer or the Vulkan back end was open at the abort: the idle threads wait on "
            "something else (guest events or condition variables).",
        )
    ring = df["last_events_oldest_first"]
    if ring:
        latest = {}
        for e in ring:
            latest[e["kind"]] = e["ago_s"]
        add(
            "info",
            "Seconds before the abort of the last event of each kind in the ring: "
            + ", ".join(f"{k} {v}" for k, v in sorted(latest.items(), key=lambda kv: kv[1]))
            + ".",
        )
    c = df["counts"] or {}
    for k in ("CATCHUP_TIMEOUT", "ACQUIRE_TIMEOUT"):
        if c.get(k):
            add("warn", f"{k}: {c[k]} (a wait that the NVN layer bounds gave up).")


def render_md(d: dict) -> str:
    L = [f"# Orbitals log report ({d['tool']})", "",
         f"- Input: `{d['input']}` ({d['size_bytes']:,} bytes, {d['lines']:,} lines, sha256 `{d['sha256'][:16]}…`)",
         f"- Generated: {d['generated']}"]  # fmt: skip
    r = d["run"]
    if r["app"]:
        L.append(f"- Runtime: {r['app']}")
    if r["program"]:
        p = r["program"]
        L.append(f"- Program {p['program_id']}, build {p['build_id']}, game version {p['game_version']}")
    if r["gpu"]:
        L.append(f"- GPU: {r['gpu']}")
    L += ["", "## Findings", ""]
    L += [f"- **{f['level']}** {f['text']}" for f in d["findings"]]
    th = d["threads"]
    if th["count"]:
        L += ["", "## Key threads at the abort", "", "| thread | state | top frames |", "|---|---|---|"]
        for t in th["key"]:
            L.append(f"| {t['name']} | {t['state']} | {' ← '.join('`' + x + '`' for x in t['frames'][:3])} |")
        if th["running"]:
            L.append(f"\nRunning or ready (a scheduler label, not proof of progress): {', '.join(th['running'])}")
    df = d["diag_fence"]
    if df["on"]:
        L += ["", "## ORBITALS_DIAG_FENCE", ""]
        if df["heartbeats"]:
            L += ["| t (s) | vsync tick | submits | presents | acquires | fence waits | fence submitted | queued | done | frames |",
                  "|---|---|---|---|---|---|---|---|---|---|"]  # fmt: skip
            L += [
                f"| {b['t']} | {b['tick']} | {b['submits']} | {b['presents']} | {b['acquires']} | {b['fence_waits']} | "
                f"{b['submitted']} | {b['queued']} | {b['done']} | {b['frames']} |"
                for b in df["heartbeats"][-24:]
            ]
        if df["last_events_oldest_first"]:
            L += [
                "",
                "Last events before the abort, oldest first (host tid, guest handle as in the thread dump):",
                "",
                "```",
            ]
            L += [
                f"-{e['ago_s']:>9.3f}s tid {e['host_tid']:<6} guest {e['guest']:<6} {e['kind']:<16} obj {e['obj']} a {e['a']} b {e['b']}"
                for e in df["last_events_oldest_first"][-40:]
            ]
            L.append("```")
    sh = d["shaders"]
    if sh["missing_hashes"]:
        L += ["", "## Shaders without prepared SPIR-V", ""] + [f"- `{h}`" for h in sh["missing_hashes"]]
        if sh["stages_without_spirv"]:
            L.append("\nStages without SPIR-V (stage, program): " + ", ".join(map(str, sh["stages_without_spirv"])))
    if d["nvn_sync_call_sites"]:
        L += ["", "## NVN sync / event / present call sites (first call per site)", ""]
        L += [f"- `{k}`: {', '.join(v)}" for k, v in d["nvn_sync_call_sites"].items()]
    if d["sdk"]["summaries"]:
        L += ["", "## SDK functions without a host override", ""]
        L += [f"- {m}: {v['count']} ({v['per_namespace']})" for m, v in d["sdk"]["summaries"].items()]
    if d["nvn2_ignored"]:
        L += ["", "## NVN2 calls ignored or dropped", ""] + [f"- {k}: {n}" for k, n in d["nvn2_ignored"].items()]
    L += ["", "## Excerpts", ""]
    for name, p in d["patterns"].items():
        if not p["excerpts"]:
            continue
        L.append(f"### {name} ({p['matches']} match(es))")
        for e in p["excerpts"]:
            L += ["```", *e["text"], "```"]
    a = d["allocations"]
    L += ["", "## Counted, not listed", "",
          f"- Guest page allocations: {a['count']:,} ({a['bytes'] / 2**20:,.0f} MiB, largest {a['largest'] / 2**20:,.1f} MiB, "
          f"{a['failed']} failed)",
          *[f"- {k.strip('] ')}: {n:,} line(s)" for k, n in d["noise_lines"].items() if k != "AllocateMemoryPages"]]  # fmt: skip
    L += ["", "## What this report does not show", "",
          "- Why a signal never came. The abort dump is taken while the GameThread handles the abort; its stack is not "
          "the stack it waited on.",
          "- Host threads. Replay, recorder, present and submit threads are not in the dump.",
          "- Frames: the log has no per-frame line. With ORBITALS_DIAG_FENCE=1 it has a counters line every 5 s and, at an "
          "abort, the last events of the NVN layer and the back end; guest-side events (nn::os) are not recorded.",
          "- crit.bin is not read by this version."]  # fmt: skip
    return "\n".join(L) + "\n"


def default_log() -> Path | None:
    here = Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve().parent
    for base in (Path.cwd(), here, here.parent):
        for rel in ("logs/orbitals.log", "orbitals.log"):
            if (base / rel).is_file():
                return base / rel
    return None


def pick_file() -> Path | None:
    try:
        import tkinter
        from tkinter import filedialog

        root = tkinter.Tk()
        root.withdraw()
        name = filedialog.askopenfilename(
            title="Choose orbitals.log", filetypes=[("Log", "*.log *.txt"), ("All", "*.*")]
        )
        root.destroy()
        return Path(name) if name else None
    except Exception:  # no Tk: ask on the console
        name = input("Path of orbitals.log: ").strip().strip('"')
        return Path(name) if name else None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", "-i", type=Path, help="orbitals.log (default: logs\\orbitals.log, else a file dialog)")
    ap.add_argument(
        "--output", "-o", type=Path, help="report file (default: next to the log when double-clicked, else stdout)"
    )
    ap.add_argument("--format", choices=("md", "json"), default="md")
    ap.add_argument("--context", type=int, default=3, help="lines of context around each excerpt (default 3)")
    ap.add_argument("--version", action="version", version=f"shader_analyzer {VERSION}")
    args = ap.parse_args(argv)
    interactive = args.input is None
    path = args.input or default_log() or pick_file()
    if not path or not path.is_file():
        print(f"shader_analyzer {VERSION}: no log file.", file=sys.stderr)
        if interactive:
            input("Press Enter to close.")
        return 2
    d = analyse(path, max(0, args.context))
    text = json.dumps(d, indent=2, ensure_ascii=False) if args.format == "json" else render_md(d)
    out = args.output or (path.with_name(f"orbitals-report-{VERSION}.{args.format}") if interactive else None)
    if out:
        out.write_text(text, encoding="utf-8")
        print(f"shader_analyzer {VERSION}: report written to {out}")
        if interactive:
            try:
                os.startfile(out)  # type: ignore[attr-defined]  (Windows only)
            except Exception:
                pass
            input("Press Enter to close.")
    else:
        sys.stdout.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
