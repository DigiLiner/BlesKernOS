#!/usr/bin/env bash
# Build the native i386 TinyX/Wine application gates, GPL-3.0-or-later.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
destination=${1:?usage: build-stage1-gates.sh ROOTFS}
bin="$destination/WINEGATE/bin"
mkdir -p "$bin"

common=(
    -m32 -O2 -Wall -Wextra -no-pie
    -Wl,--disable-new-dtags
    -Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2
    -Wl,-rpath,/WINEGATE/lib
)
xlib=(-L/usr/lib/i386-linux-gnu -l:libX11.so.6)

gcc "${common[@]}" "$root/ports/tinyx/tests/xlib-rectangle.c" \
    -o "$bin/xlib-rectangle" "${xlib[@]}"
gcc "${common[@]}" "$root/ports/tinyx/tests/wine-stage1-launch.c" \
    -o "$bin/wine-stage1-launch" "${xlib[@]}"
gcc "${common[@]}" -DSTAGE1_DEFAULT_SECONDS=90 \
    "$root/ports/tinyx/tests/wine-stage1-launch.c" \
    -o "$bin/wine-notepad-smoke-launch" "${xlib[@]}"
gcc "${common[@]}" -DSTAGE1_TRACE_KEYBOARD \
    "$root/ports/tinyx/tests/wine-stage1-launch.c" \
    -o "$bin/wine-stage1-keytrace-launch" "${xlib[@]}"
gcc "${common[@]}" -DSTAGE1_TRACE_X11 -DSTAGE1_DEFAULT_SECONDS=60 \
    "$root/ports/tinyx/tests/wine-stage1-launch.c" \
    -o "$bin/wine-stage1-xtrace-launch" "${xlib[@]}"
gcc "${common[@]}" '-DSTAGE1_DEFAULT_PROGRAM="regedit.exe"' \
    -DSTAGE1_DEFAULT_SECONDS=60 \
    "$root/ports/tinyx/tests/wine-stage1-launch.c" \
    -o "$bin/wine-regedit-launch" "${xlib[@]}"
gcc "${common[@]}" '-DSTAGE1_DEFAULT_PROGRAM="winemine.exe"' \
    -DSTAGE1_DEFAULT_SECONDS=60 \
    "$root/ports/tinyx/tests/wine-stage1-launch.c" \
    -o "$bin/wine-winemine-launch" "${xlib[@]}"
