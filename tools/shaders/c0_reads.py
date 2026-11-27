"""Which words of the driver bank c0 the dumped programs read at fixed addresses, by stage: every constant bank operand
c[0x0][x] below --hi, counted per program. Only addresses and counts are printed; the programs stay on the player's
machine.

Why: Orbitals pushes 48 bytes of graphics push constants per draw (nvnCommandBufferPushGraphicsConstants, stage 0
offset 0) and the runtime drops them, while its compute pushes are read from c0 at 0x10 (vk_exec_bind.c). Whether the
graphics programs read c0 0x10-0x3f says where those bytes belong (17/11/2026: the colony drew black but for its
emissive parts).

Usage (from the folder that holds orbitals-together.exe and setup\\; forward slashes):
    setup\\python\\python.exe setup/src/tools/shaders/c0_reads.py --dumped spirv-dump [--spec FILE] [--hi 0x110]
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dumped", type=Path, required=True, help="the ORBITALS_SPIRV_DUMP folder of a run")
    ap.add_argument("--spec", type=Path, default=Path.cwd() / "sm86/instructions.json")
    ap.add_argument("--hi", type=lambda s: int(s, 0), default=0x110, help="count c0 words below this byte offset")
    ap.add_argument("--banks", action="store_true", help="instead: per stage and bank, programs and offset range read")
    a = ap.parse_args()
    if not a.spec.is_file():
        print(f"{a.spec} missing: run dumped_shaders.py once (it builds it), or give --spec", file=sys.stderr)
        return 1
    dec = Decoder(a.spec)
    words, progs, total = Counter(), Counter(), Counter()
    banks, lo, hi = Counter(), {}, {}   # --banks: (stage, bank) -> programs; offset range
    for code in sorted(a.dumped.glob("*.code")):
        raw = code.read_bytes()
        stage = sph_stage(raw) or "cs"
        _, insns = dec.program(trim(raw, stage), stage)
        total[stage] += 1
        seen = set()
        for i in insns:
            for k, iv in i.f.items():
                if not k.startswith("ConstBankAddress"):
                    continue
                (_, bank), (slot, addr) = iv.slot_values
                byte = addr * 4 if slot.width == 14 else addr
                if a.banks:
                    seen.add((bank, byte))
                elif bank == 0 and byte < a.hi:
                    seen.add(byte)
        if a.banks:
            for bank in {b for b, _ in seen}:
                banks[(stage, bank)] += 1
            for bank, byte in seen:
                lo[(stage, bank)] = min(lo.get((stage, bank), byte), byte)
                hi[(stage, bank)] = max(hi.get((stage, bank), byte), byte)
            continue
        for byte in seen:
            words[(stage, byte)] += 1
        if seen:
            progs[stage] += 1
    print("programs by stage:", dict(total))
    if a.banks:
        for (st, bank), n in sorted(banks.items()):
            print(f"  {st:4} c[{bank:#x}]  {n} programs, offsets {lo[(st, bank)]:#x}..{hi[(st, bank)]:#x}")
        return 0
    print(f"programs reading c0 below {a.hi:#x} at fixed addresses:", dict(progs))
    for (st, byte), n in sorted(words.items()):
        print(f"  {st:4} c0[{byte:#06x}]  {n} programs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
