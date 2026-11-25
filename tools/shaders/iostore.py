"""Read Unreal Engine IoStore containers (.utoc + .ucas) and pull out the shader chunks.

Why: Orbitals' GPU programs are not in Nintendo archives but in Unreal's shader libraries inside
Orbitals/Content/Paks/Orbitals-Switch2.utoc/.ucas, so tools/shaders/extract.py finds none and the runtime has to
translate programs while the player plays. Reading the libraries at setup is the first step to translating them all
before the first start. Measured on the 1.1.2.0 container (utoc version 8, flags 9 = compressed + indexed, no
encryption, no signature): 2 ShaderCodeLibrary chunks (1.6 MB) and 2748 ShaderCode chunks (66 MB), all stored in
blocks of method 0 (not compressed by the IoStore). Everything else is mostly Oodle; this reader refuses Oodle blocks
rather than guessing, so it never needs the proprietary library.

The format follows Unreal's public IoStore documentation (FIoStoreTocHeader, FIoStoreTocCompressedBlockEntry,
FIoOffsetAndLength); written for this project, no code taken from another project.

Usage:
  uv run tools/shaders/iostore.py info  Orbitals-Switch2.utoc
  uv run tools/shaders/iostore.py pack  Orbitals-Switch2.utoc --out build/shader-chunks.zip
`pack` writes the ShaderCodeLibrary and ShaderCode chunks (read from the .ucas next to the .utoc) and index.tsv
(entry, chunk type, chunk id, size). The archive holds the game's data: it stays on the player's machine and is never
committed (AGENTS.md); sent for analysis it goes to the analysis-data release only.
"""

import argparse
import struct
import sys
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
    ap.add_argument("command", choices=["info", "pack"])
    ap.add_argument("utoc", type=Path)
    ap.add_argument("--out", type=Path, default=Path("build/shader-chunks.zip"))
    a = ap.parse_args()
    try:
        toc = Toc(a.utoc)
        cmd_info(toc) if a.command == "info" else cmd_pack(toc, a.out)
    except (IoStoreError, OSError) as e:
        print(f"iostore: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
