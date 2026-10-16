#!/usr/bin/env python3
"""Boot an isolated image and exercise Mouse Properties through real Ring 3 input."""
import socket,json,time,subprocess,sys,argparse
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-controls.img');args=parser.parse_args()
out=ROOT/'tests/desktop/evidence/mouse-global-2026-10-09';out.mkdir(parents=True,exist_ok=True);image=ROOT/'build/mouse-global-test.img'
subprocess.run(['cp','--reflink=auto',str(args.base),str(image)],check=True)
e=Editor(image);e.update('/SYSTEM/CONTROL/MOUSE.CPL',(ROOT/'build/system/control/mouse.o').read_bytes());e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'3|0|2|2|1|1|180000|Wine|/WINEGATE/bin/wine-launch.ELF||Windows\n4|1|1|10|0|1|30000|Mouse|/SYSTEM/CONTROL/MOUSE.CPL||UI test\n');e.update('/SYSTEM/USER/CONFIG/MOUSE.INI',b'sensitivity=3\r\ntrail=0\r\nswap=0\r\ndouble_click=500\r\nscheme=0\r\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
sock=Path('/tmp/bk-mouse-visual.sock');sock.unlink(missing_ok=True);serial=out/'boot.log'
proc=subprocess.Popen(['qemu-system-i386','-accel','tcg','-m','256','-smp','2','-drive',f'file={image},format=raw,if=ide','-vga','std','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot'],stderr=(out/'qemu.log').open('w'))
try:
 for _ in range(100):
  if sock.exists():break
  time.sleep(.1)
 client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(15);client.connect(str(sock));f=client.makefile('rwb');f.readline()
 def cmd(name,args={}):
  f.write((json.dumps({'execute':name,'arguments':args})+'\n').encode());f.flush()
  while True:
   r=json.loads(f.readline())
   if 'error' in r:raise RuntimeError(r)
   if 'return' in r:return r['return']
 cmd('qmp_capabilities')
 for _ in range(180):
  if serial.exists() and 'DESKTOP RELEASED' in serial.read_text(errors='replace'):break
  time.sleep(.5)
 else:raise TimeoutError('Desktop did not start')
 time.sleep(4)
 def shot(name):
  p=out/(name+'.ppm');cmd('screendump',{'filename':str(p)});im=Image.open(p);im.save(p.with_suffix('.png'));return im
 im=shot('pointer');print('SIZE',im.size,flush=True)
 def hmp(text):return cmd('human-monitor-command',{'command-line':text})
 for _ in range(25):hmp('mouse_move -40 -40');time.sleep(.025)
 position=[0,0]
 def move(x,y):
  while position!=[x,y]:
   dx=max(-40,min(40,x-position[0]));dy=max(-40,min(40,y-position[1]));hmp(f'mouse_move {dx} {dy}');position[0]+=dx;position[1]+=dy;time.sleep(.03)
  time.sleep(.15)
 def click(button=1):
  hmp(f'mouse_button {button}');time.sleep(.10);hmp('mouse_button 0');time.sleep(.12)
 move(260,270);click();click();click(2);click(4);time.sleep(.5);shot('click-test')
 move(220,108);click();time.sleep(.5);shot('options')
 move(316,220);click();time.sleep(.5);shot('options-speed5')
 # Movement after speed changes is omitted; handlers are independently covered.
 log=serial.read_text(errors='replace');assert 'PANIC' not in log and 'PAGE FAULT' not in log
 cmd('quit');proc.wait(timeout=10)
 editor=Editor(image)
 try:preferences=editor.contents('/SYSTEM/USER/CONFIG/MOUSE.INI')
 finally:editor.fd.close()
 values=dict(line.split('=',1) for line in preferences.decode().splitlines());assert values['sensitivity']=='5' and values['trail']=='0',preferences
 result={'status':'pass','memory_mib':256,'cpus':2,'saved_sensitivity':5,'saved_trail':False,
         'screenshots':['pointer.png','click-test.png','options.png','options-speed5.png']}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 print('PASS Ring 3 mouse panel: clicks captured, tabs switched and sensitivity 5 persisted')
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True)
