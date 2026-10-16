#!/usr/bin/env python3
"""Observe the Wine boot barrier in QEMU without modifying the supplied disk."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--kernel-elf', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--ram', type=int, default=256)
p.add_argument('--expect-failure', action='store_true')
p.add_argument('--expect-skip', action='store_true')
a = p.parse_args()
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
symbols = subprocess.check_output(['nm', str(a.kernel_elf)], text=True)
addresses = {}
for name in ['g_retired', 'g_wine_boot_result', 'g_wine_boot_pid', 'g_vision_owner_pid']:
    match = re.search(r'^([0-9a-f]+) [bBdD] '+name+r'$', symbols, re.M)
    if not match:
        raise ValueError('missing symbol: '+name)
    addresses[name] = int(match[1], 16)
sock = Path('/tmp/bk-wine-boot-'+str(os.getpid())+'.sock')
serial = out/'boot.log'
assert not sock.exists(), 'output directory already has a QMP socket'
serial.unlink(missing_ok=True)
started = time.monotonic()
vm = subprocess.Popen(['qemu-system-i386', '-accel', 'kvm', '-cpu', 'host',
    '-smp', '2', '-m', str(a.ram), '-snapshot', '-drive',
    f'file={a.image.resolve()},format=raw,if=ide', '-vga', 'std', '-nodefaults',
    '-display', 'none', '-serial', f'file:{serial}', '-qmp',
    f'unix:{sock},server=on,wait=off', '-no-reboot', '-no-shutdown'],
    stderr=(out/'qemu.log').open('w'))

def request(f, name, args=None):
    f.write((json.dumps({'execute':name, 'arguments':args or {}})+'\n').encode())
    while True:
        response = json.loads(f.readline())
        if 'error' in response:
            raise RuntimeError(response)
        if 'return' in response:
            return response['return']

try:
    while not sock.exists():
        assert vm.poll() is None, 'QEMU exited'
        time.sleep(.1)
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(20)
    s.connect(str(sock))
    f = s.makefile('rwb', buffering=0)
    f.readline()
    request(f, 'qmp_capabilities')
    samples = []
    captured = False
    while time.monotonic()-started < 240:
        time.sleep(2)
        log = serial.read_text(errors='replace')
        assert '[PANIC]' not in log and 'PAGE FAULT' not in log
        request(f, 'stop')
        try:
            sample = {'seconds':time.monotonic()-started}
            for name, address in addresses.items():
                size = 1 if name == 'g_retired' else 4
                scratch = out/'state.bin'
                request(f, 'pmemsave', {'val':address, 'size':size, 'filename':str(scratch)})
                sample[name] = struct.unpack('<'+('B' if size==1 else 'i'), scratch.read_bytes())[0]
                scratch.unlink()
            samples.append(sample)
            if not a.expect_failure and not a.expect_skip:
                assert not sample['g_retired'] or sample['g_wine_boot_result']==1, 'desktop released before Wine READY'
            if sample['g_wine_boot_pid'] and not sample['g_retired'] and not captured:
                request(f, 'screendump', {'filename':str(out/'loading-wine.ppm')})
                captured = True
        finally:
            request(f, 'cont')
        if a.expect_skip:
            assert not sample['g_wine_boot_pid'] and not sample['g_wine_boot_result']
            assert '[WINE:BOOT] READY' not in log and 'LOADING WINE' not in log
            if '[RAM:IDLE]' in log:
                assert '[WINE:BOOT] SKIPPED' in log and 'DESKTOP RELEASED' in log
                assert sample['g_retired'] and sample['g_vision_owner_pid'] > 0
                break
        elif a.expect_failure:
            if '[RECOVERY] Wine no pudo prepararse' in log:
                assert 'DESKTOP RELEASED' not in log and '[WINE:BOOT] READY' not in log
                assert sample['g_wine_boot_result']==-1 and sample['g_vision_owner_pid']==0
                break
        elif '[RAM:IDLE]' in log:
            assert log.index('[WINE:BOOT] READY') < log.index('[BKBOOT] DESKTOP RELEASED')
            assert captured and sample['g_wine_boot_result']==1
            break
    else:
        raise TimeoutError('Wine boot barrier')
    request(f, 'screendump', {'filename':str(out/'final.ppm')})
    result = {'passed':True, 'ram_mib':a.ram, 'smp':2, 'expected_failure':a.expect_failure,
              'expected_skip':a.expect_skip,
              'seconds':time.monotonic()-started, 'captured_splash':captured, 'samples':samples}
    (out/'result.json').write_text(json.dumps(result, indent=2))
    from PIL import Image
    for name in ['loading-wine', 'final']:
        if (out/(name+'.ppm')).exists():
            Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
    print(json.dumps({k:v for k,v in result.items() if k!='samples'}, indent=2))
    request(f, 'quit')
    vm.wait(timeout=15)
finally:
    if vm.poll() is None:
        vm.terminate()
        vm.wait(timeout=15)
    sock.unlink(missing_ok=True)
