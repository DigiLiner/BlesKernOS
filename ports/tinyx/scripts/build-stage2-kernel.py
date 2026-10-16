#!/usr/bin/env python3
"""Build a Wine application gate on the current kernel and patch a test image."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from build_svga3d_validation import build_command as command_for  # noqa: E402
from kernel_image import KERNEL_MAX_BYTES  # noqa: E402


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="disposable Stage 2 BFS4 or FAT32 image")
    parser.add_argument("--launcher", default="/WINEGATE/bin/wine-stage1-launch")
    parser.add_argument("--argument", default="", help="argv[1] of the launcher")
    parser.add_argument("--association", action="store_true",
                        help="open --launcher through the normal BlesKernOS app association")
    parser.add_argument("--profile-w3", action="store_true",
                        help="sample Wine startup syscall and storage costs for 45 seconds")
    parser.add_argument("--no-warmup", action="store_true",
                        help="disable background Wine warmup for a cold-start measurement")
    parser.add_argument("--launch-delay", type=int, default=0,
                        help="seconds after desktop startup before launching the target")
    parser.add_argument("--runtime-perf", action="store_true",
                        help="report runtime counters every 10 seconds")
    args = parser.parse_args()
    if args.profile_w3 and not args.association:
        parser.error("--profile-w3 requires --association")
    if not 0 <= args.launch_delay <= 600:
        parser.error("--launch-delay must be between 0 and 600")
    if not args.image.is_file():
        parser.error("image must exist")
    run("make", "-j4", "build/kernel.bin")
    source = (ROOT / "kernel/core/kernel.c").read_text()
    if source.count("void kernel_main(void) {") != 1 or \
       source.count('    kprintf("[BKBOOT] DESKTOP RELEASED\\n");') != 1:
        raise RuntimeError("kernel entry changed; update Stage 2 gate")
    worker = r'''
#include "include/elf_loader.h"
#include "include/pit.h"
static void stage2_validation_worker(void *unused UNUSED) {
    const char *path = STAGE2_LAUNCHER;
    int status = -999, result = -1;
    kprintf("[GATE:STAGE2] START %s\n", path);
    int pid = elf_spawn_program_ex(path, gui_get_desktop(), STAGE2_ARGUMENT);
    uint32_t start = pit_get_ticks();
    if (pid >= 0) {
        while ((result = task_waitpid((uint32_t)pid, &status)) == 0 &&
               pit_get_ticks() - start < 1200U * pit_get_frequency_hz())
            task_yield();
        if (result == 0) {
            task_request_exit_process((uint32_t)pid, -998);
            status = -998;
        }
    } else kprintf("[GATE:STAGE2] LOAD_ERROR %s\n", elf_last_error());
    kprintf("[GATE:STAGE2] RESULT %s status=%d\n", path, status);
}
'''.replace("STAGE2_LAUNCHER", json.dumps(args.launcher)).replace(
        "STAGE2_ARGUMENT", json.dumps(args.argument) if args.argument else "NULL")
    if args.association:
        worker = r'''
#include "include/api.h"
#include "include/pit.h"
static void stage2_validation_worker(void *unused UNUSED) {
    const char *path = STAGE2_LAUNCHER;
    bool started = bk_app_launch(path, NULL);
    kprintf("[GATE:ASSOC] path=%s started=%d\n", path, started);
    uint32_t start = pit_get_ticks();
    while (pit_get_ticks() - start < 600U * pit_get_frequency_hz())
        task_yield();
}
'''.replace("STAGE2_LAUNCHER", json.dumps(args.launcher))
    if args.no_warmup:
        warmup_guard = 'if (!installer_boot && !compat_mode_is_low_memory())'
        if source.count(warmup_guard) != 1:
            raise RuntimeError("Wine warmup guard changed; update Stage 2 gate")
        source = source.replace(warmup_guard,
                                'if (false && !installer_boot && !compat_mode_is_low_memory())')
    if args.profile_w3:
        if not args.no_warmup:
            source = source.replace('        kernel_wait_wine_warmup();',
                '    {\n        w3p_reset();\n        kernel_wait_wine_warmup();\n'
                '        w3p_dump();\n    }')
        worker = worker.replace('static void stage2_validation_worker',
                                '#include "include/w3_profile.h"\nstatic void stage2_validation_worker')
        worker = worker.replace('    bool started = bk_app_launch(path, NULL);',
                                '    w3p_reset();\n    bool started = bk_app_launch(path, NULL);')
        worker = worker.replace('    uint32_t start = pit_get_ticks();',
                                '    uint32_t start = pit_get_ticks();\n    bool dumped = false;')
        worker = worker.replace('        task_yield();',
                                '    {\n        if (!dumped && pit_get_ticks() - start >= 45U * pit_get_frequency_hz()) {\n'
                                '            w3p_dump(); dumped = true;\n        }\n        task_yield();\n    }')
    if args.launch_delay:
        worker = worker.replace('    const char *path = ',
            '    uint32_t delay_start = pit_get_ticks();\n'
            f'    while (pit_get_ticks() - delay_start < {args.launch_delay}U * pit_get_frequency_hz())\n'
            '        task_yield();\n    const char *path = ', 1)
    # A runnable gate must not compete with the application being measured.
    worker = worker.replace('task_yield();', 'task_sleep(1U);')
    if args.runtime_perf:
        worker += r'''
#include "include/perfmon.h"
static void stage2_runtime_perf(void *unused UNUSED) {
    for (;;) {
        task_sleep(10U * pit_get_frequency_hz());
        perfmon_force_report();
        perfmon_poll();
    }
}
'''
        source = source.replace('    perfmon_init();',
                                '    perfmon_init();\n    perfmon_set_enabled(true);')
    source = source.replace("void kernel_main(void) {", worker + "\nvoid kernel_main(void) {")
    source = source.replace('    kprintf("[BKBOOT] DESKTOP RELEASED\\n");',
        '    kprintf("[BKBOOT] DESKTOP RELEASED\\n");\n'
        '    task_create_kernel("stage2-gate", stage2_validation_worker, NULL);')
    if args.runtime_perf:
        source = source.replace('    task_create_kernel("stage2-gate", stage2_validation_worker, NULL);',
            '    task_create_kernel("stage2-gate", stage2_validation_worker, NULL);\n'
            '    task_create_kernel("stage2-perf", stage2_runtime_perf, NULL);')
    output = args.image.parent / "stage2-kernel"
    output.mkdir(exist_ok=True)
    c_file = output / "kernel-test.c"
    o_file = output / "kernel-test.o"
    elf_file = output / "kernel-test.elf"
    bin_file = output / "kernel-test.bin"
    stage_file = output / "stage2.bin"
    c_file.write_text(source)
    kernel_command = command_for("kernel/core/kernel.c", "build/kernel/core/kernel.o",
                                 {"kernel/core/kernel.c": c_file, "build/kernel/core/kernel.o": o_file})
    kernel_command[1:1] = ["-iquote", str(ROOT / "kernel/core")]
    if args.profile_w3:
        kernel_command.insert(1, "-DW3_PROFILE=1")
    run(*kernel_command)
    linked = {"build/kernel/core/kernel.o": o_file, "build/kernel.elf": elf_file}
    if args.profile_w3:
        for source_path in ("kernel/api/syscall.c", "kernel/drivers/core/vfs.c",
                            "kernel/drivers/core/block.c", "kernel/drivers/storage/fat.c",
                            "kernel/drivers/storage/ahci.c", "kernel/drivers/storage/bfs4.c"):
            object_path = "build/" + source_path[:-2] + ".o"
            profiled = output / (Path(source_path).stem + "-w3-profile.o")
            command = command_for(source_path, object_path, {object_path: profiled})
            command.insert(1, "-DW3_PROFILE=1")
            run(*command)
            linked[object_path] = profiled
        profiler = output / "w3-profile.o"
        command = command_for("kernel/api/syscall.c", "build/kernel/api/syscall.o",
                              {"kernel/api/syscall.c": ROOT / "ports/wine/tests/w3-profile.c",
                               "build/kernel/api/syscall.o": profiler})
        command.insert(1, "-DW3_PROFILE=1")
        run(*command)
    link_command = command_for("build/kernel/core/kernel.o", "build/kernel.elf", linked)
    if args.profile_w3:
        # The kernel no longer necessarily uses --whole-archive. Put the
        # profiler before libraries so its runtime helpers resolve either way.
        link_command.insert(1, profiler)
    run(*link_command)
    run("objcopy", "-O", "binary", elf_file, bin_file)
    kernel = bin_file.read_bytes()
    if len(kernel) > KERNEL_MAX_BYTES:
        raise RuntimeError("kernel exceeds boot reservation")
    run("nasm", "-f", "bin", f"-DKERNEL_SECTORS={(len(kernel) + 511) // 512}",
        f"-DKERNEL_CHECKSUM={sum(kernel) & 0xffffffff}",
        "boot/stage2.asm", "-o", stage_file)
    with args.image.open("r+b") as disk:
        mbr = disk.read(512)
        if mbr[510:512] != b"\x55\xaa":
            raise RuntimeError("missing MBR")
        offset = (0 if mbr[82:90] == b"FAT32   " else
                  struct.unpack_from("<I", mbr, 454)[0] * 512)
        disk.seek(offset)
        bpb = disk.read(512)
        disk.seek(offset + 4096 * 512)
        bfs4 = disk.read(4) == b"BFS4"
        if bfs4:
            if 9 * 512 + len(kernel) > 4096 * 512:
                raise RuntimeError("kernel exceeds BFS4 boot reservation")
        elif (struct.unpack_from("<H", bpb, 11)[0] != 512 or
              struct.unpack_from("<H", bpb, 14)[0] * 512 < 9 * 512 + len(kernel)):
            raise RuntimeError("unexpected boot geometry")
        disk.seek(offset + 512)
        disk.write(stage_file.read_bytes())
        disk.seek(offset + 9 * 512)
        disk.write(kernel)
        disk.write(bytes((-len(kernel)) % 512))
    print(f"patched {args.image}: kernel {len(kernel)} bytes")


if __name__ == "__main__":
    main()
