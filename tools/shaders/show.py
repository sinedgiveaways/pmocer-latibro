"""Prints dumped programs of one NVN stage (control+0x56; geometry by default) as decoded instructions: program header
words, then per instruction its address, mnemonic, encoding class and the decoded fields. It prints the game's shader
code, so paste its output into a chat or an issue comment only, never into git or a release.

Why: Orbitals draws its colour-grading LUT (32 layers of a 3D texture) with a geometry stage the runtime did not run;
the translator has no geometry support, and writing it needs the instructions those programs use (18/11/2026).

Usage (from the folder that holds orbitals-together.exe and setup\\; forward slashes):
    setup\\python\\python.exe setup/src/tools/shaders/show.py [--dumped spirv-dump] [--stage 2] [--keys PREFIX,...]
Stages: 0 vertex, 1 fragment, 2 geometry, 3 tessellation control, 4 tessellation evaluation, 5 compute.
Identical programs (the same code) are printed once, with the keys that share it.
"""

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sm86/translate"))
sys.path.insert(0, str(ROOT / "tools/shaders"))

from extract import sph_stage, trim  # noqa: E402
from program import Decoder  # noqa: E402

NOISE = ("__", "TABLES_", "VarLat", "Opcode", "req_bit_set", "PixMask", "e_desc", "input_reg_sz")


def fields(i) -> str:
    """The fields that say what the instruction does: scheduling, opcode and default modifiers left out; a value with a
    symbol shown as the symbol (R5, UR4, P0, FMZ)."""
    out = []
    for name, iv in i.f.items():
        if name.startswith(NOISE):
            continue
        r = iv.render().split(" <- ")[0]
        if "(" in r and r.endswith(")"):
            r = r[r.index("(") + 1 : -1].split("|")[0]
        if name.endswith(("@not", "@absolute", "@negate")) and r == "0":
            continue
        if name in ("Pg", "UPg", "Pp", "Pu") and r in ("PT", "UPT", "P7"):
            continue
        if r.startswith("no") or r.startswith("INVALID") or (name == "pm_pred" and r == "PMN"):
            continue
        out.append(f"{name}={r}")
    return " ".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dumped", type=Path, default=Path.cwd() / "spirv-dump")
    ap.add_argument("--spec", type=Path, default=Path.cwd() / "sm86/instructions.json")
    ap.add_argument("--stage", type=int, default=2)
    ap.add_argument("--keys", help="only programs whose key starts with one of these prefixes")
    a = ap.parse_args()
    prefixes = [k.strip().lower() for k in a.keys.split(",")] if a.keys else None
    dec = Decoder(a.spec)
    groups: dict[bytes, list[str]] = {}
    for ctl in sorted(a.dumped.glob("*.ctl")):
        if prefixes and not any(ctl.stem.startswith(p) for p in prefixes):
            continue
        c = ctl.read_bytes()
        code = ctl.with_suffix(".code")
        if len(c) < 0x58 or c[0x56] != a.stage or not code.is_file():
            continue
        raw = code.read_bytes()
        groups.setdefault(trim(raw, sph_stage(raw) or "cs"), []).append(ctl.stem)
    if not groups:
        print(f"no dumped program of stage {a.stage} in {a.dumped}")
        return 1
    for body, keys in groups.items():
        stage = sph_stage(body) or "cs"
        print(f"== {len(keys)} program(s): {' '.join(k[:16] for k in keys)}; header stage {stage}, {len(body)} bytes")
        sph, insns = dec.program(body, stage)
        if sph is not None:
            words = struct.unpack_from("<32I", body)
            print("SPH " + " ".join(f"{w:08x}" for w in words))
        for i in insns:
            print(f"  {i.pc:04x} {i.op} [{i.cls}] {fields(i)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
