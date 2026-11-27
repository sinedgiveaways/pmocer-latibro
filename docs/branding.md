# Branding: the executable's icon

`orbitals-together.exe` carries the icon of Orbitals, as a Windows `.ico` of 16, 32, 48 and 256 pixels. Its version
information (`FileDescription`, `ProductName`, `OriginalFilename`, `ProductVersion`) is in `runtime/platform/orbitals-together.rc`.

## Source picture

| | |
|---|---|
| Source | SteamGridDB, `https://cdn2.steamgriddb.com/thumb/1973ddb8a87e4da50befa772a5a1474d.png` |
| File | `assets/icon/orbitals_logo.png`, 419 x 196 px, 8-bit RGBA, 136,278 bytes |
| SHA-256 | `daefe63212606dc82a4ad0412883d1a2c95e549de8cb9bd8009719908001e13a` |
| Content | Promotional picture of the game (two characters, the title in Chinese and "ORBITALS"), wider than tall |
| Rights | Stated by the project author to be free of rights. The picture is not committed, see below |

The picture is not square, so `make_ico.py` cuts a square of the picture's height (196 px) around its middle, which
holds the title, and scales it to 256 px with Lanczos resampling. The smaller sizes are scaled from that square.

## Regenerating

```sh
curl -fsSL -o assets/icon/orbitals_logo.png "https://cdn2.steamgriddb.com/thumb/1973ddb8a87e4da50befa772a5a1474d.png"
uv run tools/build/make_ico.py                 # assets/icon/orbitals.ico, Pillow comes from the script's own header
uv run tools/build/make_ico.py --crop left     # another cut: center (default), left, right or X,Y,SIZE
uv run tools/build/make_ico.py --src my.png --out my.ico
```

`tools/build/build.py --target windows` copies `assets/icon/orbitals.ico` into the resource when it exists. Without it
the icon `tools/build/icon.py` draws is used. The workflow `.github/workflows/build-windows.yml` downloads the picture,
checks its SHA-256 and runs `make_ico.py` before building.

## Replacing the icon

Put any PNG at `assets/icon/orbitals_logo.png` (a square one needs no `--crop`) and run `make_ico.py`. No code changes.
To change the version information, edit `runtime/platform/orbitals-together.rc` (renamed with the other artifacts later in the plan,
`docs/mkw-removal-plan.md`).

## Why the picture and the `.ico` are not committed

`.gitignore` lists both. The picture is the game's own promotional art, and the project rule is that nothing of the game
goes in the repository. The CI fetches it by URL and hash instead. To version them, delete the two lines under
"Icon" in `.gitignore` and `git add -f` the files; nothing else changes.

## Not covered yet

The window's icon (`tools/build/icon.py`, `app_icon.c`) and the Linux AppImage icon are still the drawn one; only the
Windows executable's file icon uses this picture.
