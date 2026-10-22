#!/usr/bin/env python3
"""Exercise the selector with real File/Paint windows on a disposable guest."""
import json,subprocess,sys,time
from pathlib import Path
from PIL import Image,ImageChops
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from run_medium_ui_smoke import UI
out=ROOT/'tests/desktop/evidence/switcher-2026-10-18';out.mkdir(parents=True,exist_ok=True)
(out/'result.json').unlink(missing_ok=True)
image=ROOT/'build/switcher-test.img';sock=ROOT/'build/switcher-test.sock';serial=out/'boot.log'
subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-ata-user.img'),str(image)],check=True)
e=Editor(image);e.update('/SYSTEM/SERVICES/VISION.BEX',(ROOT/'build/system/services/vision.o').read_bytes());e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'');e.update('/SYSTEM/USER/CONFIG/MOUSE.INI',b'sensitivity=3\r\ntrail=0\r\nswap=0\r\ndouble_click=500\r\nscheme=0\r\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
sock.unlink(missing_ok=True);serial.unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','tcg','-cpu','qemu32','-m','256','-smp','2','-vga','std','-drive',f'file={image},format=raw,if=ide','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot'],stderr=(out/'qemu.log').open('w'))
def picture(name):return Image.open(out/(name+'.png')).convert('RGB')
def equal(a,b):return not ImageChops.difference(a,b).getbbox()
try:
 for _ in range(240):
  if proc.poll() is not None:raise RuntimeError('QEMU exited')
  if serial.exists() and '[VISION:ENGINE]' in serial.read_text(errors='replace'):break
  time.sleep(.5)
 else:raise TimeoutError('Vision boot')
 time.sleep(2);ui=UI(sock,out);ui.calibrate();ui.move(750,400);ui.shot('00-empty')
 for _ in range(30):
  if picture('00-empty').getpixel((400,580)) != (0,0,0):break
  time.sleep(1);ui.shot('00-empty')
 else:raise TimeoutError('Desktop not visible')
 ui.click(674,580);ui.move(750,400);ui.shot('00b-empty-click')
 slot=(655,561,693,596);top=(655,550,693,561)
 assert equal(picture('00-empty').crop(slot),picture('00b-empty-click').crop(slot))
 ui.click(123,580);time.sleep(3);ui.move(750,400);ui.shot('01-files')
 # Launcher and selected program use the same existing icon, at native 32px.
 assert equal(picture('01-files').crop((658,562,690,594)),picture('01-files').crop((108,562,140,594))),'File icon does not follow window'
 assert equal(picture('00-empty').crop(top),picture('01-files').crop(top))
 ui.click(235,580)
 deadline=time.monotonic()+30
 while time.monotonic()<deadline:
  if 'icon=Mspaint' in serial.read_text(errors='replace'):break
  time.sleep(.5)
 else:raise TimeoutError('Paint did not finish opening')
 ui.move(750,400);ui.shot('02-paint')
 assert equal(picture('02-paint').crop((658,562,690,594)),picture('02-paint').crop((219,562,251,594))),'Paint icon does not follow window'
 assert not equal(picture('01-files').crop(top),picture('02-paint').crop(top))
 ui.click(674,556);ui.shot('03-menu')
 # Two items: File then Paint. Select File and check its icon returns.
 ui.click(560,488);time.sleep(1);ui.move(750,400);ui.shot('04-files-selected')
 assert equal(picture('01-files').crop(slot),picture('04-files-selected').crop(slot))
 # Minimize File, then select it from the menu to restore it.
 ui.click(675,51);time.sleep(1);ui.click(674,556);ui.shot('05-minimized-menu')
 ui.click(560,488);time.sleep(1);ui.move(750,400);ui.shot('06-restored')
 assert equal(picture('04-files-selected').crop((60,40,705,420)),picture('06-restored').crop((60,40,705,420))),'File was not restored'
 # Close File: Paint should occupy the selector and the arrow disappear.
 ui.click(692,51);time.sleep(1);ui.move(750,400);ui.shot('07-paint-only')
 assert equal(picture('02-paint').crop(slot),picture('07-paint-only').crop(slot))
 assert equal(picture('00-empty').crop(top),picture('07-paint-only').crop(top))
 ui.click(674,580);time.sleep(1)
 ui.click(597,56);time.sleep(1);ui.move(750,400);ui.shot('08-empty-again')
 assert equal(picture('00-empty').crop(slot),picture('08-empty-again').crop(slot))
 assert equal(picture('00-empty').crop(top),picture('08-empty-again').crop(top))
 text=serial.read_text(errors='replace');assert '[PANIC]' not in text and '[TASK] excepcion' not in text
 (out/'result.json').write_text(json.dumps({'status':'pass','desktop_icon':True,'real_icons':True,'conditional_arrow':True,'switch_focus':True,'restore_minimized':True,'close_updates_selector':True},indent=2)+'\n')
 ui.click(400,578);ui.move(400,379);time.sleep(1);ui.shot('09-start-programs')
 ui.command('quit');proc.wait(timeout=10);ui.stream.close();ui.socket.close()
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True)
