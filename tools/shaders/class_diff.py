"""The instruction forms two dumped programs do not share: per program, each (mnemonic, encoding class, size or format
modifiers) and its count, then the forms whose counts differ. Only those names and counts are printed; the programs
stay on the player's machine.

Why: Orbitals' pixel programs 238181b4 and 9255edc1 are near twins (592 and 600 instructions, the same textures read
from the same constant words), yet running 238181b4 turns the whole level black and running 9255edc1 does not
(SKIP_PROGRAMS A/B, 18/11/2026). The forms only the bad one has point at the instruction the translator gets wrong.

Usage (from the folder that holds orbitals-together.exe and setup\\; forward slashes):
    setup\\python\\python.exe setup/src/tools/shaders/class_diff.py --dumped spirv-dump --keys BAD,GOOD [--spec FILE]
"""

import argparse
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sm86/translate"))
sys.path.insert(0, str(ROOT / "tools/shaders"))

from extract import sph_stage, trim  # noqa: E402
from program import Decoder  # noqa: E402

MODS = (
    "sz",
    "fmt",
    "dstfmt",
    "srcfmt",
    "paramA",
    "lodlc",
    "dc",
    "toff",
    "aoffi",
    "op",
    "mode",
    "rnd",
    "ftz",
    "sat",
    "cmp",
    "bop",
    "sem",
    "ipaop",
    "query",
    "comp",
    "scr",
    "f16rm",
    "ndv",
    "redop",
    "mufuop",
    "swz",
    "shfl",
    "fmz",
    "scale",
)


def header(sph) -> str:
    """The program header's summary: render targets written (RGBA bits per target), depth output, generic inputs used
    (attribute: components) and which are perspective-interpolated. No code, only the interface."""
    if sph is None or sph.stage != "ps":
        return "no pixel header"
    outs = " ".join(
        f"rt{t}={(sph.omap_targets >> (4 * t)) & 15:04b}" for t in range(8) if (sph.omap_targets >> (4 * t)) & 15
    )
    ins = []
    for a in range(32):
        comps = "".join(str(c) for c in range(4) if sph.imap_used(a, c))
        if comps:
            byte = (sph.words[6 + (a >> 2)] >> ((a & 3) * 8)) & 0xFF
            kinds = {(byte >> (2 * c)) & 3 for c in range(4) if sph.imap_used(a, c)}
            ins.append(f"a{a}:{comps}/{''.join(str(k) for k in sorted(kinds))}")
    return f"targets [{outs}] depth={int(sph.omap_depth)} inputs [{' '.join(ins)}]"


def reads(insns) -> set[str]:
    """Where the program reads from outside its registers: IPA attribute addresses and constant bank words."""
    out = set()
    for i in insns:
        if i.op == "IPA" and i.has("attr"):
            out.add(f"a[{i.v('attr') * 4:#x}].{i.sym('ipaop') or 'PASS'}")
        for k, iv in i.f.items():
            if k.startswith("ConstBankAddress"):
                (_, bank), (slot, addr) = iv.slot_values
                out.add(f"c[{bank}][{(addr * 4 if slot.width == 14 else addr):#x}]")
    return out


def forms(dec: Decoder, code: Path) -> tuple[Counter, str, set[str]]:
    raw = code.read_bytes()
    stage = sph_stage(raw) or "cs"
    sph, insns = dec.program(trim(raw, stage), stage)
    out = Counter()
    for i in insns:
        mods = ".".join(f"{m}={i.sym(m)}" for m in MODS if i.sym(m) and i.sym(m).lower() not in ("", "no" + m))
        out[f"{i.op} {i.cls}{' ' + mods if mods else ''}"] += 1
    return out, header(sph), reads(insns)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dumped", type=Path, default=Path.cwd() / "spirv-dump")
    ap.add_argument("--spec", type=Path, default=Path.cwd() / "sm86/instructions.json")
    ap.add_argument("--keys", required=True, help="BAD,GOOD: two program key prefixes")
    a = ap.parse_args()
    keys = [k.strip().lower() for k in a.keys.split(",") if k.strip()]
    if len(keys) != 2:
        print("--keys takes two key prefixes", file=sys.stderr)
        return 1
    dec = Decoder(a.spec)
    found = []
    for k in keys:
        code = next((c for c in sorted(a.dumped.glob("*.code")) if c.stem.startswith(k)), None)
        if code is None:
            print(f"no dumped program {k}* in {a.dumped}", file=sys.stderr)
            return 1
        found.append((k, *forms(dec, code)))
    (ka, fa, ha, ra), (kb, fb, hb, rb) = found
    print(f"{ka}: {ha}\n{kb}: {hb}")
    print(
        f"{ka}: {sum(fa.values())} instructions, {len(fa)} forms; {kb}: {sum(fb.values())} instructions, {len(fb)} forms"
    )
    print(f"forms whose counts differ ({ka} vs {kb}):")
    for f in sorted(set(fa) | set(fb), key=lambda f: (-(fa[f] - fb[f]), f)):
        if fa[f] != fb[f]:
            print(f"  {fa[f]:4} {fb[f]:4}  {f}")
    print(f"reads only {ka}: {' '.join(sorted(ra - rb)) or '-'}")
    print(f"reads only {kb}: {' '.join(sorted(rb - ra)) or '-'}")
    print(f"reads both: {len(ra & rb)} words; IPA in {ka}: {' '.join(sorted(r for r in ra if r.startswith('a[')))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
