"""Regression check for the second Orbitals empty-sub-table guard site."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "recomp"))
import patches  # noqa: E402


def test_vector_subtable_guard_patch_is_branchless_and_uses_zero_page():
    mods = [
        ("main", 0x7000000000, 0x08000000),
        ("sdk", 0x7100000000, 0x01000000),
    ]
    table = patches.load(ROOT / "recomp" / "patches-v1.txt", mods)

    set_index = next(
        i for i, item in enumerate(table.sets) if item[0] == "rec-null-guard-vector"
    )
    assert table.sets[set_index] == (
        "rec-null-guard-vector",
        "ORBITALS_PATCH_REC_NULL_GUARD_VECTOR",
        1,
    )

    patch_set, replacement, original = table.sites["main"][0x175DBEC]
    assert patch_set == set_index
    assert original == 0xF9400008
    assert replacement == [
        0xF9400008,  # ldr x8, [x0]
        0xCB0803EE,  # neg x14, x8
        0xAA0801CE,  # orr x14, x14, x8
        0xD37FFDCE,  # lsr x14, x14, #63
        0xD10005CE,  # sub x14, x14, #1
        ("page", 15),
        0x8A0E01EF,  # and x15, x15, x14
        0xAA0F0108,  # orr x8, x8, x15
    ]
    assert all(
        patches.branch_target(word, 0x7000000000 + 0x175DBEC) is None
        for word in replacement
        if isinstance(word, int)
    )

    absolute_site = 0x7000000000 + 0x175DBEC
    emitted = table.for_module("main", 0x7000000000)[absolute_site]
    assert emitted[0] == set_index
    assert emitted[1][5] == patches.adrp(absolute_site, table.page, 15)
