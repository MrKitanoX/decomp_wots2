#!/usr/bin/env python3
"""Makes the Windows release package: the programs, their DLLs, the mods and
the translation patch — and nothing of the game.

    package_windows.py <build dir> <output dir> [--zip] [--msys C:/msys64/ucrt64/bin]

The player's disc is unpacked by WotS2-Instalador.exe on their own machine.
"""
import argparse
import pathlib
import shutil
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DLLS = ["SDL2.dll", "libarchive-13.dll", "libb2-1.dll", "libbz2-1.dll", "libiconv-2.dll", "liblz4.dll",
        "liblzma-5.dll", "libwinpthread-1.dll", "libz-1.dll", "libzstd.dll"]
MODS = ["fps_unlock", "lighting", "resolution", "widescreen"]
# Never in a package: the disc's files (or anything made from them).
FORBIDDEN = {"slus_208.93", "volume.dat", "system.cnf"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--msys", type=pathlib.Path, default=pathlib.Path("C:/msys64/ucrt64/bin"))
    args = ap.parse_args()

    out = args.out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    def copy(src, rel):
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    copy(args.build / "wots2.exe", "wots2.exe")
    copy(args.build / "WotS2-Instalador.exe", "WotS2-Instalador.exe")
    for dll in DLLS:
        copy(args.msys / dll, dll)
    for mod in MODS:
        copy(ROOT / "mods" / mod / f"{mod}.dll", f"mods/{mod}/{mod}.dll")
        copy(ROOT / "mods" / mod / "mod.ini", f"mods/{mod}/mod.ini")
    copy(ROOT / "mods" / "README.md", "mods/README.md")
    copy(ROOT / "mods" / "ptbr" / "mod.ini", "mods/ptbr/mod.ini")
    copy(ROOT / "mods" / "ptbr" / "ptbr.wpatch", "mods/ptbr/ptbr.wpatch")
    copy(ROOT / "packaging" / "LEIA-ME-windows.txt", "LEIA-ME.txt")
    copy(ROOT / "packaging" / "README-windows.txt", "README-english.txt")

    for p in out.rglob("*"):
        if p.name.lower() in FORBIDDEN or p.suffix.lower() in (".iso", ".pss", ".trs"):
            raise SystemExit(f"refusing to package game data: {p}")

    total = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"{out}: {sum(1 for p in out.rglob('*') if p.is_file())} files, {total / 1e6:.1f} MB")
    if args.zip:
        archive = out.with_suffix(".zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in sorted(out.rglob("*")):
                if p.is_file():
                    z.write(p, pathlib.Path(out.name) / p.relative_to(out))
        print(f"{archive}: {archive.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    sys.exit(main())
