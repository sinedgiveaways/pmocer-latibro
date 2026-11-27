"""Where the texture and storage-buffer handles of dumped programs come from: the instructions before each texture access
whose handle is in a register, and before each global access whose base is a uniform register, with the writes of
those registers marked.

Why: translate.py fails Orbitals' programs with "texture handle in a register not traced to a constant bank" (56) and
"base not a traced storage buffer" (24); the translator follows only a ULDC of a constant word (mem.py uldc). Which
instructions make those registers says what to follow next. Prints decoded instructions only (op, class and fields),
no game code goes anywhere.

Usage: python tools/shaders/handle_trace.py DUMP_DIR [--spec FILE] [--programs N] [--before N] [--out FILE]
       Windows, from the folder of orbitals-together.exe:
       setup\\python\\python.exe setup\\src\\tools\\shaders\\handle_trace.py spirv-dump --spec sm86\\instructions.json
Reads <key>.code of the programs translate.py listed in DUMP_DIR/../spirv/failed.tsv when it exists, else all of them.
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sm86/translate"))
sys.path.insert(0, str(ROOT / "tools/shaders"))
from extract import sph_stage  # noqa: E402
from program import Decoder  # noqa: E402

REG_FIELDS = ("URc", "URb", "Ra_URb", "Ra_URc")


def stage_of(code: bytes) -> str:
    return sph_stage(code) or "cs"   # translate.py's rule


def fields(i) -> str:
    out = []
    for k, iv in i.f.items():
        syms = f"={'/'.join(iv.symbols)}" if getattr(iv, "symbols", None) else ""
        out.append(f"{k}:{iv.value}{syms}")
    return " ".join(out)


def writes_ur(i, n: int) -> bool:   # a 64-bit write (ULDC.64, UMOV.64...) covers n and n + 1
    return i.has("URd") and i.v("URd") in (n, n - 1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", type=Path)
    ap.add_argument("--spec", type=Path, default=ROOT / "build/sm86/instructions.json")
    ap.add_argument("--programs", type=int, default=6, help="programs to show (default 6)")
    ap.add_argument("--before", type=int, default=30, help="instructions shown before each access (default 30)")
    ap.add_argument("--out", type=Path, default=Path("handle_trace.txt"))
    args = ap.parse_args()
    failed = args.dump.parent / "spirv" / "failed.tsv"
    keys = []
    if failed.is_file():
        for line in failed.read_text().splitlines():
            reason, *ks = line.split("\t")
            if "not traced" in reason:
                keys += [(reason, k) for k in " ".join(ks).split()]
    else:
        keys = [("", p.stem) for p in sorted(args.dump.glob("*.code"))]
    dec = Decoder(args.spec)
    lines: list[str] = []
    shown_per_reason: dict[str, int] = {}
    for reason, key in keys:
        if shown_per_reason.get(reason, 0) >= args.programs:
            continue
        p = args.dump / f"{key}.code"
        if not p.is_file():
            continue
        code = p.read_bytes()
        _, insns = dec.program(code, stage_of(code))
        hits = []
        for n, i in enumerate(insns):
            regs = [(f, i.v(f)) for f in REG_FIELDS if i.has(f)]
            is_tex = i.cls.startswith(("tex", "tld", "tmml", "txq", "sust", "suld", "suatom", "sured")) and not i.has("bank5")
            is_glob = "_uniform_" in i.cls or i.cls.startswith(("ldg_uniform", "stg_uniform"))
            if regs and (is_tex or is_glob):
                hits.append((n, i, regs))
        if not hits:
            continue
        shown_per_reason[reason] = shown_per_reason.get(reason, 0) + 1
        lines.append(f"==== {key} ({reason or 'all'}): {len(insns)} instructions, {len(hits)} accesses")
        for n, i, regs in hits[:2]:
            want = {v for _, v in regs}
            lines.append(f"-- access at pc {i.pc:#x}: {i.op} [{i.cls}] registers {regs}")
            for j in insns[max(0, n - args.before) : n + 1]:
                mark = "W" if any(writes_ur(j, r) for r in want) else " "
                lines.append(f" {mark} {j.pc:#06x} {j.op:10} [{j.cls}] {fields(j)}")
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(lines)} lines written to {args.out.resolve()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
