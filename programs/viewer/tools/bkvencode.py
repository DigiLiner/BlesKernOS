#!/usr/bin/env python3
"""Bles Video encoder for Viewer 2.0.

BKV2 is the recommended high-performance format. It stores an RGB565 color per
small tile and delta-compresses unchanged tiles between frames. The player can
render those tiles directly through the public BlesKernOS surface API.

Examples:
  python3 bkvencode.py --fps 30 --block 8 -o VIDEO.BKV frames/*.ppm
  python3 bkvencode.py --fps 60 --block 16 --keyint 60 -o FAST.BKV frames/*.ppm
  python3 bkvencode.py --legacy-v1 --fps 15 -o OLD.BKV frames/*.ppm
"""

from __future__ import annotations

import argparse
import glob
import struct
import sys
from pathlib import Path

BKV1_HEADER = 40
BKV2_HEADER = 48
FLAG_LOOP = 0x0001
CODEC_TILE_DELTA = 1
FRAME_DELTA = 0
FRAME_KEY = 1
MAX_W = 1920
MAX_H = 1080


def _token(stream) -> bytes:
    token = bytearray()
    while True:
        ch = stream.read(1)
        if not ch:
            raise ValueError("unexpected end of PPM header")
        if ch == b"#":
            stream.readline()
            continue
        if not ch.isspace():
            token.append(ch[0])
            break
    while True:
        ch = stream.read(1)
        if not ch or ch.isspace():
            break
        token.append(ch[0])
    return bytes(token)


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    with path.open("rb") as f:
        if _token(f) != b"P6":
            raise ValueError(f"{path}: only binary P6 PPM is supported")
        width = int(_token(f))
        height = int(_token(f))
        maxval = int(_token(f))
        if width <= 0 or height <= 0 or width > MAX_W or height > MAX_H:
            raise ValueError(f"{path}: invalid dimensions {width}x{height}")
        if maxval != 255:
            raise ValueError(f"{path}: maxval must be 255")
        data = f.read(width * height * 3)
        if len(data) != width * height * 3:
            raise ValueError(f"{path}: truncated pixel data")
        return width, height, data


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rle_frame(rgb: bytes) -> bytes:
    out = bytearray()
    current = None
    run = 0
    for i in range(0, len(rgb), 3):
        color = rgb565(rgb[i], rgb[i + 1], rgb[i + 2])
        if current == color and run < 0xFFFF:
            run += 1
            continue
        if current is not None:
            out += struct.pack("<HH", run, current)
        current = color
        run = 1
    if current is not None:
        out += struct.pack("<HH", run, current)
    return bytes(out)


def tile_frame(rgb: bytes, width: int, height: int, block: int) -> list[int]:
    cols = (width + block - 1) // block
    rows = (height + block - 1) // block
    colors: list[int] = []
    for by in range(rows):
        y0 = by * block
        y1 = min(height, y0 + block)
        for bx in range(cols):
            x0 = bx * block
            x1 = min(width, x0 + block)
            sr = sg = sb = count = 0
            for y in range(y0, y1):
                pos = (y * width + x0) * 3
                for _x in range(x0, x1):
                    sr += rgb[pos]
                    sg += rgb[pos + 1]
                    sb += rgb[pos + 2]
                    count += 1
                    pos += 3
            colors.append(rgb565(sr // count, sg // count, sb // count))
    return colors


def expand_inputs(patterns: list[str]) -> list[Path]:
    files: list[Path] = []
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if matches:
            files.extend(Path(p) for p in matches)
        elif Path(pattern).is_file():
            files.append(Path(pattern))
    seen: set[Path] = set()
    result: list[Path] = []
    for p in files:
        rp = p.resolve()
        if rp not in seen:
            seen.add(rp)
            result.append(p)
    return result


def encode_v1(output: Path, frames: list[Path], fps: int, loop: bool) -> None:
    encoded: list[bytes] = []
    width = height = 0
    for i, path in enumerate(frames):
        w, h, rgb = read_ppm(path)
        if i == 0:
            width, height = w, h
        elif (w, h) != (width, height):
            raise ValueError(f"{path}: expected {width}x{height}, got {w}x{h}")
        encoded.append(rle_frame(rgb))
    index_offset = BKV1_HEADER
    data_offset = index_offset + len(encoded) * 4
    offsets: list[int] = []
    cursor = data_offset
    for packet in encoded:
        offsets.append(cursor)
        cursor += 4 + len(packet)
    header = struct.pack(
        "<4sHHHHHHIIIIII", b"BKV1", BKV1_HEADER, width, height, fps, 1,
        FLAG_LOOP if loop else 0, len(encoded), index_offset, data_offset,
        0, cursor, 0,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as f:
        f.write(header)
        for off in offsets:
            f.write(struct.pack("<I", off))
        for packet in encoded:
            f.write(struct.pack("<I", len(packet)))
            f.write(packet)
    print(f"[OK] {output} BKV1 {width}x{height} {len(encoded)} frames @ {fps} fps")


def encode_v2(output: Path, frames: list[Path], fps: int, loop: bool,
              block: int, keyint: int, threshold: int) -> None:
    width = height = 0
    packets: list[bytes] = []
    previous: list[int] | None = None
    total_records = 0
    keyframes = 0
    for frame_index, path in enumerate(frames):
        w, h, rgb = read_ppm(path)
        if frame_index == 0:
            width, height = w, h
            cols = (width + block - 1) // block
            rows = (height + block - 1) // block
            if cols * rows > 65535:
                raise ValueError(
                    f"{width}x{height} with {block}x{block} blocks needs "
                    f"{cols*rows} blocks; choose a larger --block"
                )
        elif (w, h) != (width, height):
            raise ValueError(f"{path}: expected {width}x{height}, got {w}x{h}")
        colors = tile_frame(rgb, width, height, block)
        key = previous is None or frame_index % keyint == 0
        records: list[tuple[int, int]] = []
        if key:
            records = list(enumerate(colors))
            kind = FRAME_KEY
            keyframes += 1
        else:
            assert previous is not None
            for i, (old, new) in enumerate(zip(previous, colors)):
                # threshold is measured in raw RGB565 integer distance. It is a
                # deliberately cheap temporal noise filter for old CPUs.
                if abs(new - old) > threshold:
                    records.append((i, new))
            kind = FRAME_DELTA
        if len(records) > 65535:
            raise ValueError("too many tile records for BKV2 frame")
        payload = bytearray(struct.pack("<BBH", kind, 0, len(records)))
        for index, color in records:
            payload += struct.pack("<HH", index, color)
        packets.append(bytes(payload))
        total_records += len(records)
        previous = colors

    index_offset = BKV2_HEADER
    data_offset = index_offset + len(packets) * 4
    offsets: list[int] = []
    cursor = data_offset
    for packet in packets:
        offsets.append(cursor)
        cursor += 4 + len(packet)
    header = struct.pack(
        "<4sHHHHHHHBBHHIIIIII",
        b"BKV2", BKV2_HEADER, width, height, fps, 1,
        FLAG_LOOP if loop else 0, CODEC_TILE_DELTA,
        block, block, keyint, 0,
        len(packets), index_offset, data_offset, cursor, 0, 0,
    )
    if len(header) != BKV2_HEADER:
        raise AssertionError(f"BKV2 header is {len(header)}, expected {BKV2_HEADER}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as f:
        f.write(header)
        for off in offsets:
            f.write(struct.pack("<I", off))
        for packet in packets:
            f.write(struct.pack("<I", len(packet)))
            f.write(packet)
    avg = total_records / len(packets) if packets else 0
    print(f"[OK] {output} BKV2 {width}x{height} {len(packets)} frames @ {fps} fps")
    print(f"     block={block}x{block}, keyint={keyint}, keyframes={keyframes}")
    print(f"     average tile records/frame={avg:.1f}, file={cursor} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description="Viewer 2.0 BKV encoder")
    parser.add_argument("frames", nargs="+", help="P6 PPM frames or glob patterns")
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--loop", action="store_true")
    parser.add_argument("--legacy-v1", action="store_true",
                        help="write the exact RGB565/RLE BKV1 format")
    parser.add_argument("--block", type=int, choices=(4, 8, 16, 32), default=8,
                        help="BKV2 tile size; larger is faster, smaller is sharper")
    parser.add_argument("--keyint", type=int, default=30,
                        help="BKV2 keyframe interval")
    parser.add_argument("--threshold", type=int, default=0,
                        help="ignore tiny RGB565 tile changes to improve compression")
    args = parser.parse_args()
    try:
        if args.fps < 1 or args.fps > 120:
            raise ValueError("fps must be between 1 and 120")
        if args.keyint < 1 or args.keyint > 1000:
            raise ValueError("keyint must be between 1 and 1000")
        if args.threshold < 0 or args.threshold > 65535:
            raise ValueError("threshold must be between 0 and 65535")
        frames = expand_inputs(args.frames)
        if not frames:
            raise ValueError("no input frames")
        if args.legacy_v1:
            encode_v1(args.output, frames, args.fps, args.loop)
        else:
            encode_v2(args.output, frames, args.fps, args.loop,
                      args.block, args.keyint, args.threshold)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
