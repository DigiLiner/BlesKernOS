#!/usr/bin/env python3
"""Measure production Paint frames and exercise drawing/icons in isolated QEMU.

--baseline-source accepts a saved pre-fix paint.c for the same probe and machine.
The default run also verifies the saved BMP after pencil, fill, eraser, line,
rectangle and repeated repaints/window movement.
"""
import argparse
import atexit
import io
import json
import re
import socket
import subprocess
import sys
import time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from bfs4_update import Editor
from run_medium_ui_smoke import UI


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base', type=Path, default=ROOT/'build/bleskernos-ata-user.img')
    p.add_argument('--accel',choices=['tcg','kvm'],default='tcg')
    p.add_argument('--baseline-source', type=Path)
    p.add_argument('--out', type=Path, default=ROOT/'tests/desktop/evidence/paint-2026-10-17')
    args = p.parse_args()
    tag = 'before' if args.baseline_source else 'after'
    out = args.out.resolve()/tag
    out.mkdir(parents=True, exist_ok=True)
    (out/'result.json').unlink(missing_ok=True)
    obj = out/'probe.o'
    flags = ['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2',
             '-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic',
             '-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include')]
    extra = []
    if args.baseline_source:
        source = out/'paint-before.c'
        source.write_text(args.baseline_source.read_text().replace('"../kernel/include/api.h"','"kernel/include/api.h"'))
        extra = ['-DPAINT_PROBE_BASELINE', f'-DPAINT_SOURCE_PATH="{source}"']
    subprocess.run(flags+extra+['-c',str(ROOT/'tests/userland/paint_probe.c'),'-o',str(obj)],check=True)
    if not args.baseline_source:
        migrated=out/'probe-lbs.o'
        subprocess.run(['python3',str(ROOT/'tools/generate_lbs_client.py'),'--source',str(obj),'--output',str(migrated),'--destination','/SYSTEM/PROGRAMS/PAINTPROBE.BEX'],check=True)
        obj=migrated
    app = ROOT/'build/lbs/apps/programs/paint.o'
    if args.baseline_source:
        app = out/'paint-before.o'
        subprocess.run(flags+['-c',str(source),'-o',str(app)],check=True)
    image = ROOT/f'build/paint-{tag}-test.img'
    if image.resolve() == args.base.resolve():
        p.error('base must differ from disposable test image')
    atexit.register(image.unlink, missing_ok=True)
    subprocess.run(['cp','--reflink=auto',str(args.base),str(image)],check=True)
    e = Editor(image)
    for library in (ROOT/'build/system/libs/native').glob('*.LBS'):e.update('/SYSTEM/'+library.name,library.read_bytes())
    e.update('/SYSTEM/PROGRAMS/PAINTPROBE.BEX',obj.read_bytes())
    e.update('/SYSTEM/PROGRAMS/PAINT.BEX',app.read_bytes())
    e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Paint probe|/SYSTEM/PROGRAMS/PAINTPROBE.BEX||Paint regression\n')
    e.update('/SYSTEM/USER/CONFIG/MOUSE.INI',b'sensitivity=3\r\ntrail=0\r\nswap=0\r\ndouble_click=500\r\nscheme=0\r\n')
    e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT')
    e.finish()
    sock = ROOT/f'build/paint-{tag}-qmp.sock'
    sock.unlink(missing_ok=True)
    serial = out/'boot.log'
    with (out/'qemu.log').open('w') as log:
        proc = subprocess.Popen(['qemu-system-i386','-accel',args.accel,'-m','256','-smp','2',
            '-drive',f'file={image},format=raw,if=ide','-vga','std','-display','none',
            '-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot'],stderr=log)
        try:
            for _ in range(360):
                if proc.poll() is not None: raise RuntimeError('QEMU exited')
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if sock.exists() and '[PAINTPROBE] DONE' in text: break
                time.sleep(.5)
            else: raise TimeoutError('Paint probe')
            match = re.search(r'frames=(\d+) render_ms=(\d+) icons_alive=(\d+)',text)
            assert match,text[-3000:]
            failures = int(re.search(r'DONE failures=(\d+)',text)[1])
            result = {'frames':int(match[1]),'render_ms':int(match[2]),'icons_alive':int(match[3]),
                      'probe_failures':failures,'ram_mib':256,'cpus':2,'accel':args.accel}
            print(tag,result,flush=True)
            if not args.baseline_source: assert failures == 0,text[-4000:]
            time.sleep(3)
            ui = UI(sock,out)
            ui.calibrate()
            ui.shot('01-initial')
            if not args.baseline_source:
                ui.key('1')
                ui.click(444,275)  # red
                ui.key('p')
                ui.move(117,173)
                ui.hmp('mouse_button 1')
                for x,y in [(380,173),(380,310),(117,310),(117,173)]: ui.move(x,y)
                ui.hmp('mouse_button 0')
                ui.shot('02-pencil')
                ui.click(492,275)  # blue
                ui.key('f')
                ui.click(250,230)
                ui.shot('03-fill')
                ui.key('e')
                ui.key('8')
                ui.move(150,173)
                ui.hmp('mouse_button 1')
                ui.move(350,173)
                ui.hmp('mouse_button 0')
                ui.shot('04-eraser')
                ui.key('l')
                ui.click(468,275)  # green
                ui.key('1')
                ui.move(147,203)
                ui.hmp('mouse_button 1')
                ui.move(347,253)
                ui.hmp('mouse_button 0')
                ui.key('r')
                ui.click(444,255)  # black
                ui.move(157,213)
                ui.hmp('mouse_button 1')
                ui.move(317,283)
                ui.hmp('mouse_button 0')
                ui.key('s')
                ui.shot('05-lines-rectangle-saved')
                ui.move(250,57)
                ui.hmp('mouse_button 1')
                ui.move(300,87)
                ui.hmp('mouse_button 0')
                ui.shot('06-moved-icons')
            ui.command('quit')
            proc.wait(timeout=10)
            ui.stream.close()
            ui.socket.close()
        finally:
            if proc.poll() is None:
                proc.terminate();proc.wait(timeout=10)
            sock.unlink(missing_ok=True)
    if not args.baseline_source:
        e = Editor(image)
        try: bmp = e.contents('/DRAWING.BMP')
        finally: e.fd.close()
        with Image.open(io.BytesIO(bmp)) as im:
            im = im.convert('RGB')
            assert im.size == (320,200), im.size
            expected = {(20,80):(255,0,0),(100,100):(0,0,255),
                        (100,20):(255,255,255),(150,75):(0,255,0),(60,90):(0,0,0)}
            for point,color in expected.items():
                assert im.getpixel(point) == color,(point,im.getpixel(point),color)
            # Every pixel of the untouched left edge must remain connected.
            assert all(im.getpixel((20,y)) == (255,0,0) for y in range(20,158))
            assert all(im.getpixel((283,y)) == (255,0,0) for y in range(20,158))
            assert all(im.getpixel((x,157)) == (255,0,0) for x in range(20,284))
            im.save(out/'saved-canvas.png')
        (out/'DRAWING.BMP').write_bytes(bmp)
        result['saved_bmp'] = 'pass'
        # Compare the four inactive icons before/after drawing and dragging
        # the window. Pencil is active (and shifted by 1 px) in the first shot.
        with Image.open(out/'01-initial.png') as initial, Image.open(out/'06-moved-icons.png') as moved:
            for i in range(1,5):
                x = 102 + i * 60
                a = initial.crop((x,101,x+16,117))
                b = moved.crop((x+50,131,x+66,147))
                assert a.tobytes() == b.tobytes(), ('icon changed',i)
                assert len(a.getcolors(256) or []) > 3, ('icon missing',i)
        result['toolbar_after_draw_and_move'] = 'pass'
        result['stroke_continuity'] = 'pass'
    result['status'] = 'baseline' if args.baseline_source else 'pass'
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Completed',tag,flush=True)


if __name__ == '__main__': main()
