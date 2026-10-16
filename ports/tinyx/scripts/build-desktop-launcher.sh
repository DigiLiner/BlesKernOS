#!/usr/bin/env bash
# Build the Wine/TinyX desktop launcher used by the normal BlesKernOS image.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out="$root/build/wine/wine-launch"
mkdir -p "$(dirname "$out")"
gcc -m32 -O2 -Wall -Wextra -no-pie \
    -Wl,--disable-new-dtags \
    -Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2 \
    -Wl,-rpath,/WINEGATE/lib \
    "$root/ports/tinyx/wine-launch.c" \
    -o "$out" -L/usr/lib/i386-linux-gnu -l:libX11.so.6 -l:libXfixes.so.3
readelf -l "$out" | grep -F '/WINEGATE/lib/ld-linux.so.2'
printf 'Wine desktop launcher: %s\n' "$out"
