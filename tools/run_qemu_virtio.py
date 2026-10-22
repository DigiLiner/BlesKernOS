#!/usr/bin/env python3
"""Launch Virtio with KVM when available, including Ubuntu's QEMU GL module workaround."""
import argparse
import os
from pathlib import Path
import subprocess


def graphics_environment(device, qemu='qemu-system-i386'):
    env = os.environ.copy()
    probe = subprocess.run([qemu, '-device', device + ',help'],
                           capture_output=True, text=True, env=env)
    if probe.returncode and 'undefined symbol: qemu_egl_display' in probe.stderr:
        module = Path('/usr/lib/x86_64-linux-gnu/qemu/ui-opengl.so')
        if module.is_file():
            env['LD_PRELOAD'] = str(module) + (':' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--gl', action='store_true')
    parser.add_argument('--display', choices=['sdl', 'gtk'],
                        help='window backend (default: GTK)')
    parser.add_argument('--qemu', default='qemu-system-i386')
    parser.add_argument('--memory', type=int, default=1536, help='guest RAM in MiB (Wine default: 1536)')
    parser.add_argument('--smp', type=int, default=2)
    parser.add_argument('--no-ioeventfd',action='store_true',help='disable KVM eventfd queue notifications for comparison')
    parser.add_argument('--disk-interface', choices=['ide','ahci'], default='ide',
                        help='system disk controller (use ahci for faster installation/loading)')
    parser.add_argument('--cdrom', type=Path, help='optional test/application ISO')
    parser.add_argument('--audio',choices=['pa','alsa','sdl','none'],default='pa',
                        help='AC97 host audio backend (default: PulseAudio/PipeWire)')
    args = parser.parse_args()
    if args.memory < 64 or args.smp < 1:
        parser.error('memory must be >=64 MiB and smp >=1')
    if args.cdrom and not args.cdrom.is_file():
        parser.error('CD-ROM image does not exist')
    if not args.image.is_file():
        parser.error('build the disk image first')
    device = 'virtio-vga-gl' if args.gl else 'virtio-vga'
    display = args.display or 'gtk'
    display_options = display
    if args.gl:
        display_options += ',gl=on'
        if display == 'gtk':
            display_options += ',zoom-to-fit=off,show-cursor=on'
    kvm = os.access('/dev/kvm', os.R_OK | os.W_OK)
    device_options=device+(',ioeventfd=on' if kvm and args.gl and not args.no_ioeventfd else '')
    print('QEMU: ' + ('KVM, CPU host' if kvm else 'TCG: sin acceso a /dev/kvm; rendimiento reducido') +
          ('; VirGL 3D' if args.gl else '; Virtio 2D') + '; display=' + display +
          ('; ioeventfd=on' if kvm and args.gl and not args.no_ioeventfd else ''), flush=True)
    command = [args.qemu]
    command += ['-enable-kvm', '-cpu', 'host'] if kvm else ['-cpu', 'qemu32,+rdrand']
    if args.disk_interface == 'ahci':
        command += ['-drive', f'file={args.image},format=raw,if=none,id=osdisk',
                    '-device', 'ich9-ahci,id=ahci', '-device', 'ide-hd,drive=osdisk,bus=ahci.0']
    else:
        command += ['-drive', f'file={args.image},format=raw,if=ide']
    command += ['-vga', 'none',
                '-device', device_options, '-display', display_options,
                '-boot', 'c', '-m', str(args.memory), '-smp', str(args.smp), '-serial', 'stdio', '-no-reboot', '-no-shutdown']
    if args.audio != 'none':
        command += ['-audiodev',f'{args.audio},id=blesaudio','-device','AC97,audiodev=blesaudio']
    if args.cdrom:
        command += ['-drive', f'file={args.cdrom},media=cdrom,readonly=on']
    os.execvpe(command[0], command, graphics_environment(device, args.qemu))


if __name__ == '__main__':
    main()
