# BlesKernOS ARM32 / Raspberry Pi 3 — Stage 13

This tree is an incremental ARM platform bring-up. It does not replace the x86
build and does not yet link the full BlesKernOS kernel.

## Implemented

- Stage 0: AArch32 entry at `0x8000`, banked stacks, early Mini UART.
- Stage 1: exception vectors, abort diagnostics, IRQ entry.
- Stage 2: generic BCM283x GPU IRQ dispatch + 100 Hz system timer.
- Stage 3: ARMv7 short-descriptor MMU, cacheable identity RAM and Device MMIO.
- Stage 4: VideoCore property mailbox, cache coherency helpers and a 32-bpp
  mailbox framebuffer (default request 1024x768). The framebuffer test draws a
  teal BlesKernOS-like desktop and taskbar so HDMI gives a visible proof of life.
- Stage 5: BCM2837 Arasan eMMC/SD controller in polling/PIO mode. It performs SD
  card reset/identification, supports SDSC and SDHC/SDXC block addressing and
  reads 512-byte sectors. Boot probes LBA0 and reports its `55 AA` signature.

## Build

    make -f mk/rpi3.mk rpi3-clean
    make -f mk/rpi3.mk rpi3

Outputs:

    build/rpi3/kernel7.elf
    build/rpi3/kernel7.img

Host packages used by this bootstrap are `clang`, `lld` and `llvm` (for
`llvm-objcopy`).

## Boot files

Put `kernel7.img` on a normal Raspberry Pi firmware boot partition along with
firmware files suitable for a Pi 3. Add the settings from `config.txt.example`.
For UART, GPIO14/15 are Mini UART TX/RX at 115200 8N1 and require a 3.3 V serial
adapter (never RS-232 voltage levels).

## Expected serial milestones

    [MBOX] ARM memory firmware range: ...
    [FB] online 1024x768 ...
    [IRQ] enabled
    [SD] online RCA=... SDHC/SDXC clk=... Hz
    [SD] LBA0 read OK signature=0x000055AA (MBR/boot signature OK)

Framebuffer failure is non-fatal and serial boot continues. Likewise SD failure
prints the driver return code and the controller's last interrupt status.

## Important design choices

The CPU is still AArch32 on the Cortex-A53. This avoids doing the x86->ARM port
and a 32->64-bit ABI migration simultaneously.

The VideoCore mailbox request buffers live in normal cached RAM. Stage 4 cleans
them to the point of coherency before firmware access and invalidates them after
VideoCore writes the response. The framebuffer's returned VideoCore bus address
is converted to an ARM physical address by removing the SDRAM alias bits.
Framebuffer drawing is cached and `rpi3_framebuffer_flush()` publishes it.

Stage 5 deliberately uses polling and single-block CMD17 reads. DMA, multiblock
CMD18, writes and 4-bit/high-speed negotiation can come later, after the driver
is connected to BlesKernOS's existing block layer. The existing FAT/VFS code is
not duplicated here.


## Stage 6: shared BlesKernOS core

Stage 6 stops treating the ARM bring-up as a separate demo kernel.  The ARM
image now compiles and executes the existing BlesKernOS `block.c`, `fat.c` and
`vfs.c` sources.  The Raspberry Pi SD controller is registered as `sd0` through
the normal block layer, then `vfs_mount("sd0")` probes superfloppy/MBR FAT
layouts using the same filesystem implementation as x86.

ARM-specific glue currently provides the early single-core implementation of
`kmalloc`/memory primitives, `kprintf`, task cwd/yield hooks, locks and a few
optional subsystem stubs.  These hooks are intentionally ABI-compatible with
the existing kernel interfaces so they can be replaced by the real scheduler,
SMP and console implementations as those subsystems are ported.

For a meaningful QEMU filesystem test, attach an actual FAT BlesKernOS image as
the emulated SD card rather than the old 64 MiB signature-only smoke-test image.
A successful mount prints `[BLES] shared block/FAT/VFS core ONLINE` and lists
the root directory over the serial console.


## QEMU raw-kernel load address fix

Do not boot the hardware `kernel7.img` with QEMU `-kernel`.  QEMU's generic
32-bit ARM raw-kernel loader places that file at `0x10000`, while Raspberry Pi
firmware loads a 32-bit `kernel7.img` at `0x8000`.  The Stage 6 shared core is
large enough that running it at the wrong address is no longer survivable.

Build the QEMU-specific image with:

    make -f mk/rpi3.mk rpi3-qemu

This produces `build/rpi3/kernel7-qemu.img`, linked at `0x10000`.  Keep using
`build/rpi3/kernel7.img` on real Raspberry Pi hardware.  The common code is
also compiled to the ARMv7-A/Cortex-A7 baseline, which is supported by both the
QEMU raspi2b Cortex-A7 and the Raspberry Pi 3 Cortex-A53 in AArch32 mode.

## Stage 10: preemptive ARM kernel tasks

Stage 10 replaces the early single-task task/yield stubs with a native AArch32
kernel-task scheduler. Kernel tasks execute in ARM System mode with independent
64 KiB stacks. IRQ and SVC vectors preserve `r0-r12`, System `sp/lr`, resume
`pc` and `cpsr`, so a timer interrupt may return into a different task.

The scheduler is intentionally single-core for this stage even when QEMU is
started with four CPUs; secondary cores remain parked. The 100 Hz BCM283x timer
uses a five-tick (50 ms) maximum quantum, while `task_yield()` uses `svc #0`
for an immediate reschedule and `task_sleep()` blocks until a timer wakeup.

After the real desktop is initialized, Stage 10 creates three kernel tasks:

- `gui`: polls USB/keyboard/mouse events and services the real desktop.
- `heartbeat`: sleeps for one second and reports scheduler switch statistics.
- `idle`: executes `wfi` whenever the other tasks are sleeping.

A healthy boot reaches messages similar to:

    [TASK:ARM] scheduler core initialized quantum=5 ticks stack=64 KiB
    [TASK:ARM] create ... name=gui
    [TASK:ARM] create ... name=heartbeat
    [BLES] Stage 10 ARM kernel tasks ONLINE
    [TASK:ARM] preemptive scheduler START first=gui ... mode=SYS
    [TASK:ARM] timer preemption verified idle -> gui tick=...
    [TASK:ARM] heartbeat tick=... switches=... live=...

User mode, ELF `EM_ARM` loading and the public SVC syscall ABI remain Stage 11.

## Stage 13: stability and native GUI applications

The ARM heap now rejects repeated frees, tracks allocation ownership and
reclaims process memory. Ring-3 allocations reuse holes and return free tail
space instead of permanently consuming their 4 MiB window. Kernel stacks have
overflow canaries, the task table has 32 entries, and detached task cleanup no
longer waits on the desktop mutex with preemption disabled. Timer recovery also
always programs a future deadline after long IRQ-off sections.

Desktop launches run through a short-lived worker so FAT/ELF work cannot stall
an input event transaction. Alongside Terminal and Calculator, the ARM image
contains native kernel frontends for About, Run, Calendar, Text Editor, File
Browser, Image Viewer, Paint, Process Manager and Games. Control Panel plus the
Mouse, Keyboard and Language panels are native too. The normal language catalog
and `LANGUAGE.INI` are loaded from the FAT volume.

Spinlocks now use `LDREX`/`STREX`, identify their owning core through MPIDR and
publish releases with barriers. Application scheduling remains deliberately on
CPU0: the current short-descriptor user window is global, so enabling tasks on
secondary cores before per-core translation tables would break process
isolation. Secondary cores remain parked until that MMU/scheduler work lands.

## Stage 11: ARM USR mode, SVC ABI and EM_ARM ELF

Stage 11 introduces real unprivileged AArch32 processes. Native ARM tasks run
in CPU mode `USR` (`CPSR.M=0x10`) and enter the kernel exclusively through
`SVC #0`. The syscall numbers remain the same public BlesKernOS ABI used by
x86; the ARM transport uses `r7` for the syscall number, `r0-r3` for arguments
and `r0` for the result.

Each early ARM process receives a 4 MiB identity-mapped slot starting at
`0x10000000`. The shared short-descriptor MMU table changes section AP bits on
context switch so only the current process slot is user-accessible. This gives
kernel/user privilege isolation and basic process-slot isolation without
committing the port to its final per-process page-table design yet.

The ARM loader accepts ELF32 little-endian `ET_REL`, machine `EM_ARM`, and
supports the relocation forms currently emitted by Clang for freestanding ARM
applications (`ABS32`, `REL32`, ARM branch/call, `MOVW/MOVT`, `PREL31` and
related compatibility relocations). Undefined public `bk_*` symbols are
rejected intentionally: Stage 12 will add the ARM public-API thunk layer.

`make -f mk/rpi3.mk rpi3-all` also produces:

    build/rpi3/ARMTEST.BEX

That file is a real `EM_ARM` BlesKernOS user executable. A byte-for-byte copy is
embedded in the Stage 11 kernel and loaded through the same ELF parser during
boot. It verifies USR mode, SVC, allocation, FAT/VFS file open/read, sleep,
preemption and clean process exit. A healthy serial trace includes:

    [ELF:ARM] EM_ARM ET_REL loaded pid=... entry=...
    [USER:ARM] USR mode + SVC ABI online
    [USER:ARM] /README.TXT first bytes: ...
    [USER:ARM] alloc + VFS + sleep + preemption OK; exiting status=0
    [TASK:ARM] USER exit pid=... status=0

The desktop launcher now attempts the ARM ELF loader rather than using the old
"launch deferred" stub. Existing i386 `.BEX` files are correctly rejected as
an incompatible architecture until ARM builds of those applications are added.

## Stage 12: native ARM SDK, real Terminal and command suite

Stage 12 turns the Stage 11 execution proof into usable native userland.  The
ARM syscall transport grows a small file/runtime extension while preserving the
existing x86 syscall numbers and ABI version.  ARM reports syscall ABI 6; x86
continues to report ABI 5.

User tasks now inherit their parent's current working directory and console
route, and the ELF loader transports its `launch_arg` into the new process.
The public ARM SDK implements console output, allocation, launch arguments,
process launch/wait, basic file I/O, stat/listdir/chdir and the small string and
memory runtime required by the existing command sources.

The ARM build compiles the original sources in `system/commands/` as real
`ELF32 ET_REL / EM_ARM` BEX files.  Stage 12 initially includes:

    about help dir ls cat type echo calc uptime uname tree
    hexdump checksum find sleep

They are emitted under `build/rpi3/arm-programs/` and have no undefined
symbols. A copy of this initial suite is embedded in the kernel so it can be
tested against an unchanged Stage 11 FAT image; later builds can place the same
BEX files on disk without changing the loader.

The desktop's existing `/SYSTEM/PROGRAMS/SHELL.BEX` icon is intercepted only on
ARM and opens the real shared `programs/shelllauncher.c` frontend. Commands
entered in that Terminal are spawned as isolated USR-mode EM_ARM processes and
their stdout follows the inherited console route back into the Terminal window
while still being mirrored to Mini UART.

Useful Stage 12 commands include:

    armapps
    uname
    dir /
    cat /README.TXT
    calc 12 + 30
    uptime
    tree /DOCS
    checksum /README.TXT

`cd`, `pwd`, `clear` and `exit` are kernel-shell builtins; the other commands
above execute through the ARM ELF loader and SVC ABI. Graphical Ring-3 callback
/upcall support is intentionally deferred to Stage 13, so existing i386 GUI
BEX applications are still rejected rather than emulated or silently run in
kernel mode.

## Stage 12.1: input service fix and expanded native tools

The DWC2 HID poller now runs in its own 100 Hz kernel task instead of sharing
the GUI/compositor service loop.  This keeps relative mouse reports flowing
while ARM user processes perform syscalls or the desktop repaints.  The first
non-empty mouse report is logged once on serial for bring-up diagnostics.

The automatic Stage 11 `ARMTEST.BEX` and Stage 12 `uname` smoke launches were
removed: a normal desktop boot no longer starts user programs on its own.
Native applications run only after explicit user action.

The embedded native command set grows from 15 to 34 commands, adding file
management and utility programs such as `copy`, `cp`, `rm`, `mkdir`, `rmdir`,
`move`, `rename`, `touch`, `more`, `strings`, `diff`, `mem`, `benchmark`,
`compress`, `extract` and `backup`.  The original graphical Calculator source
is also linked as a shared ARM kernel frontend so the existing Calculator icon
works before graphical Ring-3 upcalls arrive in Stage 13.


## Stage 12.3: atomic task publication / Terminal crash fix

Kernel and USR task creation are now non-preemptible while a task frame is
being assembled, and `ARM_TASK_READY` is published only after SP/LR/PC/CPSR and
all metadata are valid.  This closes a scheduler race where opening Terminal or
other runtime-created tasks could be selected by a timer IRQ while their context
was only partially initialized.  The ARM Terminal frontend also has explicit
open/spawn diagnostics and single-instance protection.


## Stage 12.4: synchronous Terminal command ownership / zombie reaping

Terminal no longer creates a second kernel worker for every command.  The
Terminal task itself waits for the ARM USER child while GUI/input continue on
their own tasks.  `waitpid()` now consumes zombie state and frees resources,
and task allocation prefers UNUSED slots so a child exit status cannot be
overwritten before its parent observes it.


## Stage 12.5: preempt-safe ARM heap and runtime hardening

The ARM bootstrap heap is now protected against scheduler preemption while its
linked list is split/merged.  This closes a Stage 10+ race where GUI, Terminal,
ELF loading and task stack allocation could interleave `kmalloc()`/`kfree()`
and corrupt the allocator.  Heap traversal also validates block ordering and
range before following links, turning a damaged list into a serial diagnostic
instead of an infinite preemption-disabled traversal.

Task-slot reaping and user-slot acquisition are published atomically, and the
Terminal console sink serializes scrollback mutation against GUI painting.
The scheduler emits a direct-UART warning if any task holds preemption disabled
for more than one second, which makes any remaining hard freeze attributable to
a specific task/critical section.


## Stage 12.6: reliable QEMU absolute pointer polling

QEMU's `usb-tablet` remains enumerated through the DWC2 host controller, but
its report-protocol absolute pointer is now sampled with HID `GET_REPORT`
control requests at the normal input cadence.  QEMU returns the latest absolute
tablet state for this request even when no interrupt event is pending.  This
avoids depending on interrupt-endpoint DATA0/DATA1/NAK timing for the emulator
tablet while preserving the boot-HID interrupt path used by keyboards, mice
and physical Raspberry Pi USB input devices.

The first changed absolute sample emits
`[INPUT] USB tablet movement confirmed ...` so serial logs distinguish
enumeration from real pointer motion.


## Stage 12.7: synchronized shared kernel runtime

Stage 10 made ARM32 preemptive, so the old bring-up no-op `kmutex`/`krwlock`
stubs are no longer valid. Stage 12.7 ports recursive task-owned mutexes and
reader/writer locks for the current CPU0 scheduler. Shared block I/O, FAT/VFS
volume state, graphics resources and desktop paint locking therefore regain
the synchronization semantics expected by the original BlesKernOS core.

The ARM GUI service now treats event dispatch plus compositor paint as one
serialized desktop transaction. Window publication/removal/destruction and
widget-list mutation share that recursive lock, preventing application kernel
threads from freeing or relinking GUI objects while the GUI task is using them.
This is a platform/runtime fix rather than a Calculator-specific workaround.
