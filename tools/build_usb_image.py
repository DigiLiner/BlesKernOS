#!/usr/bin/env python3
import sys
from pathlib import Path

SECTOR_SIZE = 512
PARTITION_START_LBA = 2048
PARTITION_TYPE_FAT32_LBA = 0x0C
PARTITION_TYPE_BFS4 = 0x83


def chs_lba(lba):
    sectors_per_track = 63
    heads = 255
    cylinder = lba // (heads * sectors_per_track)
    temp = lba % (heads * sectors_per_track)
    head = temp // sectors_per_track
    sector = (temp % sectors_per_track) + 1
    if cylinder > 1023:
        return bytes((0xFE, 0xFF, 0xFF))
    return bytes((head & 0xFF, ((sector & 0x3F) | ((cylinder >> 2) & 0xC0)),
                  cylinder & 0xFF))


def main():
    if len(sys.argv) != 4:
        print("usage: build_usb_image.py <out.img> <partition.img> <mbr.bin>",
              file=sys.stderr)
        return 1

    out_path = Path(sys.argv[1])
    part_path = Path(sys.argv[2])
    mbr_path = Path(sys.argv[3])

    with part_path.open('rb') as source:
        source.seek(4096 * SECTOR_SIZE)
        bfs4 = source.read(4) == b"BFS4"
    part_size = part_path.stat().st_size
    mbr = bytearray(mbr_path.read_bytes())
    if len(mbr) != SECTOR_SIZE:
        raise ValueError(f"MBR debe medir 512 bytes: {mbr_path}")
    if part_size % SECTOR_SIZE:
        raise ValueError("La imagen FAT32 debe estar alineada a sectores")

    part_sectors = part_size // SECTOR_SIZE
    total_sectors = PARTITION_START_LBA + part_sectors
    part_offset = PARTITION_START_LBA * SECTOR_SIZE

    entry = bytearray(16)
    entry[0] = 0x80
    entry[1:4] = chs_lba(PARTITION_START_LBA)
    entry[4] = PARTITION_TYPE_BFS4 if bfs4 else PARTITION_TYPE_FAT32_LBA
    entry[5:8] = chs_lba(PARTITION_START_LBA + part_sectors - 1)
    entry[8:12] = PARTITION_START_LBA.to_bytes(4, "little")
    entry[12:16] = part_sectors.to_bytes(4, "little")
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Stream large disks instead of holding both partition and disk in RAM.
    with out_path.open('wb') as output, part_path.open('rb') as source:
        output.truncate(total_sectors * SECTOR_SIZE)
        output.write(mbr)
        output.seek(part_offset)
        zero_chunk = bytes(1024 * 1024)
        while chunk := source.read(len(zero_chunk)):
            if chunk != zero_chunk[:len(chunk)]:
                output.write(chunk)
            else:
                output.seek(len(chunk), 1)
    with out_path.open('r+b') as output:
        output.seek(part_offset + 50)
        backup_boot_sector = 0 if bfs4 else int.from_bytes(output.read(2), 'little')
        for sector in (PARTITION_START_LBA, *([PARTITION_START_LBA + backup_boot_sector] if backup_boot_sector else [])):
            output.seek(sector * SECTOR_SIZE + 28)
            output.write(PARTITION_START_LBA.to_bytes(4, 'little'))
    print(f"USB image: {out_path} ({total_sectors * SECTOR_SIZE} bytes, partition LBA {PARTITION_START_LBA})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
