"""Translate the programs a run dumped (ORBITALS_SPIRV_DUMP=DIR) into the installation's spirv/ with the package's tools.

Why: Orbitals' shaders are in Unreal .utoc containers. Since 25/11/2026 the setup's shader step translates all of them
(tools/shaders/iostore.py); this script stays for the programs a run still finds missing (ones the game builds at run
time, or an installation set up before that change). The game hands its programs to
nvnProgramSetShaders at run time; a run with ORBITALS_SPIRV_DUMP writes each one it could not find as <key>.ctl and
<key>.code, and translate.py --dumped turns them into <key>.spv and <key>.res.json, the names the runtime looks up. This
script does what the shaders step does (the SM86 spec from NVIDIA's nvdisasm, then translate.py), with the dumped programs
as the only input. Run it again after each run that dumped more: what is translated stays.

Usage (Windows, from the folder that holds orbitals-together.exe and setup\\):
    setup\\python\\python.exe setup\\src\\tools\\prepare\\dumped_shaders.py --dumped spirv-dump
    [--game DIR] [--jobs N]
--game is the installation (default: the current folder). Nothing of the game goes anywhere but the installation.
"""

import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src
sys.path.insert(0, str(ROOT / "tools/build"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from machine import Failed, default_cc, default_glslc, run, shader_jobs, tool_env  # noqa: E402
from shaderstep import sm86_spec  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dumped", type=Path, required=True, help="the ORBITALS_SPIRV_DUMP folder of a run")
    ap.add_argument("--game", type=Path, default=Path.cwd(), help="the installation (default: the current folder)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel translations (default: as the setup)")
    args = ap.parse_args()
    game, dumped = args.game.absolute(), args.dumped.absolute()
    codes = sorted(dumped.glob("*.code"))
    if not codes:
        print(f"no <key>.code in {dumped}: run the game with ORBITALS_SPIRV_DUMP={dumped} first", file=sys.stderr)
        return 1
    print(f"{len(codes)} dumped programs in {dumped}")
    work = game / "setup-work"
    empty = work / "shaders-none"  # translate.py reads an extraction's index.csv: an empty one, the dump is the input
    empty.mkdir(parents=True, exist_ok=True)
    (empty / "index.csv").write_text("sha1,stage,code_bytes,source,program\n")
    target = "windows" if sys.platform == "win32" else "linux"
    cc = default_cc(game, target)
    glslc = default_glslc(game)
    t0 = time.monotonic()
    print("the SM86 spec (built once from NVIDIA's nvdisasm, a download on the first run)...")
    spec = sm86_spec(game, work, cc)
    print(f"spec: {spec} ({time.monotonic() - t0:.0f} s)")
    jobs = args.jobs or shader_jobs()
    run([sys.executable, str(ROOT / "tools/shaders/translate.py"), "--shaders", str(empty), "--dumped", str(dumped),
         "--dumped-only", "--out", str(game / "spirv"), "--glslc", glslc, "--no-validate", "--progress",
         "--jobs", str(jobs), "--spec", str(spec)], env=tool_env(glslc), progress_prefix="progress")  # fmt: skip
    for p in (game / "spirv").glob("*.glsl"):  # the runtime reads .spv and .res.json; the GLSL only made them
        p.unlink()
    spv = len(list((game / "spirv").glob("*.spv")))
    print(f"done in {time.monotonic() - t0:.0f} s: {spv} SPIR-V files in {game / 'spirv'}; failures in "
          f"{game / 'spirv' / 'failed.tsv'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failed as e:
        print(f"failed: {e}", file=sys.stderr)
        sys.exit(1)
