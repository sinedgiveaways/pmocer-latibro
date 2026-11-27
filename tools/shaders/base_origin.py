"""Where the uniform base of the global accesses comes from, in the dumped programs the translator refuses with
"base not a traced storage buffer" (LDG/STG/ATOMG [Ra + URb]): for each such access, the instructions that last wrote
its base register before it, walked back a few steps, counted by shape (mnemonic, class and constant bank operands, no
other bits of the program). The counts say which origin glsl/mem.py global_addr should follow.

Why: 48 of the 65 programs a run of Orbitals could not translate (17/11/2026) fail there: their draws are skipped, and
the title screen, menu and character select lose their backgrounds. The programs are the player's (ORBITALS_SPIRV_DUMP)
and stay on their machine; only the counts are printed.

--all looks at every dumped program (a check of the tool); --keys k1,k2 details the texture handles of those
programs (each writer, each operand of an OR, jump targets in between); --tex counts instead where the texture handles of the
programs refused with "texture handle in a register not traced to a constant bank" come from.

Usage (Windows, from the folder that holds orbitals-together.exe and setup\\; forward slashes, as a "\\n" in a pasted
path breaks the line):
    setup\\python\\python.exe setup/src/tools/shaders/base_origin.py --dumped spirv-dump [--spec FILE] [--depth N]
--spec defaults to the installation's sm86/instructions.json (made by dumped_shaders.py).
"""

import argparse
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sm86/translate"))
sys.path.insert(0, str(ROOT / "tools/shaders"))

import glsl  # noqa: E402
from extract import sph_stage, trim  # noqa: E402
from program import Decoder  # noqa: E402

RZ, URZ = 255, 63
BASES = ("Ra_URb", "Ra_URc")


def consts(i) -> str:
    """The instruction's constant bank operands, c[bank][offset]."""
    out = []
    for k, iv in i.f.items():
        if k.startswith("ConstBankAddress"):
            (_, bank), (slot, addr) = iv.slot_values
            out.append(f"c[{bank:#x}][{addr * 4 if slot.width == 14 else addr:#x}]")   # 14 bits: words; 16: bytes
    return " ".join(out)


def writes(i, kind: str, n: int) -> bool:
    """i writes register n ("u" uniform, "r" general), a pair's high half included."""
    f = "URd" if kind == "u" else "Rd"
    if not i.has(f) or (kind == "r" and i.op in ("STG", "STS", "STL", "ST")):
        return False
    d = i.v(f)
    wide = any(s in i.cls for s in ("64", "128")) or i.sym("sz") in ("64", "128")
    return d == n or (wide and d + 1 == n) or (i.sym("sz") == "128" and d < n <= d + 3)


def origin(insns: list, at: int, kind: str, n: int, depth: int) -> list[str]:
    """The shapes of the writes behind register n before insns[at], newest first."""
    chain = []
    for _ in range(depth):
        j = next((k for k in range(at - 1, -1, -1) if writes(insns[k], kind, n)), None)
        if j is None:
            chain.append(f"(no write of {'UR' if kind == 'u' else 'R'} before: an input)")
            break
        w = insns[j]
        extra = f" lut {w.v('imm8'):#x}" if w.has("imm8") else ""
        if w.has("Rc"):
            extra += " Rc=RZ" if w.v("Rc") == RZ else " Rc=R"
        if w.v("Pg", 7) != 7 or w.v("Pg@not", 0):
            extra += " predicated"
        chain.append(f"{w.op} {w.cls} {consts(w)}{extra}".strip())
        nxt = next(((("u", w.v(r)) for r in ("URa", "URb", "URc") if w.has(r) and w.v(r) != URZ)), None) or next(
            (("r", w.v(r)) for r in ("Ra", "Rb", "Rc") if w.has(r) and w.v(r) != RZ), None
        )
        if nxt is None:
            break
        (kind, n), at = nxt, j
    return chain


def writer(insns: list, at: int, kind: str, n: int) -> int | None:
    """The index of the last instruction before insns[at] that writes register n, or None."""
    return next((k for k in range(at - 1, -1, -1) if writes(insns[k], kind, n)), None)


def guard(i) -> str:
    g = [f"P{i.v('Pg')}" if i.v("Pg", 7) != 7 or i.v("Pg@not", 0) else "",
         f"UP{i.v('UPg')}" if i.v("UPg", 7) != 7 or i.v("UPg@not", 0) else ""]
    g = " ".join(x for x in g if x)
    return f" @{'!' if i.v('Pg@not', 0) or i.v('UPg@not', 0) else ''}{g}" if g else ""


def detail(dec, dumped: Path, keys: list[str], depth: int) -> int:
    """--keys: each texture instruction with a register handle in those programs, with every writer of the handle and,
    for an OR (LOP3/ULOP3), the chain behind each operand; jump targets between a writer and the use are counted (a
    straight-line trace may then name the wrong write). Only shapes, pcs and constant offsets are printed."""
    for code in sorted(dumped.glob("*.code")):
        if not any(code.stem.startswith(k) for k in keys):
            continue
        raw = code.read_bytes()
        stage = sph_stage(raw) or "cs"
        _, insns = dec.program(trim(raw, stage), stage)
        targets = set()
        for i in insns:
            if i.op in ("BRA", "BRX", "JMP", "CALL", "BSSY", "BREAK", "SSY", "WARPSYNC", "BSYNC") and i.has("sImm"):
                targets.add(i.pc + 16 + 4 * i.sv("sImm"))   # as glsl/core.py: words of 4 bytes
        print(f"== {code.stem[:16]} ({stage}, {len(insns)} instructions, {len(targets)} jump targets)")
        for k, i in enumerate(insns):
            ur = next((f for f in ("URc", "URb") if "urc" in i.cls and i.has(f) and i.v(f) != URZ), None)
            if ur is None and ("noConst" not in i.cls or not i.has("Rb") or i.v("Rb") == RZ):
                continue
            kind, n = ("u", i.v(ur)) if ur else ("r", i.v("Rb"))
            w = writer(insns, k, kind, n)
            print(f"  {i.pc:#06x} {i.op} {i.cls}{guard(i)} handle {'UR' if kind == 'u' else 'R'}{n}")
            if w is None:
                print("      no writer before it")
                continue
            wi = insns[w]
            between = sum(1 for t in targets if wi.pc < t <= i.pc)
            print(f"      written at {wi.pc:#06x} by {wi.op} {wi.cls}{guard(wi)} {consts(wi)}"
                  f" lut {wi.v('imm8', 0):#x}; {between} jump targets between" if wi.has("imm8") else
                  f"      written at {wi.pc:#06x} by {wi.op} {wi.cls}{guard(wi)} {consts(wi)}; {between} jump targets between")
            if wi.op in ("LOP3", "ULOP3"):
                for f in (("URa", "URb", "URc") if wi.op == "ULOP3" else ("Ra", "Rb", "Rc")):
                    if not wi.has(f):
                        continue
                    r = wi.v(f)
                    zero = URZ if f.startswith("U") else RZ
                    if r == zero:
                        print(f"      {f}: zero register")
                        continue
                    chain = " <- ".join(origin(insns, w, "u" if f.startswith("U") else "r", r, depth))
                    print(f"      {f} = {'UR' if f.startswith('U') else 'R'}{r} <- {chain}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dumped", type=Path, required=True, help="the ORBITALS_SPIRV_DUMP folder of a run")
    ap.add_argument("--spec", type=Path, default=Path.cwd() / "sm86/instructions.json")
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--all", action="store_true", help="every dumped program, translated or not (a check of the tool)")
    ap.add_argument("--tex", action="store_true", help="texture handles in a register (tex.py tex_key) instead")
    ap.add_argument("--keys", default="", help="comma-separated program key prefixes: for each texture instruction "
                    "of those programs, the writers of its handle register and of each operand of an OR")
    a = ap.parse_args()
    if not a.spec.is_file():
        print(f"{a.spec} missing: run dumped_shaders.py once (it builds it), or give --spec", file=sys.stderr)
        return 1
    dec = Decoder(a.spec)
    keys = [k.strip().lower() for k in a.keys.split(",") if k.strip()]
    if keys:
        return detail(dec, a.dumped, keys, a.depth)
    shapes, progs, why = Counter(), Counter(), Counter()
    failing = 0
    for code in sorted(a.dumped.glob("*.code")):
        raw = code.read_bytes()
        stage = sph_stage(raw) or "cs"
        sph, insns = dec.program(trim(raw, stage), stage)
        ctl = code.with_suffix(".ctl")
        try:
            glsl.translate(sph, insns, stage, glsl.compute_shape(ctl.read_bytes()) if stage == "cs" else None, b"")
            if not a.all:
                continue
        except glsl.Unsupported as e:
            want = "texture handle in a register" if a.tex else "not a traced storage buffer"
            if want not in str(e) and not a.all:
                continue
            why[str(e)] += 1
        except Exception:   # noqa: BLE001 (a translator error is not this census's question)
            if not a.all:
                continue
        failing += 1
        seen = set()
        for k, i in enumerate(insns):
            if a.tex:   # texture instructions whose handle is a register: general (noConst) or uniform (_urc_)
                ur = next((f for f in ("URc", "URb") if "urc" in i.cls and i.has(f) and i.v(f) != URZ), None)
                if ur is None and ("noConst" not in i.cls or not i.has("Rb") or i.v("Rb") == RZ):
                    continue
                kind, reg = ("u", i.v(ur)) if ur else ("r", i.v("Rb"))
                chain = " <- ".join(origin(insns, k, kind, reg, a.depth))
                guard = " predicated" if i.v("Pg", 7) != 7 or i.v("Pg@not", 0) else ""
                shape = f"{i.op} {i.cls}{guard}: {'UR' if ur else 'R'} <- {chain}"
            else:
                base = next((i.v(f) for f in BASES if i.has(f)), None)
                if base is None or base == URZ:
                    continue
                chain = " <- ".join(origin(insns, k, "u", base, a.depth))
                shape = f"{i.op} {i.cls}: UR <- {chain}"
            shapes[shape] += 1
            if shape not in seen:
                seen.add(shape)
                progs[shape] += 1
    what = "texture handle" if a.tex else "base"
    print(f"{failing} dumped programs {'looked at' if a.all else f'fail on an untraced {what}'}")
    for w, c in why.most_common():
        print(f"  {c:4} {w}")
    print(f"{'register-handle texture instructions' if a.tex else 'uniform-base accesses'} in them, by the origin "
          f"of the {what} (instructions, programs):")
    for s, c in shapes.most_common(25):
        print(f"  {c:5} {progs[s]:4}  {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
