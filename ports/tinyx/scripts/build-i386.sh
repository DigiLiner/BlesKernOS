#!/usr/bin/env bash
# Build the GPLv3 TinyCore TinyX fork with the BlesKernOS DDX.
set -euo pipefail

tinyx_root=$(cd "$(dirname "$0")/.." && pwd)
tinyx_work=${TINYX_WORKDIR:-$(mktemp -d /tmp/bles-tinyx.XXXXXX)}
mkdir -p "$tinyx_work/src" "$tinyx_work/prefix/lib"
# Old libtool cannot reliably relink from a source path containing spaces.
case "$tinyx_work" in *' '*) echo 'TINYX_WORKDIR must not contain spaces' >&2; exit 2;; esac
cp -a "$tinyx_root/upstream/libfontenc-1.1.8" "$tinyx_work/src/"
cp -a "$tinyx_root/upstream/libXfont-1.5.4" "$tinyx_work/src/"
cp -a "$tinyx_root/upstream/tinycorelinux-tinyx" "$tinyx_work/src/"
tinyx_prefix="$tinyx_work/prefix"

# This Ubuntu host has the i386 runtime libraries but not all i386 -dev links.
for name in libXau libXdmcp libfreetype; do
    case "$name" in
        libfreetype) soname=libfreetype.so.6;;
        libXau) soname=libXau.so.6;;
        libXdmcp) soname=libXdmcp.so.6;;
    esac
    ln -sfn "/usr/lib/i386-linux-gnu/$soname" "$tinyx_prefix/lib/$name.so"
done

mkdir -p "$tinyx_work/build-fontenc"
cd "$tinyx_work/build-fontenc"
bash "$tinyx_work/src/libfontenc-1.1.8/configure" --prefix="$tinyx_prefix" \
    'CC=gcc -m32' 'CFLAGS=-O2 -m32' \
    "LDFLAGS=-m32 -L$tinyx_prefix/lib"
make -j2
make install

mkdir -p "$tinyx_work/build-Xfont"
cd "$tinyx_work/build-Xfont"
bash "$tinyx_work/src/libXfont-1.5.4/configure" --prefix="$tinyx_prefix" \
    'CC=gcc -m32' 'CFLAGS=-O2 -m32' \
    "LDFLAGS=-m32 -L$tinyx_prefix/lib -Wl,-rpath,$tinyx_prefix/lib" \
    "PKG_CONFIG_PATH=$tinyx_prefix/lib/pkgconfig"
make -j2
make install

cd "$tinyx_work/src/tinycorelinux-tinyx"
autoreconf -fi
mkdir -p "$tinyx_work/build-tinyx"
cd "$tinyx_work/build-tinyx"
bash "$tinyx_work/src/tinycorelinux-tinyx/configure" --prefix="$tinyx_prefix" \
    --enable-kdrive --disable-xvesa --enable-xfbdev --disable-xdmcp \
    --disable-xres --disable-dbe --disable-screensaver \
    --disable-xf86bigfont --disable-tcp-transport --disable-ipv6 \
    'CC=gcc -m32' 'CFLAGS=-O2 -m32 -DBLESK' \
    "LDFLAGS=-m32 -no-pie -L$tinyx_prefix/lib -Wl,-rpath,$tinyx_prefix/lib" \
    "PKG_CONFIG_PATH=$tinyx_prefix/lib/pkgconfig"
make -j2
# Configure probes run on Linux, so they need Linux's loader. Relink only the
# final executable for BlesKernOS's guest loader path.
mv kdrive/fbdev/Xfbdev kdrive/fbdev/Xfbdev.hostinterp
make -j2 "LDFLAGS=-m32 -no-pie -L$tinyx_prefix/lib -Wl,-rpath,/WINEGATE/lib -Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2"
cp -L kdrive/fbdev/Xfbdev "$tinyx_prefix/XFBDEV"
readelf -l "$tinyx_prefix/XFBDEV" | grep -F '/WINEGATE/lib/ld-linux.so.2'
if [ -n "${TINYX_OUTPUT:-}" ]; then
    mkdir -p "$(dirname "$TINYX_OUTPUT")"
    cp "$tinyx_prefix/XFBDEV" "$TINYX_OUTPUT.tmp"
    mv "$TINYX_OUTPUT.tmp" "$TINYX_OUTPUT"
fi
printf 'TinyX BlesKernOS i386: %s\n' "$tinyx_prefix/XFBDEV"
