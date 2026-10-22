#!/usr/bin/env python3
"""Create the FAT fixture containing the standalone extra programs."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


PROGRAMS = (
    ("CODEX.O", "codex"),
    ("HYPERZIP.O", "hyperzip"),
    ("LEXINET.O", "lexinet"),
    ("CIV2.O", "civ2"),
)


def run(*command: str) -> None:
    subprocess.run(command, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build a FAT image containing standalone BlesKernOS programs."
    )
    parser.add_argument("image", type=Path)
    for _, arg_name in PROGRAMS:
        parser.add_argument(arg_name, type=Path, nargs="?")
    args = parser.parse_args()

    for command in ("mkfs.fat", "mcopy", "mmd"):
        if shutil.which(command) is None:
            raise SystemExit(f"Missing {command}; install the mtools and dosfstools packages.")

    root = Path(__file__).resolve().parents[1]
    supplied = [getattr(args, arg_name) for _,arg_name in PROGRAMS]
    if any(supplied):
        if not all(supplied):parser.error('supply all four legacy inputs, or none for the LBS manifest')
        sources = {destination:getattr(args,arg_name) for destination,arg_name in PROGRAMS}
        for destination, source in list(sources.items()):
            output=root/'build/lbs/extras'/destination
            run('python3',str(root/'tools/generate_lbs_client.py'),
                '--source',str(source.resolve()),'--output',str(output),
                '--destination','/SYSTEM/PROGRAMS/'+destination)
            sources[destination]=output
    else:
        import json
        manifest=json.loads((root/'sdk/lbs/extra_applications.json').read_text())
        sources={name:root/'build/lbs/extras'/name for name in manifest}
    for destination,source in sources.items():
        if not source.is_file():raise SystemExit(f'Missing {source}; run make lbs-extras')

    image = args.image.resolve()
    image.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(dir=image.parent, suffix=".img")
    os.close(fd)
    temporary_image = Path(temporary_name)
    temporary_image.unlink()

    try:
        freeciv_data=root/'extras/Freeciv-BlesKernOS-Standalone/data'
        include_freeciv='FREECIV_CLIENT.O' in sources and freeciv_data.is_dir()
        size_kib='1048576' if 'QUAKE2.BEX' in sources else ('524288' if include_freeciv else '65536')
        run("mkfs.fat", "-C", "-F", "32" if include_freeciv else "16", "-n", "BKEXTRAS", str(temporary_image), size_kib)
        run("mmd", "-i", str(temporary_image), "::/SYSTEM")
        run("mmd", "-i", str(temporary_image), "::/SYSTEM/PROGRAMS")
        # Install the complete native runtime so this data disk is self-contained
        # when copied into an OS with the matching native ABI.
        for library in sorted((root/'build/system/libs/native').glob('*.LBS')):
            run('mcopy', '-o', '-i', str(temporary_image), str(library), '::/SYSTEM/'+library.name)
        for destination in sources:
            run("mcopy", "-o", "-i", str(temporary_image),
                str(sources[destination]), f"::/SYSTEM/PROGRAMS/{destination}")
        for name in ['3DPLUS.O','3DPMESA.O','3DPTINY.O']:
            if name in sources:run('mcopy','-o','-i',str(temporary_image),str(sources[name]),'::/'+name)
        for guest,source in [('/DOOM/DOOM1.WAD',root/'extras/Doom-BlesKernOS-Standalone/DOOM1.WAD'),('/QUAKE/ID1/PAK0.PAK',root/'extras/Quake-BlesKernOS-Standalone/pak0.pak'),('/EXTRAS/QUAKE2/baseq2/pak0.pak',root/'extras/quake2/baseq2/pak0.pak')]:
            if source.is_file():
                parent=''
                for part in Path(guest).parent.parts[1:]:
                    parent+='/'+part
                    run('mmd','-i',str(temporary_image),'::'+parent)
                run('mcopy','-o','-i',str(temporary_image),str(source),'::'+guest)
        if 'QUAKE2.BEX' in sources:
            run('mcopy','-o','-i',str(temporary_image),str(sources['QUAKE2.BEX']),'::/EXTRAS/QUAKE2/QUAKE2.BEX')
        if include_freeciv:
            run('mmd','-i',str(temporary_image),'::/SYSTEM/FREECIV')
            run('mcopy','-s','-o','-i',str(temporary_image),str(freeciv_data),'::/SYSTEM/FREECIV/')
            run('mcopy','-o','-i',str(temporary_image),str(sources['FREECIV_CLIENT.O']),'::/SYSTEM/PROGRAMS/FREECIV.O')
        for directory in ['models','textures']:
            source=root/'extras/3D-Plus-Standalone/assets'/directory
            if source.is_dir():
                run('mcopy','-s','-o','-i',str(temporary_image),str(source),'::/'+directory.upper())
        temporary_image.replace(image)
    finally:
        temporary_image.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
