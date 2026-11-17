"""Scan a folder, a file or a built executable for names of the project this one descends from.

Why: the product (sources, docs, packages, executables) must not carry the old game's or project's name. A text search
on a binary misses UTF-16 strings (Windows version resources, manifests), so every file is read as bytes and searched
both as UTF-8 and as UTF-16LE.

Names looked for (case-insensitive): MKW, MarioKart, Mario Kart, MKWorld, mkw-recomp, adriadam10. In a binary the
three-letter "mkw" counts only when no letter touches it, to avoid hits inside compressed data.

In an executable each match names its PE section and file offset. The bare three letters "mkw" inside compressed image
data (the IDAT chunks of an embedded PNG: the icon), inside a section of machine code (.text: instruction bytes) or in a
table of addresses (at least 8 of the 16 bytes around it are above 0x7F) are not text: they are listed apart and do not
count. The longer names count everywhere.

Paths listed in the allow file (default tools/build/check_names.allow, globs relative to the scanned root) may keep the
names: the licence texts, the third-party notices, the removal report and the one-shot migration data.

Usage: uv run tools/build/check_names.py PATH [PATH ...] [--allow FILE] [--limit N] [--strict]
       Report only by default; --strict exits 1 when something outside the allow file matches.
"""

import argparse
import fnmatch
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKIP_DIRS = {".git", "__pycache__", ".venv", "node_modules"}

# What a file name may not contain.
NAMES = ("mario kart", "mariokart", "mkworld", "mkw-recomp", "mkw_recomp", "adriadam10")
TEXT_RE = re.compile("|".join(re.escape(n) for n in NAMES) + "|mkw", re.I)


def variants(binary: bool) -> list[re.Pattern]:
    """Byte patterns: UTF-8 and UTF-16LE spellings of every name."""
    out = []
    plain = list(NAMES) + (["libmkw"] if binary else [])
    for n in plain:
        out.append(re.compile(re.escape(n.encode()), re.I))
        out.append(re.compile(re.escape(n.encode("utf-16le")), re.I))
    mkw8 = rb"(?<![A-Za-z])mkw(?![A-Za-z])" if binary else rb"mkw"
    mkw16 = (rb"(?<![A-Za-z]\x00)m\x00k\x00w\x00(?![A-Za-z]\x00)" if binary else rb"m\x00k\x00w\x00")
    out.append(re.compile(mkw8, re.I))
    out.append(re.compile(mkw16, re.I))
    return out


def load_allow(path: Path | None) -> list[str]:
    if path is None or not path.is_file():
        return []
    lines = [l.strip() for l in path.read_text().splitlines()]
    return [l for l in lines if l and not l.startswith("#")]


def allowed(rel: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(rel, p) or fnmatch.fnmatchcase(Path(rel).name, p) for p in patterns)


def files_of(root: Path):
    if root.is_file():
        yield root, root.name
        return
    for p in sorted(root.rglob("*")):
        if any(part in SKIP_DIRS for part in p.relative_to(root).parts):
            continue
        if p.is_file() and not p.is_symlink():
            yield p, p.relative_to(root).as_posix()


def pe_sections(data: bytes) -> list[tuple[str, int, int, bool]]:
    """(name, file offset, size, holds code) of each section of a PE file; [] for anything else."""
    if data[:2] != b"MZ" or len(data) < 0x40:
        return []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        return []
    count, opt = struct.unpack_from("<H12xH", data, pe + 6)
    out, at = [], pe + 24 + opt
    for i in range(count):
        name, size, off = struct.unpack_from("<8s8xII", data, at + 40 * i)
        flags = struct.unpack_from("<I", data, at + 40 * i + 36)[0]
        code = bool(flags & 0x20000020)  # IMAGE_SCN_CNT_CODE or IMAGE_SCN_MEM_EXECUTE
        out.append((name.rstrip(b"\0").decode("ascii", "replace"), off, size, code))
    return out


def png_image_data(data: bytes) -> list[tuple[int, int]]:
    """[start, end) of the data of every IDAT chunk of the PNG files embedded in data."""
    out = []
    for m in re.finditer(rb"\x89PNG\r\n\x1a\n", data):
        at = m.end()
        while at + 12 <= len(data):
            n, kind = struct.unpack_from(">I4s", data, at)
            if not kind.isalpha() or at + 12 + n > len(data):
                break
            if kind == b"IDAT":
                out.append((at + 8, at + 8 + n))
            at += 12 + n
            if kind == b"IEND":
                break
    return out


def where(off: int, sections: list[tuple[str, int, int, bool]]) -> tuple[str, bool]:
    """The section and offset of a file offset, and whether that section holds machine code."""
    for name, start, size, code in sections:
        if start <= off < start + size:
            return f"{name}+{off - start:#x}", code
    return f"file+{off:#x}", False


def scan(path: Path) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """(kind, snippet) of every match in the file, and apart the bare "mkw" found in image data or machine code."""
    data = path.read_bytes()
    binary = b"\x00" in data[:8192] and not data[:2] in (b"\xff\xfe", b"\xfe\xff")
    sections = pe_sections(data) if binary else []
    images = png_image_data(data) if binary else []
    hits, in_images = [], []
    for rx in variants(binary):
        bare = rx.pattern in (rb"(?<![A-Za-z])mkw(?![A-Za-z])", rb"(?<![A-Za-z]\x00)m\x00k\x00w\x00(?![A-Za-z]\x00)")
        for m in rx.finditer(data):
            a = max(0, m.start() - 20)
            snip = data[a : m.end() + 20]
            wide = b"\x00" in m.group(0)
            snip = snip.decode("utf-16le" if wide else "utf-8", "replace")
            place, code = where(m.start(), sections) if sections else ("", False)
            kind = ("utf-16" if wide else "utf-8") + (f" {place}" if place else "")
            image = bare and any(lo <= m.start() < hi for lo, hi in images)
            code = bare and code and not image
            # A table of addresses or offsets (a debug or unwind section) holds "mkw" by chance: the bytes around it are
            # mostly above 0x7F, which text in any section of this build never is.
            around = data[max(0, m.start() - 8) : m.end() + 8]
            table = bare and not wide and not image and not code and sum(b >= 0x80 for b in around) >= 8
            note = " png data" if image else " code bytes" if code else " binary table" if table else ""
            (in_images if image or code or table else hits).append((kind + note, " ".join(snip.split())))
    return hits, in_images


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--allow", type=Path, default=HERE / "check_names.allow")
    ap.add_argument("--limit", type=int, default=3, help="snippets shown per file")
    ap.add_argument("--strict", action="store_true", help="exit 1 when a file outside the allow file matches")
    args = ap.parse_args()
    patterns = load_allow(args.allow)

    bad = kept = image_bytes = 0
    for root in args.paths:
        if not root.exists():
            print(f"check_names: {root} does not exist", file=sys.stderr)
            return 2
        # Filenames are checked too.
        for p, rel in files_of(root):
            ok = allowed(rel, patterns)
            hits, in_images = scan(p)
            for kind, snip in in_images:
                print(f"{rel}: not text, not counted [{kind}] {snip[:80]!r}")
            image_bytes += len(in_images)
            if TEXT_RE.search(rel):
                hits.insert(0, ("filename", rel))
            if not hits:
                continue
            if ok:
                kept += len(hits)
                continue
            bad += len(hits)
            print(f"{rel}: {len(hits)} match(es)")
            for kind, snip in hits[: args.limit]:
                print(f"    [{kind}] {snip[:110]}")
    print(f"check_names: {bad} match(es) outside the allow file, {kept} inside it, {image_bytes} in image data or code")
    return 1 if (bad and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
