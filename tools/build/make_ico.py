# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=10"]
# ///
"""Makes the Windows icon of orbitals-together.exe from a PNG: assets/icon/orbitals_logo.png -> assets/icon/orbitals.ico.

Why: the executable's icon (runtime/platform/*.rc, embedded by tools/build/build.py) is an .ico with the sizes Windows
asks for (16, 32, 48 and 256 pixels). The source picture is not square, so a square is cut from it first (--crop) and
scaled with Lanczos resampling; each size is resampled from the crop, not from the previous size.

Usage: uv run tools/build/make_ico.py [--src PNG] [--out ICO] [--crop center | left | right | X,Y,SIZE]
       center (default): the square of the picture's height around its middle; left/right: against that edge;
       X,Y,SIZE: that square in pixels of the source.
"""

import argparse
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SIZES = (16, 32, 48, 256)


def square(img: Image.Image, crop: str) -> Image.Image:
    w, h = img.size
    if crop in ("center", "left", "right"):
        side = min(w, h)
        x = {"center": (w - side) // 2, "left": 0, "right": w - side}[crop]
        y = (h - side) // 2
    else:
        try:
            x, y, side = (int(v) for v in crop.split(","))
        except ValueError:
            raise SystemExit(f"--crop: center, left, right or X,Y,SIZE, not {crop!r}")
        if x < 0 or y < 0 or side < 1 or x + side > w or y + side > h:
            raise SystemExit(f"--crop {crop} is outside the {w}x{h} picture")
    return img.crop((x, y, x + side, y + side))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", type=Path, default=ROOT / "assets/icon/orbitals_logo.png")
    ap.add_argument("--out", type=Path, default=ROOT / "assets/icon/orbitals.ico")
    ap.add_argument("--crop", default="center")
    args = ap.parse_args()
    if not args.src.is_file():
        print(f"make_ico: {args.src} does not exist", file=sys.stderr)
        return 1
    sq = square(Image.open(args.src).convert("RGBA"), args.crop)
    big = sq.resize((256, 256), Image.Resampling.LANCZOS)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Pillow's ICO writer scales `big` to every size in `sizes` itself (Lanczos on the 256 px image).
    big.save(args.out, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"make_ico: {args.out} ({', '.join(str(s) for s in SIZES)} px) from {args.src.name}, crop {args.crop}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
