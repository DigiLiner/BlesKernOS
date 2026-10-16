#!/usr/bin/env python3
"""Create a bootable FAT32 image containing the BlesKernOS source tree."""

import subprocess
import fnmatch
import sys
import tempfile
from pathlib import Path

from build_fat32_ata import (patch_boot_sector, set_fat32_info_sectors,
                              write_raw_sector_data)


ROOT = Path(__file__).resolve().parent.parent
IMAGE_SIZE_KB = 96 * 1024
RESERVED_ARG = 3593
RESERVED_SECTORS = RESERVED_ARG + 3
KERNEL_LBA = 9
STAGE2_LBA = 1
STAGE2_SECTORS = 8
KERNEL_SECTORS = 3584
SOURCE_ROOTS = ("boot", "kernel", "gui", "programs", "system", "sdk",
                "config", "mk", "docs", "libs", "assets", "tools", "third_party",
                "LICENSES")


def run(*args):
    subprocess.check_call([str(arg) for arg in args])


def mmd(image, destination):
    run("mmd", "-i", image, destination)


def mcopy(image, source, destination):
    run("mcopy", "-o", "-i", image, source, destination)


def ignored(path, root):
    relative = path.relative_to(root)
    return (any(part in {"__pycache__", ".pytest_cache", ".git"} or
                any(fnmatch.fnmatch(part.lower(), pattern) for pattern in
                    ("*.orig", "*.rej", "*.bak", "*.bak.*", "*.bak-*",
                     "*.before-*", "*.broken-*", "*.pre-*", "*.tmp", "*~"))
                for part in relative.parts) or
            (root.name == "programs" and "build" in relative.parts) or
            path.suffix.lower() == ".pyc" or
            ".pre-" in path.name)


def copy_tree(image, source_root, destination):
    mmd(image, destination)
    for directory in sorted(path for path in source_root.rglob("*")
                            if path.is_dir() and
                            not ignored(path, source_root)):
        relative = directory.relative_to(source_root).as_posix().upper()
        mmd(image, f"{destination}/{relative}")
    for source in sorted(path for path in source_root.rglob("*")
                         if path.is_file() and not ignored(path, source_root)):
        relative = source.relative_to(source_root).as_posix().upper()
        mcopy(image, source, f"{destination}/{relative}")


def main():
    image = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "source" / "bleskernos-source.img"
    boot = ROOT / "build" / "boot_fat32.bin"
    stage2 = ROOT / "build" / "stage2.bin"
    kernel = ROOT / "build" / "kernel.bin"
    for required in (boot, stage2, kernel):
        if not required.is_file():
            raise SystemExit(f"falta artefacto de arranque: {required}")

    image.parent.mkdir(parents=True, exist_ok=True)
    if image.exists():
        image.unlink()
    run("mkfs.fat", "-C", "-F", "32", "-n", "BLES-SOURCE",
        "-R", RESERVED_SECTORS, "-b", RESERVED_ARG + 1, image,
        IMAGE_SIZE_KB)
    fsinfo_sector = RESERVED_ARG
    backup_boot_sector = RESERVED_ARG + 1
    set_fat32_info_sectors(image, fsinfo_sector, backup_boot_sector)
    patch_boot_sector(image, boot, 0)
    write_raw_sector_data(image, STAGE2_LBA, stage2.read_bytes(),
                          STAGE2_SECTORS, "stage2")
    write_raw_sector_data(image, KERNEL_LBA, kernel.read_bytes(),
                          KERNEL_SECTORS, "kernel")

    mmd(image, "::/SYSTEM")
    mmd(image, "::/SYSTEM/SOURCE")
    # The native self-host build writes its first artifacts directly under
    # build/ (for example build/boot_fat32.bin).  Keep the directory in the
    # source image even though host build artifacts themselves are excluded.
    # This also avoids depending on GNU Make's auxiliary `build` target before
    # the first Win32 tool is started inside BlesKernOS.
    mmd(image, "::/SYSTEM/SOURCE/BUILD")
    with tempfile.TemporaryDirectory(prefix="bles-source-image-") as temporary:
        readme = Path(temporary) / "README.TXT"
        readme.write_text(
            "BlesKernOS source image\r\n"
            "Source tree for rebuilding the OS from inside BlesKernOS.\r\n"
            "The 'extras' tree is intentionally excluded.\r\n"
            "Run: make -f /SYSTEM/SOURCE/SELFHOST.MK selfhost\r\n",
            encoding="ascii")
        mcopy(image, readme, "::/SYSTEM/SOURCE/README.TXT")
    for root_name in SOURCE_ROOTS:
        copy_tree(image, ROOT / root_name,
                  f"::/SYSTEM/SOURCE/{root_name.upper()}")
    for source in (ROOT / "Makefile", ROOT / "GNUmakefile", ROOT / "LICENSE",
                   ROOT / "NOTICE.md", ROOT / "THIRD_PARTY_LICENSES.md", ROOT / "mk/selfhost.mk",
                   ROOT / "docs" / "build" / "self-hosting.md"):
        mcopy(image, source, f"::/SYSTEM/SOURCE/{source.name.upper()}")
    print(f"[SOURCE] FAT32 source image: {image} ({IMAGE_SIZE_KB // 1024} MiB)")


if __name__ == "__main__":
    main()
