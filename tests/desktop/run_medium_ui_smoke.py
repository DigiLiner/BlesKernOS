#!/usr/bin/env python3
"""Exercise the six medium UI audit fixes on a disposable BFS4 image.

Requires the affected objects to be built. Captures the control panel layouts,
exercises all Edit actions and clipboard shortcuts, saves the resulting text,
and checks persisted preferences after QEMU exits. Never modifies the base.
"""
import argparse
import atexit
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from bfs4_update import Editor

TEXT = b'alpha beta\rsecond line\r'
PAYLOADS = {
    '/SYSTEM/CONTROL/DATETIME.CPL': 'build/system/control/datetime.o',
    '/SYSTEM/CONTROL/SYSTEM.CPL': 'build/system/control/system.o',
    '/SYSTEM/CONTROL/LANGUAGE.CPL': 'build/system/control/language.o',
    '/SYSTEM/PROGRAMS/FILE.BEX': 'build/programs/filebrowser.o',
    '/SYSTEM/PROGRAMS/TEXTEDITOR.BEX': 'build/programs/texteditor.o',
    '/SYSTEM/PROGRAMS/RUNBOX.BEX': 'build/programs/runbox.o',
}


class UI:
    def __init__(self, sock, out):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(20)
        self.socket.connect(str(sock))
        self.stream = self.socket.makefile('rwb')
        self.stream.readline()
        self.out = out
        self.position = [0, 0]
        self.command('qmp_capabilities')

    def command(self, name, args=None):
        request = {'execute': name, 'arguments': args or {}}
        self.stream.write((json.dumps(request) + '\n').encode())
        self.stream.flush()
        while True:
            response = json.loads(self.stream.readline())
            if 'error' in response:
                raise RuntimeError(response)
            if 'return' in response:
                return response['return']

    def hmp(self, text):
        return self.command('human-monitor-command', {'command-line': text})

    def calibrate(self):
        for _ in range(35):
            self.hmp('mouse_move -40 -40')
            time.sleep(.08)
        self.position = [0, 0]

    def move(self, x, y):
        while self.position != [x, y]:
            dx = max(-35, min(35, x - self.position[0]))
            dy = max(-35, min(35, y - self.position[1]))
            self.hmp(f'mouse_move {dx} {dy}')
            self.position[0] += dx
            self.position[1] += dy
            time.sleep(.10)
        time.sleep(.25)

    def click(self, x, y):
        self.move(x, y)
        self.hmp('mouse_button 1')
        time.sleep(.3)
        self.hmp('mouse_button 0')
        time.sleep(.5)

    def key(self, key):
        self.hmp('sendkey ' + key)
        time.sleep(.45 if key.startswith(('ctrl-', 'alt-')) else .25)

    def text(self, text):
        keys = {'.': 'dot', '/': 'slash', ' ': 'spc', '-': 'minus'}
        for char in text:
            self.key(keys.get(char, char.lower()))

    def shot(self, name):
        time.sleep(.8)
        ppm = self.out / (name + '.ppm')
        self.command('screendump', {'filename': str(ppm)})
        with Image.open(ppm) as im:
            assert im.size == (800, 600), 'This test uses the 800x600 desktop'
            im.save(ppm.with_suffix('.png'))
        ppm.unlink()
        print('Captured', name, flush=True)

    def launch(self, name, previous=''):
        self.click(300, 294)
        for _ in previous:
            self.key('backspace')
        self.text(name)
        self.key('ret')
        if hasattr(self, 'wait_for_launch'):
            self.wait_for_launch(name)
        else:
            time.sleep(2)

    def edit(self, row):
        self.click(175, 79)
        time.sleep(.2)
        self.click(195, 100 + 18 * row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--accel', choices=['tcg','kvm'], default='tcg')
    parser.add_argument('--lbs', action='store_true', help='exercise migrated LBS application payloads')
    parser.add_argument('--base', type=Path, default=ROOT / 'build/bleskernos-ata-user.img')
    parser.add_argument('--out', type=Path,
                        default=ROOT / 'tests/desktop/evidence/ui-medium-2026-10-17')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'result.json').unlink(missing_ok=True)
    image = ROOT / 'build/ui-medium-test.img'
    if args.base.resolve() == image:
        parser.error('base must be different from the disposable image')
    atexit.register(image.unlink, missing_ok=True)
    subprocess.run(['cp', '--reflink=auto', str(args.base), str(image)], check=True)
    kernel = (ROOT / 'build/kernel.bin').read_bytes()
    stage2 = (ROOT / 'build/stage2.bin').read_bytes()
    assert len(kernel) <= 1280 * 1024 and len(stage2) == 4096
    with image.open('r+b') as boot:
        boot.seek(512)
        boot.write(stage2)
        boot.seek(9 * 512)
        boot.write(kernel.ljust(1280 * 1024, b'\0'))
    editor = Editor(image)
    from lbs_payload import migrated_source, remove_old_libraries
    if args.lbs:
        remove_old_libraries(editor)
        for library in (ROOT / 'build/system/libs/native').glob('*.LBS'):
            editor.update('/SYSTEM/' + library.name, library.read_bytes())
    for guest, host in PAYLOADS.items():
        source = migrated_source(ROOT / host) if args.lbs else ROOT / host
        editor.update(guest, source.read_bytes())
    editor.update('/SYSTEM/USER/CONFIG/STARTUP.INI',
        b'4|1|1|10|0|1|30000|Runbox|/SYSTEM/PROGRAMS/RUNBOX.BEX||UI audit\n')
    editor.update('/SYSTEM/USER/CONFIG/MOUSE.INI',
        b'sensitivity=3\r\ntrail=0\r\nswap=0\r\ndouble_click=500\r\nscheme=0\r\n')
    editor.update('/SYSTEM/USER/CONFIG/LANGUAGE.INI', b'language=ES\r\n')
    editor.update('/UITEST.TXT', TEXT)
    editor.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT')
    editor.finish()

    sock = ROOT / 'build/ui-medium-qmp.sock'
    sock.unlink(missing_ok=True)
    serial = out / 'boot.log'
    serial.unlink(missing_ok=True)
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen([
            'qemu-system-i386', '-accel', args.accel, '-cpu', 'host' if args.accel=='kvm' else 'qemu32', '-m', '256', '-smp', '2',
            '-drive', f'file={image},format=raw,if=ide', '-vga', 'std',
            '-display', 'none', '-serial', f'file:{serial}',
            '-qmp', f'unix:{sock},server=on,wait=off', '-no-reboot',
        ], stderr=log)
        try:
            for _ in range(600):
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited before the desktop')
                if sock.exists() and serial.exists() and 'DESKTOP RELEASED' in serial.read_text(errors='replace'):
                    break
                time.sleep(.5)
            else:
                raise TimeoutError('desktop boot')
            time.sleep(3)
            ui = UI(sock, out)
            def wait_for_launch(name):
                target = {'/uitest.txt': '/SYSTEM/PROGRAMS/TEXTEDITOR.BEX',
                          'files': '/SYSTEM/PROGRAMS/FILE.BEX'}.get(name.lower(),
                          '/SYSTEM/CONTROL/' + name.upper())
                process_name = 'texteditor' if name.lower() == '/uitest.txt' else Path(target).name
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    boot = serial.read_text(errors='replace')
                    if '[LBS:APP] FAIL' in boot or 'excepcion Ring 3' in boot:
                        raise RuntimeError(boot[-3000:])
                    marker = '[LBS:APP] ' + target + ' modules='
                    if (not args.lbs or marker in boot) and 'process=' + process_name + ' icon=' in boot:
                        time.sleep(1)
                        return
                    time.sleep(.2)
                raise TimeoutError('window startup: ' + target)
            ui.wait_for_launch = wait_for_launch
            ui.calibrate()
            ui.shot('01-runbox')
            ui.launch('datetime.cpl')
            ui.click(265, 123)
            ui.shot('02-datetime-zone')
            ui.click(306, 335)
            ui.shot('03-datetime-12h')
            ui.click(665, 78)
            ui.launch('system.cpl', 'datetime.cpl')
            ui.click(226, 84)
            ui.shot('04-system-kernel')
            ui.click(310, 84)
            ui.shot('05-system-hardware')
            ui.click(692, 54)
            ui.launch('language.cpl', 'system.cpl')
            ui.shot('06-language-es')
            ui.click(377, 225)
            ui.shot('07-language-en')
            ui.click(531, 225)
            ui.shot('08-language-it')
            ui.click(223, 225)
            ui.click(665, 82)
            ui.launch('/uitest.txt', 'language.cpl')
            ui.click(175, 79)
            ui.shot('09-editor-edit-menu')
            ui.click(195, 172)
            ui.shot('10-editor-select-all')
            ui.edit(0)
            ui.shot('11-editor-cut')
            ui.edit(2)
            ui.shot('12-editor-paste')
            ui.key('ctrl-a')
            ui.edit(1)
            ui.key('ctrl-x')
            ui.shot('13-editor-ctrl-cut')
            ui.key('ctrl-v')
            ui.key('ctrl-a')
            ui.edit(3)
            ui.shot('14-editor-delete')
            ui.edit(2)
            ui.click(225, 104)
            ui.shot('15-editor-saved')
            ui.click(563, 57)
            ui.launch('files', '/uitest.txt')
            ui.shot('16-filebrowser')
            ui.move(704, 425)
            ui.hmp('mouse_button 1')
            ui.move(282, 221)
            ui.hmp('mouse_button 0')
            ui.shot('17-filebrowser-small')
            ui.click(272, 52)
            ui.click(300, 294)
            for _ in 'files':
                ui.key('backspace')
            ui.click(460, 395)
            ui.shot('18-runbox-empty')
            ui.click(300, 294)
            ui.text('/system/programs/' + 'long-path-' * 7 + 'sample.bex')
            ui.shot('19-runbox-long-path')
            ui.command('quit')
            process.wait(timeout=10)
            ui.stream.close()
            ui.socket.close()
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            sock.unlink(missing_ok=True)

    editor = Editor(image)
    try:
        saved = editor.contents('/UITEST.TXT')
        assert saved == TEXT, repr(saved)
        assert b'format=12' in editor.contents('/SYSTEM/USER/CONFIG/DATETIME.INI')
        assert b'language=ES' in editor.contents('/SYSTEM/USER/CONFIG/LANGUAGE.INI')
    finally:
        editor.fd.close()
    # Check that Cut/Delete really emptied the document, and Paste restored it.
    for name in ['11-editor-cut', '13-editor-ctrl-cut', '14-editor-delete']:
        with Image.open(out / (name + '.png')) as im:
            colors = im.crop((122, 126, 540, 278)).getcolors(100000)
            assert max(count for count, _ in colors) > 63000, (name, colors)
    with Image.open(out / '10-editor-select-all.png') as im:
        assert sum(1 for p in im.crop((120, 125, 230, 150)).getdata()
                   if p == (0, 0, 128)) > 200, 'Select All did not select the text'
    boot = serial.read_text(errors='replace')
    assert '[PANIC]' not in boot and 'KERNEL PANIC' not in boot
    result = {'status': 'pass', 'ram_mib': 256, 'cpus': 2, 'accel': args.accel, 'lbs': args.lbs,
              'saved_text': saved.decode(), 'time_format': 12,
              'language': 'ES', 'screenshots': 19,
              'visual_review_required': True}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: Edit actions, clipboard shortcuts, saved text and preferences', flush=True)


if __name__ == '__main__':
    main()
