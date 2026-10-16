#!/usr/bin/env python3
"""Build an isolated Tux boot/input test; never modify a personal VMware VM."""
import argparse
import hashlib
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
from kernel_image import KERNEL_MAX_BYTES,copy_boot_image


ROOT = Path(__file__).resolve().parents[1]


def run(args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def build_command(source, target, replacements):
    plan = subprocess.check_output(
        ['make', '-n', '-W', source, target], cwd=ROOT, text=True)
    for line in plan.splitlines():
        args = shlex.split(line)
        if '-o' in args and args[args.index('-o') + 1] == target:
            return [replacements.get(a, a) for a in args]
    raise RuntimeError(f'could not obtain build command for {target}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/svga3d-smoke'))
    parser.add_argument('--edition', default='user', choices=['user', 'developer'])
    parser.add_argument('--modern', action='store_true',
                        help='negative test with virtual hardware 22')
    parser.add_argument('--manual', action='store_true',
                        help='autostart Tux but do not inject GUI input')
    parser.add_argument('--perf', action='store_true', help='report OS/app FPS and graphics timings every 10 seconds')
    args = parser.parse_args()
    out = (ROOT / args.output).resolve()
    if out == ROOT or out == ROOT / 'build':
        parser.error('use a dedicated output subdirectory')
    vmx = out / 'test.vmx'
    if (out / 'qmp.sock').exists():
        parser.error('use a fresh output directory after a QEMU run')
    if vmx.with_suffix('.vmx.lck').exists() or (out / 'test.vmdk.lck').exists():
        parser.error('stop the disposable VM before rebuilding its disk')
    if shutil.which('vmrun'):
        running = subprocess.check_output(['vmrun', 'list'], text=True)
        if str(vmx) in running.splitlines():
            parser.error('stop the disposable VM before rebuilding its disk')
    out.mkdir(parents=True, exist_ok=True)
    run(['make', '-j4', f'EDITION={args.edition}',
         f'build/bleskernos-usb-{args.edition}.img', 'build/kernel.elf'])

    source = (ROOT / 'kernel/core/kernel.c').read_text()
    anchor = '    gui_run();'
    if source.count(anchor) != 1:
        raise RuntimeError('kernel GUI startup changed; update the test harness')
    startup = '    elf_spawn_program_ex("/SYSTEM/PROGRAMS/TUXRACER.BEX", gui_get_desktop(), NULL);\n'
    if not args.manual:
        source = source.replace('void kernel_main(void) {',
            '#include "tools/svga3d_validation_input.h"\n\nvoid kernel_main(void) {')
        startup += '    task_create_kernel("svga-input-test", svga_validation_input, NULL);\n'
    if args.perf:
        # Record resident driver image bases too, before their first load.
        source = source.replace('    perfmon_init();',
            '    perfmon_init();\n    perfmon_set_enabled(true);')
        source = source.replace('void kernel_main(void) {',
            '#include "tools/svga3d_validation_perf.h"\n\nvoid kernel_main(void) {')
        startup += '    perfmon_set_enabled(true);\n    task_create_kernel("graphics-perf", svga_validation_perf, NULL);\n'
    source = source.replace(anchor, startup + anchor)
    test_c, test_o = out / 'kernel-test.c', out / 'kernel-test.o'
    test_elf, test_bin = out / 'kernel-test.elf', out / 'kernel-test.bin'
    test_c.write_text(source)
    run(build_command('kernel/core/kernel.c', 'build/kernel/core/kernel.o', {
        'kernel/core/kernel.c': test_c, 'build/kernel/core/kernel.o': test_o}))
    run(build_command('build/kernel/core/kernel.o', 'build/kernel.elf', {
        'build/kernel/core/kernel.o': test_o, 'build/kernel.elf': test_elf}))
    run(['objcopy', '-O', 'binary', test_elf, test_bin])
    kernel = test_bin.read_bytes()
    if len(kernel) > KERNEL_MAX_BYTES:
        raise RuntimeError('kernel supera reserva de stage2')
    stage2 = out / 'stage2-test.bin'
    run(['nasm', '-f', 'bin', f'-DKERNEL_SECTORS={(len(kernel)+511)//512}',
         f'-DKERNEL_CHECKSUM={sum(kernel)&0xffffffff}',
         'boot/stage2.asm', '-o', stage2])
    image = out / 'test.img'
    copy_boot_image(ROOT / f'build/bleskernos-usb-{args.edition}.img', image)
    with image.open('r+b') as disk:
        mbr = disk.read(512)
        offset = struct.unpack_from('<I', mbr, 446 + 8)[0] * 512
        disk.seek(offset)
        bpb = disk.read(512)
        sector_size, reserved = struct.unpack_from('<H', bpb, 11)[0], struct.unpack_from('<H', bpb, 14)[0]
        if sector_size != 512 or reserved * 512 < 9*512 + len(kernel):
            raise RuntimeError('unexpected boot partition geometry')
        disk.seek(offset + 512)
        disk.write(stage2.read_bytes())
        disk.seek(offset + 9*512)
        disk.write(kernel)
    run(['qemu-img', 'convert', '-f', 'raw', '-O', 'vmdk', '-o',
         'subformat=monolithicSparse,compat6=on,adapter_type=ide',
         image, out / 'test.vmdk'])
    run([sys.executable, 'tools/build_vmware_config.py', out / 'test.vmdk', vmx])
    config = vmx.read_text().replace('BlesKernOS 0.9 - SVGA3D legacy',
                                    'BlesKernOS SVGA3D validation')
    if args.modern:
        config = config.replace('virtualHW.version = "10"', 'virtualHW.version = "22"')
        config = config.replace('svga.vgpu10 = "FALSE"', 'svga.vgpu10 = "TRUE"')
    vmx.write_text(config)
    # Preserve the exact symbols even after the next normal build overwrites
    # build/programs and build/system/drivers.
    for source in [ROOT / 'build/programs/tuxracer.o',
                   ROOT / 'build/system/drivers/VMWARESVGA.DVR',
                   ROOT / 'build/system/drivers/VMWARESVGA3D.DVR',
                   ROOT / 'build/system/drivers/VIRTIOGPU.DVR',
                   ROOT / 'build/system/drivers/ATIR1283D.DVR']:
        shutil.copy2(source, out / source.name)
    artifacts = [test_elf, test_bin, out / 'tuxracer.o',
                 out / 'VMWARESVGA.DVR', out / 'VMWARESVGA3D.DVR',
                 out / 'VIRTIOGPU.DVR', out / 'ATIR1283D.DVR',
                 image, out / 'test.vmdk', vmx]
    (out / 'artifacts.sha256').write_text(''.join(
        f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n'
        for path in artifacts))
    serial = out / 'bleskernos-serial.log'
    if serial.exists():
        serial.replace(out / 'bleskernos-serial-previous.log')
    print(f'Open {vmx}; inspect {serial}. This script does not start VMware.')


if __name__ == '__main__':
    main()
