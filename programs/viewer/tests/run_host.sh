#!/bin/sh
# Portable host-side smoke test for AVI + MPEG1 streaming. Requires gcc,
# ffmpeg and a host with ASan/UBSan support. Does NOT emulate BlesKernOS.
set -eu
cd "$(dirname "$0")/.."
: "${CC:=gcc}"
command -v ffmpeg >/dev/null || exit 1
sandbox=$(mktemp -d)
trap 'rm -rf "$sandbox"' EXIT HUP INT TERM
ffmpeg -hide_banner -loglevel error \
    -f lavfi -i testsrc2=size=96x64:rate=25 \
    -f lavfi -i sine=frequency=440:sample_rate=44100 \
    -t 1 -c:v mpeg1video -b:v 500k -c:a mp2 -b:a 96k \
    -shortest -f mpeg -y "$sandbox/sample.mpg"
ffmpeg -hide_banner -loglevel error \
    -f lavfi -i testsrc2=size=96x64:rate=10 \
    -f lavfi -i sine=frequency=440:sample_rate=22050 \
    -t 1 -c:v rawvideo -pix_fmt bgr24 -c:a pcm_s16le \
    -shortest -y "$sandbox/sample.avi"
"$CC" -g -O1 -DPLM_NO_STDIO=1 -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Itests/stubs -Ilib -I. \
    tests/test_streaming.c lib/video.c lib/avi.c lib/mpeg.c lib/bkv.c \
    lib/gif.c lib/pl_mpeg_bles.c -lm -o "$sandbox/test"
(cd "$sandbox" && ./test sample.mpg sample.avi)
