#!/usr/bin/env python3
"""Run an isolated graphics test disk in QEMU/KVM and retain logs/screenshots.
Build the scripted disk with build_svga3d_validation.py --perf first.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import time
from run_qemu_virtio import graphics_environment

ROOT = Path(__file__).resolve().parents[1]


def qmp(path, command, arguments=None):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(os.path.relpath(path))
        with sock.makefile('rwb') as stream:
            stream.readline()
            result = None
            for name, args in [('qmp_capabilities', None), (command, arguments)]:
                request = {'execute': name}
                if args is not None:
                    request['arguments'] = args
                stream.write((json.dumps(request) + '\n').encode())
                stream.flush()
                while True:
                    result = json.loads(stream.readline())
                    if 'return' in result or 'error' in result:
                        break
            if 'error' in result:
                raise RuntimeError(result['error']['desc'])
            return result['return']


def capture(out, pid, name):
    # QEMU 8.2 GL scanouts have no CPU DisplaySurface for screendump.
    # Capture only this VM's X11 window when the QMP path is unavailable.
    try:
        qmp(out / 'qmp.sock', 'screendump', {'filename': str(out / (name + '.ppm'))})
        return
    except (OSError, RuntimeError):
        pass
    if not shutil.which('xdotool') or not shutil.which('import'):
        return
    result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(pid)],
                            capture_output=True, text=True)
    for window in result.stdout.splitlines():
        if not window.isdigit():
            continue
        screenshot = out / (name + '.png')
        if subprocess.run(['import', '-window', window, str(screenshot)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device', choices=['virgl', 'virtio'], default='virgl')
    parser.add_argument('--seconds', type=int, default=210)
    parser.add_argument('--record', action='store_true', help='record this VM window at 2 FPS; requires ffmpeg/X11')
    args = parser.parse_args()
    image, out = args.image.resolve(), args.output.resolve()
    if not image.is_file() or not os.access('/dev/kvm', os.R_OK | os.W_OK):
        parser.error('a built raw image and access to /dev/kvm are required')
    if out.exists() and any(out.iterdir()):
        parser.error('use a new empty output directory; existing evidence is preserved')
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(image, out / 'test.img')
    for name in ['kernel-test.elf', 'kernel-test.bin', 'tuxracer.o',
                 'VIRTIOGPU.DVR', 'ATIR1283D.DVR']:
        source = image.parent / name
        if source.is_file():
            shutil.copy2(source, out / name)
    (out / 'artifacts.sha256').write_text(''.join(
        f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n'
        for p in sorted(out.iterdir()) if p.is_file()))
    device = 'virtio-vga-gl' if args.device == 'virgl' else 'virtio-vga'
    env = graphics_environment(device)
    command = ['qemu-system-i386', '-enable-kvm', '-cpu', 'host', '-smp', '1', '-m', '256M',
               '-drive', f'file={out / "test.img"},format=raw,if=ide', '-vga', 'none',
               '-device', device + ',xres=800,yres=600', '-display',
               'sdl,gl=on' if args.device == 'virgl' else 'gtk,show-cursor=on',
               '-serial', f'file:{out / "serial.log"}', '-qmp',
               f'unix:{os.path.relpath(out / "qmp.sock", ROOT)},server=on,wait=off', '-nic', 'none',
               '-no-reboot', '-no-shutdown', '-name', 'BlesKernOS graphics validation']
    (out / 'command.json').write_text(json.dumps({'argv': command,
        'LD_PRELOAD': env.get('LD_PRELOAD'), 'device': args.device}, indent=2) + '\n')
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=log)
        (out / 'qemu.pid').write_text(str(process.pid) + '\n')
        start = time.monotonic()
        captures = set()
        recording = None
        recording_log = None
        try:
            while process.poll() is None and time.monotonic() - start < args.seconds:
                time.sleep(5)
                serial = out / 'serial.log'
                text = serial.read_text(errors='replace').replace('\0', '') if serial.exists() else ''
                if args.record and recording is None and shutil.which('ffmpeg'):
                    found = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)],
                                           capture_output=True, text=True)
                    windows = [w for w in found.stdout.splitlines() if w.isdigit()]
                    if windows:
                        recording_log = (out / 'capture.log').open('w')
                        recording = subprocess.Popen(['ffmpeg', '-y', '-nostdin', '-loglevel', 'warning',
                            '-f', 'x11grab', '-framerate', '2', '-window_id', windows[-1],
                            '-i', env.get('DISPLAY', ':0'), '-c:v', 'libx264', '-preset', 'ultrafast',
                            '-crf', '23', '-threads', '1', str(out / 'visual-validation.mkv')],
                            stdout=recording_log, stderr=recording_log)
                heartbeats = text.count('alive after click')
                phase = 'reopened' if 'reopened Tux' in text else ('race' if 'click inside Tux' in text else 'boot')
                print(f'{int(time.monotonic()-start)}s phase={phase} heartbeats={heartbeats}', flush=True)
                for marker, ready in [('race-1', heartbeats >= 1), ('race-4', heartbeats >= 4),
                                      ('reopened', phase == 'reopened')]:
                    if ready and marker not in captures:
                        capture(out, process.pid, marker)
                        captures.add(marker)
                if phase == 'reopened' and heartbeats >= 6:
                    break
        finally:
            if recording is not None and recording.poll() is None:
                recording.send_signal(signal.SIGINT)
                try:
                    recording.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    recording.kill()
                    recording.wait()
            if recording_log is not None:
                recording_log.close()
            if process.poll() is None:
                try:
                    qmp(out / 'qmp.sock', 'quit')
                    process.wait(timeout=10)
                except (OSError, RuntimeError, subprocess.TimeoutExpired):
                    process.terminate()
                    process.wait(timeout=10)
        if process.returncode:
            raise SystemExit(f'QEMU exited with {process.returncode}; inspect {out / "qemu.log"}')
    print(f'Evidence: {out}. Check serial and pixel-test results before accepting performance.')


if __name__ == '__main__':
    main()
