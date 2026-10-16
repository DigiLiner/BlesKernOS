#!/usr/bin/env python3
"""Run the same offscreen XRender workload on a selected guest TinyX build."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root/'tools'))
from bfs4_update import Editor

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--tinyx', type=Path, required=True)
p.add_argument('--base', type=Path, default=root/'build/bleskernos-ata-user.img')
a = p.parse_args()
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
image = out/'test.img'
if image.exists():
    p.error('use a new output directory')

def run(*args):
    subprocess.run([str(v) for v in args], cwd=root, check=True)

run('make', '-j2', 'build/stage2.bin', 'build/system/services/vision.o')
binary = out/'render-bench.ELF'
run('gcc', '-m32', '-O2', '-Wall', '-Wextra', '-Werror', '-no-pie',
    '-Wl,--disable-new-dtags', '-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2',
    '-Wl,-rpath,/WINEGATE/lib', root/'ports/tinyx/tests/bench-render.c',
    '-o', binary, '-L/usr/lib/i386-linux-gnu', '-l:libX11.so.6', '-l:libXrender.so.1')
run('cp', '--reflink=auto', a.base, image)
editor = Editor(image)
editor.update('/SYSTEM/SERVICES/VISION.BEX', (root/'build/system/services/vision.o').read_bytes())
editor.update('/WINEGATE/bin/XFBDEV', a.tinyx.read_bytes())
editor.update('/WINEGATE/bin/render-bench.ELF', binary.read_bytes())
editor.update('/SYSTEM/USER/CONFIG/SCREENSV.INI', b'enabled=0\ntimeout=3600\n')
editor.finish()
run(sys.executable, 'ports/tinyx/scripts/build-stage2-kernel.py', image,
    '--launcher', '/WINEGATE/bin/render-bench.ELF', '--launch-delay', '3')
command = ['qemu-system-i386', '-accel', 'kvm', '-cpu', 'host', '-smp', '2', '-m', '1536',
           '-drive', f'file={image},format=raw,if=ide', '-vga', 'vmware', '-display', 'none',
           '-serial', f'file:{out}/serial.log', '-nodefaults', '-no-reboot', '-no-shutdown']
process = subprocess.Popen(command, stderr=(out/'qemu.log').open('w'))
begin = time.monotonic()
events = []
pending = ''
passed = False
finished = False
try:
    while not (out/'serial.log').exists():
        if process.poll() is not None:
            raise RuntimeError('QEMU exited before serial startup')
        time.sleep(.01)
    with (out/'serial.log').open(errors='replace') as serial:
        while time.monotonic()-begin < 180:
            pending += serial.read()
            lines = pending.split('\n')
            pending = lines.pop()
            for line in lines:
                events.append({'host_s': time.monotonic()-begin, 'line': line})
                if '[RENDER-BENCH]' in line:
                    print(events[-1], flush=True)
                if '[RENDER-BENCH] FAIL' in line or '[PANIC]' in line:
                    raise RuntimeError(line)
                if '[GATE:STAGE2] RESULT' in line:
                    passed = 'status=0' in line
                    finished = True
                    break
            if finished:
                break
            if process.poll() is not None:
                raise RuntimeError('QEMU exited during benchmark')
            time.sleep(.01)
finally:
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait()
    (out/'host.json').write_text(json.dumps({'command':command, 'events':events}, indent=2)+'\n')
assert passed, 'XRender gate failed or timed out; see serial.log'
phases = []
for alpha in [0, 1]:
    start = next(row for row in events if f'BEGIN alpha={alpha}' in row['line'])
    end = next(row for row in events if f'END alpha={alpha}' in row['line'])
    checksum = next(row['line'] for row in events if f'HASH alpha={alpha}' in row['line'])
    elapsed = end['host_s']-start['host_s']
    phases.append({'alpha':alpha, 'frames':80, 'host_s':elapsed, 'compositions_per_s':80/elapsed,
                   'guest_us':int(re.search(r'us=(\d+)',end['line'])[1]), 'hash':checksum.split('value=')[1].strip()})
(out/'result.json').write_text(json.dumps({'result':'PASS', 'phases':phases}, indent=2)+'\n')
print(json.dumps(phases, indent=2))
