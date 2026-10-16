#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 INPUT_VIDEO OUTPUT.BKV [fps] [width] [block]" >&2
    exit 2
fi

INPUT=$1
OUTPUT=$2
FPS=${3:-30}
WIDTH=${4:-640}
BLOCK=${5:-8}
TMP=${TMPDIR:-/tmp}/viewer-bkv-$$
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP"

ffmpeg -loglevel warning -i "$INPUT" \
  -vf "fps=${FPS},scale=${WIDTH}:-2:flags=bilinear" \
  "$TMP/%06d.ppm"

python3 "$SCRIPT_DIR/bkvencode.py" \
  --fps "$FPS" --block "$BLOCK" --keyint "$FPS" \
  -o "$OUTPUT" "$TMP/*.ppm"
