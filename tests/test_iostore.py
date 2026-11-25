"""tools/shaders/iostore.py on a synthetic Unreal shader library: the library's arrays, a shader's place in its group
and the program key the runtime computes (runtime/gfx/nvn/procs/nvn2.c nvn2_program_key). No game data: the bytes are
made up with the layout measured on Orbitals' containers (25/11/2026)."""

import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/shaders"))
import iostore  # noqa: E402


def control(code_size: int) -> bytes:
    ctl = bytearray(iostore.CONTROL_SIZE)
    ctl[0:4] = iostore.CONTROL_MAGIC
    struct.pack_into("<I", ctl, iostore.CODE_SIZE_AT, code_size)
    return bytes(ctl)


def record(name: bytes, code: bytes) -> bytes:
    """A shader's record in its group: name, code, a u32, the control section, a tail."""
    return name + b"\0" + code + struct.pack("<I", 0xE0) + control(len(code)) + b"\x01\x02tail"


def pixel_code() -> bytes:
    sph = bytearray(iostore.SPH_SIZE)
    struct.pack_into("<I", sph, 0, 2 | 4 << 5)  # type 2 (pixel), version 4
    return bytes(sph) + b"\x11" * 32 + bytes(16) + b"\x22" * 16  # two instructions, then the zero word that ends it


def library(groups: list[list[tuple[int, int]]], raw: list[int]) -> bytes:
    """groups: per group, (offset, frequency) of its shaders."""
    shaders = [s for g in groups for s in g]
    out = struct.pack("<I", 1)
    out += struct.pack("<I", 1) + bytes(20)  # one shader map
    out += struct.pack("<I", len(shaders)) + bytes(20 * len(shaders))
    out += struct.pack("<I", len(groups)) + b"".join(
        struct.pack("<QHBB", 0x1000 + g, g, 0, 9) for g in range(len(groups))
    )
    out += struct.pack("<I", 1) + struct.pack("<II", 0, len(shaders))
    out += struct.pack("<I", len(shaders)) + b"".join(struct.pack("<Q", off << 34 | f) for off, f in shaders)
    entries, first = b"", 0
    for g, shs in enumerate(groups):
        entries += struct.pack("<IIII", first, len(shs), raw[g], raw[g])
        first += len(shs)
    out += struct.pack("<I", len(groups)) + entries
    out += struct.pack("<I", len(shaders)) + b"".join(struct.pack("<I", i) for i in reversed(range(len(shaders))))
    return out


def test_library_records_and_key():
    code = pixel_code()
    rec_a, rec_b = record(b"Global/FShaderA", code), record(b"Global/FShaderB", code[:-16] + b"\x33" * 16)
    group = rec_a + rec_b
    lib = iostore.parse_library(library([[(0, 3), (len(rec_a), 5)]], [len(group)]))
    assert lib.group_ids == [(0x1000, 0, 9)]
    shaders = lib.shaders_of(0)
    assert [off for off, _ in shaders] == [0, len(rec_a)]
    ctl, got = iostore.split_record(group[: len(rec_a)])
    assert got == code and ctl == control(len(code))
    # the key covers the control section and the code up to its first all-zero word, as the runtime trims it
    want = hashlib.sha256(ctl + code[: iostore.SPH_SIZE + 32]).hexdigest()
    assert iostore.program_key(ctl, got) == want


def test_split_record_skips_a_magic_inside_the_code():
    code = bytes(16) + iostore.CONTROL_MAGIC + bytes(60)  # the magic inside the code is not the control section
    ctl, got = iostore.split_record(record(b"x", code))
    assert got == code and ctl[:4] == iostore.CONTROL_MAGIC


def test_compute_code_has_no_header():
    code = b"\x44" * 32 + bytes(16)
    assert iostore.trimmed(code) == b"\x44" * 32
