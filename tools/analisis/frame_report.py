"""One text report on nn::vi::ListFrameHistory and the code that calls it, from the game's ExeFS.

Why: the frame-21 stall is a wait for ListFrameHistory entries (the game asks "how many" with out=0 count=0, then calls
again with count=1 and a buffer, main+0x1540460 and main+0x15404c0). Orbitals answers 0 and stores nothing because the
layout of nn::vi::FrameInfo is unknown. This report holds what is needed to read it: the symbols, the game's code after
the second call (which offsets of the buffer it reads) and the SDK's own ListFrameHistory (which offsets it writes). It is
built as a Windows executable by .github/workflows/frame-report.yml so it runs without git, uv or Python.

Usage: orbitals-frame-report.exe [EXEFS_DIR] [--out frame_report.txt] [--sym TEXT ...]
       uv run tools/analisis/frame_report.py [EXEFS_DIR] [--out frame_report.txt] [--sym TEXT ...]
--sym TEXT lists only the symbols whose name contains TEXT (full mangled names, every module, imports too):
no disassembly. Used to find the exact name of an SDK function to replace.
--func TEXT disassembles every function whose symbol name contains TEXT (the first FUNCTION_CAP instructions of each),
with calls named: how an SDK function reads and writes the objects it is given, before a host override replaces it.
--stack LOG names the game frames of the abort's thread dump in an orbitals.log (q.py fn for each: callers, named
callees, strings, and the code before the call, where its arguments are set), for the threads named like --thread
(default STACK_THREADS): Unreal's "GameThread timed out waiting for RenderThread" stops in game code the sandbox cannot
name without the ExeFS (logs81, 27/11/2026). Double-clicked next to exefs/ with a logs/orbitals.log, it does that.
EXEFS_DIR holds main, sdk and subsdk0 (default: ORBITALS_GAME/exefs, ./exefs, or exefs next to the executable).
Builds the binary database in a temporary folder (about a minute), then writes the report. Nothing of the game is kept.
"""

import argparse
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

try:  # from the source tree; the frozen executable carries these modules itself
    _root = Path(__file__).resolve().parents[2]
    for _p in (_root / "recomp", _root / "tools/gpu", _root / "tools/analisis"):
        sys.path.insert(0, str(_p))
except IndexError:
    pass

import bindb  # noqa: E402
from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs  # noqa: E402
from nso import Nso  # noqa: E402

CALLER_SITES = ("main+0x1540460", "main+0x15404c0")
CODE_WINDOW = ("main", 0x1540400, 0x1540700)  # module, start, end: around the Viewport thread's wait and both calls
FUNCTION_CAP = 160  # instructions printed for a whole function
NAMES = ("ListFrameHistory", "GetLatestFrameNumber")
# --stack: the threads of Unreal's "GameThread timed out waiting for RenderThread" (logs75, logs78, logs81)
STACK_THREADS = ("RenderThread", "GameThread", "RHIThread", "RHIInterruptThread", "Foreground Worker")


def demangle_or_same(names: list[str]) -> dict[str, str]:
    """bindb's c++filt is not on Windows: keep the mangled names there (they still contain the readable words)."""
    try:
        res = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True)
        return dict(zip(names, res.stdout.split("\n"), strict=False))
    except OSError, subprocess.CalledProcessError:
        return {n: n for n in names}


bindb.demangle = demangle_or_same


def find_exefs(arg: str | None) -> Path:
    cands = []
    if arg:
        cands.append(Path(arg))
    if os.environ.get("ORBITALS_GAME"):
        cands.append(Path(os.environ["ORBITALS_GAME"]) / "exefs")
    cands += [Path("exefs"), Path(sys.argv[0]).resolve().parent / "exefs"]
    for c in cands:
        if (c / "main").exists():
            return c
    sys.exit(
        "ExeFS not found (needs a folder with main, sdk and subsdk0). Pass it: orbitals-frame-report.exe C:\\path\\exefs"
    )


class Report:
    def __init__(self, db: sqlite3.Connection, exefs: Path) -> None:
        self.db = db
        self.exefs = exefs
        # real sizes: main is large enough that sdk starts within main's first 256 MiB (a fixed span named sdk code main+...)
        self.spans = db.execute("SELECT name, base, size FROM module ORDER BY base").fetchall()
        self.base = {m: b for m, b, _ in self.spans}
        self.names: dict[int, str] = {}
        for addr, name in db.execute("SELECT addr, name FROM plt WHERE name IS NOT NULL"):
            self.names[addr] = name
        for addr, name in db.execute("SELECT addr, name FROM func WHERE name IS NOT NULL AND name != ''"):
            self.names.setdefault(addr, name)
        self.images: dict[str, bytes] = {}
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.lines: list[str] = []

    def out(self, text: str = "") -> None:
        print(text)
        self.lines.append(text)

    def rel(self, addr: int) -> str:
        for mod, base, size in self.spans:
            if base <= addr < base + size:
                return f"{mod}+{addr - base:#x}"
        return f"{addr:#x}"

    def image(self, mod: str) -> bytes:
        if mod not in self.images:
            self.images[mod] = bytes(Nso(str(self.exefs / mod)).image)
        return self.images[mod]

    def module_of(self, addr: int) -> str:
        return next(m for m, base, size in self.spans if base <= addr < base + size)

    def disasm(self, lo: int, hi: int, cap: int | None = None) -> None:
        mod = self.module_of(lo)
        base = self.base[mod]
        rows = list(self.md.disasm(self.image(mod)[lo - base : hi - base], lo))
        if cap and len(rows) > cap:
            self.out(f"  (function has {len(rows)} instructions, first {cap} shown)")
            rows = rows[:cap]
        for i in rows:
            note = ""
            if i.mnemonic in ("bl", "b") and i.op_str.startswith("#"):
                t = int(i.op_str[1:], 16)
                note = f"  ; {self.rel(t)} {self.names.get(t, '')}"
            self.out(f"  {self.rel(i.address):>16}  {i.mnemonic:8} {i.op_str}{note}")

    def function_at(self, addr: int) -> tuple[int, int, str | None] | None:
        return self.db.execute(
            "SELECT addr, end, name FROM func WHERE addr<=? AND end>? ORDER BY addr DESC LIMIT 1", (addr, addr)
        ).fetchone()

    def symbols(self, patterns: list[str]) -> None:
        for pat in patterns:
            self.out(f"== Symbols containing {pat!r} (module, address, kind, name)")
            rows = self.db.execute(
                "SELECT module, addr, kind, name FROM sym WHERE name LIKE ? ORDER BY name, module", (f"%{pat}%",)
            ).fetchall()
            for mod, addr, kind, name in rows:
                where = self.rel(addr) if addr is not None else "(import)"
                self.out(f"  {mod:8} {where:20} {kind:7} {name}")
            if not rows:
                self.out("  (none)")
            self.out()

    def functions(self, patterns: list[str]) -> None:
        seen = set()
        for pat in patterns:
            rows = self.db.execute(
                "SELECT addr, name FROM sym WHERE kind='func' AND addr IS NOT NULL AND name LIKE ? ORDER BY addr",
                (f"%{pat}%",),
            ).fetchall()
            if not rows:
                self.out(f"== No function contains {pat!r}\n")
            for addr, name in rows:
                f = self.function_at(addr)
                if not f or f[0] in seen:
                    continue
                seen.add(f[0])
                self.out(f"== {name} at {self.rel(f[0])} ({(f[1] - f[0]) // 4} instructions)")
                self.disasm(f[0], f[1], FUNCTION_CAP)
                self.out()

    def name_of(self, addr: int) -> str:
        n = self.names.get(addr)
        if n:
            return n
        row = self.db.execute("SELECT name FROM sym WHERE addr=? AND kind='func' LIMIT 1", (addr,)).fetchone()
        return row[0] if row and row[0] else ""

    def summary(self, addr: int, limit: int = 10) -> None:
        """q.py fn for the function containing addr: callers, named callees first, strings."""
        f = self.function_at(addr)
        if not f:
            self.out(f"    {self.rel(addr)}: no function found")
            return
        start, end, _ = f
        ncallers = self.db.execute(
            "SELECT count(DISTINCT caller) FROM call WHERE target=? OR target IN (SELECT addr FROM plt WHERE target=?)",
            (start, start),
        ).fetchone()[0]
        out = self.db.execute(
            "SELECT target, count(*) FROM call WHERE site>=? AND site<? AND NOT (target>=? AND target<?) "
            "GROUP BY target ORDER BY count(*) DESC",
            (start, end, start, end),
        ).fetchall()
        self.out(
            f"    in {self.rel(start)} {self.name_of(start)} ({(end - start) // 4} instructions, {ncallers} callers, "
            f"{len(out)} callees)"
        )
        named = sorted(out, key=lambda t: not self.name_of(t[0]))
        for t, n in named[:limit]:
            self.out(f"      -> {self.rel(t)} {self.name_of(t)}" + (f" x{n}" if n > 1 else ""))
        strs = self.db.execute(
            "SELECT DISTINCT s.text FROM ref r JOIN str s ON s.addr=r.target WHERE r.site>=? AND r.site<?", (start, end)
        ).fetchall()
        for (s,) in strs[:limit]:
            self.out(f'      "{s[:120]}"')

    def stack(self, log: Path, threads: list[str]) -> None:
        """The game frames of the abort's thread dump: what each one is, and the code before each call."""
        head = re.compile(r'^\s*thread (0x[0-9a-f]+) "([^"]*)"')
        frame = re.compile(r"^\s*(?:guest )?frame\s+\d+:\s*(main|sdk|subsdk0)\+(0x[0-9a-f]+)")
        stacks: list[tuple[str, list[tuple[str, int]]]] = []
        cur = None
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            m = head.match(line)
            if m:
                cur = None
                if any(t.lower() in m[2].lower() for t in threads):
                    cur = (f"{m[1]} {m[2]}", [])
                    stacks.append(cur)
                continue
            m = frame.match(line)
            if m and cur is not None:
                cur[1].append((m[1], int(m[2], 16)))
        if not stacks:
            self.out(f"== No thread named like {threads} in {log}")
            return
        seen: set[int] = set()
        for title, frames in stacks:
            self.out(f"== {title}: {len(frames)} frames")
            for mod, off in frames:
                if mod not in self.base:
                    continue
                addr = self.base[mod] + off
                self.out(f"  {mod}+{off:#x}")
                if addr in seen:
                    self.out("    (above)")
                    continue
                seen.add(addr)
                self.summary(addr)
                # the frame is a return address: the call is the instruction before it, its arguments just above
                self.out("    code before the call:")
                self.disasm(
                    max(addr - 4 * 16, self.function_at(addr)[0] if self.function_at(addr) else addr - 64), addr + 4
                )
            self.out()

    def run(self) -> None:
        self.out("== Symbols (module, address, kind, name)")
        wanted = []
        for pat in NAMES:
            for mod, addr, kind, name in self.db.execute(
                "SELECT module, addr, kind, name FROM sym WHERE name LIKE ? ORDER BY module", (f"%{pat}%",)
            ):
                where = self.rel(addr) if addr is not None else "(import)"
                self.out(f"  {mod:8} {where:20} {kind:7} {name}")
                if addr is not None and kind == "func":
                    wanted.append((pat, addr, name))

        self.out("\n== The functions that contain the two ListFrameHistory calls")
        for site in CALLER_SITES:
            mod, off = site.split("+")
            addr = self.base[mod] + int(off, 16)
            f = self.function_at(addr)
            if not f:
                self.out(f"  {site}: no function found")
                continue
            callers = self.db.execute("SELECT caller FROM call WHERE target=? LIMIT 8", (f[0],)).fetchall()
            self.out(f"  {site}: function {self.rel(f[0])}..{self.rel(f[1])} ({(f[1] - f[0]) // 4} instructions)")
            self.out("     called from: " + (", ".join(self.rel(c[0]) for c in callers) or "(no direct caller)"))

        mod, lo, hi = CODE_WINDOW
        self.out(f"\n== The game's code {mod}+{lo:#x}..{hi:#x} (both calls, the wait at +0x1540434, what follows)")
        self.disasm(self.base[mod] + lo, self.base[mod] + hi)

        for pat, addr, name in wanted:
            f = self.function_at(addr)
            if not f:
                continue
            self.out(f"\n== The SDK's own {pat}: {name} at {self.rel(f[0])} ({(f[1] - f[0]) // 4} instructions)")
            self.disasm(f[0], f[1], FUNCTION_CAP)
        if not wanted:
            self.out("\n(no ListFrameHistory/GetLatestFrameNumber function symbol with an address: only imports)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("exefs", nargs="?")
    ap.add_argument("--out", type=Path, default=Path("frame_report.txt"))
    ap.add_argument("--sym", action="append", default=[], help="list the symbols whose name contains this text")
    ap.add_argument(
        "--func", action="append", default=[], help="disassemble the functions whose name contains this text"
    )
    ap.add_argument("--stack", type=Path, help="an orbitals.log with an abort's thread dump: name its game frames")
    ap.add_argument(
        "--thread",
        action="append",
        default=[],
        help="with --stack, the threads whose name contains this text (default: " + ", ".join(STACK_THREADS) + ")",
    )
    args = ap.parse_args()
    if len(sys.argv) == 1 and getattr(sys, "frozen", False):
        # double-clicked: an orbitals.log next to the executable or in logs\ is named, else the old report
        for c in (
            Path("orbitals.log"),
            Path("logs/orbitals.log"),
            Path(sys.argv[0]).resolve().parent / "logs/orbitals.log",
        ):
            if c.exists():
                args.stack = c
                break
    exefs = find_exefs(args.exefs)
    print(f"ExeFS: {exefs.resolve()}\nBuilding the binary database (about a minute)...")
    # The database file stays open under Windows until the process ends, so the temporary folder cannot always be removed.
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        dbpath = Path(tmp) / "bindb.sqlite"
        bindb.build(exefs, dbpath)
        db = sqlite3.connect(dbpath)
        rep = Report(db, exefs)
        print("\n----- report -----")
        if args.sym:
            rep.symbols(args.sym)
        if args.func:
            rep.functions(args.func)
        if args.stack:
            rep.stack(args.stack, args.thread or list(STACK_THREADS))
        if not args.sym and not args.func and not args.stack:
            rep.run()
        args.out.write_text("\n".join(rep.lines) + "\n", encoding="utf-8")
        db.close()
    print(f"\nReport written to {args.out.resolve()} ({len(rep.lines)} lines)")


if __name__ == "__main__":
    try:
        main()
    finally:
        if len(sys.argv) == 1 and getattr(sys, "frozen", False):
            input("Press Enter to close")
