#!/usr/bin/env python3
"""Package shared GRAPHICS.PAK icons for Wine shell classes."""

import argparse
import io
import struct
import subprocess
import tempfile
from pathlib import Path
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
ICONS = (
    ("Folder", 1),
    ("FolderOpen", 2),  # Wine increments this index for GIL_OPENICON.
    ("Computer", 3),
    ("FileText", 4),
    ("WindowsExplorer", 5),
    ("ReaderCd", 6),
    ("ReaderDisket", 7),
)


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    pak = (ROOT / 'assets/graphics/GRAPHICS.PAK').read_bytes()
    magic, version, count = struct.unpack_from('<4sII', pak)
    if magic != b'BKGP' or version != 1:
        raise ValueError('Invalid shared graphics package')
    images = {}
    wanted = {prefix + item[0] for prefix in ('', '16/', '32/') for item in ICONS}
    for index in range(count):
        name, w, h, kind, depth, offset, size = struct.unpack_from('<48sHHHHII', pak, 12+64*index)
        name = name.split(b'\0', 1)[0].decode('ascii')
        if name in wanted:
            images[name] = Image.open(io.BytesIO(pak[offset:offset+size])).convert('RGBA')
    with tempfile.TemporaryDirectory(prefix="bles-retro-icons-") as directory:
        work = Path(directory)
        for name in sorted({name for name, _ in ICONS}):
            sizes = [(n, n) for n in (16, 32, 48, 64)]
            variants = [images['16/'+name], images['32/'+name]]
            variants += [images[name].resize(size, Image.Resampling.NEAREST) for size in sizes[2:]]
            variants[-1].save(work / f'{name}.ICO', sizes=sizes, append_images=variants[:-1])
        (work / "retro.rc").write_text(
            "\n".join(f'{index} ICON "{name}.ICO"'
                      for name, index in ICONS) + "\n")
        run("i686-w64-mingw32-windres", "-i", "retro.rc", "-o", "retro.o",
            cwd=work)
        run("i686-w64-mingw32-gcc", "-shared", "-Wl,--no-insert-timestamp",
            "-o", str(output), "retro.o", cwd=work)
    print(f"[WINE] classic icon resources: {output}")


if __name__ == "__main__":
    main()
