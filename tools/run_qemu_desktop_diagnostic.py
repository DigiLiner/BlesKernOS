#!/usr/bin/env python3
"""Run the reported q35/SVGA/AHCI configuration on a private disk copy.

Press Enter in this terminal when the desktop freezes. Capture guest CPU state,
locks, task contexts and kernel stacks without depending on the guest scheduler.
The source disk is never opened for writing. Requires QEMU, GCC i386 and GDB.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time

from bfs4_journal import recover

ROOT = Path(__file__).resolve().parents[1]


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(15)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile('rwb')
        self.stream.readline()
        self.call('qmp_capabilities')

    def call(self, name, arguments=None):
        self.stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
        self.stream.flush()
        while True:
            result = json.loads(self.stream.readline())
            if 'error' in result:
                raise RuntimeError(result['error'])
            if 'return' in result:
                return result['return']

    def close(self):
        self.stream.close()
        self.sock.close()


def source_is_open(image):
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if 'qemu-system' in (proc / 'comm').read_text():
                if any(fd.resolve() == image for fd in (proc / 'fd').iterdir()):
                    return True
        except OSError:
            pass
    return False


def capture(qmp, port, out):
    qmp.call('stop')
    for name, command in [('cpus', 'info cpus'), ('registers', 'info registers')]:
        (out / (name + '.txt')).write_text(qmp.call('human-monitor-command', {'command-line': command}))
    try:
        qmp.call('screendump', {'filename': str(out / 'desktop.ppm')})
    except Exception as error:
        (out / 'screenshot-error.txt').write_text(str(error))
    types = out / 'types.c'
    types.write_text('#include "kernel/include/task.h"\n#include "kernel/include/klock.h"\n'
                     '#include "kernel/include/memory.h"\n#include "gui/gui.h"\ntask_t *debug_task;\n')
    obj = out / 'types.o'
    subprocess.run(['gcc', '-m32', '-g', '-fno-eliminate-unused-debug-types', '-ffreestanding',
                    '-I' + str(ROOT), '-I' + str(ROOT / 'kernel/libc/include'), '-c', str(types), '-o', str(obj)], check=True)
    commands = out / 'capture.gdb'
    # No guest function calls: all observations remain useful with IRQs masked
    # or a sleeping-lock deadlock. Kernel stacks are identity-addressed.
    commands.write_text(f'''set pagination off
set confirm off
file "{ROOT / 'build/kernel.elf'}"
add-symbol-file "{obj}" 0
target remote :{port}
info threads
thread apply all bt
thread apply all info registers eax ebx ecx edx esi edi eip esp ebp eflags cr3
p *(kmutex_t(*)[9])&g_domains
p *(kmutex_t*)&g_desktop_paint_lock
p *(kmutex_t*)&g_bfs_lock
p *(kspinlock_t*)&g_mm_heap_lock
p *(unsigned*)&kernel_ticks
p *(unsigned*)&g_vision_frame_tid
p *(unsigned*)&g_vision_waiting_tid
set $i=0
while $i<128
set $t=((task_t*)&tasks)+$i
if $t->state && $t->state!=5
printf "\\nTASK pid=%u process=%u state=%u exit=%u entry=%x thread=%u name=%s api=%s\\n",$t->pid,$t->process_id,$t->state,$t->exit_requested,$t->entry,$t->thread,$t->name,$t->api_guard_name
p *$t->context
set $bp=$t->context->ebp
set $depth=0
while $bp>=(unsigned)$t->stack && $bp+8<=(unsigned)$t->stack+$t->stack_size && $depth<32
set $pc=*(unsigned*)($bp+4)
printf "frame bp=%x pc=%x ",$bp,$pc
info symbol $pc
set $next=*(unsigned*)$bp
if $next<=$bp
set $depth=32
else
set $bp=$next
set $depth=$depth+1
end
end
end
set $i=$i+1
end
set $b=*(heap_block_t**)&heap_head
set $n=0
while $b && $n<20000
if $b->magic!=0xb1e5c0de
printf "INVALID HEAP HEADER at %x after %u blocks\\n",$b,$n
x/24wx $b-1
set $b=0
else
set $b=$b->next
set $n=$n+1
end
end
printf "Heap headers inspected: %u\\n",$n
detach
quit
''')
    with (out / 'guest-state.txt').open('w') as log:
        result = subprocess.run(['gdb', '-q', '-batch', '-x', str(commands)], stdout=log,
                                stderr=subprocess.STDOUT, timeout=45)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'build/bleskernos-lbs-user.img')
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--capture-after', type=float, help='automated capture after this many seconds')
    args = parser.parse_args()
    if args.capture_after is not None and args.capture_after < 0:
        parser.error('--capture-after debe ser mayor o igual a cero')
    source = args.source.resolve()
    if source_is_open(source):
        parser.error('Cerrá el QEMU que tiene abierto el disco antes de crear la copia de diagnóstico.')
    kernel = (ROOT / 'build/kernel.bin').read_bytes()
    with source.open('rb') as disk:
        disk.seek(9 * 512)
        if disk.read(len(kernel)) != kernel:
            parser.error('El kernel del disco difiere de build/kernel.bin; los símbolos no serían fiables.')
    out = Path(tempfile.mkdtemp(prefix='trash-manual-', dir=ROOT / 'build')).resolve()
    image = out / 'disk.img'
    subprocess.run(['cp', '--reflink=auto', str(source), str(image)], check=True)
    recovered = recover(image)
    # Keep UNIX sockets short even when the workspace path contains spaces.
    socket_dir = tempfile.TemporaryDirectory(prefix='bk-trash-')
    sock = Path(socket_dir.name) / 'qmp.sock'
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    cmd = ['qemu-system-i386', '-enable-kvm', '-cpu', 'host', '-machine', 'q35,accel=kvm',
           '-m', '1024', '-smp', '2', '-vga', 'vmware', '-display', 'none' if args.headless else 'gtk',
           '-device', 'AC97', '-device', 'ich9-ahci,id=ahci',
           '-drive', f'if=none,id=disk0,file={image},format=raw,cache=writeback',
           '-device', 'ide-hd,drive=disk0,bus=ahci.0', '-boot', 'c', '-rtc', 'base=localtime',
           '-serial', f'file:{out / "serial.log"}', '-qmp', f'unix:{sock},server=on,wait=off',
           '-gdb', f'tcp:127.0.0.1:{port}', '-no-reboot']
    (out / 'inputs.json').write_text(json.dumps({'source': str(source), 'kernel_sha256': hashlib.sha256(kernel).hexdigest(),
        'kernel_elf_sha256': hashlib.sha256((ROOT / 'build/kernel.elf').read_bytes()).hexdigest(),
        'recovered_sectors_in_copy': recovered, 'command': cmd}, indent=2) + '\n')
    print('Disco original conservado. Copia y registros:', out, flush=True)
    print('Reproducí el bloqueo en la ventana de QEMU y presioná Enter AQUÍ para capturarlo.', flush=True)
    print('La captura detiene esta ejecución de prueba y conserva su disco y registros.', flush=True)
    qmp = None
    started = time.monotonic()
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen(cmd, stderr=log)
        try:
            while not sock.exists():
                if process.poll() is not None or time.monotonic() - started > 30:
                    raise RuntimeError('QEMU no inició; consultar ' + str(out / 'qemu.log'))
                time.sleep(.1)
            qmp = QMP(sock)
            while process.poll() is None:
                if args.capture_after is not None:
                    if time.monotonic() - started >= args.capture_after:
                        break
                    time.sleep(.1)
                elif select.select([0], [], [], .2)[0]:
                    if not os.read(0, 1024):
                        raise RuntimeError('La captura manual necesita una terminal con entrada abierta.')
                    break
            if process.poll() is None:
                code = capture(qmp, port, out)
                (out / 'capture.json').write_text(json.dumps({'gdb_exit_code': code, 'elapsed_seconds': round(time.monotonic()-started, 2)}) + '\n')
                print('Captura guardada:', out / 'guest-state.txt', flush=True)
                if code:
                    raise RuntimeError('GDB no completó toda la captura; consultar guest-state.txt')
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)
            socket_dir.cleanup()


if __name__ == '__main__':
    main()
