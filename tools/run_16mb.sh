#!/usr/bin/env bash
# Run the user disk with 16 MiB and /SYSTEM/SWAP.BKS on that same disk.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec qemu-system-i386 \
    -nodefaults -accel tcg -cpu qemu32,+rdrand -smp 1 -m 16M -vga std \
    -drive "file=${repo_root}/build/bleskernos-ata-user.img,format=raw,if=ide,index=0" \
    -serial "file:${repo_root}/build/qemu-16mb.log" \
    -boot c -no-reboot -no-shutdown "$@"
