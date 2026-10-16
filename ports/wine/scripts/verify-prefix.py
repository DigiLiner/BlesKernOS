#!/usr/bin/env python3
"""Cold-boot a successful W3 disk and verify its existing prefix."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=1200)
    parser.add_argument('--normal-start', action='store_true',
                        help='start Wine on the saved prefix without forcing wineboot --init')
    parser.add_argument('--in-place', action='store_true',
                        help='boot the exact source image again instead of copying it')
    parser.add_argument('--profile-w3', action='store_true',
                        help='include launcher phase timestamps (kernel must already be profiled)')
    args = parser.parse_args()
    source, out = args.run.resolve(), args.output.resolve()
    first = json.loads((source / 'result.json').read_text())
    if not first.get('passed') or first['results'].get('/WINEGATE/bin/wineboot-launch') != 0:
        parser.error('requires a successful wineboot run')
    initial_log = (source / 'serial.log').read_text(errors='replace')
    if 'W3-PERSIST-v1' not in initial_log:
        parser.error('source run did not query the persistence marker')
    if ('could not save registry branch' in initial_log or
            'persisted marker missing from saved user.reg' in initial_log):
        parser.error('source run failed to save the registry hives')
    if out.exists() and any(out.iterdir()):
        parser.error('use a new empty output directory')
    out.mkdir(parents=True, exist_ok=True)
    image = source / 'test.img' if args.in_place else out / 'test.img'
    if not args.in_place:
        shutil.copyfile(source / 'test.img', image)
    # Preserve the exact kernel and FAT metadata; only replace the test driver.
    launcher = out / 'wineboot-launch'
    subprocess.run(['gcc', '-m32', '-O2', '-no-pie', '-DWINEBOOT_REUSE_PREFIX',
        *(['-DWINEBOOT_NORMAL_START'] if args.normal_start else []),
        *(['-DWINEBOOT_PROFILE_PHASES'] if args.profile_w3 else []),
        '-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2',
        '-Wl,-rpath,/WINEGATE/lib', str(ROOT / 'ports/wine/tests/wineboot-launch.c'),
        '-o', str(launcher)], check=True)
    with image.open('rb') as disk:
        offset = struct.unpack_from('<I', disk.read(512), 454)[0] * 512
    subprocess.run(['mcopy', '-o', '-i', f'{image}@@{offset}', str(launcher),
        '::/WINEGATE/BIN/WINEBOOT-LAUNCH'], check=True)
    command = json.loads((source / 'command.json').read_text())
    drive_index = command.index('-drive') + 1
    prior_drive = command[drive_index]
    if 'file=' not in prior_drive:
        parser.error('source command has no disk image')
    command[drive_index] = re.sub(r'file=[^,]+', f'file={image}', prior_drive, count=1)
    command[command.index('-serial') + 1] = f'file:{out / "serial.log"}'
    with tempfile.TemporaryDirectory(prefix='w3-reboot-') as monitor:
        command[command.index('-monitor') + 1] = f'unix:{monitor}/monitor.sock,server,nowait'
        (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        start = time.monotonic()
        serial = ''
        with (out / 'qemu.log').open('w') as log:
            process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=log)
            try:
                while process.poll() is None and time.monotonic() - start < args.seconds:
                    time.sleep(1)
                    path = out / 'serial.log'
                    serial = path.read_text(errors='replace') if path.exists() else ''
                    if '[GATE:A] END' in serial:
                        break
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        path = out / 'serial.log'
        serial = path.read_text(errors='replace') if path.exists() else ''
    results = dict((name, int(status)) for name, status in re.findall(
        r'\[GATE:A\] RESULT (\S+) status=(-?\d+)', serial))
    faults = [line for line in serial.splitlines() if any(marker in line for marker in
        ('[LINUX:FAULT]', '[PANIC]', '[MM] ERROR: free invalido', '[MM] CORRUPCION',
         '[LINUX:IPC] pair table full', 'fail=vm-resources', 'fail=task-create',
         'boot event wait timed out'))]
    completion = ('[WINE:BOOT] cold reboot, persisted registry and normal startup PASS'
                  if args.normal_start else
                  '[WINE:BOOT] cold reboot, persisted registry and reused init PASS')
    required = ('[GATE:A] END', '[WINE:BOOT] reusing persisted prefix after cold boot',
        'W3-PERSIST-v1', completion)
    passed = (not faults and all(marker in serial for marker in required) and
        all(results.get(name) == 0 for name in first['expected']))
    result = dict(passed=passed,
        mode='cold-reboot-normal' if args.normal_start else 'cold-reboot-init',
        source=str(source), image=str(image), in_place=args.in_place,
        results=results, fatal_faults=faults, elapsed=time.monotonic()-start,
        kernel_sha256=hashlib.sha256((source / 'kernel-test.bin').read_bytes()).hexdigest())
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if not passed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
