#!/usr/bin/env python3
"""Clone an initialized Wine image for PS/2 and repeatable Stage 1 tests."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="image with Wine 11.0 installed")
    parser.add_argument("destination", type=Path, help="new disposable FAT32 image")
    parser.add_argument("--ps2-driver", type=Path,
                        default=ROOT / "build/system/drivers/PS2MOUSE.DVR")
    args = parser.parse_args()
    if not args.source.is_file() or not args.ps2_driver.is_file():
        parser.error("source image and PS/2 driver must exist")
    if args.destination.exists():
        parser.error("destination must not exist")
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(("cp", "--reflink=auto", str(args.source), str(args.destination)),
                   check=True)
    with args.destination.open("rb") as image:
        mbr = image.read(512)
    if mbr[510:512] != b"\x55\xaa":
        raise RuntimeError("source image has no MBR signature")
    offset = struct.unpack_from("<I", mbr, 454)[0] * 512
    fat = f"{args.destination}@@{offset}"
    subprocess.run(("mcopy", "-o", "-i", fat, str(args.ps2_driver),
                    "::/SYSTEM/DRIVERS/PS2MOUSE.DVR"), check=True)
    # Upstream Wine explicitly supports this marker for a validated prefix.
    # The old fixture contains "64" while its installed wine.inf has a newer
    # mtime; without this Wine repeats its costly configuration on every boot.
    with tempfile.TemporaryDirectory(prefix="bles-stage1-") as temp:
        marker = Path(temp) / ".update-timestamp"
        marker.write_text("disable\n")
        subprocess.run(("mcopy", "-o", "-i", fat, str(marker),
                        "::/WINEGATE/PREFIX-W3/.update-timestamp"), check=True)
    print(args.destination)


if __name__ == "__main__":
    main()
