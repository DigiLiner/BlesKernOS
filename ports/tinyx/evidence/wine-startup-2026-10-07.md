# Wine startup on BFS4, 2026-10-07

> Historical measurement: the later [FD-neutral cache change](wine-fd-neutral-2026-10-07.md) restores the 128-file limit and replaces background warmup with a boot barrier.

Measured through the normal `.exe` association in QEMU/KVM, two vCPUs,
1536 MiB RAM and AHCI. The target was
`/SYSTEM/WINE/windows/system32/notepad.exe`. Times are launcher monotonic
timestamps up to the first native surface. Each variant was measured once;
host load affects the result. Other Windows applications have not been timed.

| Configuration | First surface | Eager file reads | File I/O reported by VM counters |
| --- | ---: | ---: | ---: |
| Previous kernel, no background warmup | 16,653 ms | 2,003 | 374,070 KiB |
| Expanded mapping/file tables, no warmup | 16,123 ms | 152 | 192,462 KiB |
| Tables plus realtime tick cache, no warmup | 10,843 ms | 152 | 192,462 KiB |
| Final kernel, application 30 s after desktop startup | 3,646 ms | 60 | 70,152 KiB |

The cold-start reduction is approximately 35%. Background warmup completed
after 14,353 ms in the warm run. Its time is separate from the 3,646 ms
application launch time. The warm run used the ordinary production kernel
objects without W3 profiling; the cold variants used the diagnostic profiler.

Wine's live DLL views exhausted the original 32 shared-mapping entries.
The VFS now permits 128 on hosts with at least 256 MiB RAM, retaining the
32-entry admission limit on smaller hosts and reserving four entries for
shared/anonymous objects during private-file prefetch. Cached views retain
open descriptions: expanding only the mapping table exhausted the old global
128-file table and was rejected. The global file table is now 512 entries.
All truncate, rename and unlink scans cover the full mapping table.

Linux realtime queries previously sampled CMOS date/time for every call.
The kernel now shares a sample within one PIT tick, with an SMP spinlock.
Every changed tick refreshes the sample, preserving the existing clock
resolution and RTC fallback. In the 45-second diagnostic samples,
`clock_gettime64` accumulated 5,492 ms before the changes and 1,273 ms after
them; those samples include activity after Notepad's first surface.

Validation:

- `make -j4 all` rebuilt the user ATA, USB and VMware images successfully.
- BFS4 checks passed on the rebuilt ATA and USB images.
- Notepad displayed a native window and received the text `bles` via PS/2.
- The glibc i386 `mapping-capacity.c` gate passed at 512 MiB with 80
  simultaneous file mappings. It checks rename, mapped-file truncate
  rejection, unlink without recreating the pathname, private COW isolation,
  release and realtime refresh after a sleep.
- At 64 MiB with one CPU, all 16-mapping and clock assertions passed, but
  the runner did not observe process exit; this run is not a full gate pass.
  Repeating with the original syscall/VFS/storage objects reproduced the
  same missing exit notification. Both logs are retained.
- `check_high_kernel_layout.py` and `test_kernel_reserve.py` still report
  failures from their old BSS placement assumptions. The baseline gate's BSS
  already starts at `0x00800000`; this change does not move it. These checks
  are not reported as passing.

Logs, the screenshot and source/kernel hashes are in
[`wine-startup-2026-10-07/`](wine-startup-2026-10-07/result.json).

The Stage 2 gate now accepts BFS4 boot reservations, `--no-warmup` for a
cold launch and `--launch-delay` for a launch after normal background warmup.
For a cold diagnostic kernel, operate on a disposable image copy:

```sh
mkdir -p /tmp/wine-cold
cp --reflink=auto build/bleskernos-ata-user.img /tmp/wine-cold/test.img
python3 ports/tinyx/scripts/build-stage2-kernel.py /tmp/wine-cold/test.img \
  --launcher /SYSTEM/WINE/windows/system32/notepad.exe \
  --association --no-warmup --profile-w3
```

Boot that copy with KVM, two CPUs, 1536 MiB and AHCI, recording serial output.
The profile is dumped 45 seconds after launch. For a warm measurement omit
`--no-warmup --profile-w3` and add `--launch-delay 30`. The gate preserves
normal production kernel objects; it patches only the selected test image.
