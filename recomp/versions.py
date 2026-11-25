"""The game builds this port knows, keyed by the build id of exefs/main.

Why: each build of a game is a different executable (other addresses, another sdk build), so everything tied to one
build (the patch sites, the RomFS size the setup checks, the number the runtime chooses its row of fixed values by) is
chosen here, from the dump itself, in one place. The runtime keeps the matching row in runtime/core/boot/version.c,
keyed by `number`.

Usage: from versions import detect; v = detect(exefs)  ->  Version, or ValueError for a build not listed.
       uv run recomp/versions.py [EXEFS]   prints the version of a dump.
"""

import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = "Orbitals"


@dataclass(frozen=True)
class Version:
    name: str  # what the port shows for this build (the start screen, the setup's log and keys)
    number: int  # RtGame.version (runtime/core/boot/gamelib.h): the row of runtime/core/boot/version.c; 0: no row
    main_build_id: str  # first 8 bytes of the NSO build id of exefs/main, hex
    romfs_files: int  # files of its RomFS, counted on a dump; 0: not counted, no check
    patches: Path | None  # recomp/patches-*.txt for this executable; None: setup finds the spin loops
    # setup steps that do not apply to this build: the warm-up runs the game to a known scene, which this build has not
    # (tools/prepare/prepare.py)
    skip_steps: tuple[str, ...] = ("warm",)
    # where the setup's shader step finds the programs: "iostore", Unreal's shader libraries in the RomFS's .utoc/.ucas
    # (tools/shaders/iostore.py; Orbitals since 25/11/2026), or "agl", BNSH and agl archives (tools/shaders/extract.py)
    shader_source: str = "iostore"
    program_ids: tuple[str, ...] = ()  # base, update, add-on (main.npdm ACI0 and the .nup's packages), hex
    base_version: int = 0  # the base package's version number
    nintendo_version: int = 0  # the update's version number (a build number, not a display version)
    module_build_ids: dict[str, str] = field(default_factory=dict, hash=False)  # first 8 bytes of each module's NSO build id
    module_mib: dict[str, float] = field(default_factory=dict, hash=False)  # each module's image size, MiB, as port_check rounds it


# Read from the .nup by tools/analisis/port_check.py (port_check.md, 16/11/2026).
VERSIONS = (
    Version(
        name="0.2.0",
        number=1,
        main_build_id="d6efcc0a51c1c792",
        romfs_files=0,
        patches=ROOT / "recomp" / "patches-v1.txt",
        program_ids=("040032B023CA4000", "040032B023CA4800", "040032B023CA5001"),
        base_version=65536,
        nintendo_version=327680,
        module_build_ids={"main": "d6efcc0a51c1c792", "rtld": "f9060b30c6fa2b69", "sdk": "daa31832ca979cdf",
                          "subsdk0": "caf9437bacb2aa16"},  # fmt: skip
        module_mib={"main": 157.8, "rtld": 0.1, "sdk": 16.4, "subsdk0": 0.8},
    ),
)


def build_id_of(head: bytes, where: str = "exefs/main") -> str:
    """The build id of exefs/main from its first 0x48 bytes (a folder's file or a .nup package's entry)."""
    if head[:4] != b"NSO0":
        raise ValueError(f"{where} is not an NSO: decrypted exefs needed")
    return head[0x40:0x48].hex()


def main_build_id(exefs: Path) -> str:
    return build_id_of((Path(exefs) / "main").read_bytes()[:0x48], str(Path(exefs) / "main"))


def detect(exefs: Path) -> Version:
    return detect_id(main_build_id(exefs))


def detect_id(got: str) -> Version:
    for v in VERSIONS:
        if v.main_build_id == got:
            return v
    names = ", ".join(v.name for v in VERSIONS)
    raise ValueError(f"exefs/main is not a known build of {GAME} ({names}; build id {got})")


def unknown(got: str) -> Version:
    """A build not listed (another game, or another build of this one), set up from its own exefs: number 0, no RomFS
    count to check, and no checked-in patch table (setup finds its spin loops, tools/prepare/prepare.py spin_table).
    The runtime runs it with no fixed values (runtime/core/boot/version.c)."""
    return Version(f"port-{got}", 0, got, 0, None)


def detect_any(exefs: Path) -> Version:
    """detect, or unknown() for a build not listed."""
    got = main_build_id(exefs)
    try:
        return detect_id(got)
    except ValueError:
        return unknown(got)


def modules(exefs: Path) -> list[str]:
    """The game modules present in the ExeFS, in load order (recomp/layout.py ORDER)."""
    sys.path.insert(0, str(ROOT / "recomp"))
    from layout import ORDER

    return [m for m in ORDER if (Path(exefs) / m).is_file()]


if __name__ == "__main__":
    exefs = Path(sys.argv[1] if len(sys.argv) > 1 else "game/exefs")
    v = detect(exefs)
    patches = v.patches.name if v.patches else "found at setup"
    print(f"{GAME} {v.name} (version {v.number}), modules {', '.join(modules(exefs))}, patches {patches}")
