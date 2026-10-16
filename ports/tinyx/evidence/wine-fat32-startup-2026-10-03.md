# Wine 11 startup on FAT32

Measured in QEMU/KVM with two vCPUs, 1536 MiB RAM, AHCI, a fresh copy of the
preseeded ATA user image, and `notepad.exe` launched through the normal `.exe`
association. The guest kernel printed monotonic timestamps from `wine-launch`.

| Event | Time since launcher start |
| --- | ---: |
| TinyX ready | 1,130 ms |
| Wine target launch | 1,370 ms |
| First Notepad surface | 39,810 ms |

The rootless graphics backend now skips full-framebuffer copies while it has
no native surfaces. Expected VFS open failures and high-frequency successful
VM/fork/exec diagnostics are compiled out by default; define
`BLESK_WINE_VERBOSE_TRACE` to restore them. Real failures remain logged.

The diagnostic W3 run observed roughly 836,000 FAT path resolutions and
425,000 directory entry searches during 45 seconds of Wine startup. A larger
path cache, a directory lookup cache, and IPC wake experiments did not produce
a reliable improvement; these experiments were removed from the release build.
Startup still varies with host CPU contention, so 39.8 seconds is a measured
result for this configuration rather than a guaranteed upper bound.
