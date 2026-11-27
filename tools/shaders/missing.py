"""The dumped programs that have no SPIR-V in the installation, by stage and with the translator's reason: which
compute programs the game runs without (their dispatches are skipped). Only hashes, stages and reasons are printed.

Why: in Orbitals' first level every draw that places its objects from Unreal's GPU scene buffers (storage buffers
addressed from c3) loses all its triangles to clipping, with or without the VRAM mirror (17-18/11/2026). Those
buffers are filled by compute dispatches; one of them missing would leave the scene zero.

Usage (from the folder that holds orbitals-together.exe and setup\\; forward slashes):
    setup\\python\\python.exe setup/src/tools/shaders/missing.py [--dumped spirv-dump] [--spirv spirv] [--log logs/orbitals.log]
--log also marks the programs that run looked up and found missing ("no prepared SPIR-V").
"""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/shaders"))

from extract import sph_stage  # noqa: E402


def reasons(failed: Path) -> dict[str, str]:
    """{key: reason} from the translator's failed.tsv (a reason, a tab, the keys it refused, space-separated)."""
    out: dict[str, str] = {}
    if failed.is_file():
        for line in failed.read_text(errors="replace").splitlines():
            why, _, keys = line.partition("\t")
            for k in keys.split():
                out[k] = why
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dumped", type=Path, default=Path.cwd() / "spirv-dump")
    ap.add_argument("--spirv", type=Path, default=Path.cwd() / "spirv")
    ap.add_argument("--log", type=Path, help="an orbitals.log: mark the programs its run missed")
    a = ap.parse_args()
    why = reasons(a.spirv / "failed.tsv")
    looked = set()
    if a.log and a.log.is_file():
        looked = set(re.findall(r"no prepared SPIR-V ([0-9a-f]{64})", a.log.read_text(errors="replace")))
    total, missing, by_reason = Counter(), Counter(), Counter()
    rows = []
    for code in sorted(a.dumped.glob("*.code")):
        key = code.stem
        stage = sph_stage(code.read_bytes()[:4]) or "cs"
        total[stage] += 1
        if (a.spirv / f"{key}.spv").is_file():
            continue
        missing[stage] += 1
        r = why.get(key, "not translated yet (run dumped_shaders.py)")
        by_reason[(stage, r)] += 1
        rows.append((stage, key, r, key in looked))
    print("dumped programs by stage:", dict(total))
    print("without SPIR-V by stage:", dict(missing))
    for (st, r), n in sorted(by_reason.items()):
        print(f"  {st:4} {n:3}  {r}")
    print("each one (stage, key, met by the logged run, reason):")
    for st, key, r, met in sorted(rows):
        print(f"  {st:4} {key[:16]} {'run' if met else '   '} {r}")
    if looked:
        gone = looked - {k.stem for k in a.dumped.glob('*.code')}
        if gone:
            print(f"{len(gone)} programs the run missed are not in {a.dumped} (dumping off?)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
