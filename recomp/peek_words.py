# Print the raw instruction words of a module around main-relative offsets, in the recomp/patches-v1.txt
# word format (8 lowercase hex digits, little-endian of the .text dword). Read-only: it lifts nothing and
# patches nothing, it just reads the extracted NSO so a patch site can be stated with the game's own words.
#
# Usage (from the repo root, against the extracted ExeFS the generator uses):
#   python recomp/peek_words.py --exefs build/extracted/v121/exefs main 0x1f9800 0x1f9a00
#   python recomp/peek_words.py --exefs DIR main 0xb9200 0xb9260 0x181cc0 0x181d00
# The last arguments are pairs of [start, end) main-relative offsets; default window 0x40 past the first.
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))

from nso import Nso  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description='print .text words of an extracted NSO, patches-v1 format')
    ap.add_argument('--exefs', required=True, help='extracted ExeFS directory (main, sdk, ...)')
    ap.add_argument('module', help='module name: main, sdk, subsdk0, ...')
    ap.add_argument('spans', nargs='+', help='[start end] pairs of main-relative offsets, 0x-prefixed')
    args = ap.parse_args()

    path = Path(args.exefs) / args.module
    if path.is_dir():
        path = path / args.module  # some layouts keep main/main.nso
    if not path.exists():
        print(f'no such module file: {path}', file=sys.stderr)
        return 1

    nso = Nso(str(path))
    img = nso.image
    t_off, _, t_size = nso.text
    if len(args.spans) % 2:
        args.spans.append(hex(int(args.spans[-1], 16) + 0x40))
    for i in range(0, len(args.spans), 2):
        start, end = int(args.spans[i], 16), int(args.spans[i + 1], 16)
        start &= ~3
        end = min(end, t_size)
        for a in range(start, end, 4):
            w = int.from_bytes(img[t_off + a:t_off + a + 4], 'little')
            print(f'main+{a:#x} {w:08x}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
