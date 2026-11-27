"""Read Unreal Engine IoStore containers (.utoc + .ucas) and write every GPU program of their shader libraries.

Why: Orbitals' GPU programs are not in Nintendo archives but in Unreal's shader libraries inside
Orbitals/Content/Paks/Orbitals-Switch2.utoc/.ucas, so tools/shaders/extract.py finds none and, before this, the runtime
translated programs while the player played (a black or wrong picture at each new place). With this the setup writes
all of them before the first start, as <key>.ctl and <key>.code: the files the runtime dumps for a program it lacks
(gfx_shader.c), so translate.py --dumped turns them into the SPIR-V the runtime looks up.

Measured on the 1.1.2.0 Deluxe container (utoc version 8, flags 9 = compressed + indexed, no encryption, no signature):
2 ShaderCodeLibrary chunks and 2748 ShaderCode chunks, stored in IoStore blocks of method None; each ShaderCode chunk
is one shader group, Oodle-compressed by Unreal itself, decoded by orbitals-together --oodle-batch (runtime/sdk/fs,
ooz). 23 432 programs: 3 556 vertex, 16 459 pixel, 3 415 compute, 2 geometry. The key of every one of the 820 programs
a run recorded in pipeline-keys.bin came out of these files (25/11/2026).

Library layout (FIoStoreShaderCodeArchiveHeader, version 1, as read here): u32 version, then arrays of u32 count +
items: shader map hashes (20 bytes), shader hashes (20), group chunk ids (12: FIoChunkId), shader map entries (8: u32
first index, u32 count into the index array), shader entries (8: bits 0-3 frequency, bits 34+ the offset of the
shader in its decompressed group), group entries (16: u32 first index, u32 count, u32 uncompressed size, u32
compressed size), shader indices (u32). In a group, a shader's record holds its name, then the NVN2 code (the size at
control+0x44), a u32, and the 0xd8-byte control section (magic 34 12 76 98).

Written for this project from Unreal's public API documentation and measurements; no code taken from another project.

Usage:
  uv run tools/shaders/iostore.py info     Orbitals-Switch2.utoc
  uv run tools/shaders/iostore.py pack     Orbitals-Switch2.utoc --out build/shader-chunks.zip
  uv run tools/shaders/iostore.py programs DIR_OR_UTOC... --exe orbitals-together(.exe) --out setup-work/unreal-programs
`pack` writes the raw shader chunks and index.tsv (for analysis); `programs` takes .utoc files or folders searched for
them. Their output holds the game's data: it stays on the player's machine and is never committed (AGENTS.md).
"""

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"-==--==--==--==-"
HEADER_SIZE = 144
CHUNK_TYPES = {  # EIoChunkType5
    0: "Invalid",
    1: "ExportBundleData",
    2: "BulkData",
    3: "OptionalBulkData",
    4: "MemoryMappedBulkData",
    5: "ScriptObjects",
    6: "ContainerHeader",
    7: "ExternalFile",
    8: "ShaderCodeLibrary",
    9: "ShaderCode",
    10: "PackageStoreEntry",
    11: "DerivedData",
    12: "EditorDerivedData",
}
SHADER_TYPES = (8, 9)
FLAG_COMPRESSED, FLAG_ENCRYPTED, FLAG_SIGNED, FLAG_INDEXED = 1, 2, 4, 8
BLOCK_ALIGN = 16


class IoStoreError(ValueError):
    pass


@dataclass
class Chunk:
    index: int
    chunk_id: int  # the 64-bit id
    chunk_index: int  # the 16-bit index
    type: int
    offset: int  # in the container's uncompressed address space
    length: int


@dataclass
class Block:
    offset: int  # in the .ucas
    compressed: int
    uncompressed: int
    method: int  # 0 = none, else 1 + the position in Toc.methods


class Toc:
    def __init__(self, path: Path):
        self.path = Path(path)
        d = self.path.read_bytes()
        if d[:16] != MAGIC:
            raise IoStoreError(f"{path}: not an IoStore .utoc (magic {d[:16]!r})")
        (
            self.version,
            _,
            _,
            header_size,
            n_entries,
            n_blocks,
            block_entry_size,
            n_methods,
            method_len,
            self.block_size,
            self.dir_index_size,
            self.partitions,
        ) = struct.unpack_from("<BBHIIIIIIIII", d, 16)
        (self.flags,) = struct.unpack_from("<B", d, 16 + 64)
        (n_seeds,) = struct.unpack_from("<I", d, 16 + 68)
        (self.partition_size,) = struct.unpack_from("<Q", d, 16 + 72)
        (n_no_hash,) = struct.unpack_from("<I", d, 16 + 80)
        if header_size != HEADER_SIZE or block_entry_size != 12:
            raise IoStoreError(f"{path}: unexpected header size {header_size} or block entry size {block_entry_size}")
        o = header_size
        raw_ids = []
        for i in range(n_entries):
            chunk_id, chunk_index, _, ctype = struct.unpack_from("<QHBB", d, o + i * 12)
            raw_ids.append((chunk_id, chunk_index, ctype))
        o += n_entries * 12
        self.chunks: list[Chunk] = []
        for i, (chunk_id, chunk_index, ctype) in enumerate(raw_ids):
            r = d[o + i * 10 : o + i * 10 + 10]  # two 40-bit big-endian numbers
            self.chunks.append(
                Chunk(i, chunk_id, chunk_index, ctype, int.from_bytes(r[:5], "big"), int.from_bytes(r[5:], "big"))
            )
        o += n_entries * 10 + n_seeds * 4 + n_no_hash * 4
        self.blocks: list[Block] = []
        for i in range(n_blocks):
            r = d[o + i * 12 : o + i * 12 + 12]
            self.blocks.append(
                Block(
                    int.from_bytes(r[0:5], "little"),
                    int.from_bytes(r[5:8], "little"),
                    int.from_bytes(r[8:11], "little"),
                    r[11],
                )
            )
        o += n_blocks * 12
        self.methods = [
            d[o + i * method_len : o + (i + 1) * method_len].rstrip(b"\0").decode() for i in range(n_methods)
        ]
        if self.flags & FLAG_ENCRYPTED:
            raise IoStoreError(f"{path}: encrypted container (needs the AES key; not supported)")

    @property
    def ucas(self) -> Path:
        return self.path.with_suffix(".ucas")

    def method_name(self, block: Block) -> str:
        return (
            "None"
            if block.method == 0
            else (self.methods[block.method - 1] if block.method <= len(self.methods) else "?")
        )

    def read(self, chunk: Chunk, ucas) -> bytes:
        """The chunk's bytes. Blocks of a method other than None raise IoStoreError."""
        first = chunk.offset // self.block_size
        last = (chunk.offset + chunk.length - 1) // self.block_size if chunk.length else first - 1
        out = bytearray()
        for k in range(first, last + 1):
            b = self.blocks[k]
            if b.method != 0:
                raise IoStoreError(f"chunk {chunk.index}: block {k} is {self.method_name(b)}-compressed")
            ucas.seek(b.offset)
            data = ucas.read(b.compressed)
            if len(data) != b.compressed:
                raise IoStoreError(f"chunk {chunk.index}: block {k} runs past the end of {self.ucas.name}")
            out += data
        start = chunk.offset - first * self.block_size
        return bytes(out[start : start + chunk.length])


CONTROL_MAGIC = b"\x34\x12\x76\x98"
CONTROL_SIZE = 0xD8
CODE_SIZE_AT = 0x44  # u32 in the control section: code bytes, SPH included
SPH_SIZE = 0x80
NOP_LOW16 = 0x7918  # low 16 bits of the SM86 NOP encoding (padding)
FREQUENCIES = {0: "vs", 3: "ps", 4: "gs", 5: "cs"}  # EShaderFrequency values seen in Orbitals' libraries


@dataclass
class Library:
    version: int
    group_ids: list[tuple[int, int, int]]  # (chunk id, chunk index, chunk type) of each group
    shader_entries: list[int]
    group_entries: list[tuple[int, int, int, int]]  # (first index, count, uncompressed, compressed)
    indices: list[int]

    def shaders_of(self, group: int) -> list[tuple[int, int]]:
        """(offset in the decompressed group, shader index) of the group's shaders, in offset order."""
        first, count, _, _ = self.group_entries[group]
        return sorted((self.shader_entries[i] >> 34, i) for i in self.indices[first : first + count])


def parse_library(d: bytes) -> Library:
    o = 0

    def array(size: int) -> bytes:
        nonlocal o
        (n,) = struct.unpack_from("<I", d, o)
        o += 4
        if o + n * size > len(d):
            raise IoStoreError(f"shader library: array of {n} x {size} bytes past the end")
        a = d[o : o + n * size]
        o += n * size
        return a

    (version,) = struct.unpack_from("<I", d, 0)
    o = 4
    if version != 1:
        raise IoStoreError(f"shader library version {version}, only 1 is known")
    array(20)  # shader map hashes
    array(20)  # shader hashes
    gid = array(12)
    array(8)  # shader map entries
    sent = array(8)
    gent = array(16)
    idx = array(4)
    if o != len(d):
        raise IoStoreError(f"shader library: {len(d) - o} bytes after the last array")
    return Library(
        version,
        [(a, b, t) for a, b, _, t in struct.iter_unpack("<QHBB", gid)],
        [x for (x,) in struct.iter_unpack("<Q", sent)],
        list(struct.iter_unpack("<IIII", gent)),
        [x for (x,) in struct.iter_unpack("<I", idx)],
    )


def sph_stage(code: bytes) -> bool:
    """True when code starts with a Maxwell-style program header (type 1 VTG / 2 PS, version 4)."""
    if len(code) < 4:
        return False
    (w,) = struct.unpack_from("<I", code)
    return w & 0x1F in (1, 2) and (w >> 5) & 0x1F == 4


def trimmed(code: bytes) -> bytes:
    """The code the runtime keys on (nvn2.c program_length_scan): up to the first all-zero 16-byte word, or to a
    program header right after NOP padding."""
    start = SPH_SIZE if sph_stage(code) else 0
    for at in range(start, len(code) - 15, 16):
        if code[at : at + 16] == bytes(16):
            return code[:at]
        if at > start and int.from_bytes(code[at - 16 : at - 14], "little") == NOP_LOW16 and sph_stage(code[at:]):
            return code[:at]
    return code


def program_key(ctl: bytes, code: bytes) -> str:
    """nvn2_program_key (runtime/gfx/nvn/procs/nvn2.c): sha256 of the control section and the trimmed code."""
    return hashlib.sha256(ctl + trimmed(code)).hexdigest()


def split_record(rec: bytes) -> tuple[bytes, bytes] | None:
    """(control, code) of one shader record: the first control magic whose code (size at control+0x44) fits before
    it with the u32 between them; None when there is none."""
    at = rec.find(CONTROL_MAGIC)
    while at >= 0:
        ctl = rec[at : at + CONTROL_SIZE]
        if len(ctl) == CONTROL_SIZE:
            (size,) = struct.unpack_from("<I", ctl, CODE_SIZE_AT)
            if 0 < size <= at - 4:
                return ctl, rec[at - 4 - size : at - 4]
        at = rec.find(CONTROL_MAGIC, at + 1)
    return None


def oodle_batch(exe: Path, streams: list[tuple[int, bytes]], work: Path) -> list[bytes | None]:
    """Decompress (raw size, stream) pairs with orbitals-together --oodle-batch (runtime/sdk/fs/oodle_batch.c)."""
    src, dst = work / "oodle-in.bin", work / "oodle-out.bin"
    with open(src, "wb") as f:
        for raw, data in streams:
            f.write(struct.pack("<II", raw, len(data)))
            f.write(data)
    r = subprocess.run([str(exe.absolute()), "--oodle-batch", str(src), str(dst)], capture_output=True, text=True)
    if r.returncode:
        raise IoStoreError(f"{exe.name} --oodle-batch failed ({r.returncode}): {r.stderr.strip() or r.stdout.strip()}")
    out: list[bytes | None] = []
    with open(dst, "rb") as f:
        for raw, _ in streams:
            (status,) = struct.unpack("<i", f.read(4))
            out.append(f.read(raw) if status >= 0 else None)
    src.unlink()
    dst.unlink()
    return out


def programs(utocs: list[Path], exe: Path, out: Path) -> dict[str, int]:
    """Write <key>.ctl / <key>.code for every program of the containers' shader libraries; returns counts."""
    out.mkdir(parents=True, exist_ok=True)
    n = Counter()
    for utoc in utocs:
        toc = Toc(utoc)
        libs = [c for c in toc.chunks if c.type == 8]
        if not libs:
            continue
        by_id = {(c.chunk_id, c.chunk_index, c.type): c for c in toc.chunks}
        with open(toc.ucas, "rb") as ucas, tempfile.TemporaryDirectory(dir=out) as tmp:
            for lc in libs:
                lib = parse_library(toc.read(lc, ucas))
                streams, stored = [], {}
                for g, (gid, (_, _, raw, comp)) in enumerate(zip(lib.group_ids, lib.group_entries)):
                    chunk = by_id.get(gid)
                    if chunk is None or chunk.length != comp:
                        n["groups missing"] += 1
                        continue
                    data = toc.read(chunk, ucas)
                    if raw == comp:  # stored: Unreal keeps a group uncompressed when compression does not pay
                        stored[g] = data
                    else:
                        streams.append((g, raw, data))
                decoded = oodle_batch(exe, [(raw, data) for _, raw, data in streams], Path(tmp))
                groups = dict(stored)
                for (g, _, _), data in zip(streams, decoded):
                    if data is None:
                        n["groups failed"] += 1
                    else:
                        groups[g] = data
                for g, data in groups.items():
                    n["groups"] += 1
                    shaders = lib.shaders_of(g)
                    ends = [off for off, _ in shaders[1:]] + [len(data)]
                    for (off, s), end in zip(shaders, ends):
                        parts = split_record(data[off:end])
                        if parts is None:
                            n["records without a control section"] += 1
                            continue
                        ctl, code = parts
                        key = program_key(ctl, code)
                        n[FREQUENCIES.get(lib.shader_entries[s] & 0xF, "other")] += 1
                        if (out / f"{key}.code").is_file():
                            n["already there"] += 1
                            continue
                        (out / f"{key}.ctl").write_bytes(ctl)
                        (out / f"{key}.code").write_bytes(code)
                        n["written"] += 1
    return dict(n)


def find_utocs(paths: list[Path]) -> list[Path]:
    found: list[Path] = []
    for p in paths:
        found += sorted(p.rglob("*.utoc")) if p.is_dir() else [p]
    return found


def cmd_info(toc: Toc) -> None:
    print(
        f"{toc.path.name}: version {toc.version}, flags {toc.flags:#x}, block size {toc.block_size}, "
        f"{len(toc.chunks)} chunks, {len(toc.blocks)} blocks, methods {toc.methods}"
    )
    print("chunks by type:", {CHUNK_TYPES.get(t, t): n for t, n in sorted(Counter(c.type for c in toc.chunks).items())})
    print("blocks by method:", dict(Counter(toc.method_name(b) for b in toc.blocks)))
    shader = [c for c in toc.chunks if c.type in SHADER_TYPES]
    print(f"shader chunks: {len(shader)}, {sum(c.length for c in shader)} bytes")


def cmd_pack(toc: Toc, out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    index = ["entry\ttype\tchunk_id\tchunk_index\tsize"]
    n = size = 0
    with open(toc.ucas, "rb") as ucas, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for c in toc.chunks:
            if c.type not in SHADER_TYPES:
                continue
            data = toc.read(c, ucas)
            name = f"{c.type}-{c.index:05d}-{c.chunk_id:016x}.bin"
            z.writestr(name, data)
            index.append(f"{name}\t{CHUNK_TYPES[c.type]}\t{c.chunk_id:016x}\t{c.chunk_index}\t{c.length}")
            n += 1
            size += c.length
        z.writestr("index.tsv", "\n".join(index) + "\n")
    print(f"{n} shader chunks, {size} bytes, written to {out}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=["info", "pack", "programs"])
    ap.add_argument("utoc", type=Path, nargs="+", help="a .utoc (programs: .utoc files or folders searched for them)")
    ap.add_argument("--out", type=Path, help="pack: the zip (build/shader-chunks.zip); programs: the folder")
    ap.add_argument("--exe", type=Path, help="programs: orbitals-together(.exe), whose --oodle-batch decodes the groups")
    a = ap.parse_args()
    try:
        if a.command == "programs":
            if not a.exe or not a.out:
                ap.error("programs needs --exe and --out")
            counts = programs(find_utocs(a.utoc), a.exe, a.out)
            print("programs: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
            return 0 if counts.get("groups") else 1
        toc = Toc(a.utoc[0])
        cmd_info(toc) if a.command == "info" else cmd_pack(toc, a.out or Path("build/shader-chunks.zip"))
    except (IoStoreError, OSError) as e:
        print(f"iostore: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
