"""Both Orbitals null-sub-table guards (recomp/patches-v1.txt) must keep a valid base and replace only a null one.

Why: rec-null-guard shipped with `asr` instead of `lsr` and a stray `lsl #8` (fixed 25/11/2026): with a valid base the
address was rewritten and the record walker read the page before its array. The sequences are modelled here word by
word, so a wrong shift or mask fails before a 10-minute game build.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "recomp"))
import patches  # noqa: E402

M64 = (1 << 64) - 1
MODS = [("main", 0x7000000000, 0x08000000), ("sdk", 0x7100000000, 0x01000000)]


def table():
    return patches.load(ROOT / "recomp" / "patches-v1.txt", MODS)


def site(name: str, off: int):
    t = table()
    index = next(i for i, s in enumerate(t.sets) if s[0] == name)
    patch_set, replacement, original = t.sites["main"][off]
    assert patch_set == index
    return t, replacement, original


def mask_from(base: int) -> int:
    """neg, orr, lsr #63, sub #1 as the words encode them: 0 for a valid base, all ones for a null one."""
    x = ((-base) & M64) | base
    return ((x >> 63) - 1) & M64


def test_rec_null_guard_words_and_behaviour():
    _, repl, original = site("rec-null-guard", 0x175A030)
    assert original == 0xB9405F29  # ldr w9, [x25, #0x5c]
    assert repl == [
        0xCB0903ED,  # neg x13, x9
        0xAA0901AD,  # orr x13, x13, x9
        0xD37FFDAD,  # lsr x13, x13, #63 (937ffdad was asr)
        0xD10005AD,  # sub x13, x13, #1
        0xD101736E,  # sub x14, x27, #0x5c
        0x8A2D0330,  # bic x16, x25, x13
        0x8A0D01D1,  # and x17, x14, x13
        0xAA100239,  # orr x25, x17, x16 (aa102239 had lsl #8)
        0xB9405F29,
    ]
    x25, x27 = 0xAAAA1234, 0xFFC1F0000
    for base, want in ((0x70026B6C0, x25), (0, (x27 - 0x5C) & M64)):
        x13 = mask_from(base)
        assert ((x27 - 0x5C) & M64 & x13) | (x25 & ~x13 & M64) == want


def test_vector_guard_words_and_behaviour():
    t, repl, original = site("rec-null-guard-vector", 0x175DBEC)
    assert original == 0xF9400008  # ldr x8, [x0]
    assert repl == [
        0xF9400008,
        0xCB0803EE,  # neg x14, x8
        0xAA0801CE,  # orr x14, x14, x8
        0xD37FFDCE,  # lsr x14, x14, #63
        0xD10005CE,  # sub x14, x14, #1
        ("page", 15),
        0x8A0E01EF,  # and x15, x15, x14
        0xAA0F0108,  # orr x8, x8, x15
    ]
    page = 0x7009DD0000
    for base, want in ((0x118A44610, 0x118A44610), (0, page)):
        assert base | (page & mask_from(base)) == want
    assert all(patches.branch_target(w, 0x7000000000 + 0x175DBEC) is None for w in repl if isinstance(w, int))
    emitted = t.for_module("main", 0x7000000000)[0x7000000000 + 0x175DBEC]
    assert emitted[1][5] == patches.adrp(0x7000000000 + 0x175DBEC, t.page, 15)
