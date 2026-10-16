# Bles Codex Pro — build/workspace layer

This patch moves Codex away from a private, reduced SDK snapshot and makes the
current BlesKernOS SDK the source of truth for serious projects.

## What changes now

- Keeps the existing TinyC backend for fast/simple sources and compatibility.
- Adds a GCC/binutils project path through the self-host toolchain already
  produced by BlesKernOS 0.9.
- Supports multi-file C projects through a normal Makefile (`SRC=a.c b.c`).
- Debug and release profiles (`PROFILE=debug|release`).
- Uses GNU11/C11 rather than Codex's tiny C subset.
- Produces one development FAT32 image containing Codex, the complete public
  SDK tree, generated SDK archives, common system libraries, templates and the
  self-host toolchain.
- The image is generated from the *current* OS build, so SDK/ABI drift is much
  less likely than with `sdk/include/bleskernos_api.h` copied inside Codex.

## Image layout

    /CODEX
    /SDK/INCLUDE
    /SDK/SOURCE
    /SDK/LIB
    /TOOLCHAIN
    /PROJECTS/HELLO
    /PROJECTS/WINDOW

## Build

From the BlesKernOS repository root:

    python3 tools/install_codex_pro.py
    make codex-pro-image

Result:

    build/codex-pro-sdk.img

Attach that IMG to QEMU as a second IDE disk.  The image is data/development
media; it is not a boot disk.

Inside BlesKernOS, mount the disk and use the bundled project Makefiles with the
self-hosted `make`, `gcc` and `ld`.  The exact mount name depends on the device
order selected in QEMU.

## IDE work this layer enables

The `CODEX.PRJ` format reserves project metadata for a richer Codex UI: project
explorer, multiple source files, build profile, output name, diagnostics panel,
line/column status, autosave and future symbol navigation.  Those UI controls
must be wired to the current `src/codex.c` implementation; this patch does not
pretend to modify an unavailable version of that file.
