#!/usr/bin/env python3
"""Build and check the native 4 KiB Bles File System image.

The first 4096 sectors of a bootable disk are reserved for the BIOS loader and
kernel. BFS starts at that fixed boundary; all block numbers below are relative
to it. This tool does not use the host kernel to mount an untrusted image.
"""

from __future__ import annotations

import argparse
import binascii
import os
from pathlib import Path
import struct
import time

BLOCK = 4096
BOOT_SECTORS = 4096
MAGIC = 0x34534642  # BFS4
VERSION = 0x00020000
INODE_SIZE = 256
INODE_COUNT = 16384
DIRECT_EXTENTS = 8
CLEAN = 0x434C454E
DIRTY = 0x44495254
MODE_FILE = 0o100000
MODE_DIR = 0o040000
MODE_LINK = 0o120000
SUPER_FORMAT = "<16I Q 32s I"
INODE_FORMAT = "<IIQHHQQQII" + "II" * DIRECT_EXTENTS + "I128sII"
assert struct.calcsize(INODE_FORMAT) == INODE_SIZE


def crc(data: bytes | bytearray) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def bit_set(bitmap: bytearray, number: int, used: bool = True) -> None:
    mask = 1 << (number & 7)
    if used:
        bitmap[number >> 3] |= mask
    else:
        bitmap[number >> 3] &= ~mask


def bit_get(bitmap: bytes | bytearray, number: int) -> bool:
    return bool(bitmap[number >> 3] & (1 << (number & 7)))


class Image:
    def __init__(self, path: Path, source: Path, size_mib: int, label: str,
                 boot_sectors: int = BOOT_SECTORS):
        self.path = path
        self.source = source
        if not source.is_dir():
            raise ValueError(f"BFS source directory is missing: {source}")
        if path.stat().st_size != size_mib * 1024 * 1024:
            raise ValueError("BFS image size does not match --size-mib")
        self.boot_sectors = boot_sectors
        self.start = boot_sectors * 512
        self.total_blocks = (size_mib * 1024 * 1024 - self.start) // BLOCK
        self.block_bitmap_start = 1
        self.block_bitmap_blocks = (self.total_blocks + BLOCK * 8 - 1) // (BLOCK * 8)
        self.inode_bitmap_start = self.block_bitmap_start + self.block_bitmap_blocks
        self.inode_bitmap_blocks = (INODE_COUNT + BLOCK * 8 - 1) // (BLOCK * 8)
        self.inode_table_start = self.inode_bitmap_start + self.inode_bitmap_blocks
        self.inode_table_blocks = INODE_COUNT * INODE_SIZE // BLOCK
        self.data_start = self.inode_table_start + self.inode_table_blocks
        if self.total_blocks <= self.data_start + 1:
            raise ValueError("BFS image is too small")
        self.block_bitmap = bytearray(self.block_bitmap_blocks * BLOCK)
        self.inode_bitmap = bytearray(self.inode_bitmap_blocks * BLOCK)
        self.inode_table = bytearray(self.inode_table_blocks * BLOCK)
        for block in range(self.data_start):
            bit_set(self.block_bitmap, block)
        bit_set(self.inode_bitmap, 0)
        bit_set(self.inode_bitmap, 1)
        self.next_block = self.data_start
        self.next_inode = 2
        self.now = int(time.time())
        self.label = label.encode("utf-8")[:31].ljust(32, b"\0")
        self.fd = path.open("r+b")

    def close(self) -> None:
        self.fd.close()

    def write_block(self, block: int, data: bytes | bytearray) -> None:
        if len(data) > BLOCK or block >= self.total_blocks:
            raise ValueError("invalid BFS block write")
        self.fd.seek(self.start + block * BLOCK)
        self.fd.write(data)

    def allocate(self, count: int) -> int:
        start = self.next_block
        if start + count > self.total_blocks:
            raise ValueError("BFS image is full")
        for block in range(start, start + count):
            bit_set(self.block_bitmap, block)
        self.next_block += count
        return start

    def write_inode(self, number: int, parent: int, name: str, mode: int,
                    size: int = 0, extents: list[tuple[int, int]] | None = None,
                    modified: int | None = None) -> None:
        name_bytes = name.encode("utf-8")
        if (len(name_bytes) > 127 or not name_bytes or
                (b"/" in name_bytes and not (number == 1 and name == "/"))):
            raise ValueError(f"BFS name is invalid: {name!r}")
        extents = extents or []
        if len(extents) > DIRECT_EXTENTS:
            raise ValueError(f"too many extents: {name}")
        pairs = extents + [(0, 0)] * (DIRECT_EXTENTS - len(extents))
        raw = bytearray(struct.pack(
            INODE_FORMAT, number, parent, size, mode, 0,
            self.now, modified or self.now, self.now, 1, len(extents),
            *(item for pair in pairs for item in pair),
            0, name_bytes.ljust(128, b"\0"), 0, 0,
        ))
        # metadata_crc32 occupies bytes 248..251.
        struct.pack_into("<I", raw, 248, crc(raw))
        offset = number * INODE_SIZE
        self.inode_table[offset:offset + INODE_SIZE] = raw
        bit_set(self.inode_bitmap, number)

    def add_tree(self) -> None:
        self.write_inode(1, 1, "/", MODE_DIR | 0o755)
        parents: dict[Path, int] = {self.source: 1}
        for current, dirs, files in os.walk(self.source, followlinks=False):
            directory = Path(current)
            parent = parents[directory]
            dirs.sort(key=str.casefold)
            files.sort(key=str.casefold)
            for name in dirs:
                child = directory / name
                if child.is_symlink():
                    files.append(name)
                    continue
                number = self.next_inode
                self.next_inode += 1
                if number >= INODE_COUNT:
                    raise ValueError("BFS inode table is full")
                self.write_inode(number, parent, name, MODE_DIR | 0o755)
                parents[child] = number
            for name in files:
                child = directory / name
                number = self.next_inode
                self.next_inode += 1
                if number >= INODE_COUNT:
                    raise ValueError("BFS inode table is full")
                is_link = child.is_symlink()
                size = len(os.readlink(child).encode()) if is_link else child.stat().st_size
                blocks = (size + BLOCK - 1) // BLOCK
                extents = [(self.allocate(blocks), blocks)] if blocks else []
                if blocks:
                    self.fd.seek(self.start + extents[0][0] * BLOCK)
                    if is_link:
                        self.fd.write(os.readlink(child).encode())
                    else:
                        with child.open("rb") as src:
                            while data := src.read(1024 * 1024):
                                self.fd.write(data)
                mode = (MODE_LINK if is_link else MODE_FILE) | (0o777 if is_link else 0o644)
                self.write_inode(number, parent, name, mode, size, extents,
                                 int(child.lstat().st_mtime))

    def finish(self) -> None:
        free = self.total_blocks - self.next_block
        fields = (
            MAGIC, VERSION, BLOCK, self.total_blocks, free,
            self.block_bitmap_start, self.block_bitmap_blocks,
            self.inode_bitmap_start, self.inode_bitmap_blocks,
            self.inode_table_start, self.inode_table_blocks,
            INODE_COUNT, 1, self.data_start, CLEAN, 1,
            self.now, self.label, 0,
        )
        superblock = bytearray(struct.pack(SUPER_FORMAT, *fields))
        struct.pack_into("<I", superblock, len(superblock) - 4, crc(superblock))
        self.write_block(0, superblock)
        for start, data in ((self.block_bitmap_start, self.block_bitmap),
                            (self.inode_bitmap_start, self.inode_bitmap),
                            (self.inode_table_start, self.inode_table)):
            self.fd.seek(self.start + start * BLOCK)
            self.fd.write(data)
        self.fd.flush()
        os.fsync(self.fd.fileno())
        print(f"BFS4: {self.path} files={self.next_inode - 2} "
              f"blocks={self.total_blocks} free={free}")


def build(path: Path, source: Path, size_mib: int, label: str,
          boot_sectors: int = BOOT_SECTORS) -> None:
    image = Image(path, source, size_mib, label, boot_sectors)
    try:
        image.add_tree()
        image.finish()
    finally:
        image.close()


def check(path: Path, boot_sectors: int = BOOT_SECTORS) -> None:
    from bfs4_journal import require_checkpoint
    with path.open("rb") as fd:
        require_checkpoint(fd,boot_sectors)
        fd.seek(boot_sectors * 512)
        raw = fd.read(BLOCK)
        fields = struct.unpack(SUPER_FORMAT, raw[:struct.calcsize(SUPER_FORMAT)])
        saved_crc = fields[-1]
        clean = bytearray(raw[:struct.calcsize(SUPER_FORMAT)])
        clean[-4:] = b"\0" * 4
        if fields[0] != MAGIC or fields[1] != VERSION or crc(clean) != saved_crc:
            raise ValueError("invalid BFS superblock")
        if fields[14] not in (CLEAN, DIRTY):
            raise ValueError("invalid BFS clean flag")
        total, free, bitmap_start, bitmap_blocks = fields[3:7]
        actual_blocks = (path.stat().st_size - boot_sectors * 512) // BLOCK
        if total > actual_blocks:
            raise ValueError("BFS geometry exceeds the image size")
        inode_bitmap_start, inode_bitmap_blocks = fields[7:9]
        inode_start, inode_blocks, inode_count = fields[9:12]
        data_start = fields[13]
        if data_start >= total or inode_count * INODE_SIZE > inode_blocks * BLOCK:
            raise ValueError("invalid BFS geometry")
        fd.seek(boot_sectors * 512 + bitmap_start * BLOCK)
        bitmap = fd.read(bitmap_blocks * BLOCK)
        if len(bitmap) != bitmap_blocks * BLOCK or any(
                not bit_get(bitmap, block) for block in range(data_start)):
            raise ValueError("BFS metadata blocks are not reserved")
        used = sum(bit_get(bitmap, i) for i in range(total))
        if total - used != free:
            raise ValueError("BFS free block count mismatch")
        fd.seek(boot_sectors * 512 + inode_bitmap_start * BLOCK)
        inode_bitmap = fd.read(inode_bitmap_blocks * BLOCK)
        if len(inode_bitmap) != inode_bitmap_blocks * BLOCK:
            raise ValueError("truncated BFS inode bitmap")
        if not bit_get(inode_bitmap, 0) or not bit_get(inode_bitmap, 1):
            raise ValueError("BFS reserved/root inodes are missing")
        fd.seek(boot_sectors * 512 + inode_start * BLOCK)
        table = fd.read(inode_blocks * BLOCK)
        names: set[tuple[int, str]] = set()
        allocated: set[int] = set(range(data_start))
        parents: dict[int, int] = {}
        directories: set[int] = set()
        files = 0
        for number in range(1, inode_count):
            if not bit_get(inode_bitmap, number):
                continue
            raw_inode = table[number * INODE_SIZE:(number + 1) * INODE_SIZE]
            values = struct.unpack(INODE_FORMAT, raw_inode)
            copy = bytearray(raw_inode)
            copy[248:252] = b"\0" * 4
            if values[0] != number or crc(copy) != values[-2]:
                raise ValueError(f"invalid BFS inode {number}")
            parent = values[1]
            name = values[-3].split(b"\0", 1)[0].decode("utf-8")
            if not name or ("/" in name and number != 1):
                raise ValueError(f"invalid BFS name in inode {number}")
            if values[9] > DIRECT_EXTENTS + BLOCK // 8:
                raise ValueError(f"too many extents in inode {number}")
            indirect_block = values[26]
            indirect = b""
            if indirect_block:
                if (indirect_block < data_start or indirect_block >= total or
                        not bit_get(bitmap, indirect_block) or
                        indirect_block in allocated):
                    raise ValueError(f"invalid BFS indirect block in inode {number}")
                allocated.add(indirect_block)
                fd.seek(boot_sectors * 512 + indirect_block * BLOCK)
                indirect = fd.read(BLOCK)
            if values[9] > DIRECT_EXTENTS and not indirect:
                raise ValueError(f"missing BFS indirect block in inode {number}")
            key = (parent, name.casefold())
            if key in names:
                raise ValueError(f"duplicate BFS name: {parent}/{name}")
            names.add(key)
            parents[number] = parent
            if values[3] & 0o170000 == MODE_DIR:
                directories.add(number)
            if number != 1:
                files += 1
            mapped = 0
            for i in range(values[9]):
                if i < DIRECT_EXTENTS:
                    start, count = values[10 + i * 2:12 + i * 2]
                else:
                    start, count = struct.unpack_from("<II", indirect,
                                                      (i - DIRECT_EXTENTS) * 8)
                if start < data_start or start + count > total or not count:
                    raise ValueError(f"invalid BFS extent in inode {number}")
                for j in range(start, start + count):
                    if not bit_get(bitmap, j) or j in allocated:
                        raise ValueError(f"unallocated/overlapping BFS block {j}")
                    allocated.add(j)
                mapped += count
            if mapped != (values[2] + BLOCK - 1) // BLOCK:
                raise ValueError(f"BFS inode {number} size/extent mismatch")
        if 1 not in directories or parents[1] != 1:
            raise ValueError("invalid BFS root directory")
        for number, parent in parents.items():
            if number != 1 and parent not in directories:
                raise ValueError(f"BFS inode {number} has no directory parent")
        if len(allocated) != used:
            raise ValueError("BFS allocated but unreferenced blocks")
        state = "clean" if fields[14] == CLEAN else "dirty"
        print(f"BFS4 OK: {files} entries, {free} free blocks, {state}")


def extract(path: Path, destination: Path,
            boot_sectors: int = BOOT_SECTORS) -> None:
    """Extract BFS files for the CD/floppy installer build pipelines."""
    check(path, boot_sectors)
    destination.mkdir(parents=True, exist_ok=True)
    with path.open("rb") as fd:
        start = boot_sectors * 512
        fd.seek(start)
        superblock = struct.unpack(SUPER_FORMAT,
                                   fd.read(struct.calcsize(SUPER_FORMAT)))
        inode_start, inode_blocks, inode_count = superblock[9:12]
        fd.seek(start + inode_start * BLOCK)
        table = fd.read(inode_blocks * BLOCK)
        nodes = {}
        for number in range(1, inode_count):
            raw = table[number * INODE_SIZE:(number + 1) * INODE_SIZE]
            if not any(raw):
                continue
            values = struct.unpack(INODE_FORMAT, raw)
            if values[0] != number:
                continue
            nodes[number] = values

        paths = {1: destination}
        pending = set(nodes) - {1}
        while pending:
            progress = False
            for number in sorted(pending):
                inode = nodes[number]
                parent = inode[1]
                if parent not in paths:
                    continue
                name = inode[-3].split(b"\0", 1)[0].decode("utf-8")
                target = paths[parent] / name
                if inode[3] & 0o170000 == MODE_DIR:
                    target.mkdir(exist_ok=True)
                else:
                    with target.open("wb") as out:
                        remain = inode[2]
                        indirect = b""
                        if inode[26]:
                            fd.seek(start + inode[26] * BLOCK)
                            indirect = fd.read(BLOCK)
                        for i in range(inode[9]):
                            if i < DIRECT_EXTENTS:
                                block, count = inode[10 + i * 2:12 + i * 2]
                            else:
                                block, count = struct.unpack_from(
                                    "<II", indirect, (i - DIRECT_EXTENTS) * 8)
                            fd.seek(start + block * BLOCK)
                            while count and remain:
                                chunk = fd.read(min(BLOCK, remain))
                                if not chunk:
                                    raise ValueError(f"truncated BFS inode {number}")
                                out.write(chunk)
                                remain -= len(chunk)
                                count -= 1
                        if remain:
                            raise ValueError(f"missing BFS data in inode {number}")
                paths[number] = target
                pending.remove(number)
                progress = True
                break
            if not progress:
                raise ValueError("BFS has orphaned directory entries")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    mkfs = sub.add_parser("pack")
    mkfs.add_argument("image", type=Path)
    mkfs.add_argument("source", type=Path)
    mkfs.add_argument("--size-mib", type=int, default=768)
    mkfs.add_argument("--label", default="BLESKERNOS")
    mkfs.add_argument("--boot-sectors", type=int, default=BOOT_SECTORS)
    info = sub.add_parser("check")
    info.add_argument("image", type=Path)
    info.add_argument("--boot-sectors", type=int, default=BOOT_SECTORS)
    unpack = sub.add_parser("extract")
    unpack.add_argument("image", type=Path)
    unpack.add_argument("destination", type=Path)
    unpack.add_argument("--boot-sectors", type=int, default=BOOT_SECTORS)
    recovery=sub.add_parser("recover",help="replay validated metadata undo on a stopped image")
    recovery.add_argument("image",type=Path)
    recovery.add_argument("--boot-sectors",type=int,default=BOOT_SECTORS)
    args = parser.parse_args()
    if args.command == "pack":
        build(args.image, args.source, args.size_mib, args.label,
              args.boot_sectors)
    elif args.command == "recover":
        from bfs4_journal import recover
        print(f"Recovered {recover(args.image,args.boot_sectors)} metadata sectors")
        check(args.image,args.boot_sectors)
    elif args.command == "check":
        check(args.image, args.boot_sectors)
    else:
        extract(args.image, args.destination, args.boot_sectors)


if __name__ == "__main__":
    main()
